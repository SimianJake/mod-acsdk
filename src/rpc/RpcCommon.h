#ifndef ACSDK_RPC_COMMON_H
#define ACSDK_RPC_COMMON_H

// gRPC before any AzerothCore header: MSVC resolves grpc::Slice's 'friend class ByteBuffer' to AC's
// global ::ByteBuffer if that is declared first.
#include <grpcpp/grpcpp.h>

#include "AcsdkModule.h"

#include <optional>
#include <string>
#include <string_view>

namespace acsdk
{
    // The caller of an RPC: the account session named by its bearer token (ARCHITECTURE.md §4.3).
    using Caller = SessionManager::Identity;

    // The session token from "authorization: Bearer <token>"; empty if absent or another scheme. Never logged.
    inline std::string BearerToken(grpc::CallbackServerContext const* ctx)
    {
        auto const& md = ctx->client_metadata();
        auto it = md.find("authorization");
        if (it == md.end())
            return "";

        std::string_view value(it->second.data(), it->second.size());
        constexpr std::string_view prefix = "Bearer ";
        if (value.size() <= prefix.size() || value.substr(0, prefix.size()) != prefix)
            return "";
        return std::string(value.substr(prefix.size()));
    }

    // For the calls that need no session (AuthService.Login, ServerService.Health/GetInfo): counts them.
    inline void CountCall()
    {
        sAcsdk.GetStats().rpcsTotal.fetch_add(1, std::memory_order_relaxed);
    }

    // Resolves the caller's session without finishing anything (streams reject on their own).
    inline std::optional<Caller> ResolveCaller(grpc::CallbackServerContext const* ctx)
    {
        CountCall();
        std::optional<Caller> caller = sAcsdk.Sessions().Resolve(BearerToken(ctx), SessionManager::Clock::now());
        if (!caller)
            sAcsdk.GetStats().rpcsUnauthenticated.fetch_add(1, std::memory_order_relaxed);
        return caller;
    }

    inline grpc::Status NoSession()
    {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "missing, invalid or expired session; log in with AuthService.Login");
    }

    // Every handler that needs a session starts with this. Counts the call and resolves the caller's
    // session; if there is none, finishes the reactor with UNAUTHENTICATED and returns false.
    //
    // gRPC C++ server interceptors cannot fail a call, so the check lives here rather than in an
    // interceptor; one call per handler keeps it impossible to miss in review.
    inline bool Authorize(grpc::CallbackServerContext* ctx, grpc::ServerUnaryReactor* reactor, Caller& caller)
    {
        std::optional<Caller> resolved = ResolveCaller(ctx);
        if (!resolved)
        {
            reactor->Finish(NoSession());
            return false;
        }
        caller = std::move(*resolved);
        return true;
    }

    // Authorize, and the session must be an admin's (GM level >= ACSDK.Auth.AdminGmLevel).
    inline bool AuthorizeAdmin(grpc::CallbackServerContext* ctx, grpc::ServerUnaryReactor* reactor, Caller& caller)
    {
        if (!Authorize(ctx, reactor, caller))
            return false;
        if (caller.admin)
            return true;
        reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "admin session required (ACSDK.Auth.AdminGmLevel)"));
        return false;
    }

    // World thread, next to every CheckLease: the caller's session is still live and `guid` is a character on
    // its account. A character of another account is PERMISSION_DENIED whether or not it exists.
    inline grpc::Status CheckOwner(Caller const& caller, ObjectGuid guid)
    {
        if (!sAcsdk.Sessions().IsActive(caller.sessionId, SessionManager::Clock::now()))
            return NoSession();
        if (!sAcsdk.Owns(caller.accountId, guid))
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "character is not on this session's account");
        return grpc::Status::OK;
    }

    // x-acsdk-controller-id: a human-readable label for logs and ListLeases. It grants nothing (PLAN.md §4.4);
    // AuthService.Login uses it as the session label when the request has none.
    inline std::string ControllerLabel(grpc::CallbackServerContext const* ctx)
    {
        auto const& md = ctx->client_metadata();
        auto it = md.find("x-acsdk-controller-id");
        if (it == md.end())
            return "";
        return std::string(it->second.data(), std::min<std::size_t>(it->second.size(), 64));
    }

    inline ObjectGuid PlayerGuidFromApi(uint64_t counter)
    {
        return ObjectGuid::Create<HighGuid::Player>(static_cast<ObjectGuid::LowType>(counter));
    }

    // An RPC whose answer is produced on the world thread. Tracks it so shutdown can wait for it.
    class WorldCall
    {
    public:
        explicit WorldCall(grpc::ServerUnaryReactor* reactor) : _reactor(reactor)
        {
            sAcsdk.GetStats().asyncCallsInFlight.fetch_add(1, std::memory_order_relaxed);
        }

        // Finish exactly once, from any thread.
        void Finish(grpc::Status const& status)
        {
            _reactor->Finish(status);
            sAcsdk.GetStats().asyncCallsInFlight.fetch_sub(1, std::memory_order_relaxed);
        }

    private:
        grpc::ServerUnaryReactor* _reactor;
    };

    using WorldCallPtr = std::shared_ptr<WorldCall>;

    // Queue `task` (taking a WorldCallPtr, which it keeps alive until it calls Finish) to run on the
    // world thread. If the queue is full or closed the call fails immediately and the task is dropped.
    template <typename Task>
    void PostToWorld(grpc::ServerUnaryReactor* reactor, Task&& task)
    {
        auto call = std::make_shared<WorldCall>(reactor);
        bool queued = sAcsdk.Post([call, task = std::forward<Task>(task)]() mutable { task(call); });

        if (!queued)
        {
            sAcsdk.GetStats().commandsRejectedQueueFull.fetch_add(1, std::memory_order_relaxed);
            call->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "world command queue is full or stopping"));
        }
    }
}

#endif
