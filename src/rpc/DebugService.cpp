/*
 * Development-only helpers (proto/acsdk/v1/debug.proto). Served only when ACSDK.Debug.Enable = 1.
 * Test provisioning only: account and character creation stay outside the SDK proper (PLAN.md §5.1).
 */

#include "acsdk/v1/debug.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "EnumNames.generated.h"

#include "AccountMgr.h"
#include "ByteBuffer.h"
#include "CharacterCache.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace acsdk
{
    namespace
    {
        // SMSG_CHAR_CREATE results of the temporary creation sessions (AcsdkPacketScripts forwards them).
        std::mutex charCreateMutex;
        std::unordered_map<WorldSession const*, uint8> charCreateResults;

        std::optional<uint8> TakeCharCreateResult(WorldSession const* session)
        {
            std::lock_guard lock(charCreateMutex);
            auto it = charCreateResults.find(session);
            if (it == charCreateResults.end())
                return std::nullopt;
            uint8 code = it->second;
            charCreateResults.erase(it);
            return code;
        }

        bool MatchesHeadlessPrefix(std::string name)
        {
            std::string const& prefix = sAcsdk.GetConfig().headlessAccountPrefix;
            std::transform(name.begin(), name.end(), name.begin(), ::toupper);
            return !prefix.empty() && name.rfind(prefix, 0) == 0;
        }

        // Drives AzerothCore's own character creation for a test account: a temporary socketless session
        // receives CMSG_CHAR_CREATE exactly as from a client and is pumped until the character exists.
        void CreateCharacterOnWorldThread(WorldCallPtr call, v1::CreateCharacterRequest request, v1::CreateCharacterResponse* response,
                                          std::chrono::steady_clock::time_point accountWaitUntil)
        {
            auto fail = [&](char const* reason)
            {
                response->set_reason(reason);
                call->Finish(grpc::Status::OK);
            };

            if (!MatchesHeadlessPrefix(request.account_username()))
                return fail("ACCOUNT_NOT_ELIGIBLE");

            uint32 accountId = AccountMgr::GetId(request.account_username());
            if (!accountId)
            {
                // AccountMgr::CreateAccount inserts asynchronously; give a just-created account a moment.
                if (std::chrono::steady_clock::now() < accountWaitUntil)
                {
                    sAcsdk.AddTicker([call, request, response, accountWaitUntil]()
                    {
                        CreateCharacterOnWorldThread(call, request, response, accountWaitUntil);
                        return false;
                    });
                    return;
                }
                return fail("ACCOUNT_NOT_FOUND");
            }

            if (sWorldSessionMgr->FindSession(accountId) || sAcsdk.Runtime().HoldsAccount(accountId))
                return fail("ACCOUNT_ALREADY_ONLINE");

            // AzerothCore stores the name capitalised ("examplepaladin" -> "Examplepaladin"); look it up the same way.
            std::string name = request.name();
            if (!normalizePlayerName(name))
                return fail("CHAR_NAME_NO_NAME");
            if (sCharacterCache->GetCharacterGuidByName(name))
                return fail("NAME_IN_USE");

            std::string username = request.account_username();
            auto* session = new WorldSession(accountId, std::move(username), 0, nullptr, SEC_PLAYER,
                                             EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, true, 0);
            session->LoadPermissions(); // dev-only tool: a synchronous RBAC load is acceptable here

            WorldPacket packet(CMSG_CHAR_CREATE, 32);
            packet << name;
            packet << uint8(request.race()) << uint8(request.class_id()) << uint8(request.gender());
            packet << uint8(0) << uint8(0) << uint8(0) << uint8(0) << uint8(0); // skin, face, hair style/color, facial hair
            packet << uint8(0);                                                  // outfit id

            {   // name checks answer synchronously inside the handler
                std::lock_guard lock(charCreateMutex);
                charCreateResults.erase(session);
            }
            try
            {
                opcodeTable[CMSG_CHAR_CREATE]->Call(session, packet);
            }
            catch (ByteBufferException const&)
            {
                TakeCharCreateResult(session);
                delete session;
                return fail("MALFORMED");
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            sAcsdk.AddTicker([call, response, session, name, deadline]()
            {
                session->ProcessQueryCallbacks();

                // AC answers with SMSG_CHAR_CREATE (ResponseCodes); success is also visible in the cache.
                ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
                std::optional<uint8> code = guid ? std::nullopt : TakeCharCreateResult(session);
                bool failed = code && *code != CHAR_CREATE_SUCCESS;
                bool timedOut = std::chrono::steady_clock::now() >= deadline;
                if (!guid && !failed && !timedOut)
                    return true; // keep pumping

                response->set_ok(bool(guid));
                response->set_name(name);
                if (guid)
                    response->set_character_guid(guid.GetCounter());
                else if (failed)
                {
                    char const* reason = ResponseCodesName(*code);
                    response->set_reason(reason ? reason : "CHAR_CREATE_FAILED");
                }
                else
                    response->set_reason("TIMEOUT"); // no answer from AzerothCore; see the worldserver log
                TakeCharCreateResult(session);
                delete session;
                call->Finish(grpc::Status::OK);
                return false;
            });
        }

        class DebugServiceImpl final : public v1::DebugService::CallbackService
        {
        public:
            grpc::ServerUnaryReactor* CreateAccount(grpc::CallbackServerContext* ctx, v1::CreateAccountRequest const* request, v1::DebugResult* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                std::string username = request->username();
                std::string password = request->password();
                PostToWorld(reactor, [username, password, response](WorldCallPtr call)
                {
                    if (!MatchesHeadlessPrefix(username))
                    {
                        response->set_reason("ACCOUNT_NOT_ELIGIBLE");
                        call->Finish(grpc::Status::OK);
                        return;
                    }

                    // Dev-only: AccountMgr writes synchronously; accounts are player-level by default.
                    AccountOpResult result = sAccountMgr->CreateAccount(username, password);
                    response->set_ok(result == AOR_OK);
                    switch (result)
                    {
                        case AOR_OK: break;
                        case AOR_NAME_ALREADY_EXIST: response->set_reason("NAME_ALREADY_EXISTS"); break;
                        case AOR_NAME_TOO_LONG: response->set_reason("NAME_TOO_LONG"); break;
                        case AOR_PASS_TOO_LONG: response->set_reason("PASSWORD_TOO_LONG"); break;
                        default: response->set_reason("FAILED"); break;
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* CreateCharacter(grpc::CallbackServerContext* ctx, v1::CreateCharacterRequest const* request, v1::CreateCharacterResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                v1::CreateCharacterRequest copy = *request;
                PostToWorld(reactor, [copy, response](WorldCallPtr call)
                {
                    CreateCharacterOnWorldThread(std::move(call), copy, response, std::chrono::steady_clock::now() + std::chrono::seconds(5));
                });
                return reactor;
            }

        };
    }

    void OnCharCreateResponse(WorldSession const* session, uint8 code)
    {
        std::lock_guard lock(charCreateMutex);
        charCreateResults[session] = code;
    }

    std::unique_ptr<grpc::Service> MakeDebugService()
    {
        return std::make_unique<DebugServiceImpl>();
    }
}
