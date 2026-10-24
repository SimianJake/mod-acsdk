#include "acsdk/v1/action.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"

#include <functional>
#include <unordered_map>

#include <algorithm>
#include <optional>

namespace acsdk
{
    namespace
    {
        v1::CommandStatus ToProto(CommandStatus s)
        {
            switch (s)
            {
                case CommandStatus::Started:   return v1::STARTED;
                case CommandStatus::Completed: return v1::COMPLETED;
                case CommandStatus::Failed:    return v1::FAILED;
                case CommandStatus::Timeout:   return v1::TIMEOUT;
                case CommandStatus::Cancelled: return v1::CANCELLED;
            }
            return v1::COMMAND_STATUS_UNSPECIFIED;
        }

        std::string ItemName(uint32 itemId)
        {
            ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemId);
            return t ? t->Name1 : "";
        }

        // World thread (command callbacks run there), so static item data may be read.
        void Fill(v1::CommandResult* out, CommandResult const& r)
        {
            out->set_command_id(r.id);
            out->set_status(ToProto(r.status));
            out->set_reason(r.reason);
            out->set_spell_id(r.spellId);
            if (!r.interaction)
                return;

            Interaction const& in = *r.interaction;
            v1::Interaction* w = out->mutable_interaction();
            w->set_kind(in.kind);
            w->set_npc_guid(in.npc);
            w->set_menu_id(in.menuId);
            for (packets::GossipOption const& o : in.options)
            {
                v1::GossipOption* g = w->add_options();
                g->set_index(o.index);
                g->set_icon(o.icon);
                g->set_coded(o.coded);
                g->set_box_money(o.boxMoney);
                g->set_text(o.text);
            }
            for (packets::QuestOffer const& q : in.quests)
            {
                v1::QuestOffer* o = w->add_quests();
                o->set_quest_id(q.questId);
                o->set_icon(q.icon);
                o->set_level(q.level);
                o->set_title(q.title);
            }
            for (packets::VendorItem const& v : in.vendorItems)
            {
                v1::VendorItem* o = w->add_vendor_items();
                o->set_slot(v.slot);
                o->set_item_id(v.itemId);
                o->set_name(ItemName(v.itemId));
                o->set_available(v.available);
                o->set_price_copper(v.price);
                o->set_buy_count(v.buyCount);
                o->set_extended_cost(v.extendedCost);
            }
            w->set_loot_money(in.lootMoney);
            for (Interaction::Looted const& l : in.loot)
            {
                v1::LootedItem* o = w->add_loot();
                o->set_slot(l.slot);
                o->set_item_id(l.itemId);
                o->set_name(ItemName(l.itemId));
                o->set_count(l.count);
                o->set_taken(l.taken);
            }
        }

        std::chrono::milliseconds TimeoutOr(uint32 requestedMs, uint32 defaultMs)
        {
            uint32 ms = requestedMs ? requestedMs : defaultMs;
            return std::chrono::milliseconds(std::min<uint32>(ms, 10 * 60 * 1000)); // server-side cap: 10 min
        }

        // Common path for every command RPC: lease check and submission on the world thread; answer with the
        // terminal result (wait) or right away with STARTED + command id.
        void SubmitCommand(grpc::ServerUnaryReactor* reactor, v1::CharacterRef const& ref, bool wait, CommandSpec spec, v1::CommandResult* response)
        {
            ObjectGuid guid = PlayerGuidFromApi(ref.character_guid());
            std::string token = ref.lease_token();
            PostToWorld(reactor, [guid, token, wait, spec = std::move(spec), response](WorldCallPtr call)
            {
                if (!sAcsdk.CheckLease(guid, token))
                {
                    response->set_status(v1::FAILED);
                    response->set_reason("INVALID_LEASE_TOKEN");
                    call->Finish(grpc::Status::OK);
                    return;
                }

                auto answered = std::make_shared<bool>(false);
                uint64_t id = sAcsdk.Commands().Submit(guid, spec, [call, response, wait, answered](CommandResult const& r)
                {
                    if (!wait || *answered)
                        return;
                    *answered = true;
                    Fill(response, r);
                    call->Finish(grpc::Status::OK);
                });

                if (!wait && !*answered)
                {
                    *answered = true;
                    // The command may already have failed synchronously (e.g. CHARACTER_OFFLINE, NO_PATH).
                    std::optional<CommandResult> done = sAcsdk.Commands().Finished(id);
                    Fill(response, done ? *done : CommandResult{ id, CommandStatus::Started, "", 0 });
                    call->Finish(grpc::Status::OK);
                }
            });
        }

        template <typename Request>
        bool Begin(grpc::CallbackServerContext* ctx, grpc::ServerUnaryReactor* reactor, Request const* request)
        {
            if (!Authorize(ctx, reactor))
                return false;
            if (!request->has_character())
            {
                reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "character is required"));
                return false;
            }
            return true;
        }

        // Client opcodes the escape hatch never sends: session/character management belongs to the SDK's own
        // lifecycle (PLAN.md §4.6.4 denylist).
        bool DeniedRawOpcode(uint16 op)
        {
            switch (op)
            {
                case CMSG_PLAYER_LOGIN: case CMSG_CHAR_CREATE: case CMSG_CHAR_DELETE: case CMSG_CHAR_RENAME:
                case CMSG_CHAR_ENUM: case CMSG_CHAR_CUSTOMIZE: case CMSG_CHAR_FACTION_CHANGE: case CMSG_CHAR_RACE_CHANGE:
                case CMSG_LOGOUT_REQUEST: case CMSG_PLAYER_LOGOUT: case CMSG_LOGOUT_CANCEL:
                case CMSG_REALM_SPLIT: case CMSG_AUTH_SESSION:
                    return true;
                default:
                    return false;
            }
        }

        // Per-character rate limit for raw packets (world thread only).
        bool RawRateOk(ObjectGuid guid, uint32 perSecond)
        {
            struct Bucket { std::chrono::steady_clock::time_point windowStart; uint32 count = 0; };
            static std::unordered_map<ObjectGuid, Bucket> buckets;
            auto now = std::chrono::steady_clock::now();
            Bucket& b = buckets[guid];
            if (now - b.windowStart >= std::chrono::seconds(1))
            {
                b.windowStart = now;
                b.count = 0;
            }
            return ++b.count <= perSecond;
        }

        class ActionServiceImpl final : public v1::ActionService::CallbackService
        {
        public:
            grpc::ServerUnaryReactor* SendClientPacket(grpc::CallbackServerContext* ctx, v1::SendClientPacketRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;

                Config const& config = sAcsdk.GetConfig();
                if (!config.rawPacketsEnable)
                {
                    reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "raw packets are disabled (ACSDK.RawPackets.Enable)"));
                    return reactor;
                }

                uint32 op = request->opcode();
                bool allowed = op < NUM_OPCODE_HANDLERS && opcodeTable[static_cast<Opcodes>(op)] &&
                               opcodeTable[static_cast<Opcodes>(op)]->Status != STATUS_NEVER &&
                               opcodeTable[static_cast<Opcodes>(op)]->Status != STATUS_UNHANDLED && !DeniedRawOpcode(static_cast<uint16>(op)) &&
                               (config.rawPacketsAllow.empty() || std::find(config.rawPacketsAllow.begin(), config.rawPacketsAllow.end(), op) != config.rawPacketsAllow.end());
                if (!allowed)
                {
                    reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "opcode is not an allowed client opcode"));
                    return reactor;
                }

                ObjectGuid guid = PlayerGuidFromApi(request->character().character_guid());
                std::string token = request->character().lease_token();
                std::string payload = request->payload();
                uint32 perSecond = config.rawPacketsPerSecond;
                PostToWorld(reactor, [guid, token, op, payload, perSecond, response](WorldCallPtr call)
                {
                    if (!sAcsdk.CheckLease(guid, token))
                    {
                        response->set_status(v1::FAILED);
                        response->set_reason("INVALID_LEASE_TOKEN");
                    }
                    else if (!RawRateOk(guid, perSecond))
                    {
                        response->set_status(v1::FAILED);
                        response->set_reason("RATE_LIMITED");
                    }
                    else
                    {
                        auto packet = std::make_unique<WorldPacket>(static_cast<uint16>(op), payload.size());
                        if (!payload.empty())
                            packet->append(reinterpret_cast<uint8 const*>(payload.data()), payload.size());
                        bool queued = sAcsdk.Runtime().Inject(guid, std::move(packet));
                        response->set_status(queued ? v1::COMPLETED : v1::FAILED);
                        if (!queued)
                            response->set_reason("CHARACTER_OFFLINE");
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* MoveTo(grpc::CallbackServerContext* ctx, v1::MoveToRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::MoveTo;
                spec.x = request->target().x();
                spec.y = request->target().y();
                spec.z = request->target().z();
                spec.tolerance = request->tolerance() > 0 ? request->tolerance() : 1.0f;
                spec.timeout = TimeoutOr(request->timeout_ms(), 30000);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* Stop(grpc::CallbackServerContext* ctx, v1::StopRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::Stop;
                SubmitCommand(reactor, request->character(), true, spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* Follow(grpc::CallbackServerContext* ctx, v1::FollowRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::Follow;
                spec.targetGuid = request->target_guid();
                spec.distance = request->distance() > 0 ? request->distance() : 2.0f;
                spec.angle = request->angle();
                spec.timeout = std::chrono::hours(24); // never completes on its own
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* Face(grpc::CallbackServerContext* ctx, v1::FaceRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::Face;
                if (request->what_case() == v1::FaceRequest::kTargetGuid)
                    spec.targetGuid = request->target_guid();
                else
                    spec.orientation = request->orientation();
                spec.timeout = std::chrono::seconds(3);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* SetTarget(grpc::CallbackServerContext* ctx, v1::SetTargetRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::SetTarget;
                spec.targetGuid = request->target_guid();
                SubmitCommand(reactor, request->character(), true, spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* Attack(grpc::CallbackServerContext* ctx, v1::AttackRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::Attack;
                spec.targetGuid = request->target_guid();
                spec.untilStopped = request->until_stopped();
                spec.timeout = TimeoutOr(request->timeout_ms(), 120000);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* StopAttack(grpc::CallbackServerContext* ctx, v1::StopAttackRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::StopAttack;
                SubmitCommand(reactor, request->character(), true, spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* CastSpell(grpc::CallbackServerContext* ctx, v1::CastSpellRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::CastSpell;
                spec.spellId = request->spell_id();
                if (request->target_case() == v1::CastSpellRequest::kTargetGuid)
                    spec.targetGuid = request->target_guid();
                else if (request->target_case() == v1::CastSpellRequest::kDestination)
                {
                    spec.destX = request->destination().x();
                    spec.destY = request->destination().y();
                    spec.destZ = request->destination().z();
                }
                spec.timeout = TimeoutOr(request->timeout_ms(), 30000);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* CancelCast(grpc::CallbackServerContext* ctx, v1::CancelCastRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::CancelCast;
                spec.spellId = request->spell_id();
                SubmitCommand(reactor, request->character(), true, spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* Say(grpc::CallbackServerContext* ctx, v1::SayRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::Say;
                spec.text = request->text();
                spec.timeout = std::chrono::seconds(5);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* Whisper(grpc::CallbackServerContext* ctx, v1::WhisperRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = CommandType::Whisper;
                spec.text = request->text();
                spec.whisperTo = request->to();
                spec.timeout = std::chrono::seconds(5);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            // ---- interact channel ----
            template <typename Request>
            grpc::ServerUnaryReactor* Interact(grpc::CallbackServerContext* ctx, Request const* request, v1::CommandResult* response,
                                               CommandType type, std::function<void(CommandSpec&)> fill)
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                CommandSpec spec;
                spec.type = type;
                spec.timeout = TimeoutOr(request->timeout_ms(), 5000);
                fill(spec);
                SubmitCommand(reactor, request->character(), request->wait(), spec, response);
                return reactor;
            }

            grpc::ServerUnaryReactor* ReleaseSpirit(grpc::CallbackServerContext* ctx, v1::InteractRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::ReleaseSpirit, [](CommandSpec&) { });
            }

            grpc::ServerUnaryReactor* ReclaimCorpse(grpc::CallbackServerContext* ctx, v1::InteractRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::ReclaimCorpse, [](CommandSpec&) { });
            }

            grpc::ServerUnaryReactor* SpiritHealerResurrect(grpc::CallbackServerContext* ctx, v1::InteractRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::SpiritHealer, [r](CommandSpec& s) { s.npcGuid = r->npc_guid(); });
            }

            grpc::ServerUnaryReactor* Talk(grpc::CallbackServerContext* ctx, v1::InteractRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::Talk, [r](CommandSpec& s) { s.npcGuid = r->npc_guid(); });
            }

            grpc::ServerUnaryReactor* OpenVendor(grpc::CallbackServerContext* ctx, v1::InteractRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::OpenVendor, [r](CommandSpec& s) { s.npcGuid = r->npc_guid(); });
            }

            grpc::ServerUnaryReactor* SelectGossipOption(grpc::CallbackServerContext* ctx, v1::GossipOptionRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::GossipSelect, [r](CommandSpec& s)
                {
                    s.npcGuid = r->npc_guid();
                    s.menuId = r->menu_id();
                    s.optionIndex = r->option_index();
                    s.text = r->code();
                });
            }

            grpc::ServerUnaryReactor* AcceptQuest(grpc::CallbackServerContext* ctx, v1::QuestRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::AcceptQuest, [r](CommandSpec& s) { s.npcGuid = r->npc_guid(); s.questId = r->quest_id(); });
            }

            grpc::ServerUnaryReactor* CompleteQuest(grpc::CallbackServerContext* ctx, v1::QuestRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::CompleteQuest, [r](CommandSpec& s)
                {
                    s.npcGuid = r->npc_guid();
                    s.questId = r->quest_id();
                    s.rewardIndex = r->reward_index();
                });
            }

            grpc::ServerUnaryReactor* AbandonQuest(grpc::CallbackServerContext* ctx, v1::QuestRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::AbandonQuest, [r](CommandSpec& s) { s.questId = r->quest_id(); });
            }

            grpc::ServerUnaryReactor* BuyItem(grpc::CallbackServerContext* ctx, v1::BuyItemRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::BuyItem, [r](CommandSpec& s)
                {
                    s.npcGuid = r->npc_guid();
                    s.itemId = r->item_id();
                    s.count = r->count();
                });
            }

            grpc::ServerUnaryReactor* SellItem(grpc::CallbackServerContext* ctx, v1::SellItemRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::SellItem, [r](CommandSpec& s)
                {
                    s.npcGuid = r->npc_guid();
                    s.objectGuid = r->item_guid();
                    s.count = r->count();
                });
            }

            grpc::ServerUnaryReactor* EquipItem(grpc::CallbackServerContext* ctx, v1::EquipItemRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::EquipItem, [r](CommandSpec& s) { s.objectGuid = r->item_guid(); });
            }

            grpc::ServerUnaryReactor* Loot(grpc::CallbackServerContext* ctx, v1::LootRequest const* r, v1::CommandResult* response) override
            {
                return Interact(ctx, r, response, CommandType::Loot, [r](CommandSpec& s) { s.npcGuid = r->target_guid(); });
            }

            grpc::ServerUnaryReactor* Cancel(grpc::CallbackServerContext* ctx, v1::CancelRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Begin(ctx, reactor, request))
                    return reactor;
                ObjectGuid guid = PlayerGuidFromApi(request->character().character_guid());
                std::string token = request->character().lease_token();
                uint64_t commandId = request->command_id();
                PostToWorld(reactor, [guid, token, commandId, response](WorldCallPtr call)
                {
                    response->set_command_id(commandId);
                    if (!sAcsdk.CheckLease(guid, token))
                    {
                        response->set_status(v1::FAILED);
                        response->set_reason("INVALID_LEASE_TOKEN");
                    }
                    else if (sAcsdk.Commands().Cancel(guid, commandId, "CANCELLED"))
                        response->set_status(v1::COMPLETED);
                    else
                    {
                        response->set_status(v1::FAILED);
                        response->set_reason("NOT_RUNNING");
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* WaitCommand(grpc::CallbackServerContext* ctx, v1::WaitCommandRequest const* request, v1::CommandResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;
                uint64_t commandId = request->command_id();
                auto waitFor = TimeoutOr(request->timeout_ms(), 30000);
                PostToWorld(reactor, [commandId, waitFor, response](WorldCallPtr call)
                {
                    auto answered = std::make_shared<bool>(false);
                    bool known = sAcsdk.Commands().Wait(commandId, [call, response, answered](CommandResult const& r)
                    {
                        if (*answered)
                            return;
                        *answered = true;
                        Fill(response, r);
                        call->Finish(grpc::Status::OK);
                    });

                    if (!known)
                    {
                        *answered = true;
                        response->set_command_id(commandId);
                        response->set_status(v1::FAILED);
                        response->set_reason("UNKNOWN_COMMAND");
                        call->Finish(grpc::Status::OK);
                        return;
                    }

                    if (*answered)
                        return;

                    // Bound how long this call waits; the command itself keeps running.
                    auto deadline = std::chrono::steady_clock::now() + waitFor;
                    sAcsdk.AddTicker([call, response, answered, commandId, deadline]()
                    {
                        if (*answered)
                            return false;
                        if (std::chrono::steady_clock::now() < deadline)
                            return true;
                        *answered = true;
                        response->set_command_id(commandId);
                        response->set_status(v1::STARTED); // still running
                        call->Finish(grpc::Status::OK);
                        return false;
                    });
                });
                return reactor;
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeActionService()
    {
        return std::make_unique<ActionServiceImpl>();
    }
}
