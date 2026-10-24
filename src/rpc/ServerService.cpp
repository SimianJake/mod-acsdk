#include "acsdk/v1/server.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"


namespace acsdk
{
    namespace
    {
        constexpr uint32_t ApiVersion = 1;
        constexpr char const* SdkVersion = "1.0.0";

        // Answers entirely from atomics/immutable data, so it never touches the world thread.
        class ServerServiceImpl final : public v1::ServerService::CallbackService
        {
        public:
            grpc::ServerUnaryReactor* Health(grpc::CallbackServerContext* ctx, v1::HealthRequest const* /*request*/, v1::HealthResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                response->set_serving(true);
                reactor->Finish(grpc::Status::OK);
                return reactor;
            }

            grpc::ServerUnaryReactor* GetInfo(grpc::CallbackServerContext* ctx, v1::GetInfoRequest const* /*request*/, v1::GetInfoResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ServerInfo const& info = sAcsdk.Info();
                auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - info.startedAt);
                response->set_api_version(ApiVersion);
                response->set_sdk_version(SdkVersion);
                response->set_core_revision(info.coreRevision);
                response->set_realm_name(info.realmName);
                response->set_uptime_ms(static_cast<uint64_t>(uptime.count()));
                response->set_world_ticks(info.worldTicks.load(std::memory_order_relaxed));
                response->set_last_tick_diff_ms(info.lastTickDiffMs.load(std::memory_order_relaxed));
                reactor->Finish(grpc::Status::OK);
                return reactor;
            }

            grpc::ServerUnaryReactor* GetStats(grpc::CallbackServerContext* ctx, v1::GetStatsRequest const* /*request*/, v1::GetStatsResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                Stats const& s = sAcsdk.GetStats();
                response->set_rpcs_total(s.rpcsTotal.load(std::memory_order_relaxed));
                response->set_rpcs_unauthenticated(s.rpcsUnauthenticated.load(std::memory_order_relaxed));
                response->set_commands_enqueued(s.commandsEnqueued.load(std::memory_order_relaxed));
                response->set_commands_executed(s.commandsExecuted.load(std::memory_order_relaxed));
                response->set_commands_rejected_queue_full(s.commandsRejectedQueueFull.load(std::memory_order_relaxed));
                response->set_command_queue_depth(static_cast<uint32_t>(sAcsdk.QueueDepth()));
                response->set_last_tick_commands(s.lastTickCommands.load(std::memory_order_relaxed));
                response->set_last_tick_dispatch_us(s.lastTickDispatchUs.load(std::memory_order_relaxed));
                response->set_max_tick_dispatch_us(s.maxTickDispatchUs.load(std::memory_order_relaxed));
                response->set_events_published(sAcsdk.Events().Published());
                response->set_events_dropped(sAcsdk.Events().Dropped());
                response->set_event_subscribers(static_cast<uint32_t>(sAcsdk.Events().SubscriberCount()));
                reactor->Finish(grpc::Status::OK);
                return reactor;
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeServerService()
    {
        return std::make_unique<ServerServiceImpl>();
    }
}
