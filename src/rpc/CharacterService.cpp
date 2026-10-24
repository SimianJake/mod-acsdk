#include "acsdk/v1/character.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "Bag.h"
#include "Corpse.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "Group.h"
#include "MoveSpline.h"
#include "Player.h"

#include <algorithm>
#include <vector>
#include "QueryResult.h"
#include "StringFormat.h"


#include <unordered_map>

namespace acsdk
{
    namespace
    {
        v1::LifecycleState ToProto(Lifecycle state)
        {
            switch (state)
            {
                case Lifecycle::Offline:    return v1::OFFLINE;
                case Lifecycle::LoggingIn:  return v1::LOGGING_IN;
                case Lifecycle::Online:     return v1::ONLINE;
                case Lifecycle::LoggingOut: return v1::LOGGING_OUT;
            }
            return v1::LIFECYCLE_STATE_UNSPECIFIED;
        }

        // LIKE pattern for "username starts with prefix", escaping LIKE wildcards with '!'.
        std::string PrefixLikePattern(std::string prefix)
        {
            std::string escaped;
            for (char c : prefix)
            {
                if (c == '%' || c == '_' || c == '!')
                    escaped += '!';
                escaped += c;
            }
            escaped += '%';
            LoginDatabase.EscapeString(escaped);
            return escaped;
        }

        // List runs two async queries chained on the world thread's QueryCallbackProcessor, so neither
        // the world thread nor a gRPC thread ever blocks on the database.
        void ListOnWorldThread(WorldCallPtr call, v1::ListCharactersResponse* response)
        {
            Config const& config = sAcsdk.GetConfig();
            if (config.headlessAccountPrefix.empty())
            {
                call->Finish(grpc::Status::OK); // no eligible accounts configured
                return;
            }

            std::string accountSql = Acore::StringFormat(
                "SELECT id, username FROM account WHERE username LIKE '{}' ESCAPE '!' ORDER BY id",
                PrefixLikePattern(config.headlessAccountPrefix));

            uint32 const maxResults = config.maxListResults;

            sAcsdk.Queries().AddCallback(LoginDatabase.AsyncQuery(accountSql).WithCallback(
                [call, response, maxResults](QueryResult accounts)
                {
                    if (!accounts)
                    {
                        call->Finish(grpc::Status::OK);
                        return;
                    }

                    auto accountNames = std::make_shared<std::unordered_map<uint32, std::string>>();
                    std::string ids;
                    do
                    {
                        Field* fields = accounts->Fetch();
                        uint32 id = fields[0].Get<uint32>();
                        (*accountNames)[id] = fields[1].Get<std::string>();
                        if (!ids.empty())
                            ids += ',';
                        ids += std::to_string(id);
                    } while (accounts->NextRow());

                    std::string characterSql = Acore::StringFormat(
                        "SELECT guid, name, account, race, class, level, online FROM characters "
                        "WHERE account IN ({}) AND deleteInfos_Account IS NULL ORDER BY account, name LIMIT {}",
                        ids, maxResults + 1);

                    sAcsdk.Queries().AddCallback(CharacterDatabase.AsyncQuery(characterSql).WithCallback(
                        [call, response, maxResults, accountNames](QueryResult characters)
                        {
                            if (characters)
                            {
                                do
                                {
                                    if (static_cast<uint32>(response->characters_size()) >= maxResults)
                                    {
                                        response->set_truncated(true);
                                        break;
                                    }

                                    Field* fields = characters->Fetch();
                                    v1::CharacterSummary* c = response->add_characters();
                                    c->set_character_guid(fields[0].Get<uint32>());
                                    c->set_name(fields[1].Get<std::string>());
                                    uint32 accountId = fields[2].Get<uint32>();
                                    c->set_account_id(accountId);
                                    auto it = accountNames->find(accountId);
                                    if (it != accountNames->end())
                                        c->set_account_name(it->second);
                                    c->set_race(fields[3].Get<uint8>());
                                    c->set_class_id(fields[4].Get<uint8>());
                                    c->set_level(fields[5].Get<uint8>());
                                    c->set_online(fields[6].Get<uint8>() != 0);
                                    HeadlessCharacter const* h = sAcsdk.Runtime().Find(ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>()));
                                    c->set_state(h ? ToProto(h->state) : v1::OFFLINE);
                                } while (characters->NextRow());
                            }

                            call->Finish(grpc::Status::OK);
                        }));
                }));
        }

        void FillLogin(v1::LoginResponse* response, LoginResult const& r)
        {
            response->set_ok(r.ok);
            response->set_reason(r.reason);
            response->set_character_guid(r.guid.GetCounter());
            response->set_name(r.name);
            HeadlessCharacter const* h = r.guid ? sAcsdk.Runtime().Find(r.guid) : nullptr;
            response->set_state(h ? ToProto(h->state) : v1::OFFLINE);
        }

        void FillItem(Item const* item, v1::InventoryItem* out)
        {
            ItemTemplate const* t = item->GetTemplate();
            out->set_item_guid(item->GetGUID().GetRawValue());
            out->set_item_id(item->GetEntry());
            out->set_count(item->GetCount());
            out->set_bag(item->GetBagSlot());
            out->set_slot(item->GetSlot());
            out->set_durability(item->GetUInt32Value(ITEM_FIELD_DURABILITY));
            out->set_max_durability(item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY));
            if (t)
            {
                out->set_name(t->Name1);
                out->set_quality(t->Quality);
                out->set_item_level(t->ItemLevel);
                out->set_required_level(t->RequiredLevel);
                out->set_inventory_type(t->InventoryType);
                out->set_sell_price_copper(t->SellPrice);
            }
        }

        void FillQuests(Player* p, v1::CharacterState* s)
        {
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = p->GetQuestSlotQuestId(slot);
                Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
                if (!quest)
                    continue;

                v1::QuestLogEntry* e = s->add_quests();
                e->set_quest_id(questId);
                e->set_title(quest->GetTitle());
                e->set_level(quest->GetQuestLevel());
                QuestStatus status = p->GetQuestStatus(questId);
                e->set_status(status == QUEST_STATUS_COMPLETE ? "COMPLETE" : status == QUEST_STATUS_FAILED ? "FAILED" : "INCOMPLETE");

                auto qs = p->getQuestStatusMap().find(questId);
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    int32 id = quest->RequiredNpcOrGo[i];
                    if (!id || !quest->RequiredNpcOrGoCount[i])
                        continue;
                    v1::QuestObjective* o = e->add_objectives();
                    o->set_required(quest->RequiredNpcOrGoCount[i]);
                    o->set_current(qs != p->getQuestStatusMap().end() ? qs->second.CreatureOrGOCount[i] : 0);
                    if (id > 0)
                    {
                        o->set_kind("CREATURE");
                        o->set_id(id);
                        if (CreatureTemplate const* c = sObjectMgr->GetCreatureTemplate(id))
                            o->set_name(c->Name);
                    }
                    else
                    {
                        o->set_kind("GAMEOBJECT");
                        o->set_id(-id);
                        if (GameObjectTemplate const* g = sObjectMgr->GetGameObjectTemplate(-id))
                            o->set_name(g->name);
                    }
                }
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    uint32 itemId = quest->RequiredItemId[i];
                    if (!itemId || !quest->RequiredItemCount[i])
                        continue;
                    v1::QuestObjective* o = e->add_objectives();
                    o->set_kind("ITEM");
                    o->set_id(itemId);
                    o->set_required(quest->RequiredItemCount[i]);
                    o->set_current(std::min<uint32>(p->GetItemCount(itemId, false), quest->RequiredItemCount[i]));
                    if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemId))
                        o->set_name(t->Name1);
                }
            }
        }

        // Whitelisted read-only snapshot of AC objects, built on the world thread (PLAN.md Â§4.6.3, Â§4.7).
        void FillState(ObjectGuid guid, std::vector<int> const& sections, v1::CharacterState* s)
        {
            auto wants = [&](v1::StateSection section)
            {
                return sections.empty() || std::find(sections.begin(), sections.end(), section) != sections.end();
            };

            s->set_character_guid(guid.GetCounter());
            HeadlessCharacter const* c = sAcsdk.Runtime().Find(guid);
            s->set_state(c ? ToProto(c->state) : v1::OFFLINE);
            if (c)
                s->set_name(c->name);
            if (auto lease = sAcsdk.Leases().Get(guid.GetCounter(), LeaseManager::Clock::now()))
                s->set_lease_holder(lease->holderLabel);

            Player* p = sAcsdk.Runtime().FindOnlinePlayer(guid);
            if (!p)
                return;

            s->set_in_world(true);
            s->set_name(p->GetName());

            if (wants(v1::IDENTITY))
            {
                s->set_race(p->getRace());
                s->set_class_id(p->getClass());
                s->set_level(p->GetLevel());
                s->set_gender(p->getGender());
            }

            if (wants(v1::POSITION))
            {
                s->set_map_id(p->GetMapId());
                s->set_zone_id(p->GetZoneId());
                s->set_area_id(p->GetAreaId());
                v1::Vector3* pos = s->mutable_position();
                pos->set_x(p->GetPositionX());
                pos->set_y(p->GetPositionY());
                pos->set_z(p->GetPositionZ());
                s->set_orientation(p->GetOrientation());
                s->set_moving(!p->movespline->Finalized());
            }

            if (wants(v1::VITALS))
            {
                s->set_health(p->GetHealth());
                s->set_max_health(p->GetMaxHealth());
                Powers power = p->getPowerType();
                s->set_power_type(power);
                s->set_power(p->GetPower(power));
                s->set_max_power(p->GetMaxPower(power));
            }

            if (wants(v1::STATUS))
            {
                s->set_dead(!p->IsAlive());
                s->set_ghost(p->HasPlayerFlag(PLAYER_FLAGS_GHOST));
                s->set_in_combat(p->IsInCombat());
                s->set_teleporting(p->IsBeingTeleported());
            }

            if (wants(v1::TARGET))
                s->set_target_guid(p->GetTarget().GetRawValue());

            if (wants(v1::MONEY))
                s->set_money_copper(p->GetMoney());

            if (wants(v1::GROUP))
                if (Group* group = p->GetGroup())
                {
                    s->set_in_group(true);
                    s->set_group_leader_guid(group->GetLeaderGUID().GetRawValue());
                    for (Group::MemberSlot const& m : group->GetMemberSlots())
                        s->add_group_member_guids(m.guid.GetRawValue());
                }

            if (wants(v1::SPELLS))
                for (auto const& [spellId, spell] : p->GetSpellMap())
                    if (spell->State != PLAYERSPELL_REMOVED && spell->Active)
                        s->add_known_spells(spellId);

            if (wants(v1::CORPSE))
                if (Corpse* corpse = p->GetCorpse())
                {
                    s->set_has_corpse(true);
                    s->set_corpse_map_id(corpse->GetMapId());
                    v1::Vector3* pos = s->mutable_corpse_position();
                    pos->set_x(corpse->GetPositionX());
                    pos->set_y(corpse->GetPositionY());
                    pos->set_z(corpse->GetPositionZ());
                    // Same rule as WorldSession::HandleReclaimCorpseOpcode.
                    time_t allowedAt = time_t(corpse->GetGhostTime() + p->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP));
                    time_t now = time_t(GameTime::GetGameTime().count());
                    s->set_corpse_reclaim_in_s(allowedAt > now ? uint32(allowedAt - now) : 0);
                }

            if (wants(v1::EQUIPMENT))
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        FillItem(item, s->add_equipment());

            if (wants(v1::INVENTORY))
            {
                for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                    if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        FillItem(item, s->add_inventory());
                for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                    if (Item* bagItem = p->GetItemByPos(INVENTORY_SLOT_BAG_0, bag); Bag* container = bagItem ? bagItem->ToBag() : nullptr)
                        for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                            if (Item* item = container->GetItemByPos(uint8(slot)))
                                FillItem(item, s->add_inventory());
                s->set_free_bag_slots(p->GetFreeInventorySpace());
            }

            if (wants(v1::QUESTS))
                FillQuests(p, s);
        }

        class CharacterServiceImpl final : public v1::CharacterService::CallbackService
        {
        public:
            grpc::ServerUnaryReactor* GetState(grpc::CallbackServerContext* ctx, v1::GetStateRequest const* request, v1::CharacterState* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                std::vector<int> sections(request->sections().begin(), request->sections().end());
                PostToWorld(reactor, [guid, sections, response](WorldCallPtr call)
                {
                    FillState(guid, sections, response);
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* Login(grpc::CallbackServerContext* ctx, v1::LoginRequest const* request, v1::LoginResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                if (request->name().empty())
                {
                    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "name is required"));
                    return reactor;
                }

                std::string name = request->name();
                PostToWorld(reactor, [name, response](WorldCallPtr call)
                {
                    sAcsdk.Runtime().Login(name, [call, response](LoginResult const& r)
                    {
                        FillLogin(response, r);
                        call->Finish(grpc::Status::OK);
                    });
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* Logout(grpc::CallbackServerContext* ctx, v1::LogoutRequest const* request, v1::LogoutResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                std::string token = request->lease_token();
                PostToWorld(reactor, [guid, token, response](WorldCallPtr call)
                {
                    // A leased character can only be logged out by its lease holder (PLAN.md Â§7).
                    if (sAcsdk.Leases().IsLeased(guid.GetCounter(), LeaseManager::Clock::now()) && !sAcsdk.CheckLease(guid, token))
                    {
                        response->set_reason("INVALID_LEASE_TOKEN");
                        call->Finish(grpc::Status::OK);
                        return;
                    }

                    sAcsdk.Runtime().Logout(guid, [call, response](bool ok, std::string const& reason)
                    {
                        response->set_ok(ok);
                        response->set_reason(reason);
                        call->Finish(grpc::Status::OK);
                    });
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* List(grpc::CallbackServerContext* ctx, v1::ListCharactersRequest const* /*request*/, v1::ListCharactersResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                PostToWorld(reactor, [response](WorldCallPtr call) { ListOnWorldThread(std::move(call), response); });
                return reactor;
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeCharacterService()
    {
        return std::make_unique<CharacterServiceImpl>();
    }
}
