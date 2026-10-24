#include "AcsdkModule.h"

#include "Errors.h"
#include "MotionMaster.h"
#include "Player.h"
#include "Log.h"
#include "PacketEvents.h"

#include <algorithm>

namespace acsdk
{
    Module& Module::Instance()
    {
        static Module instance;
        return instance;
    }

    Module::Module() : _queue(4096), _leases(std::make_unique<LeaseManager>(GenerateLeaseToken, std::chrono::seconds(15), true)) { }

    void Module::Startup(std::string realmName, std::string coreRevision)
    {
        _worldThread = std::this_thread::get_id();
        _config = Config::Load();

        if (!_config.enable)
        {
            LOG_INFO("module", "mod-acsdk: disabled by ACSDK.Enable.");
            return;
        }

        _queue.SetCapacity(_config.commandQueueSize);
        _runtime.Configure(_config.headlessAccountPrefix, _config.maxHeadlessCharacters, _config.packetSinkSize);
        _commands.SetKnockbackReflex(_config.reflexKnockback);
        _leases = std::make_unique<LeaseManager>(GenerateLeaseToken, std::chrono::seconds(_config.leaseTtlSec), _config.leaseAllowForceTakeover);
        _events.SetQueueCapacity(_config.eventQueueSize);
        _events.SetEventOpcodes(EventSourceOpcodes());
        _prompts.SetTimeout(std::chrono::seconds(_config.promptTimeoutSec));
        // Opcode-first capture: event-source opcodes are copied only while someone is subscribed (§4.5).
        PacketSink::SetExtraWants([](uint16_t opcode) { return sAcsdk.Events().WantsOpcode(opcode); });

        // Every captured packet: event translation (official SMSG sources, docs/events.md) and prompts.
        _commands.SetPacketObserver([this](ObjectGuid character, CapturedPacket const& packet)
        {
            PublishPacketEvents(_events, character, packet, [this, character](uint8_t castCount) { return _commands.CommandForCast(character, castCount); });
            _prompts.OnPacket(character, packet);
        });
        // CommandResult events (SDK-internal source).
        _commands.SetResultObserver([this](ObjectGuid character, CommandResult const& r)
        {
            if (!_events.HasSubscribers())
                return;
            auto e = std::make_unique<v1::Event>();
            v1::CommandResultEvent* c = e->mutable_command_result();
            c->set_command_id(r.id);
            c->set_status(static_cast<uint32_t>(r.status));
            c->set_reason(r.reason);
            c->set_spell_id(r.spellId);
            _events.Publish(character.GetRawValue(), std::move(e));
        });

        _info.realmName = std::move(realmName);
        _info.coreRevision = std::move(coreRevision);
        _info.startedAt = std::chrono::steady_clock::now();

        if (!_server.Start(_config))
            return;

        // Config-declared logins are not autonomy (PLAN.md §5.1).
        for (std::string const& name : _config.autostartCharacters)
        {
            _runtime.Login(name, [](LoginResult const& r)
            {
                if (!r.ok)
                    LOG_ERROR("module", "mod-acsdk: autostart login of {} failed: {}.", r.name, r.reason);
            });
        }
    }

    bool Module::CheckLease(ObjectGuid guid, std::string const& token) const
    {
        return _leases->Validate(guid.GetCounter(), token, LeaseManager::Clock::now());
    }

    void Module::OnLeaseEnded(ObjectGuid guid, bool stopMovement)
    {
        HeadlessCharacter* c = _runtime.Find(guid);
        if (!c)
            return;

        c->unleasedSince = std::chrono::steady_clock::now();
        // In-flight commands belong to the lease that ended (PLAN.md §4.4).
        _commands.CancelAll(guid, stopMovement ? "LEASE_LOST" : "LEASE_RELEASED");
        if (!stopMovement)
            return;

        // PLAN.md §4.4: the character stops and stays where it is. Nothing else.
        if (Player* player = _runtime.FindOnlinePlayer(guid))
        {
            player->GetMotionMaster()->Clear();
            player->StopMoving();
        }
    }

    void Module::UpdateLeases()
    {
        auto const now = LeaseManager::Clock::now();

        for (uint64_t counter : _leases->Expire(now))
        {
            LOG_INFO("module", "mod-acsdk: lease on character {} expired; it is now orphaned.", counter);
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(static_cast<ObjectGuid::LowType>(counter));
            PublishLeaseLost(guid, "EXPIRED", "");
            OnLeaseEnded(guid, true);
        }

        // Leases never outlive the headless character.
        for (LeaseManager::LeaseInfo const& lease : _leases->List(now))
            if (!_runtime.Find(ObjectGuid::Create<HighGuid::Player>(static_cast<ObjectGuid::LowType>(lease.characterGuid))))
                _leases->Forget(lease.characterGuid);

        if (_config.leaseLogoutOnOrphanSec == 0)
            return;

        auto const limit = std::chrono::seconds(_config.leaseLogoutOnOrphanSec);
        for (HeadlessCharacter const* c : _runtime.All())
        {
            if (c->state != Lifecycle::Online || c->logoutRequested || _leases->IsLeased(c->guid.GetCounter(), now))
                continue;
            if (std::chrono::steady_clock::now() - c->unleasedSince < limit)
                continue;

            LOG_INFO("module", "mod-acsdk: logging out {}: unleased for {}s (ACSDK.Lease.LogoutOnOrphanSec).", c->name, _config.leaseLogoutOnOrphanSec);
            _runtime.Logout(c->guid, [](bool, std::string const&) { });
        }
    }

    void Module::PublishLeaseLost(ObjectGuid guid, char const* reason, std::string const& newHolder)
    {
        if (!_events.HasSubscribers())
            return;
        auto e = std::make_unique<v1::Event>();
        e->mutable_lease_lost()->set_reason(reason);
        e->mutable_lease_lost()->set_new_holder(newHolder);
        _events.Publish(guid.GetRawValue(), std::move(e));
    }

    void Module::OnLeaseTakenOver(ObjectGuid guid, std::string const& newHolder)
    {
        PublishLeaseLost(guid, "TAKEN_OVER", newHolder);
        _commands.CancelAll(guid, "LEASE_LOST");
    }

    // CharacterStateChanged: one source, a per-tick diff of each character's lifecycle and sub-flags read from
    // the Player (docs/events.md). Catches paths that bypass hooks (KillPlayer, GM kicks, teleports).
    void Module::UpdateCharacterStates()
    {
        if (!_events.HasSubscribers())
        {
            _states.clear();
            return;
        }

        auto publish = [this](ObjectGuid guid, StateSnapshot const& s, char const* reason)
        {
            auto e = std::make_unique<v1::Event>();
            v1::CharacterStateChangedEvent* c = e->mutable_character_state_changed();
            c->set_state(static_cast<v1::LifecycleState>(static_cast<int>(s.lifecycle) + 1)); // OFFLINE = 1 ...
            c->set_dead(s.dead);
            c->set_ghost(s.ghost);
            c->set_in_combat(s.inCombat);
            c->set_teleporting(s.teleporting);
            c->set_map_id(s.mapId);
            c->set_reason(reason);
            _events.Publish(guid.GetRawValue(), std::move(e));
        };

        std::unordered_map<ObjectGuid, StateSnapshot> seen;
        for (HeadlessCharacter const* c : _runtime.All())
        {
            StateSnapshot s;
            s.lifecycle = c->state;
            if (Player* p = _runtime.FindOnlinePlayer(c->guid))
            {
                s.dead = !p->IsAlive();
                s.ghost = p->HasPlayerFlag(PLAYER_FLAGS_GHOST);
                s.inCombat = p->IsInCombat();
                s.teleporting = p->IsBeingTeleported();
                s.mapId = p->GetMapId();
            }
            seen[c->guid] = s;

            auto it = _states.find(c->guid);
            if (it == _states.end() || !(it->second == s))
                publish(c->guid, s, it == _states.end() ? "INITIAL" : "CHANGED");
        }

        for (auto const& [guid, old] : _states)
            if (!seen.contains(guid))
                publish(guid, StateSnapshot{}, "LOGGED_OUT");

        _states = std::move(seen);
    }

    bool Module::Post(CommandQueue::Task task)
    {
        if (!_queue.TryPush(std::move(task)))
            return false;

        _stats.commandsEnqueued.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void Module::Update(uint32_t diff)
    {
        if (!_server.IsRunning())
            return;

        ASSERT(IsWorldThread(), "mod-acsdk: Update called off the world thread");

        _info.worldTicks.fetch_add(1, std::memory_order_relaxed);
        _info.lastTickDiffMs.store(diff, std::memory_order_relaxed);

        auto const start = CommandQueue::Clock::now();

        std::size_t ran = _queue.Drain(_config.maxCommandsPerTick, std::chrono::milliseconds(_config.maxTickBudgetMs));
        _queries.ProcessReadyCallbacks();
        _runtime.Update();
        _commands.Update();
        _prompts.Update();
        UpdateLeases();
        UpdateCharacterStates();
        RunTickers();
        _events.Dispatch(); // last: everything published this tick reaches subscribers now

        auto const us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(CommandQueue::Clock::now() - start).count());
        _stats.commandsExecuted.fetch_add(ran, std::memory_order_relaxed);
        _stats.lastTickCommands.store(static_cast<uint32_t>(ran), std::memory_order_relaxed);
        _stats.lastTickDispatchUs.store(us, std::memory_order_relaxed);
        if (us > _stats.maxTickDispatchUs.load(std::memory_order_relaxed))
            _stats.maxTickDispatchUs.store(us, std::memory_order_relaxed);
    }

    void Module::RunTickers()
    {
        if (_tickers.empty())
            return;

        std::vector<std::function<bool()>> tickers = std::move(_tickers);
        _tickers.clear();
        for (auto& tick : tickers)
            if (tick())
                _tickers.push_back(std::move(tick));
    }

    void Module::Shutdown()
    {
        if (!_server.IsRunning())
            return;

        _queue.Close();
        // Log out every headless character first: pending Login calls fail with SERVER_SHUTDOWN and later
        // logins are refused, so the pump below cannot start new work.
        _runtime.ShutdownAll();

        // Keep finishing world-thread work (queued tasks, DB callbacks) until every RPC is done.
        _server.Stop([this]()
        {
            std::size_t ran = _queue.Drain(SIZE_MAX, std::chrono::milliseconds(50));
            _stats.commandsExecuted.fetch_add(ran, std::memory_order_relaxed);
            _queries.ProcessReadyCallbacks();
            RunTickers();
            _events.Dispatch();
        });
    }
}
