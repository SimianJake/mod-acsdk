#ifndef ACSDK_EVENT_BUS_H
#define ACSDK_EVENT_BUS_H

// gRPC/protobuf first (see rpc/RpcCommon.h for why).
#include "acsdk/v1/event.pb.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace acsdk
{
    using EventPtr = std::shared_ptr<v1::Event const>;

    // One subscriber (one Subscribe stream). Its queue is bounded: on overflow the oldest events are dropped
    // and the next delivery is preceded by Lagged{dropped=N} (PLAN.md §4.1 #4). Nothing here ever blocks the
    // world thread on a slow client.
    class EventSubscriber
    {
    public:
        struct Filter
        {
            std::unordered_set<uint64_t> characters;   // empty = all headless characters
            uint32_t typeMask = 0;                      // bit per v1::EventType; 0 = all except RAW_PACKET
            std::unordered_set<uint16_t> rawOpcodes;
        };

        EventSubscriber(Filter filter, std::size_t capacity) : _filter(std::move(filter)), _capacity(capacity) { }

        bool Matches(v1::Event const& event, v1::EventType type) const;
        Filter const& GetFilter() const { return _filter; }

        // World thread (dispatch). Returns true when the writer should be woken.
        bool Push(EventPtr const& event);

        // Writer side (any thread). Returns the next event to write, or nullptr when the queue is empty, in
        // which case the subscriber is marked idle and the next Push wakes it.
        EventPtr Next();

        uint64_t TotalDropped() const { return _totalDropped.load(std::memory_order_relaxed); }

    private:
        Filter _filter;
        std::size_t _capacity;
        std::mutex _mutex;
        std::deque<EventPtr> _queue;
        uint64_t _droppedSinceReport = 0;
        bool _writing = false;
        std::atomic<uint64_t> _totalDropped{0};
    };

    class EventBus
    {
    public:
        using Waker = std::function<void()>;

        EventBus();

        // Any thread. Cheap check sources make before building an event (zero cost when unused, §4.5).
        bool HasSubscribers() const { return _subscriberCount.load(std::memory_order_relaxed) > 0; }

        // Any thread (PacketSink, opcode-first). True if some subscriber needs this server opcode.
        bool WantsOpcode(uint16_t opcode) const;
        // True if some subscriber asked for this opcode as RAW_PACKET.
        bool WantsRaw(uint16_t opcode) const;

        // Any thread. Takes ownership; stamps sequence/time on dispatch.
        void Publish(uint64_t characterGuid, std::unique_ptr<v1::Event> event);

        // World thread, once per tick: move published events to matching subscribers.
        void Dispatch();

        // gRPC threads.
        std::shared_ptr<EventSubscriber> Subscribe(EventSubscriber::Filter filter, Waker waker);
        void Unsubscribe(EventSubscriber const* subscriber);

        void SetQueueCapacity(std::size_t capacity) { _capacity = capacity; }
        void SetEventOpcodes(std::vector<uint16_t> opcodes);

        uint64_t Published() const { return _published.load(std::memory_order_relaxed); }
        uint64_t Dropped() const;
        std::size_t SubscriberCount() const;

        static v1::EventType TypeOf(v1::Event const& event);

    private:
        static constexpr std::size_t MaxOpcode = 0x600;
        static constexpr std::size_t MaxIngress = 65536;

        struct Entry
        {
            std::shared_ptr<EventSubscriber> subscriber;
            Waker waker;
        };

        std::atomic<int> _subscriberCount{0};
        std::array<std::atomic<uint16_t>, MaxOpcode> _opcodeWants{};
        std::array<std::atomic<uint16_t>, MaxOpcode> _rawWants{};
        std::vector<uint16_t> _eventOpcodes;

        std::mutex _ingressMutex;
        std::vector<std::unique_ptr<v1::Event>> _ingress;
        std::atomic<uint64_t> _ingressDropped{0};

        mutable std::mutex _subscribersMutex;
        std::vector<Entry> _subscribers;
        uint64_t _droppedOfRemoved = 0;

        std::size_t _capacity = 4096;
        uint64_t _sequence = 0;
        std::atomic<uint64_t> _published{0};
        std::chrono::steady_clock::time_point _started = std::chrono::steady_clock::now();
    };
}

#endif
