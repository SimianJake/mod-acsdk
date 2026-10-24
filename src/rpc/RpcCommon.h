#ifndef ACSDK_RPC_COMMON_H
#define ACSDK_RPC_COMMON_H

// gRPC before any AzerothCore header: MSVC resolves grpc::Slice's 'friend class ByteBuffer' to AC's
// global ::ByteBuffer if that is declared first.
#include <grpcpp/grpcpp.h>
#include <openssl/crypto.h>

#include "AcsdkModule.h"

#include <string>
#include <string_view>

namespace acsdk
{
    // Constant-time check of "authorization: Bearer <ACSDK.AuthToken>". Never logs the token (PLAN.md §4.4).
    inline bool HasValidServerToken(grpc::CallbackServerContext const* ctx, std::string const& expectedToken)
    {
        auto const& md = ctx->client_metadata();
        auto it = md.find("authorization");
        if (it == md.end())
            return false;

        std::string_view value(it->second.data(), it->second.size());
        constexpr std::string_view prefix = "Bearer ";
        if (value.size() != prefix.size() + expectedToken.size() || value.substr(0, prefix.size()) != prefix)
            return false;

        return CRYPTO_memcmp(value.data() + prefix.size(), expectedToken.data(), expectedToken.size()) == 0;
    }

    // Every handler starts with this. Counts the call and, if the server token is wrong, finishes the
    // reactor with UNAUTHENTICATED and returns false.
    //
    // gRPC C++ server interceptors cannot fail a call, so the check lives here rather than in an
    // interceptor; one call per handler keeps it impossible to miss in review.
    inline bool Authorize(grpc::CallbackServerContext* ctx, grpc::ServerUnaryReactor* reactor)
    {
        Stats& stats = sAcsdk.GetStats();
        stats.rpcsTotal.fetch_add(1, std::memory_order_relaxed);

        if (HasValidServerToken(ctx, sAcsdk.GetConfig().authToken))
            return true;

        stats.rpcsUnauthenticated.fetch_add(1, std::memory_order_relaxed);
        reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "missing or invalid server token"));
        return false;
    }

    // x-acsdk-controller-id: a human-readable label for logs and ListLeases. It grants nothing (PLAN.md §4.4).
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
