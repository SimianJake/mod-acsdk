#include "acsdk/v1/world.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GameObject.h"
#include "GridNotifiersImpl.h"
#include "ModelIgnoreFlags.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <list>

// <windows.h> (pulled in by AzerothCore headers) defines GetObject as GetObjectA/W, which would rename our
// override of the generated WorldService::GetObject.
#ifdef GetObject
#undef GetObject
#endif

namespace acsdk
{
    namespace
    {
        // Whitelisted read-only queries (PLAN.md §4.6.3): grid searchers and the spell store.
        void NearbyOnWorldThread(WorldCallPtr call, v1::NearbyObjectsRequest request, v1::NearbyObjectsResponse* response)
        {
            Player* center = sAcsdk.Runtime().FindOnlinePlayer(PlayerGuidFromApi(request.character_guid()));
            if (!center)
            {
                call->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "character is not headless-online"));
                return;
            }

            float radius = std::clamp(request.radius() > 0 ? request.radius() : 30.0f, 1.0f, 100.0f); // PLAN.md §4.5: capped
            uint32 limit = std::clamp<uint32>(request.limit() ? request.limit() : 50, 1, 200);

            std::list<Unit*> units;
            Acore::AnyUnitInObjectRangeCheck check(center, radius);
            Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(center, units, check);
            Cell::VisitObjects(center, searcher, radius);

            std::vector<Unit*> matches;
            for (Unit* u : units)
            {
                if (u == center)
                    continue;
                bool isPlayer = u->IsPlayer();
                bool isCreature = u->IsCreature();
                if (request.kind() == v1::CREATURE && !isCreature)
                    continue;
                if (request.kind() == v1::PLAYER && !isPlayer)
                    continue;
                if (!isPlayer && !isCreature)
                    continue;
                if (request.alive_only() && !u->IsAlive())
                    continue;
                if (request.hostile_only() && !center->IsHostileTo(u))
                    continue;
                matches.push_back(u);
            }

            std::sort(matches.begin(), matches.end(), [center](Unit* a, Unit* b) { return center->GetExactDist(a) < center->GetExactDist(b); });
            if (matches.size() > limit)
                matches.resize(limit);

            for (Unit* u : matches)
            {
                v1::WorldObject* o = response->add_objects();
                o->set_guid(u->GetGUID().GetRawValue());
                o->set_kind(u->IsPlayer() ? v1::PLAYER : v1::CREATURE);
                o->set_entry(u->IsCreature() ? u->GetEntry() : 0);
                o->set_name(u->GetName());
                o->set_level(u->GetLevel());
                o->mutable_position()->set_x(u->GetPositionX());
                o->mutable_position()->set_y(u->GetPositionY());
                o->mutable_position()->set_z(u->GetPositionZ());
                o->set_orientation(u->GetOrientation());
                o->set_distance(center->GetExactDist(u));
                o->set_alive(u->IsAlive());
                o->set_hostile(center->IsHostileTo(u));
                o->set_attackable(center->IsValidAttackTarget(u));
                o->set_health(u->GetHealth());
                o->set_max_health(u->GetMaxHealth());
            }
            call->Finish(grpc::Status::OK);
        }

        Player* Center(uint64_t characterGuid)
        {
            return sAcsdk.Runtime().FindOnlinePlayer(PlayerGuidFromApi(characterGuid));
        }

        void Fill(v1::WorldObject* o, Player* center, WorldObject* obj)
        {
            o->set_guid(obj->GetGUID().GetRawValue());
            o->set_name(obj->GetName());
            o->mutable_position()->set_x(obj->GetPositionX());
            o->mutable_position()->set_y(obj->GetPositionY());
            o->mutable_position()->set_z(obj->GetPositionZ());
            o->set_orientation(obj->GetOrientation());
            o->set_distance(center->GetExactDist(obj));
            if (Unit* u = obj->ToUnit())
            {
                o->set_kind(u->IsPlayer() ? v1::PLAYER : v1::CREATURE);
                o->set_entry(u->IsCreature() ? u->GetEntry() : 0);
                o->set_level(u->GetLevel());
                o->set_alive(u->IsAlive());
                o->set_hostile(center->IsHostileTo(u));
                o->set_attackable(center->IsValidAttackTarget(u));
                o->set_health(u->GetHealth());
                o->set_max_health(u->GetMaxHealth());
            }
            else
            {
                o->set_kind(v1::GAMEOBJECT);
                o->set_entry(obj->GetEntry());
            }
        }

        // Runs fn(center, response) on the world thread, or fails the call when the character is not
        // headless-online.
        template <typename Response, typename Fn>
        grpc::ServerUnaryReactor* OnCharacter(grpc::CallbackServerContext* ctx, uint64_t characterGuid, Response* response, Fn fn)
        {
            grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
            if (!Authorize(ctx, reactor))
                return reactor;
            PostToWorld(reactor, [characterGuid, response, fn](WorldCallPtr call)
            {
                Player* center = Center(characterGuid);
                if (!center)
                {
                    call->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "character is not headless-online"));
                    return;
                }
                fn(center, response);
                call->Finish(grpc::Status::OK);
            });
            return reactor;
        }

        class WorldServiceImpl final : public v1::WorldService::CallbackService
        {
        public:
            // Whitelisted read-only query (PLAN.md 4.6.3): AzerothCore's PathGenerator over the mmaps navmesh.
            grpc::ServerUnaryReactor* FindPath(grpc::CallbackServerContext* ctx, v1::FindPathRequest const* request, v1::FindPathResponse* response) override
            {
                v1::FindPathRequest req = *request;
                return OnCharacter(ctx, req.character_guid(), response, [req](Player* center, v1::FindPathResponse* out)
                {
                    PathGenerator path(center);
                    path.SetUseStraightPath(req.straight());
                    float sx = center->GetPositionX(), sy = center->GetPositionY(), sz = center->GetPositionZ();
                    if (req.has_start())
                    {
                        sx = req.start().x();
                        sy = req.start().y();
                        sz = req.start().z();
                    }
                    path.CalculatePath(sx, sy, sz, req.end().x(), req.end().y(), req.end().z(), false);

                    for (G3D::Vector3 const& p : path.GetPath())
                    {
                        v1::Vector3* v = out->add_points();
                        v->set_x(p.x);
                        v->set_y(p.y);
                        v->set_z(p.z);
                    }
                    uint32 type = path.GetPathType();
                    out->set_path_flags(type);
                    out->set_length(path.getPathLength());
                    G3D::Vector3 const& end = path.GetActualEndPosition();
                    out->mutable_actual_end()->set_x(end.x);
                    out->mutable_actual_end()->set_y(end.y);
                    out->mutable_actual_end()->set_z(end.z);
                    out->set_reachable((type & PATHFIND_NORMAL) && !(type & (PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_SHORT)));
                });
            }

            grpc::ServerUnaryReactor* GetHeight(grpc::CallbackServerContext* ctx, v1::GetHeightRequest const* request, v1::GetHeightResponse* response) override
            {
                v1::GetHeightRequest req = *request;
                return OnCharacter(ctx, req.character_guid(), response, [req](Player* center, v1::GetHeightResponse* out)
                {
                    Map* map = center->GetMap();
                    float h = map->GetHeight(center->GetPhaseMask(), req.x(), req.y(), req.z(), req.include_vmaps());
                    out->set_found(h > INVALID_HEIGHT);
                    out->set_height(h);
                    out->set_water_level(map->GetWaterLevel(req.x(), req.y()));
                });
            }

            grpc::ServerUnaryReactor* LineOfSight(grpc::CallbackServerContext* ctx, v1::LineOfSightRequest const* request, v1::LineOfSightResponse* response) override
            {
                v1::LineOfSightRequest req = *request;
                return OnCharacter(ctx, req.character_guid(), response, [req](Player* center, v1::LineOfSightResponse* out)
                {
                    out->set_clear(center->GetMap()->isInLineOfSight(req.from().x(), req.from().y(), req.from().z(),
                                                                     req.to().x(), req.to().y(), req.to().z(),
                                                                     center->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing));
                });
            }

            grpc::ServerUnaryReactor* GetObject(grpc::CallbackServerContext* ctx, v1::GetObjectRequest const* request, v1::GetObjectResponse* response) override
            {
                uint64_t guid = request->guid();
                return OnCharacter(ctx, request->character_guid(), response, [guid](Player* center, v1::GetObjectResponse* out)
                {
                    WorldObject* obj = ObjectAccessor::GetWorldObject(*center, ObjectGuid(guid));
                    if (!obj || !obj->IsInWorld() || obj->GetMapId() != center->GetMapId())
                        return;
                    out->set_found(true);
                    Fill(out->mutable_object(), center, obj);
                    out->set_in_line_of_sight(center->IsWithinLOSInMap(obj));
                });
            }

            grpc::ServerUnaryReactor* GetItemInfo(grpc::CallbackServerContext* ctx, v1::GetItemInfoRequest const* request, v1::GetItemInfoResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;
                std::vector<uint32> ids(request->item_ids().begin(), request->item_ids().end());
                if (ids.size() > 1000)
                    ids.resize(1000);
                PostToWorld(reactor, [ids, response](WorldCallPtr call)
                {
                    for (uint32 id : ids)
                        if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(id))
                        {
                            v1::ItemInfo* i = response->add_items();
                            i->set_id(id);
                            i->set_name(t->Name1);
                            i->set_quality(t->Quality);
                            i->set_item_level(t->ItemLevel);
                            i->set_required_level(t->RequiredLevel);
                            i->set_item_class(t->Class);
                            i->set_item_subclass(t->SubClass);
                            i->set_inventory_type(t->InventoryType);
                            i->set_sell_price(t->SellPrice);
                        }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* GetCreatureInfo(grpc::CallbackServerContext* ctx, v1::GetCreatureInfoRequest const* request, v1::GetCreatureInfoResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;
                std::vector<uint32> ids(request->entries().begin(), request->entries().end());
                if (ids.size() > 1000)
                    ids.resize(1000);
                PostToWorld(reactor, [ids, response](WorldCallPtr call)
                {
                    for (uint32 id : ids)
                        if (CreatureTemplate const* t = sObjectMgr->GetCreatureTemplate(id))
                        {
                            v1::CreatureInfo* c = response->add_creatures();
                            c->set_entry(id);
                            c->set_name(t->Name);
                            c->set_subname(t->SubName);
                            c->set_min_level(t->minlevel);
                            c->set_max_level(t->maxlevel);
                            c->set_faction(t->faction);
                            c->set_rank(t->rank);
                            c->set_creature_type(t->type);
                        }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }
            grpc::ServerUnaryReactor* NearbyObjects(grpc::CallbackServerContext* ctx, v1::NearbyObjectsRequest const* request, v1::NearbyObjectsResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;
                v1::NearbyObjectsRequest copy = *request;
                PostToWorld(reactor, [copy, response](WorldCallPtr call) { NearbyOnWorldThread(std::move(call), copy, response); });
                return reactor;
            }

            grpc::ServerUnaryReactor* GetSpellInfo(grpc::CallbackServerContext* ctx, v1::GetSpellInfoRequest const* request, v1::GetSpellInfoResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;
                if (request->spell_ids_size() > 1000)
                {
                    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "at most 1000 spell ids per call"));
                    return reactor;
                }

                std::vector<uint32> ids(request->spell_ids().begin(), request->spell_ids().end());
                PostToWorld(reactor, [ids, response](WorldCallPtr call)
                {
                    for (uint32 id : ids)
                    {
                        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                        if (!info)
                            continue;
                        v1::SpellInfo* s = response->add_spells();
                        s->set_id(id);
                        s->set_name(info->SpellName[LOCALE_enUS] ? info->SpellName[LOCALE_enUS] : "");
                        s->set_rank(info->Rank[LOCALE_enUS] ? info->Rank[LOCALE_enUS] : "");
                        s->set_cast_time_ms(info->CalcCastTime());
                        s->set_channeled(info->IsChanneled());
                        s->set_passive(info->IsPassive());
                        s->set_max_range(info->GetMaxRange(false));
                        s->set_power_type(info->PowerType);
                        s->set_power_cost(info->ManaCost);
                        s->set_recovery_ms(std::max(info->RecoveryTime, info->CategoryRecoveryTime));
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeWorldService()
    {
        return std::make_unique<WorldServiceImpl>();
    }
}
