#ifndef ACSDK_MODULE_H
#define ACSDK_MODULE_H

#include "AcsdkConfig.h"
#include "CommandQueue.h"
#include "Commands.h"
#include "EventBus.h"
#include "Prompts.h"
#include "GrpcServer.h"
#include "HeadlessRuntime.h"
#include "LeaseManager.h"

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "QueryCallback.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace acsdk
{
    // Diagnostic counters (PLAN.md §4.5, ServerService.GetStats). Written from any thread.
    struct Stats
    {
        std::atomic<uint64_t> rpcsTotal{0};
        std::atomic<uint64_t> rpcsUnauthenticated{0};
        std::atomic<uint64_t> commandsEnqueued{0};
        std::atomic<uint64_t> commandsExecuted{0};
        std::atomic<uint64_t> commandsRejectedQueueFull{0};
        std::atomic<uint32_t> lastTickCommands{0};
        std::atomic<uint32_t> lastTickDispatchUs{0};
        std::atomic<uint32_t> maxTickDispatchUs{0};
        std::atomic<int64_t> asyncCallsInFlight{0};
    };

    // Values gRPC threads may read: immutable after Startup() or atomic (PLAN.md §4.1 #1).
    struct ServerInfo
    {
        std::string realmName;
        std::string coreRevision;
        std::chrono::steady_clock::time_point startedAt;
        std::atomic<uint64_t> worldTicks{0};
        std::atomic<uint32_t> lastTickDiffMs{0};
    };

    // Process-wide state of mod-acsdk. Owned by the world thread; see per-member notes for what
    // other threads may touch.
    class Module
    {
    public:
        static Module& Instance();

        // World thread.
        void Startup(std::string realmName, std::string coreRevision);
        void Update(uint32_t diff);
        void Shutdown();

        // Any thread: queue a task to run on the world thread during the next Update().
        // Returns false if the queue is full or the module is stopping.
        bool Post(CommandQueue::Task task);

        // World thread only.
        QueryCallbackProcessor& Queries() { return _queries; }
        // Runs `tick` once per world tick until it returns false.
        void AddTicker(std::function<bool()> tick) { _tickers.push_back(std::move(tick)); }
        HeadlessRuntime& Runtime() { return _runtime; } // IsHeadless/SinkFor are safe from any thread
        LeaseManager& Leases() { return *_leases; }
        CommandManager& Commands() { return _commands; }
        EventBus& Events() { return _events; }             // Publish/HasSubscribers are safe from any thread
        PromptManager& Prompts() { return _prompts; }
        // True if `token` is the current lease token of `guid` (PLAN.md §4.4).
        bool CheckLease(ObjectGuid guid, std::string const& token) const;
        // A lease ended without the holder releasing it (expiry/takeover/release): stop movement, stay put.
        void OnLeaseEnded(ObjectGuid guid, bool stopMovement);
        bool IsRunning() const { return _server.IsRunning(); }
        bool IsWorldThread() const { return std::this_thread::get_id() == _worldThread; }

        // Immutable once the listener runs.
        Config const& GetConfig() const { return _config; }

        Stats& GetStats() { return _stats; }
        ServerInfo& Info() { return _info; }
        std::size_t QueueDepth() const { return _queue.Size(); }

    private:
        Module();
        void RunTickers();
        void UpdateLeases();
        void UpdateCharacterStates();
        void PublishLeaseLost(ObjectGuid guid, char const* reason, std::string const& newHolder);

    public:
        // A forced takeover replaced `guid`'s lease holder (LeaseService).
        void OnLeaseTakenOver(ObjectGuid guid, std::string const& newHolder);

    private:

        Config _config;
        Stats _stats;
        ServerInfo _info;
        CommandQueue _queue;
        QueryCallbackProcessor _queries;
        HeadlessRuntime _runtime;
        std::unique_ptr<LeaseManager> _leases;
        CommandManager _commands{ _runtime };
        EventBus _events;
        PromptManager _prompts{ _runtime, _events };

        struct StateSnapshot
        {
            Lifecycle lifecycle = Lifecycle::Offline;
            bool dead = false, ghost = false, inCombat = false, teleporting = false;
            uint32_t mapId = 0;
            bool operator==(StateSnapshot const&) const = default;
        };
        std::unordered_map<ObjectGuid, StateSnapshot> _states;
        std::vector<std::function<bool()>> _tickers;
        GrpcServer _server;
        std::thread::id _worldThread;
    };
}

#define sAcsdk acsdk::Module::Instance()

#endif
