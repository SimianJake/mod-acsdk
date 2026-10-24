/*
 * mod-acsdk world hooks: owns the gRPC listener and the world-thread tick (PLAN.md §4.1 #2).
 * WorldScript::OnUpdate runs on the main thread after sessions and all map updates have finished
 * (docs/research/headless-sessions.md), so game objects may be touched from it.
 */

#include "AcsdkModule.h"
#include "GitRevision.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "World.h"

namespace
{
    class acsdk_world_script : public WorldScript
    {
    public:
        acsdk_world_script() : WorldScript("acsdk_world_script", {
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE,
            WORLDHOOK_ON_SHUTDOWN
        }) { }

        void OnStartup() override
        {
            // realm.Name comes from the realmlist row; World::GetRealmName() is never set by the core.
            sAcsdk.Startup(realm.Name, GitRevision::GetFullVersion());
        }

        void OnUpdate(uint32 diff) override
        {
            sAcsdk.Update(diff);
        }

        void OnShutdown() override
        {
            sAcsdk.Shutdown();
        }
    };
}

void AddSC_acsdk_world()
{
    new acsdk_world_script();
}
