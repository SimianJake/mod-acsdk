/*
 * Hook-sourced events (docs/events.md): Death, AuraApplied, AuraRemoved, LevelUp.
 *
 * These hooks fire for every unit on the server and mostly on map worker threads. Each one first checks
 * "is anyone subscribed?" and "is this a headless character?" (zero cost when unused, PLAN.md §4.5), then
 * copies plain fields into a self-contained event; no game object is touched after the hook returns.
 */

#include "AcsdkModule.h"

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "Unit.h"
#include "WorldSession.h"

namespace
{
    Player* HeadlessPlayer(Unit* unit)
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return nullptr;
        WorldSession* session = player->GetSession();
        return session && session->IsHeadless() && sAcsdk.Runtime().IsHeadless(session) ? player : nullptr;
    }

    class acsdk_unit_script : public UnitScript
    {
    public:
        acsdk_unit_script() : UnitScript("acsdk_unit_script", true, {
            UNITHOOK_ON_UNIT_DEATH,
            UNITHOOK_ON_AURA_APPLY,
            UNITHOOK_ON_AURA_REMOVE
        }) { }

        void OnUnitDeath(Unit* unit, Unit* killer) override
        {
            if (!sAcsdk.Events().HasSubscribers())
                return;

            uint64_t victim = unit ? unit->GetGUID().GetRawValue() : 0;
            uint64_t killerGuid = killer ? killer->GetGUID().GetRawValue() : 0;

            if (Player* dead = HeadlessPlayer(unit))
            {
                auto e = std::make_unique<acsdk::v1::Event>();
                e->mutable_death()->set_role(acsdk::v1::DeathEvent::DIED);
                e->mutable_death()->set_victim_guid(victim);
                e->mutable_death()->set_killer_guid(killerGuid);
                sAcsdk.Events().Publish(dead->GetGUID().GetRawValue(), std::move(e));
            }

            if (Player* winner = HeadlessPlayer(killer); winner && killer != unit)
            {
                auto e = std::make_unique<acsdk::v1::Event>();
                e->mutable_death()->set_role(acsdk::v1::DeathEvent::KILLED);
                e->mutable_death()->set_victim_guid(victim);
                e->mutable_death()->set_killer_guid(killerGuid);
                sAcsdk.Events().Publish(winner->GetGUID().GetRawValue(), std::move(e));
            }
        }

        void OnAuraApply(Unit* unit, Aura* aura) override
        {
            if (!sAcsdk.Events().HasSubscribers() || !aura)
                return;
            Player* player = HeadlessPlayer(unit);
            if (!player)
                return;

            auto e = std::make_unique<acsdk::v1::Event>();
            acsdk::v1::AuraEvent* a = e->mutable_aura_applied();
            a->set_target_guid(player->GetGUID().GetRawValue());
            a->set_caster_guid(aura->GetCasterGUID().GetRawValue());
            a->set_spell_id(aura->GetId());
            a->set_stacks(aura->GetStackAmount());
            a->set_duration_ms(aura->GetDuration());
            sAcsdk.Events().Publish(player->GetGUID().GetRawValue(), std::move(e));
        }

        void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
        {
            if (!sAcsdk.Events().HasSubscribers() || !aurApp)
                return;
            Player* player = HeadlessPlayer(unit);
            if (!player)
                return;

            Aura const* aura = aurApp->GetBase();
            auto e = std::make_unique<acsdk::v1::Event>();
            acsdk::v1::AuraEvent* a = e->mutable_aura_removed();
            a->set_target_guid(player->GetGUID().GetRawValue());
            a->set_caster_guid(aura->GetCasterGUID().GetRawValue());
            a->set_spell_id(aura->GetId());
            a->set_stacks(aura->GetStackAmount());
            a->set_remove_mode(static_cast<uint32>(mode));
            sAcsdk.Events().Publish(player->GetGUID().GetRawValue(), std::move(e));
        }
    };

    class acsdk_player_script : public PlayerScript
    {
    public:
        acsdk_player_script() : PlayerScript("acsdk_player_script", { PLAYERHOOK_ON_LEVEL_CHANGED }) { }

        void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
        {
            if (!sAcsdk.Events().HasSubscribers() || !HeadlessPlayer(player))
                return;

            auto e = std::make_unique<acsdk::v1::Event>();
            e->mutable_level_up()->set_old_level(oldLevel);
            e->mutable_level_up()->set_new_level(player->GetLevel());
            sAcsdk.Events().Publish(player->GetGUID().GetRawValue(), std::move(e));
        }
    };
}

void AddSC_acsdk_events()
{
    new acsdk_unit_script();
    new acsdk_player_script();
}
