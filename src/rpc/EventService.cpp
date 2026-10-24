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
        class SubscribeReactor final : public grpc::ServerWriteReactor<v1::Event>
        {
        public:
            SubscribeReactor(EventSubscriber::Filter filter)
            {
                // The waker can run on the world thread during Dispatch; keep `this` alive via the guard below.
                _subscriber = sAcsdk.Events().Subscribe(std::move(filter), [this]() { WriteNext(); });
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
                sAcsdk.Events().Unsubscribe(_subscriber.get());
                // Unsubscribe holds the bus lock, so no waker can be running for us any more.
                delete this;
            }

        private:
            void WriteNext()
            {
                std::lock_guard lock(_mutex);
                if (_finished || _current)
                    return;
                _current = _subscriber->Next();
                if (_current)
                    StartWrite(_current.get());
            }

            void Finish(grpc::Status const& status)
            {
                std::lock_guard lock(_mutex);
                if (_finished)
                    return;
                _finished = true;
                ServerWriteReactor<v1::Event>::Finish(status);
            }

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
                Stats& stats = sAcsdk.GetStats();
                stats.rpcsTotal.fetch_add(1, std::memory_order_relaxed);
                if (!HasValidServerToken(ctx, sAcsdk.GetConfig().authToken))
                {
                    stats.rpcsUnauthenticated.fetch_add(1, std::memory_order_relaxed);
                    return Reject(grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "missing or invalid server token"));
                }

                EventSubscriber::Filter filter;
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

                return new SubscribeReactor(std::move(filter));
            }

            grpc::ServerUnaryReactor* RespondToPrompt(grpc::CallbackServerContext* ctx, v1::RespondToPromptRequest const* request, v1::PromptResponse* response) override
            {
                grpc::ServerUnaryReactor* reactor = ctx->DefaultReactor();
                if (!Authorize(ctx, reactor))
                    return reactor;

                ObjectGuid guid = PlayerGuidFromApi(request->character_guid());
                std::string token = request->lease_token();
                uint64_t promptId = request->prompt_id();
                bool accept = request->accept();
                uint32_t vote = request->roll_vote();
                PostToWorld(reactor, [guid, token, promptId, accept, vote, response](WorldCallPtr call)
                {
                    if (!sAcsdk.CheckLease(guid, token))
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
