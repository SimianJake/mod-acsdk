#include "acsdk/v1/auth.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "AccountAuth.h"
#include "AccountMgr.h"
#include "Base64.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "Util.h"

namespace acsdk
{
    namespace
    {
        uint32 MsUntil(SessionManager::Clock::time_point t)
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - SessionManager::Clock::now()).count();
            return ms > 0 ? static_cast<uint32>(std::min<int64>(ms, UINT32_MAX)) : 0;
        }

        // "ipv4:127.0.0.1:50000" -> "ipv4:127.0.0.1": the throttle keys on the address, not the connection.
        std::string PeerAddress(grpc::CallbackServerContext const* ctx)
        {
            std::string peer = ctx->peer();
            std::size_t colon = peer.rfind(':');
            return colon == std::string::npos || colon < 5 ? peer : peer.substr(0, colon);
        }

        // keys = { "user:<NAME>", "peer:<address>" }.
        std::chrono::milliseconds RetryAfter(std::vector<std::string> const& keys)
        {
            auto now = LoginThrottle::Clock::now();
            return std::max(sAcsdk.UserThrottle().RetryAfter({ keys[0] }, now), sAcsdk.PeerThrottle().RetryAfter({ keys[1] }, now));
        }

        void RecordFailure(std::vector<std::string> const& keys)
        {
            auto now = LoginThrottle::Clock::now();
            sAcsdk.UserThrottle().Failure({ keys[0] }, now);
            sAcsdk.PeerThrottle().Failure({ keys[1] }, now);
        }

        struct Credentials
        {
            std::string username;
            std::string password;
        };

        // "authorization: Basic base64(username:password)", split at the first ':' and upper-cased as AzerothCore
        // does (Utf8ToUpperOnlyLatin). Returns an error message, or "" on success.
        std::string ParseBasic(grpc::CallbackServerContext const* ctx, Credentials& out)
        {
            auto const& md = ctx->client_metadata();
            auto it = md.find("authorization");
            constexpr std::string_view prefix = "Basic ";
            if (it == md.end() || it->second.size() <= prefix.size() || std::string_view(it->second.data(), prefix.size()) != prefix)
                return "AuthService.Login needs 'authorization: Basic base64(username:password)' metadata";

            auto decoded = Acore::Encoding::Base64::Decode(std::string(it->second.data() + prefix.size(), it->second.size() - prefix.size()));
            if (!decoded)
                return "malformed Basic credentials";

            std::string plain(decoded->begin(), decoded->end());
            std::size_t colon = plain.find(':');
            if (colon == std::string::npos || colon == 0)
                return "malformed Basic credentials";

            out.username = plain.substr(0, colon);
            out.password = plain.substr(colon + 1);
            if (!Utf8ToUpperOnlyLatin(out.username) || !Utf8ToUpperOnlyLatin(out.password))
                return "credentials are not valid UTF-8";
            if (utf8length(out.username) > MAX_ACCOUNT_STR || utf8length(out.password) > MAX_PASS_STR)
                return "username or password longer than AzerothCore allows";
            return "";
        }

        // Stand-in salt/verifier for unknown accounts, so they cost the same SRP6 work as a wrong password.
        std::vector<uint8_t> const& DummyBytes()
        {
            static std::vector<uint8_t> const bytes = []()
            {
                auto random = Acore::Crypto::GetRandomBytes<32>();
                return std::vector<uint8_t>(random.begin(), random.end());
            }();
            return bytes;
        }

        void LoginOnWorldThread(WorldCallPtr call, Credentials credentials, std::string label,
                                std::vector<std::string> throttleKeys, std::string peer, v1::AuthLoginResponse* response)
        {
            std::string escaped = credentials.username;
            LoginDatabase.EscapeString(escaped);

            sAcsdk.Queries().AddCallback(LoginDatabase.AsyncQuery(AccountRowSql("a.username = '" + escaped + "'")).WithCallback(
                [call, credentials = std::move(credentials), label = std::move(label), throttleKeys = std::move(throttleKeys),
                 peer = std::move(peer), response](QueryResult result)
                {
                    std::optional<AccountRow> row;
                    if (result)
                        row = ReadAccountRow(result->Fetch());

                    // Unknown account and wrong password: same work, same answer.
                    bool ok = false;
                    if (row)
                        ok = VerifyPassword(credentials.username, credentials.password, row->salt, row->verifier);
                    else
                        (void)VerifyPassword(credentials.username, credentials.password, DummyBytes(), DummyBytes());
                    if (!ok)
                    {
                        RecordFailure(throttleKeys);
                        sAcsdk.GetStats().rpcsUnauthenticated.fetch_add(1, std::memory_order_relaxed);
                        LOG_INFO("module", "mod-acsdk: failed SDK login for '{}' from {}.", credentials.username, peer);
                        call->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "invalid username or password"));
                        return;
                    }

                    sAcsdk.UserThrottle().Success(throttleKeys.front());
                    Config const& config = sAcsdk.GetConfig();
                    if (std::string refusal = AccountRefusal(*row, config); !refusal.empty())
                    {
                        LOG_INFO("module", "mod-acsdk: SDK login for '{}' refused: {}.", row->username, refusal);
                        call->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "account may not use the SDK: " + refusal));
                        return;
                    }

                    bool admin = IsAdmin(*row, config);
                    std::string token = sAcsdk.Sessions().Create(row->id, row->username, label.empty() ? row->username : label, admin,
                                                                 VerifierTag(*row), SessionManager::Clock::now());
                    LOG_INFO("module", "mod-acsdk: SDK session started for account {} ({}){} from {}.", row->username, row->id,
                             admin ? " [admin]" : "", peer);

                    response->set_session_token(token);
                    response->set_account_id(row->id);
                    response->set_username(row->username);
                    response->set_admin(admin);
                    response->set_idle_timeout_s(config.authSessionIdleSec);
                    response->set_max_lifetime_s(config.authSessionMaxSec);
                    call->Finish(grpc::Status::OK);
                }));
        }

        class AuthServiceImpl final : public v1::AuthService::CallbackService
        {
        public:
            grpc::ServerUnaryReactor* Login(grpc::CallbackServerContext* ctx, v1::AuthLoginRequest const* request, v1::AuthLoginResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                CountCall();

                Credentials credentials;
                if (std::string error = ParseBasic(ctx, credentials); !error.empty())
                {
                    sAcsdk.GetStats().rpcsUnauthenticated.fetch_add(1, std::memory_order_relaxed);
                    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, error));
                    return reactor;
                }

                // In-memory backoff per username (strict) and per peer address (generous); nothing is written to the auth DB.
                std::string peer = PeerAddress(ctx);
                std::vector<std::string> keys{ "user:" + credentials.username, "peer:" + peer };
                if (auto wait = RetryAfter(keys); wait.count() > 0)
                {
                    sAcsdk.GetStats().rpcsUnauthenticated.fetch_add(1, std::memory_order_relaxed);
                    reactor->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                        "too many failed logins; retry in " + std::to_string((wait.count() + 999) / 1000) + " s"));
                    return reactor;
                }

                std::string label = request->label().substr(0, 64);
                if (label.empty())
                    label = ControllerLabel(ctx);

                PostToWorld(reactor, [credentials = std::move(credentials), label = std::move(label), keys = std::move(keys),
                                      peer = std::move(peer), response](WorldCallPtr call) mutable
                {
                    LoginOnWorldThread(std::move(call), std::move(credentials), std::move(label), std::move(keys), std::move(peer), response);
                });
                return reactor;
            }

            grpc::ServerUnaryReactor* Logout(grpc::CallbackServerContext* ctx, v1::AuthLogoutRequest const* /*request*/, v1::AuthLogoutResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                Caller caller;
                if (!Authorize(ctx, reactor, caller))
                    return reactor;

                // Leases and event streams are cleaned up on the next world tick (Module::UpdateSessions).
                response->set_ok(sAcsdk.Sessions().EndSession(caller.sessionId, "LOGOUT"));
                reactor->Finish(grpc::Status::OK);
                return reactor;
            }

            grpc::ServerUnaryReactor* WhoAmI(grpc::CallbackServerContext* ctx, v1::WhoAmIRequest const* /*request*/, v1::WhoAmIResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                Caller caller;
                if (!Authorize(ctx, reactor, caller))
                    return reactor;

                response->set_account_id(caller.accountId);
                response->set_username(caller.username);
                response->set_label(caller.label);
                response->set_admin(caller.admin);
                response->set_session_age_ms(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    SessionManager::Clock::now() - caller.issuedAt).count()));
                if (auto expiresAt = sAcsdk.Sessions().ExpiresAt(caller.sessionId))
                    response->set_expires_in_ms(MsUntil(*expiresAt));
                reactor->Finish(grpc::Status::OK);
                return reactor;
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeAuthService()
    {
        return std::make_unique<AuthServiceImpl>();
    }
}
