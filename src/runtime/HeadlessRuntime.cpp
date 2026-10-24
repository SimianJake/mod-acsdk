#include "HeadlessRuntime.h"

#include "AcsdkModule.h"

#include "CharacterCache.h"
#include "Common.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PacketUtilities.h"
#include "Player.h"
#include "QueryResult.h"
#include "RBAC.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>

namespace acsdk
{
    char const* LifecycleName(Lifecycle state)
    {
        switch (state)
        {
            case Lifecycle::Offline:    return "OFFLINE";
            case Lifecycle::LoggingIn:  return "LOGGING_IN";
            case Lifecycle::Online:     return "ONLINE";
            case Lifecycle::LoggingOut: return "LOGGING_OUT";
        }
        return "UNKNOWN";
    }

    // ---------------------------------------------------------------- PacketSink

    void PacketSink::Want(uint16_t opcode)
    {
        std::lock_guard lock(_mutex);
        _wanted.insert(opcode);
    }

    bool PacketSink::Wants(uint16_t opcode) const
    {
        if (ExtraWants extra = s_extraWants.load(std::memory_order_relaxed); extra && extra(opcode))
            return true;
        std::lock_guard lock(_mutex);
        return _wanted.contains(opcode);
    }

    void PacketSink::Push(uint16_t opcode, uint8_t const* data, std::size_t size)
    {
        if (!Wants(opcode))
            return;

        std::lock_guard lock(_mutex);
        if (_packets.size() >= _capacity)
        {
            _packets.pop_front(); // bounded: drop oldest (PLAN.md §4.1 #4)
            ++_dropped;
        }

        CapturedPacket& p = _packets.emplace_back();
        p.opcode = opcode;
        if (data && size)
            p.bytes.assign(data, data + size);
    }

    std::vector<CapturedPacket> PacketSink::Take()
    {
        std::lock_guard lock(_mutex);
        std::vector<CapturedPacket> out(std::make_move_iterator(_packets.begin()), std::make_move_iterator(_packets.end()));
        _packets.clear();
        return out;
    }

    uint64_t PacketSink::Dropped() const
    {
        std::lock_guard lock(_mutex);
        return _dropped;
    }

    // ---------------------------------------------------------------- HeadlessRuntime

    HeadlessRuntime::HeadlessRuntime() = default;
    HeadlessRuntime::~HeadlessRuntime() = default;

    void HeadlessRuntime::Configure(std::string accountPrefix, uint32_t maxCharacters, std::size_t sinkCapacity)
    {
        _accountPrefix = std::move(accountPrefix);
        _maxCharacters = maxCharacters;
        _sinkCapacity = sinkCapacity;
    }

    HeadlessCharacter* HeadlessRuntime::Find(ObjectGuid guid)
    {
        auto it = _characters.find(guid);
        return it == _characters.end() ? nullptr : it->second.get();
    }

    Player* HeadlessRuntime::FindOnlinePlayer(ObjectGuid guid)
    {
        HeadlessCharacter* c = Find(guid);
        if (!c || c->state != Lifecycle::Online || !c->session)
            return nullptr;

        Player* player = c->session->GetPlayer();
        return player && player->IsInWorld() ? player : nullptr;
    }

    std::vector<HeadlessCharacter const*> HeadlessRuntime::All() const
    {
        std::vector<HeadlessCharacter const*> out;
        out.reserve(_characters.size());
        for (auto const& [guid, c] : _characters)
            out.push_back(c.get());
        return out;
    }

    bool HeadlessRuntime::HoldsAccount(uint32_t accountId) const
    {
        return std::any_of(_characters.begin(), _characters.end(), [accountId](auto const& kv) { return kv.second->accountId == accountId; });
    }

    bool HeadlessRuntime::IsHeadless(WorldSession const* session) const
    {
        std::shared_lock lock(_sessionsMutex);
        return _sessions.contains(session);
    }

    std::shared_ptr<PacketSink> HeadlessRuntime::SinkFor(WorldSession const* session) const
    {
        std::shared_lock lock(_sessionsMutex);
        auto it = _sessions.find(session);
        return it == _sessions.end() ? nullptr : it->second;
    }

    void HeadlessRuntime::Login(std::string const& name, LoginDone done)
    {
        if (_stopping)
        {
            done({ false, "SERVER_SHUTDOWN", ObjectGuid::Empty, name });
            return;
        }

        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
        if (!guid)
        {
            done({ false, "CHARACTER_NOT_FOUND", ObjectGuid::Empty, name });
            return;
        }

        if (HeadlessCharacter* existing = Find(guid))
        {
            switch (existing->state)
            {
                case Lifecycle::Online:
                    done({ true, "", guid, existing->name }); // already online: no-op (PLAN.md §7 example)
                    return;
                case Lifecycle::LoggingIn:
                    existing->loginWaiters.push_back(std::move(done));
                    return;
                default:
                    done({ false, "LOGGING_OUT", guid, existing->name });
                    return;
            }
        }

        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
        if (!accountId)
        {
            done({ false, "CHARACTER_NOT_FOUND", guid, name });
            return;
        }

        // One online character per account (PLAN.md §5.1), whether the other session is a client or ours.
        if (ObjectAccessor::FindConnectedPlayer(guid))
        {
            done({ false, "CHARACTER_ALREADY_ONLINE", guid, name });
            return;
        }

        if (sWorldSessionMgr->FindSession(accountId) || sWorldSessionMgr->FindOfflineSession(accountId) ||
            std::any_of(_characters.begin(), _characters.end(), [accountId](auto const& kv) { return kv.second->accountId == accountId; }))
        {
            done({ false, "ACCOUNT_ALREADY_ONLINE", guid, name });
            return;
        }

        if (_characters.size() >= _maxCharacters)
        {
            done({ false, "MAX_HEADLESS_CHARACTERS", guid, name });
            return;
        }

        auto c = std::make_unique<HeadlessCharacter>();
        c->guid = guid;
        c->accountId = accountId;
        c->name = name;
        c->state = Lifecycle::LoggingIn;
        c->loginId = _nextLoginId++;
        c->loginWaiters.push_back(std::move(done));
        uint64_t loginId = c->loginId;
        _characters.emplace(guid, std::move(c));

        // Async account lookup; never block the world thread on the DB (PLAN.md Phase 2).
        std::string sql = Acore::StringFormat(
            "SELECT a.username, a.expansion, a.locale, a.mutetime, "
            "COALESCE((SELECT MAX(aa.gmlevel) FROM account_access aa WHERE aa.id = a.id AND (aa.RealmID = -1 OR aa.RealmID = {})), 0) "
            "FROM account a WHERE a.id = {}",
            realm.Id.Realm, accountId);

        sAcsdk.Queries().AddCallback(LoginDatabase.AsyncQuery(sql).WithCallback(
            [this, guid, loginId, accountId](QueryResult result)
            {
                HeadlessCharacter* c = Find(guid);
                if (!c || c->loginId != loginId)
                    return;

                if (!result)
                {
                    FinishLogin(*c, { false, "ACCOUNT_NOT_FOUND", guid, c->name });
                    return;
                }

                Field* fields = result->Fetch();
                std::string username = fields[0].Get<std::string>();
                std::string upper = username;
                std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
                if (_accountPrefix.empty() || upper.rfind(_accountPrefix, 0) != 0)
                {
                    FinishLogin(*c, { false, "ACCOUNT_NOT_ELIGIBLE", guid, c->name });
                    return;
                }

                uint8 security = static_cast<uint8>(fields[4].Get<uint64>());
                if (security > SEC_PLAYER)
                {
                    // Chat starting with '.' would run as GM commands (docs/research/opcode-map.md).
                    FinishLogin(*c, { false, "ACCOUNT_NOT_PLAYER_LEVEL", guid, c->name });
                    return;
                }

                StartSessionAndLoad(guid, loginId, accountId, std::move(username), security,
                                    fields[1].Get<uint8>(), fields[3].Get<int64>(),
                                    static_cast<uint8>(GetLocaleByName(fields[2].Get<std::string>())));
            }));
    }

    void HeadlessRuntime::StartSessionAndLoad(ObjectGuid guid, uint64_t loginId, uint32_t accountId, std::string accountName,
                                              uint8_t security, uint8_t expansion, int64_t muteTime, uint8_t locale)
    {
        HeadlessCharacter* c = Find(guid);
        if (!c)
            return;

        // Whitelisted direct call (PLAN.md §4.6.3, session lifecycle): there is no opcode for "open a
        // connection", so we construct the session AzerothCore would have created for a socket.
        c->session = new WorldSession(accountId, std::move(accountName), 0, nullptr, AccountTypes(security), expansion,
                                      static_cast<time_t>(muteTime), LocaleConstant(locale), 0, false, true, 0);
        c->sink = std::make_shared<PacketSink>(_sinkCapacity);
        {
            std::unique_lock lock(_sessionsMutex);
            _sessions[c->session] = c->sink;
        }

        // Load RBAC asynchronously on the world thread. Otherwise the first HasPermission() call loads it
        // synchronously, possibly from a map thread (docs/research/headless-sessions.md §1).
        WorldSession* session = c->session;
        sAcsdk.Queries().AddCallback(session->LoadPermissionsAsync().WithPreparedCallback(
            [this, guid, loginId, session](PreparedQueryResult rbac)
            {
                HeadlessCharacter* c = Find(guid);
                if (!c || c->loginId != loginId || c->session != session)
                    return;

                session->GetRBACData()->LoadFromDBCallback(rbac);

                auto holder = std::make_shared<LoginQueryHolder>(c->accountId, guid);
                if (!holder->Initialize())
                {
                    FinishLogin(*c, { false, "LOGIN_QUERY_FAILED", guid, c->name });
                    return;
                }

                // Same path as HandlePlayerLoginOpcode: AzerothCore's own async character load and login.
                session->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
                    [this, guid, loginId, session](SQLQueryHolderBase const& h)
                    {
                        session->HandlePlayerLoginFromDB(static_cast<LoginQueryHolder const&>(h));

                        // Runs inside session->ProcessQueryCallbacks(): only flag the result here; the
                        // session must not be destroyed while its own processor is on the stack.
                        if (HeadlessCharacter* c = Find(guid); c && c->loginId == loginId)
                            c->loginCallbackDone = true;
                    });
            }));
    }

    void HeadlessRuntime::FinishLogin(HeadlessCharacter& c, LoginResult const& result)
    {
        std::vector<LoginDone> waiters = std::move(c.loginWaiters);
        c.loginWaiters.clear();

        if (result.ok)
        {
            c.state = Lifecycle::Online;
            c.unleasedSince = std::chrono::steady_clock::now();
            LOG_INFO("module", "mod-acsdk: headless character {} ({}) is online.", c.name, c.guid.GetCounter());
        }
        else
        {
            LOG_INFO("module", "mod-acsdk: headless login of {} failed: {}.", c.name, result.reason);
            ObjectGuid guid = c.guid;
            DestroySession(c, false);
            Remove(guid); // c is gone after this
        }

        for (LoginDone& done : waiters)
            done(result);
    }

    bool HeadlessRuntime::Inject(ObjectGuid guid, std::unique_ptr<WorldPacket> packet)
    {
        HeadlessCharacter* c = Find(guid);
        if (!c || !c->session || c->state != Lifecycle::Online)
            return false;

        c->session->QueuePacket(packet.release()); // session takes ownership
        return true;
    }

    void HeadlessRuntime::DrainInjectedPackets(HeadlessCharacter& c)
    {
        WorldSession* session = c.session;
        // AzerothCore never drains the receive queue of a socketless session (WorldSession.cpp:414), so we
        // do what WorldSession::Update would: status gate, CanPacketReceive, handler, exception guard.
        // Runs on the world thread while no map thread is active, which also covers THREADSAFE handlers.
        for (int processed = 0; processed < 150; ++processed)
        {
            std::unique_ptr<WorldPacket> packet = session->NextQueuedPacket();
            if (!packet)
                break;

            uint16 opcode = packet->GetOpcode();
            if (opcode >= NUM_OPCODE_HANDLERS)
                continue;

            ClientOpcodeHandler const* handler = opcodeTable[static_cast<Opcodes>(opcode)];
            if (!handler)
                continue;

            Player* player = session->GetPlayer();
            bool statusOk = false;
            switch (handler->Status)
            {
                case STATUS_LOGGEDIN:
                    statusOk = player && player->IsInWorld();
                    break;
                case STATUS_TRANSFER:
                    statusOk = player && !player->IsInWorld();
                    break;
                case STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT:
                    statusOk = player || session->PlayerRecentlyLoggedOut();
                    break;
                case STATUS_AUTHED:
                    statusOk = opcode != CMSG_PLAYER_LOGIN; // login goes through Login(), never a packet
                    break;
                default:
                    statusOk = false; // STATUS_NEVER / STATUS_UNHANDLED
                    break;
            }

            if (!statusOk)
            {
                LOG_DEBUG("module", "mod-acsdk: dropped injected {} for {}: wrong session status.", handler->Name, c.name);
                continue;
            }

            if (!sScriptMgr->CanPacketReceive(session, *packet))
                continue;

            try
            {
                handler->Call(session, *packet);
            }
            catch (WorldPackets::InvalidHyperlinkException const&)
            {
                LOG_ERROR("module", "mod-acsdk: injected {} for {} had an invalid hyperlink.", handler->Name, c.name);
            }
            catch (WorldPackets::IllegalHyperlinkException const&)
            {
                LOG_ERROR("module", "mod-acsdk: injected {} for {} had an illegal hyperlink.", handler->Name, c.name);
            }
            catch (WorldPackets::PacketArrayMaxCapacityException const&)
            {
                LOG_ERROR("module", "mod-acsdk: injected {} for {} exceeded an array limit.", handler->Name, c.name);
            }
            catch (ByteBufferException const&)
            {
                LOG_ERROR("module", "mod-acsdk: injected {} for {} was malformed.", handler->Name, c.name);
            }

            if (!session->GetPlayer())
                break; // the handler logged the character out
        }
    }

    void HeadlessRuntime::RunReflexes(HeadlessCharacter& c)
    {
        // Protocol reflexes (PLAN.md §5.2): the acks a client sends so it isn't stuck mid-teleport.
        Player* player = c.session->GetPlayer();
        if (!player)
            return;

        if (player->IsBeingTeleportedFar() && sAcsdk.GetConfig().reflexTeleportAck)
        {
            // Public "for server-side calls" (WorldSession.h); what AC itself does on login reclaim.
            c.session->HandleMoveWorldportAck();
            player = c.session->GetPlayer();
            if (!player)
                return;
        }

        if (player->IsInWorld() && player->IsBeingTeleportedNear() && sAcsdk.GetConfig().reflexTeleportAck)
        {
            if (Player* mover = player->m_mover->ToPlayer())
            {
                WorldPacket ack(MSG_MOVE_TELEPORT_ACK, 20);
                ack << mover->GetPackGUID();
                ack << uint32(0); // flags
                ack << uint32(0); // time
                c.session->HandleMoveTeleportAck(ack);
            }
        }
    }

    void HeadlessRuntime::Update()
    {
        std::vector<ObjectGuid> toRemove;

        for (auto& [guid, cptr] : _characters)
        {
            HeadlessCharacter& c = *cptr;
            if (!c.session)
                continue;

            // Not registered with WorldSessionMgr, so nobody else pumps these (headless-sessions.md §2.3).
            c.session->ProcessQueryCallbacks();

            if (c.state == Lifecycle::LoggingIn && c.loginCallbackDone)
            {
                Player* player = c.session->GetPlayer();
                bool ok = player && player->IsInWorld() && !c.session->IsKicked();
                c.loginCallbackDone = false;
                std::vector<LoginDone> waiters = std::move(c.loginWaiters);
                c.loginWaiters.clear();
                LoginResult result{ ok, ok ? "" : "LOGIN_FAILED", c.guid, c.name };

                if (ok)
                {
                    c.state = Lifecycle::Online;
                    c.unleasedSince = std::chrono::steady_clock::now();
                    LOG_INFO("module", "mod-acsdk: headless character {} ({}) is online.", c.name, c.guid.GetCounter());
                }
                else
                {
                    LOG_INFO("module", "mod-acsdk: headless login of {} failed.", c.name);
                    DestroySession(c, false);
                    toRemove.push_back(c.guid);
                }

                for (LoginDone& done : waiters)
                    done(result);

                if (!ok)
                    continue;
            }

            if (c.state != Lifecycle::Online)
                continue;

            RunReflexes(c);
            DrainInjectedPackets(c);

            bool kicked = c.session->IsKicked();
            bool timedLogout = c.session->ShouldLogOut(time(nullptr)) && !c.session->PlayerLoading();
            bool gone = !c.session->GetPlayer();

            if (c.logoutRequested || kicked || timedLogout || gone)
            {
                if (kicked)
                    LOG_INFO("module", "mod-acsdk: headless character {} was kicked; logging out.", c.name);

                c.state = Lifecycle::LoggingOut;
                DestroySession(c, true);
                std::vector<LogoutDone> waiters = std::move(c.logoutWaiters);
                c.logoutWaiters.clear();
                for (LogoutDone& done : waiters)
                    done(true, "");
                toRemove.push_back(c.guid);
            }
        }

        for (ObjectGuid guid : toRemove)
            Remove(guid);
    }

    void HeadlessRuntime::Logout(ObjectGuid guid, LogoutDone done)
    {
        HeadlessCharacter* c = Find(guid);
        if (!c)
        {
            done(false, "NOT_HEADLESS_ONLINE");
            return;
        }

        // Handled in the next Update(), outside any session callback.
        c->logoutRequested = true;
        c->logoutWaiters.push_back(std::move(done));
        if (c->state == Lifecycle::LoggingIn)
            return; // logs out right after the login finishes
    }

    void HeadlessRuntime::DestroySession(HeadlessCharacter& c, bool save)
    {
        if (!c.session)
            return;

        {
            std::unique_lock lock(_sessionsMutex);
            _sessions.erase(c.session);
        }

        // Whitelisted (session lifecycle): AC's own logout path, on the world thread with no map thread
        // running (headless-sessions.md §4.2).
        if (c.session->GetPlayer())
            c.session->LogoutPlayer(save);

        delete c.session;
        c.session = nullptr;
        c.state = Lifecycle::Offline;
    }

    void HeadlessRuntime::Remove(ObjectGuid guid)
    {
        auto it = _characters.find(guid);
        if (it == _characters.end())
            return;

        // Every waiter gets exactly one answer, even when the character goes away early.
        std::unique_ptr<HeadlessCharacter> c = std::move(it->second);
        _characters.erase(it);
        for (LoginDone& done : c->loginWaiters)
            done({ false, "LOGGED_OUT", c->guid, c->name });
        for (LogoutDone& done : c->logoutWaiters)
            done(true, "");
    }

    void HeadlessRuntime::ShutdownAll()
    {
        // WorldSessionMgr::KickAll never reaches our sessions (headless-sessions.md §4.4).
        _stopping = true;
        for (auto& [guid, c] : _characters)
        {
            DestroySession(*c, true);
            for (LoginDone& done : c->loginWaiters)
                done({ false, "SERVER_SHUTDOWN", c->guid, c->name });
            for (LogoutDone& done : c->logoutWaiters)
                done(true, "");
        }
        _characters.clear();
    }
}
