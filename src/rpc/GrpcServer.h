#ifndef ACSDK_GRPC_SERVER_H
#define ACSDK_GRPC_SERVER_H

#include <atomic>
#include <memory>
#include <thread>

namespace grpc
{
    class Server;
    class Service;
}

namespace acsdk
{
    struct Config;

    // Owns the gRPC listener and its services. Uses the callback (reactor) API on gRPC's own small
    // thread pool: no thread per controller, stream or character (PLAN.md §4.5).
    class GrpcServer
    {
    public:
        GrpcServer();
        ~GrpcServer();

        // World thread. Returns false (and logs why) if the listener did not start.
        bool Start(Config const& config);

        // World thread. Begins shutdown; returns once every RPC has finished. `pump` is called while
        // waiting so RPCs whose completion runs on the world thread can still finish.
        template <typename Pump>
        void Stop(Pump&& pump);

        bool IsRunning() const { return _server != nullptr; }

    private:
        // grpc::Server::Shutdown blocks until every RPC has finished, and some RPCs finish on the
        // world thread, so shutdown runs on a helper thread while the world thread keeps pumping.
        void BeginShutdown();
        bool WaitFor(int milliseconds);
        void Release();

        std::unique_ptr<grpc::Service> _serverService;
        std::unique_ptr<grpc::Service> _characterService;
        std::unique_ptr<grpc::Service> _leaseService;
        std::unique_ptr<grpc::Service> _actionService;
        std::unique_ptr<grpc::Service> _worldService;
        std::unique_ptr<grpc::Service> _eventService;
        std::unique_ptr<grpc::Service> _debugService;
        std::unique_ptr<grpc::Server> _server;
        std::thread _shutdownThread;
        std::atomic<bool> _shutdownDone{false};
    };

    template <typename Pump>
    void GrpcServer::Stop(Pump&& pump)
    {
        if (!_server)
            return;

        BeginShutdown();
        while (!WaitFor(10))
            pump();

        Release();
    }
}

#endif
