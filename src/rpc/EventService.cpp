#include "acsdk/v1/event.grpc.pb.h"
#include "RpcCommon.h"
#include "Services.h"

#include "Log.h"

namespace acsdk
{
    namespace
    {
        // One Subscribe stream (PLAN.md §4.5: one per controller, callback API, no thread of its own).
        // The world thread pushes into the subscriber's bounded queue and wakes this writer; the writer drains
        // the queue on gRPC threads, one StartWrite at a time. A client that stops reading only fills its own
        // queue (oldest dropped, then Lagged); the world thread never waits for it.
        //
        // The stream starts only after the world thread has checked that every listed character is the
        // caller's (Begin), and it ends with UNAUTHENTICATED once the caller's session ends (EventBus::EndSession).
        class SubscribeReactor final : public grpc::ServerWriteReactor<v1::Event>
        {
        public:
            // Lets the world-thread check reach the reactor only while it is alive: OnDone clears it.
            struct Gate
            {
                std::mutex mutex;
                SubscribeReactor* reactor = nullptr;
            };

            SubscribeReactor() : _gate(std::make_shared<Gate>())
            {
                _gate->reactor = this;
            }

            std::shared_ptr<Gate> GetGate() const { return _gate; }

            // Any thread. Starts the stream with `filter`, or finishes it with `status` if that is not OK.
            static void Begin(std::shared_ptr<Gate> const& gate, grpc::Status const& status, EventSubscriber::Filter filter)
            {
                std::lock_guard lock(gate->mutex);
                SubscribeReactor* r = gate->reactor;
                if (!r)
                    return; // cancelled while the check was queued
                if (!status.ok())
                {
                    r->Finish(status);
                    return;
                }
                // The waker can run on the world thread during Dispatch; OnDone's Unsubscribe keeps `r` alive for it.
                r->_subscriber = sAcsdk.Events().Subscribe(std::move(filter), [r]() { r->WriteNext(); });
            }

            void OnWriteDone(bool ok) override
            {
                if (!ok)
                {
                    Finish(grpc::Status::CANCELLED); // client went away mid-write
                    return;
                }
                {
                    std::lock_guard lock(_mutex);
                    _current.reset();
                }
                WriteNext();
            }

            void OnCancel() override
            {
                Finish(grpc::Status::CANCELLED);
            }

            void OnDone() override
            {
                {
                    std::lock_guard lock(_gate->mutex);
                    _gate->reactor = nullptr;
                }
                if (_subscriber)
                    sAcsdk.Events().Unsubscribe(_subscriber.get());
                // Unsubscribe holds the bus lock, so no waker can be running for us any more.
                delete this;
            }

        private:
            void WriteNext()
            {
                std::lock_guard lock(_mutex);
                if (_finished || _current || !_subscriber)
                    return;
                _current = _subscriber->Next();
                if (_current)
                    StartWrite(_current.get());
                else if (_subscriber->Closed())
                    FinishLocked(NoSession()); // everything queued was delivered; the session is over
            }

            void Finish(grpc::Status const& status)
            {
                std::lock_guard lock(_mutex);
                FinishLocked(status);
            }

            void FinishLocked(grpc::Status const& status)
            {
                if (_finished)
                    return;
                _finished = true;
                ServerWriteReactor<v1::Event>::Finish(status);
            }

            std::shared_ptr<Gate> _gate;
            std::mutex _mutex;
            std::shared_ptr<EventSubscriber> _subscriber;
            EventPtr _current;   // kept alive until OnWriteDone
            bool _finished = false;
        };

        class EventServiceImpl final : public v1::EventService::CallbackService
        {
        public:
            grpc::ServerWriteReactor<v1::Event>* Subscribe(grpc::CallbackServerContext* ctx, v1::SubscribeRequest const* request) override
            {
                std::optional<Caller> caller = ResolveCaller(ctx);
                if (!caller)
                    return Reject(NoSession());

                EventSubscriber::Filter filter;
                filter.accountId = caller->accountId; // an empty character list means all of the caller's characters
                filter.sessionId = caller->sessionId;
                for (uint64_t guid : request->character_guids())
                    filter.characters.insert(guid);
                for (int type : request->types())
                    if (type > 0 && type < 32)
                        filter.typeMask |= 1u << type;
                // An explicit type list that only names RAW_PACKET must not fall back to "everything".
                if (filter.typeMask == (1u << v1::RAW_PACKET))
                    filter.typeMask |= 1u << v1::LAGGED;

                if (request->raw_opcodes_size() > 0)
                {
                    if (!sAcsdk.GetConfig().rawPacketsEnable)
                        return Reject(grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "raw packets are disabled (ACSDK.RawPackets.Enable)"));
                    for (uint32_t op : request->raw_opcodes())
                        filter.rawOpcodes.insert(static_cast<uint16_t>(op));
                }

                // Ownership is read from CharacterCache, which only the world thread may touch.
                auto* reactor = new SubscribeReactor();
                std::shared_ptr<SubscribeReactor::Gate> gate = reactor->GetGate();
                bool queued = sAcsdk.Post([gate, caller = *caller, filter]() mutable
                {
                    grpc::Status status = sAcsdk.Sessions().IsActive(caller.sessionId, SessionManager::Clock::now()) ? grpc::Status::OK : NoSession();
                    for (uint64_t guid : filter.characters)
                        if (status.ok())
                            status = CheckOwner(caller, PlayerGuidFromApi(guid));
                    SubscribeReactor::Begin(gate, status, std::move(filter));
                });
                if (!queued)
                {
                    sAcsdk.GetStats().commandsRejectedQueueFull.fetch_add(1, std::memory_order_relaxed);
                    SubscribeReactor::Begin(gate, grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "world command queue is full or stopping"), {});
                }
                return reactor;
            }

            grpc::ServerUnaryReactor* RespondToPrompt(grpc::CallbackServerContext* ctx, v1::RespondToPromptRequest const* request, v1::PromptResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                Caller caller;
                if (!Authorize(ctx, reactor, caller))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                std::string token = request->lease_token();
                uint64_t promptId = request->prompt_id();
                bool accept = request->accept();
                uint32_t vote = request->roll_vote();
                PostToWorld(reactor, [caller, guid, token, promptId, accept, vote, response](WorldCallPtr call)
                {
                    if (grpc::Status owner = CheckOwner(caller, guid); !owner.ok())
                    {
                        call->Finish(owner);
                        return;
                    }

                    if (!sAcsdk.CheckLease(guid, caller.sessionId, token))
                        response->set_reason("INVALID_LEASE_TOKEN");
                    else
                    {
                        switch (sAcsdk.Prompts().Respond(guid, promptId, accept, vote))
                        {
                            case PromptManager::RespondResult::Ok: response->set_ok(true); break;
                            case PromptManager::RespondResult::UnknownPrompt: response->set_reason("UNKNOWN_PROMPT"); break;
                            case PromptManager::RespondResult::WrongCharacter: response->set_reason("WRONG_CHARACTER"); break;
                        }
                    }
                    call->Finish(grpc::Status::OK);
                });
                return reactor;
            }

        private:
            static grpc::ServerWriteReactor<v1::Event>* Reject(grpc::Status status)
            {
                class Rejected final : public grpc::ServerWriteReactor<v1::Event>
                {
                public:
                    explicit Rejected(grpc::Status status) { Finish(std::move(status)); }
                    void OnDone() override { delete this; }
                };
                return new Rejected(std::move(status));
            }
        };
    }

    std::unique_ptr<grpc::Service> MakeEventService()
    {
        return std::make_unique<EventServiceImpl>();
    }
}
