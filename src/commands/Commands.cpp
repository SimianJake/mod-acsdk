#include "Commands.h"

#include "EnumNames.generated.h"
#include "Packets.h"

#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"

#include <cmath>

namespace acsdk
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::size_t RecentResults = 1024;
        constexpr auto StuckAfter = std::chrono::seconds(5);

        // Server packets the watchers need. Captured for every headless character; everything else is
        // dropped after an opcode check (PLAN.md §4.5).
        constexpr uint16_t WatchedOpcodes[] = {
            SMSG_CAST_FAILED, SMSG_SPELL_START, SMSG_SPELL_GO, SMSG_SPELL_FAILURE, SMSG_SPELL_FAILED_OTHER,
            MSG_CHANNEL_START, MSG_CHANNEL_UPDATE,
            SMSG_ATTACKSTART, SMSG_ATTACKSTOP, SMSG_ATTACKSWING_NOTINRANGE, SMSG_ATTACKSWING_BADFACING,
            SMSG_ATTACKSWING_DEADTARGET, SMSG_ATTACKSWING_CANT_ATTACK, SMSG_CANCEL_COMBAT,
            SMSG_MESSAGECHAT, SMSG_GM_MESSAGECHAT, SMSG_CHAT_PLAYER_NOT_FOUND, SMSG_CHAT_WRONG_FACTION, SMSG_NOTIFICATION,
            SMSG_MOVE_KNOCK_BACK,
            // Prompts (always tracked so unanswered ones can time out): see events/Prompts.cpp
            SMSG_GROUP_INVITE, SMSG_TRADE_STATUS, SMSG_DUEL_REQUESTED, SMSG_DUEL_COMPLETE, SMSG_RESURRECT_REQUEST,
            SMSG_SUMMON_REQUEST, SMSG_LOOT_START_ROLL, SMSG_LOOT_ROLL_WON, SMSG_LOOT_ALL_PASSED, MSG_RAID_READY_CHECK,
            MSG_RAID_READY_CHECK_FINISHED,
            // Interact channel
            SMSG_GOSSIP_MESSAGE, SMSG_GOSSIP_COMPLETE, SMSG_QUESTGIVER_QUEST_LIST, SMSG_QUESTGIVER_QUEST_DETAILS,
            SMSG_QUESTGIVER_REQUEST_ITEMS, SMSG_QUESTGIVER_OFFER_REWARD, SMSG_QUESTGIVER_QUEST_INVALID,
            SMSG_QUESTGIVER_QUEST_FAILED, SMSG_LIST_INVENTORY, SMSG_BUY_ITEM, SMSG_BUY_FAILED, SMSG_SELL_ITEM,
            SMSG_INVENTORY_CHANGE_FAILURE, SMSG_LOOT_RESPONSE, SMSG_LOOT_REMOVED, SMSG_LOOT_RELEASE_RESPONSE,
            SMSG_TRAINER_LIST, SMSG_SHOWTAXINODES, SMSG_SHOW_BANK, SMSG_BINDER_CONFIRM,
        };


        bool IsInteraction(CommandType type)
        {
            return type >= CommandType::ReleaseSpirit;
        }


        std::string CastResultName(uint8_t result)
        {
            char const* name = SpellCastResultName(result);
            return name ? name : "SPELL_FAILED_" + std::to_string(result);
        }

        void Inject(HeadlessRuntime& runtime, ObjectGuid character, uint16 opcode, ByteBuffer const& body)
        {
            auto packet = std::make_unique<WorldPacket>(opcode, body.size());
            if (body.size())
                packet->append(body.contents(), body.size());
            runtime.Inject(character, std::move(packet));
        }

        uint32 ChatLanguage(Player* player)
        {
            // LANG_UNIVERSAL is refused for players (ChatHandler.cpp); the client uses its faction language.
            return player->GetTeamId() == TEAM_ALLIANCE ? LANG_COMMON : LANG_ORCISH;
        }

        float AngleDiff(float a, float b)
        {
            float d = std::fabs(Position::NormalizeOrientation(a) - Position::NormalizeOrientation(b));
            return std::min(d, float(2 * M_PI) - d);
        }

        bool IsInstant(CommandType type)
        {
            return type == CommandType::Stop || type == CommandType::StopAttack || type == CommandType::SetTarget ||
                   type == CommandType::CancelCast;
        }
    }

    char const* CommandStatusName(CommandStatus status)
    {
        switch (status)
        {
            case CommandStatus::Started:   return "STARTED";
            case CommandStatus::Completed: return "COMPLETED";
            case CommandStatus::Failed:    return "FAILED";
            case CommandStatus::Timeout:   return "TIMEOUT";
            case CommandStatus::Cancelled: return "CANCELLED";
        }
        return "UNKNOWN";
    }

    Channel CommandManager::ChannelOf(CommandType type)
    {
        switch (type)
        {
            case CommandType::MoveTo:
            case CommandType::Stop:
            case CommandType::Follow:
            case CommandType::Face:
                return Channel::Move;
            case CommandType::Attack:
            case CommandType::StopAttack:
                return Channel::Melee;
            case CommandType::CastSpell:
            case CommandType::CancelCast:
                return Channel::Cast;
            case CommandType::Say:
            case CommandType::Whisper:
                return Channel::Chat;
            case CommandType::SetTarget:
                break;
            default:
                if (IsInteraction(type))
                    return Channel::Interact;
                break;
        }
        return Channel::Move; // SetTarget is instant and never occupies a channel
    }

    std::size_t CommandManager::InFlight() const
    {
        return _active.size();
    }

    uint64_t CommandManager::Submit(ObjectGuid character, CommandSpec const& spec, CommandDone onDone)
    {
        uint64_t id = _nextId++;
        auto fail = [&](std::string reason)
        {
            CommandResult r{ id, CommandStatus::Failed, std::move(reason), 0 };
            Remember(r);
            onDone(r);
            return id;
        };

        Player* player = _runtime.FindOnlinePlayer(character);
        if (!player)
            return fail("CHARACTER_OFFLINE");

        HeadlessCharacter* hc = _runtime.Find(character);
        if (hc && hc->sink && !hc->sinkWired)
        {
            for (uint16_t op : WatchedOpcodes)
                hc->sink->Want(op);
            hc->sinkWired = true;
        }

        CharacterCommands& cc = _characters[character];
        Channel channel = ChannelOf(spec.type);

        if (spec.type == CommandType::CancelCast)
        {
            // Targets the running cast rather than superseding it: AC's own interrupt replies resolve it.
            auto it = cc.byChannel.find(uint8_t(Channel::Cast));
            if (it != cc.byChannel.end())
                it->second->cancelRequested = true;
        }
        else if (spec.type != CommandType::SetTarget)
        {
            auto it = cc.byChannel.find(uint8_t(channel));
            if (it != cc.byChannel.end())
            {
                std::shared_ptr<Command> old = it->second;
                Finish(old, CommandStatus::Cancelled, "SUPERSEDED");
            }
        }

        std::shared_ptr<Command> cmd = Begin(player, spec, id, character, std::move(onDone));
        if (!cmd)
            return id; // Begin already reported a failure through Finish()

        if (!IsInstant(spec.type))
            cc.byChannel[uint8_t(channel)] = cmd;
        return id;
    }

    std::shared_ptr<CommandManager::Command> CommandManager::Begin(Player* player, CommandSpec const& spec, uint64_t id, ObjectGuid character, CommandDone onDone)
    {
        auto cmd = std::make_shared<Command>();
        cmd->waiters.push_back(std::move(onDone));
        cmd->id = id;
        cmd->character = character;
        cmd->spec = spec;
        cmd->channel = ChannelOf(spec.type);
        cmd->deadline = Clock::now() + spec.timeout;
        cmd->lastX = player->GetPositionX();
        cmd->lastY = player->GetPositionY();
        cmd->lastProgress = Clock::now();
        cmd->aliveAtStart = player->IsAlive();
        _active[id] = cmd;

        auto failNow = [&](std::string reason)
        {
            Finish(cmd, CommandStatus::Failed, std::move(reason));
            return nullptr;
        };

        switch (spec.type)
        {
            case CommandType::MoveTo:
            {
                // Whitelisted (PLAN.md §4.6.3): read-only PathGenerator query, then AC's server-side movement.
                PathGenerator path(player);
                path.CalculatePath(spec.x, spec.y, spec.z, false);
                if (path.GetPathType() & PATHFIND_NOPATH)
                    return failNow("NO_PATH");
                player->GetMotionMaster()->MovePoint(0, spec.x, spec.y, spec.z, FORCED_MOVEMENT_NONE, 0.f, 0.f, /*generatePath*/ true);
                break;
            }
            case CommandType::Stop:
                player->GetMotionMaster()->Clear();
                player->StopMoving();
                break;
            case CommandType::Follow:
            {
                Unit* target = ObjectAccessor::GetUnit(*player, ObjectGuid(spec.targetGuid));
                if (!target)
                    return failNow("TARGET_GONE");
                player->GetMotionMaster()->MoveFollow(target, spec.distance, spec.angle);
                break;
            }
            case CommandType::Face:
            {
                // Whitelisted facing API: stop first so the facing spline is not fighting a move.
                player->GetMotionMaster()->Clear();
                player->StopMoving();
                if (spec.targetGuid)
                {
                    WorldObject* target = ObjectAccessor::GetWorldObject(*player, ObjectGuid(spec.targetGuid));
                    if (!target)
                        return failNow("TARGET_GONE");
                    cmd->spec.orientation = player->GetAngle(target);
                }
                player->SetFacingTo(*cmd->spec.orientation);
                break;
            }
            case CommandType::SetTarget:
                Inject(_runtime, character, CMSG_SET_SELECTION, packets::SetSelection(spec.targetGuid));
                break;
            case CommandType::Attack:
                Inject(_runtime, character, CMSG_SET_SELECTION, packets::SetSelection(spec.targetGuid));
                Inject(_runtime, character, CMSG_ATTACKSWING, packets::AttackSwing(spec.targetGuid));
                break;
            case CommandType::StopAttack:
                Inject(_runtime, character, CMSG_ATTACKSTOP, packets::AttackStop());
                break;
            case CommandType::CastSpell:
            {
                CharacterCommands& cc = _characters[character];
                cmd->castCount = cc.nextCastCount;
                cc.nextCastCount = cc.nextCastCount == 255 ? 1 : cc.nextCastCount + 1; // 0 is AC's own triggered casts
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spec.spellId))
                    cmd->channeled = info->IsChanneled();
                cmd->actualSpellId = spec.spellId;

                packets::CastTarget target;
                target.unitGuid = spec.targetGuid;
                target.destX = spec.destX;
                target.destY = spec.destY;
                target.destZ = spec.destZ;
                Inject(_runtime, character, CMSG_CAST_SPELL, packets::CastSpell(cmd->castCount, spec.spellId, target));
                break;
            }
            case CommandType::CancelCast:
                Inject(_runtime, character, CMSG_CANCEL_CAST, packets::CancelCast(spec.spellId));
                break;
            case CommandType::Say:
                Inject(_runtime, character, CMSG_MESSAGECHAT, packets::MessageChat(CHAT_MSG_SAY, ChatLanguage(player), spec.text));
                break;
            case CommandType::Whisper:
                Inject(_runtime, character, CMSG_MESSAGECHAT, packets::MessageChat(CHAT_MSG_WHISPER, ChatLanguage(player), spec.text, spec.whisperTo));
                break;
            default:
            {
                std::string reason;
                if (!BeginInteraction(player, cmd, reason))
                    return failNow(reason);
                break;
            }
        }

        return cmd;
    }

    void CommandManager::Finish(std::shared_ptr<Command> const& cmd, CommandStatus status, std::string reason)
    {
        if (!_active.erase(cmd->id))
            return; // already finished

        auto cit = _characters.find(cmd->character);
        if (cit != _characters.end())
        {
            auto ch = cit->second.byChannel.find(uint8_t(cmd->channel));
            if (ch != cit->second.byChannel.end() && ch->second == cmd)
                cit->second.byChannel.erase(ch);
        }

        CommandResult result{ cmd->id, status, std::move(reason), cmd->spec.type == CommandType::CastSpell ? cmd->actualSpellId : 0, cmd->interaction };
        Remember(result);
        if (_resultObserver)
            _resultObserver(cmd->character, result);

        std::vector<CommandDone> waiters = std::move(cmd->waiters);
        cmd->waiters.clear();
        for (CommandDone& done : waiters)
            done(result);
    }

    void CommandManager::Remember(CommandResult const& result)
    {
        _recent.push_back(result);
        _recentById[result.id] = result;
        while (_recent.size() > RecentResults)
        {
            _recentById.erase(_recent.front().id);
            _recent.pop_front();
        }
    }

    bool CommandManager::Wait(uint64_t commandId, CommandDone done)
    {
        if (auto it = _active.find(commandId); it != _active.end())
        {
            it->second->waiters.push_back(std::move(done));
            return true;
        }
        if (auto it = _recentById.find(commandId); it != _recentById.end())
        {
            done(it->second);
            return true;
        }
        return false;
    }

    uint64_t CommandManager::CommandForCast(ObjectGuid character, uint8_t castCount) const
    {
        auto cit = _characters.find(character);
        if (cit == _characters.end())
            return 0;
        auto it = cit->second.byChannel.find(uint8_t(Channel::Cast));
        if (it == cit->second.byChannel.end() || it->second->castCount != castCount)
            return 0;
        return it->second->id;
    }

    std::optional<CommandResult> CommandManager::Finished(uint64_t commandId) const
    {
        if (_active.contains(commandId))
            return std::nullopt;
        auto it = _recentById.find(commandId);
        return it == _recentById.end() ? std::nullopt : std::optional<CommandResult>(it->second);
    }

    bool CommandManager::Cancel(ObjectGuid character, uint64_t commandId, std::string const& reason)
    {
        auto it = _active.find(commandId);
        if (it == _active.end() || it->second->character != character)
            return false;
        std::shared_ptr<Command> cmd = it->second;
        Finish(cmd, CommandStatus::Cancelled, reason);
        return true;
    }

    void CommandManager::CancelAll(ObjectGuid character, std::string const& reason)
    {
        std::vector<std::shared_ptr<Command>> mine;
        for (auto const& [id, cmd] : _active)
            if (cmd->character == character)
                mine.push_back(cmd);
        for (auto const& cmd : mine)
            Finish(cmd, CommandStatus::Cancelled, reason);
    }

    void CommandManager::Update()
    {
        // Drain every headless character's captured packets each tick, so a new command never sees a
        // stale reply meant for an older one.
        for (HeadlessCharacter const* chc : _runtime.All())
        {
            HeadlessCharacter* hc = _runtime.Find(chc->guid);
            if (!hc || !hc->sink)
                continue;

            if (!hc->sinkWired)
            {
                for (uint16_t op : WatchedOpcodes)
                    hc->sink->Want(op);
                hc->sinkWired = true;
            }

            std::vector<CapturedPacket> packets = hc->sink->Take();
            if (packets.empty())
                continue;

            Player* player = _runtime.FindOnlinePlayer(hc->guid);
            if (!player)
                continue;

            for (CapturedPacket const& p : packets)
            {
                if (p.opcode == SMSG_MOVE_KNOCK_BACK)
                    KnockbackReflex(player, p);
                if (_packetObserver)
                    _packetObserver(hc->guid, p);
            }

            auto cit = _characters.find(hc->guid);
            if (cit == _characters.end())
                continue;

            for (CapturedPacket const& p : packets)
                HandlePacket(player, cit->second, p);
        }

        // Poll state and timeouts.
        std::vector<std::shared_ptr<Command>> active;
        active.reserve(_active.size());
        for (auto const& [id, cmd] : _active)
            active.push_back(cmd);

        for (auto const& cmd : active)
        {
            if (!_active.contains(cmd->id))
                continue;

            Player* player = _runtime.FindOnlinePlayer(cmd->character);
            if (!player)
            {
                Finish(cmd, CommandStatus::Failed, "CHARACTER_OFFLINE");
                continue;
            }

            Poll(player, *cmd);

            if (_active.contains(cmd->id) && Clock::now() >= cmd->deadline)
            {
                // Many AC handlers return silently (docs/research/opcode-map.md §0.5); report that as such, with
                // the likely cause read from the game state when there is an obvious one.
                std::string why = cmd->started ? "TIMEOUT" : DiagnoseNoResponse(player, *cmd);
                Finish(cmd, CommandStatus::Timeout, why.empty() ? "NO_RESPONSE" : why);
            }
        }

        // Forget characters with nothing in flight.
        for (auto it = _characters.begin(); it != _characters.end();)
        {
            if (it->second.byChannel.empty() && !_runtime.Find(it->first))
                it = _characters.erase(it);
            else
                ++it;
        }
    }

    // Protocol reflex (owner decision 2026-10-22, PLAN.md §13): a client would simulate the knockback arc
    // it was told about; headless characters have none, so let AzerothCore move them server-side with its
    // own knockback (MotionMaster::MoveKnockbackFrom, allowClientControlled).
    void CommandManager::KnockbackReflex(Player* player, CapturedPacket const& packet)
    {
        if (!_knockbackReflex || packet.bytes.empty())
            return;

        ByteBuffer buf;
        buf.append(packet.bytes.data(), packet.bytes.size());
        try
        {
            uint64 guid = 0;
            buf.readPackGUID(guid);
            buf.read_skip<uint32>(); // movement counter
            float vcos = buf.read<float>();
            float vsin = buf.read<float>();
            float speedXY = buf.read<float>();
            float speedZ = buf.read<float>();
            if (guid != player->GetGUID().GetRawValue())
                return;

            // KnockbackFrom sends the direction away from the source; put the source one yard behind.
            player->GetMotionMaster()->MoveKnockbackFrom(player->GetPositionX() - vcos, player->GetPositionY() - vsin, speedXY, speedZ, true);
        }
        catch (ByteBufferException const&)
        {
        }
    }

    void CommandManager::HandlePacket(Player* player, CharacterCommands& cc, CapturedPacket const& packet)
    {
        uint64_t const self = player->GetGUID().GetRawValue();
        ByteBuffer buf;
        if (!packet.bytes.empty())
            buf.append(packet.bytes.data(), packet.bytes.size());

        auto current = [&](Channel channel) -> std::shared_ptr<Command>
        {
            auto it = cc.byChannel.find(uint8_t(channel));
            return it == cc.byChannel.end() ? nullptr : it->second;
        };

        if (auto cmd = current(Channel::Interact))
        {
            HandleInteractionPacket(player, cmd, packet, buf);
            buf.rpos(0);
        }

        switch (packet.opcode)
        {
            // ---- cast channel: correlated by the echoed cast count (+ caster == self) ----
            case SMSG_CAST_FAILED:
                if (auto p = packets::DecodeCastFailed(buf))
                    if (auto cmd = current(Channel::Cast); cmd && cmd->castCount == p->castCount)
                    {
                        cmd->actualSpellId = p->spellId;
                        Finish(cmd, CommandStatus::Failed, CastResultName(p->result));
                    }
                break;
            case SMSG_SPELL_START:
                if (auto p = packets::DecodeSpellStartOrGo(buf); p && p->caster == self)
                    if (auto cmd = current(Channel::Cast); cmd && cmd->castCount == p->castCount)
                    {
                        cmd->started = true;
                        cmd->actualSpellId = p->spellId;
                    }
                break;
            case SMSG_SPELL_GO:
                if (auto p = packets::DecodeSpellStartOrGo(buf); p && p->caster == self)
                    if (auto cmd = current(Channel::Cast); cmd && cmd->castCount == p->castCount)
                    {
                        cmd->actualSpellId = p->spellId;
                        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(p->spellId))
                            cmd->channeled = info->IsChanneled();
                        cmd->started = true;
                        if (!cmd->channeled)
                            Finish(cmd, CommandStatus::Completed, "");
                    }
                break;
            case SMSG_SPELL_FAILURE:
            case SMSG_SPELL_FAILED_OTHER:
                if (auto p = packets::DecodeSpellFailure(buf); p && p->caster == self)
                    if (auto cmd = current(Channel::Cast); cmd && cmd->castCount == p->castCount)
                        Finish(cmd, cmd->cancelRequested ? CommandStatus::Cancelled : CommandStatus::Failed, CastResultName(p->result));
                break;
            case MSG_CHANNEL_UPDATE:
                // Has no cast count; only one channel can be active per caster (CURRENT_CHANNELED_SPELL).
                if (auto p = packets::DecodeChannelUpdate(buf); p && p->caster == self && p->remainingMs == 0)
                    if (auto cmd = current(Channel::Cast); cmd && cmd->channeled && cmd->started)
                        Finish(cmd, cmd->cancelRequested ? CommandStatus::Cancelled : CommandStatus::Completed, cmd->cancelRequested ? "CANCELLED" : "");
                break;

            // ---- melee channel: ATTACKSTART/STOP by keys, payload-less errors by single-in-flight ----
            case SMSG_ATTACKSTART:
                if (auto p = packets::DecodeAttackStart(buf); p && p->attacker == self)
                    if (auto cmd = current(Channel::Melee); cmd && cmd->spec.type == CommandType::Attack && p->victim == cmd->spec.targetGuid)
                        cmd->started = true;
                break;
            case SMSG_ATTACKSTOP:
                if (auto p = packets::DecodeAttackStop(buf); p && p->attacker == self)
                    if (auto cmd = current(Channel::Melee); cmd && cmd->spec.type == CommandType::Attack)
                    {
                        if (p->nowDead && p->victim == cmd->spec.targetGuid && !cmd->spec.untilStopped)
                            Finish(cmd, CommandStatus::Completed, "");
                        else if (!cmd->started)
                            Finish(cmd, CommandStatus::Failed, "ATTACK_REFUSED");
                    }
                break;
            case SMSG_ATTACKSWING_NOTINRANGE:
            case SMSG_ATTACKSWING_BADFACING:
            case SMSG_ATTACKSWING_DEADTARGET:
            case SMSG_ATTACKSWING_CANT_ATTACK:
                // While a swing is merely out of range / facing away, AC keeps the attack and retries every
                // swing; that is only a failure when the attack never got going.
                if (auto cmd = current(Channel::Melee); cmd && cmd->spec.type == CommandType::Attack && !cmd->started)
                {
                    char const* reason = packet.opcode == SMSG_ATTACKSWING_NOTINRANGE ? "NOT_IN_RANGE"
                                       : packet.opcode == SMSG_ATTACKSWING_BADFACING ? "BAD_FACING"
                                       : packet.opcode == SMSG_ATTACKSWING_DEADTARGET ? "TARGET_DEAD"
                                       : "CANT_ATTACK";
                    Finish(cmd, CommandStatus::Failed, reason);
                }
                break;
            case SMSG_CANCEL_COMBAT:
                if (auto cmd = current(Channel::Melee); cmd && cmd->spec.type == CommandType::Attack)
                    Finish(cmd, CommandStatus::Failed, "COMBAT_CANCELLED");
                break;

            // ---- chat channel: echo by keys, errors by single-in-flight ----
            case SMSG_MESSAGECHAT:
            case SMSG_GM_MESSAGECHAT:
                if (auto p = packets::DecodeChat(buf))
                    if (auto cmd = current(Channel::Chat))
                    {
                        bool say = cmd->spec.type == CommandType::Say && p->type == CHAT_MSG_SAY && p->sender == self;
                        bool whisper = cmd->spec.type == CommandType::Whisper && p->type == CHAT_MSG_WHISPER_INFORM;
                        if ((say || whisper) && p->text == cmd->spec.text)
                            Finish(cmd, CommandStatus::Completed, "");
                    }
                break;
            case SMSG_CHAT_PLAYER_NOT_FOUND:
                if (auto cmd = current(Channel::Chat); cmd && cmd->spec.type == CommandType::Whisper)
                    Finish(cmd, CommandStatus::Failed, "PLAYER_NOT_FOUND");
                break;
            case SMSG_CHAT_WRONG_FACTION:
                if (auto cmd = current(Channel::Chat))
                    Finish(cmd, CommandStatus::Failed, "WRONG_FACTION");
                break;
            case SMSG_NOTIFICATION:
                if (auto cmd = current(Channel::Chat))
                    Finish(cmd, CommandStatus::Failed, "CHAT_REJECTED");
                break;
            default:
                break;
        }
    }

    void CommandManager::Poll(Player* player, Command& cmd)
    {
        std::shared_ptr<Command> self = _active[cmd.id];

        if (IsInstant(cmd.spec.type))
        {
            // Injected packets were handled earlier this tick (or the direct call already ran).
            Finish(self, CommandStatus::Completed, "");
            return;
        }

        switch (cmd.spec.type)
        {
            case CommandType::MoveTo:
            {
                if (cmd.aliveAtStart && !player->IsAlive()) // ghosts walk too
                    return Finish(self, CommandStatus::Failed, "DIED");
                if (player->IsBeingTeleported())
                    return Finish(self, CommandStatus::Failed, "TELEPORTED");

                float dist = player->GetExactDist2d(cmd.spec.x, cmd.spec.y);
                bool splineDone = player->movespline->Finalized();
                bool generatorActive = player->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE;

                if (splineDone && dist <= std::max(cmd.spec.tolerance, 0.5f))
                    return Finish(self, CommandStatus::Completed, "");
                if (splineDone && !generatorActive)
                    return Finish(self, CommandStatus::Failed, "STOPPED_SHORT");

                if (std::fabs(player->GetPositionX() - cmd.lastX) > 0.1f || std::fabs(player->GetPositionY() - cmd.lastY) > 0.1f)
                {
                    cmd.lastX = player->GetPositionX();
                    cmd.lastY = player->GetPositionY();
                    cmd.lastProgress = Clock::now();
                }
                else if (Clock::now() - cmd.lastProgress > StuckAfter)
                    return Finish(self, CommandStatus::Failed, "STUCK");
                break;
            }
            case CommandType::Follow:
            {
                Unit* target = ObjectAccessor::GetUnit(*player, ObjectGuid(cmd.spec.targetGuid));
                if (!target || !target->IsInWorld() || target->GetMapId() != player->GetMapId())
                    return Finish(self, CommandStatus::Failed, "TARGET_GONE");
                if (player->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                    return Finish(self, CommandStatus::Failed, "INTERRUPTED");
                break;
            }
            case CommandType::Face:
                if (player->movespline->Finalized() && cmd.spec.orientation && AngleDiff(player->GetOrientation(), *cmd.spec.orientation) < 0.1f)
                    return Finish(self, CommandStatus::Completed, "");
                break;
            case CommandType::Attack:
            {
                if (!player->IsAlive())
                    return Finish(self, CommandStatus::Failed, "DIED");
                Unit* victim = ObjectAccessor::GetUnit(*player, ObjectGuid(cmd.spec.targetGuid));
                if (!victim)
                    return Finish(self, CommandStatus::Failed, "TARGET_GONE");
                if (!victim->IsAlive() && !cmd.spec.untilStopped)
                    return Finish(self, CommandStatus::Completed, "");
                break;
            }
            case CommandType::CastSpell:
                if (cmd.aliveAtStart && !player->IsAlive())
                    return Finish(self, CommandStatus::Failed, "DIED");
                if (cmd.cancelRequested)
                {
                    // A queued or self-cancelled channel may end without any reply (opcode-map.md §2.3/§2.4).
                    if (cmd.injectedOnce)
                        return Finish(self, CommandStatus::Cancelled, "CANCELLED");
                    cmd.injectedOnce = true; // give AC's own replies one tick to arrive
                }
                break;
            default:
                if (IsInteraction(cmd.spec.type))
                    PollInteraction(player, self);
                break;
        }
    }
}
