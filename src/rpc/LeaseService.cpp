#include "acsdk/v1/lease.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "Log.h"

namespace acsdk
{
    namespace
    {
        uint32 MsUntil(LeaseManager::Clock::time_point t)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - LeaseManager::Clock::now()).count();
            return ms > 0 ? static_cast<uint32>(ms) : 0;
        }

        char const* ReasonOf(LeaseManager::Result r)
        {
            switch (r)
            {
                case LeaseManager::Result::Ok:            return "";
                case LeaseManager::Result::AlreadyLeased: return "ALREADY_LEASED";
                case LeaseManager::Result::InvalidToken:  return "INVALID_LEASE_TOKEN";
                case LeaseManager::Result::NotLeased:     return "NOT_LEASED";
            }
            return "UNKNOWN";
        }

        // All lease state lives on the world thread; handlers only copy request fields and post.
        class LeaseServiceImpl final : public v1::LeaseService::CallbackService
        {
        public:
            grpc::ServerUnaryReactor* ClaimCharacter(grpc::CallbackServerContext* ctx, v1::ClaimCharacterRequest const* request, v1::ClaimCharacterResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                bool force = request->force();
                std::string label = ControllerLabel(ctx);
                PostToWorld(reactor, [guid, force, label, response](WorldCallPtr call)
                {
                    HeadlessCharacter* c = sAcsdk.Runtime().Find(guid);
                    if (!c || (c->state != Lifecycle::Online && c->state != Lifecycle::LoggingIn))
                    {
                        response->set_reason("NOT_HEADLESS");
                        call->Finish(grpc::Status::OK);
                        return;
                    }

                    if (force && !sAcsdk.GetConfig().leaseAllowForceTakeover)
                    {
                        response->set_reason("TAKEOVER_DISABLED");
                        call->Finish(grpc::Status::OK);
                        return;
                    }

                    LeaseManager::Grant grant;
                    LeaseManager::Result r = sAcsdk.Leases().Claim(guid.GetCounter(), label, force, LeaseManager::Clock::now(), grant);
                    response->set_ok(r == LeaseManager::Result::Ok);
                    response->set_reason(ReasonOf(r));
                    if (r == LeaseManager::Result::Ok)
                    {
                        response->set_lease_token(grant.token);
                        response->set_ttl_ms(MsUntil(grant.expiresAt));
                        if (grant.previousHolder)
                        {
                            response->set_previous_holder(*grant.previousHolder);
                            sAcsdk.OnLeaseTakenOver(guid, label); // LeaseLost to the old holder; its in-flight commands are cancelled (§4.4)
                            LOG_INFO("module", "mod-acsdk: '{}' took over character {} from '{}'.", label, guid.GetCounter(), *grant.previousHolder);
                        }
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* ReleaseCharacter(grpc::CallbackServerContext* ctx, v1::ReleaseCharacterRequest const* request, v1::LeaseResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                std::string token = request->lease_token();
                PostToWorld(reactor, [guid, token, response](WorldCallPtr call)
                {
                    LeaseManager::Result r = sAcsdk.Leases().Release(guid.GetCounter(), token, LeaseManager::Clock::now());
                    response->set_ok(r == LeaseManager::Result::Ok);
                    response->set_reason(ReasonOf(r));
                    if (r == LeaseManager::Result::Ok)
                        sAcsdk.OnLeaseEnded(guid, /*stopMovement*/ false); // released on purpose: leave it as commanded
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* RenewLease(grpc::CallbackServerContext* ctx, v1::RenewLeaseRequest const* request, v1::RenewLeaseResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                std::string token = request->lease_token();
                PostToWorld(reactor, [guid, token, response](WorldCallPtr call)
                {
                    LeaseManager::Clock::time_point expiresAt;
                    LeaseManager::Result r = sAcsdk.Leases().Renew(guid.GetCounter(), token, LeaseManager::Clock::now(), expiresAt);
                    response->set_ok(r == LeaseManager::Result::Ok);
                    response->set_reason(ReasonOf(r));
                    if (r == LeaseManager::Result::Ok)
                        response->set_ttl_ms(MsUntil(expiresAt));
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* ListLeases(grpc::CallbackServerContext* ctx, v1::ListLeasesRequest const* /*request*/, v1::ListLeasesResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                PostToWorld(reactor, [response](WorldCallPtr call)
                {
                    for (LeaseManager::LeaseInfo const& info : sAcsdk.Leases().List(LeaseManager::Clock::now()))
                    {
                        v1::LeaseEntry* e = response->add_leases();
                        e->set_character_guid(info.characterGuid);
                        e->set_holder_label(info.holderLabel);
                        e->set_expires_in_ms(MsUntil(info.expiresAt));
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeLeaseService()
    {
        return std::make_unique<LeaseServiceImpl>();
    }
}
