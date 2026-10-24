#ifndef ACSDK_SERVICES_H
#define ACSDK_SERVICES_H

#include <cstdint>
#include <memory>

class WorldSession;

namespace grpc
{
    class Service;
}

namespace acsdk
{
    std::unique_ptr<grpc::Service> MakeServerService();
    std::unique_ptr<grpc::Service> MakeCharacterService();
    std::unique_ptr<grpc::Service> MakeLeaseService();
    std::unique_ptr<grpc::Service> MakeActionService();
    std::unique_ptr<grpc::Service> MakeWorldService();
    std::unique_ptr<grpc::Service> MakeEventService();
    std::unique_ptr<grpc::Service> MakeDebugService(); // dev-only test provisioning (ACSDK.Debug.Enable)

    // DebugService.CreateCharacter: SMSG_CHAR_CREATE seen for a session (any thread).
    void OnCharCreateResponse(WorldSession const* session, uint8_t code);
}

#endif
