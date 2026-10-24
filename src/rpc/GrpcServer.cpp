#include "GrpcServer.h"

#include "AcsdkConfig.h"
#include "Log.h"
#include "Services.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/service_type.h>

namespace acsdk
{
    GrpcServer::GrpcServer() = default;

    GrpcServer::~GrpcServer()
    {
        // Normal shutdown goes through Stop(); this only guards against leaking the helper thread.
        if (_shutdownThread.joinable())
            _shutdownThread.join();
    }

    bool GrpcServer::Start(Config const& config)
    {
        if (_server)
            return true;

        if (config.authToken.empty())
        {
            LOG_ERROR("module", "mod-acsdk: ACSDK.AuthToken is empty; refusing to start the gRPC listener.");
            return false;
        }

        _serverService = MakeServerService();
        _characterService = MakeCharacterService();
        _leaseService = MakeLeaseService();
        _actionService = MakeActionService();
        _worldService = MakeWorldService();
        _eventService = MakeEventService();
        if (config.debugService)
            _debugService = MakeDebugService();

        std::string address = config.bindAddress + ":" + std::to_string(config.port);
        int boundPort = 0;

        grpc::ServerBuilder builder;
        builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &boundPort);
        builder.RegisterService(_serverService.get());
        builder.RegisterService(_characterService.get());
        builder.RegisterService(_leaseService.get());
        builder.RegisterService(_actionService.get());
        builder.RegisterService(_worldService.get());
        builder.RegisterService(_eventService.get());
        if (_debugService)
        {
            builder.RegisterService(_debugService.get());
            LOG_WARN("module", "mod-acsdk: DebugService enabled (ACSDK.Debug.Enable); development only.");
        }
        // Reap dead controller connections (PLAN.md §4.5).
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        _server = builder.BuildAndStart();

        if (!_server || boundPort == 0)
        {
            LOG_ERROR("module", "mod-acsdk: failed to start gRPC listener on {}.", address);
            _server.reset();
            _serverService.reset();
            _characterService.reset();
            _leaseService.reset();
            _actionService.reset();
            _worldService.reset();
            _eventService.reset();
            _debugService.reset();
            return false;
        }

        LOG_INFO("module", "mod-acsdk: gRPC listener started on {}.", address);
        return true;
    }

    void GrpcServer::BeginShutdown()
    {
        _shutdownDone = false;
        grpc::Server* server = _server.get();
        _shutdownThread = std::thread([this, server]()
        {
            // After the deadline gRPC cancels outstanding calls; they still finish via the world-thread pump.
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
            server->Wait();
            _shutdownDone = true;
        });
    }

    bool GrpcServer::WaitFor(int milliseconds)
    {
        if (_shutdownDone)
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        return _shutdownDone;
    }

    void GrpcServer::Release()
    {
        if (_shutdownThread.joinable())
            _shutdownThread.join();

        _server.reset();
        _serverService.reset();
        _characterService.reset();
        _leaseService.reset();
        _actionService.reset();
        _worldService.reset();
        _eventService.reset();
        _debugService.reset();
        LOG_INFO("module", "mod-acsdk: gRPC listener stopped.");
    }
}
