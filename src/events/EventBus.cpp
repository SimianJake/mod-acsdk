#include "EventBus.h"

#include <algorithm>

namespace acsdk
{
    namespace
    {
        constexpr uint32_t Bit(v1::EventType t) { return 1u << static_cast<uint32_t>(t); }
    }

    // ---------------------------------------------------------------- EventSubscriber

    bool EventSubscriber::Matches(v1::Event const& event, v1::EventType type) const
    {
        if (type == v1::LAGGED)
            return true;

        if (!_filter.characters.empty() && !_filter.characters.contains(event.character_guid()))
            return false;

        if (type == v1::RAW_PACKET)
            return _filter.rawOpcodes.contains(static_cast<uint16_t>(event.raw_packet().opcode()));

        return _filter.typeMask == 0 || (_filter.typeMask & Bit(type));
    }

    bool EventSubscriber::Push(EventPtr const& event)
    {
        std::lock_guard lock(_mutex);
        if (_queue.size() >= _capacity)
        {
            _queue.pop_front(); // drop oldest
            ++_droppedSinceReport;
            _totalDropped.fetch_add(1, std::memory_order_relaxed);
        }
        _queue.push_back(event);

        if (_writing)
            return false;
        _writing = true;
        return true;
    }

    EventPtr EventSubscriber::Next()
    {
        std::lock_guard lock(_mutex);
        if (_droppedSinceReport)
        {
            auto lagged = std::make_shared<v1::Event>();
            lagged->mutable_lagged()->set_dropped(_droppedSinceReport);
            _droppedSinceReport = 0;
            return lagged;
        }

        if (_queue.empty())
        {
            _writing = false;
            return nullptr;
        }

        EventPtr next = std::move(_queue.front());
        _queue.pop_front();
        return next;
    }

    // ---------------------------------------------------------------- EventBus

    EventBus::EventBus()
    {
        for (auto& w : _opcodeWants)
            w.store(0, std::memory_order_relaxed);
        for (auto& w : _rawWants)
            w.store(0, std::memory_order_relaxed);
    }

    v1::EventType EventBus::TypeOf(v1::Event const& event)
    {
        switch (event.payload_case())
        {
            case v1::Event::kChat:                  return v1::CHAT;
            case v1::Event::kDamageDealt:           return v1::DAMAGE_DEALT;
            case v1::Event::kDamageTaken:           return v1::DAMAGE_TAKEN;
            case v1::Event::kDeath:                 return v1::DEATH;
            case v1::Event::kAuraApplied:           return v1::AURA_APPLIED;
            case v1::Event::kAuraRemoved:           return v1::AURA_REMOVED;
            case v1::Event::kSpellCastStart:        return v1::SPELL_CAST_START;
            case v1::Event::kSpellCastFinish:       return v1::SPELL_CAST_FINISH;
            case v1::Event::kLevelUp:               return v1::LEVEL_UP;
            case v1::Event::kLoot:                  return v1::LOOT;
            case v1::Event::kPrompt:                return v1::PROMPT;
            case v1::Event::kLeaseLost:             return v1::LEASE_LOST;
            case v1::Event::kCharacterStateChanged: return v1::CHARACTER_STATE_CHANGED;
            case v1::Event::kCommandResult:         return v1::COMMAND_RESULT;
            case v1::Event::kRawPacket:             return v1::RAW_PACKET;
            case v1::Event::kLagged:                return v1::LAGGED;
            default:                                return v1::EVENT_TYPE_UNSPECIFIED;
        }
    }

    bool EventBus::WantsOpcode(uint16_t opcode) const
    {
        return opcode < MaxOpcode && _opcodeWants[opcode].load(std::memory_order_relaxed) > 0;
    }

    bool EventBus::WantsRaw(uint16_t opcode) const
    {
        return opcode < MaxOpcode && _rawWants[opcode].load(std::memory_order_relaxed) > 0;
    }

    void EventBus::SetEventOpcodes(std::vector<uint16_t> opcodes)
    {
        _eventOpcodes = std::move(opcodes);
    }

    void EventBus::Publish(uint64_t characterGuid, std::unique_ptr<v1::Event> event)
    {
        if (!HasSubscribers())
            return;

        event->set_character_guid(characterGuid);
        std::lock_guard lock(_ingressMutex);
        if (_ingress.size() >= MaxIngress)
        {
            _ingressDropped.fetch_add(1, std::memory_order_relaxed); // bounded even if dispatch stalls
            return;
        }
        _ingress.push_back(std::move(event));
    }

    void EventBus::Dispatch()
    {
        std::vector<std::unique_ptr<v1::Event>> batch;
        {
            std::lock_guard lock(_ingressMutex);
            batch.swap(_ingress);
        }
        if (batch.empty())
            return;

        auto nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _started).count());

        std::lock_guard lock(_subscribersMutex);
        {
            for (auto& event : batch)
            {
                event->set_sequence(++_sequence);
                event->set_server_time_ms(nowMs);
                v1::EventType type = TypeOf(*event);

                // Serialized once per write, but the object itself is shared by every matching subscriber.
                EventPtr shared(std::move(event));
                for (Entry& e : _subscribers)
                    if (e.subscriber->Matches(*shared, type) && e.subscriber->Push(shared))
                        e.waker(); // under the lock: Unsubscribe (stream teardown) cannot race a wake
                _published.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::shared_ptr<EventSubscriber> EventBus::Subscribe(EventSubscriber::Filter filter, Waker waker)
    {
        auto subscriber = std::make_shared<EventSubscriber>(std::move(filter), _capacity);
        {
            std::lock_guard lock(_subscribersMutex);
            _subscribers.push_back({ subscriber, std::move(waker) });
        }

        // Opcode-first capture: only now do the event and raw opcodes start being copied.
        for (uint16_t op : _eventOpcodes)
            if (op < MaxOpcode)
                _opcodeWants[op].fetch_add(1, std::memory_order_relaxed);
        for (uint16_t op : subscriber->GetFilter().rawOpcodes)
            if (op < MaxOpcode)
            {
                _opcodeWants[op].fetch_add(1, std::memory_order_relaxed);
                _rawWants[op].fetch_add(1, std::memory_order_relaxed);
            }

        _subscriberCount.fetch_add(1, std::memory_order_relaxed);
        return subscriber;
    }

    void EventBus::Unsubscribe(EventSubscriber const* subscriber)
    {
        std::shared_ptr<EventSubscriber> removed;
        {
            std::lock_guard lock(_subscribersMutex);
            auto it = std::find_if(_subscribers.begin(), _subscribers.end(), [&](Entry const& e) { return e.subscriber.get() == subscriber; });
            if (it == _subscribers.end())
                return;
            removed = it->subscriber;
            _droppedOfRemoved += removed->TotalDropped();
            _subscribers.erase(it);
        }

        for (uint16_t op : _eventOpcodes)
            if (op < MaxOpcode)
                _opcodeWants[op].fetch_sub(1, std::memory_order_relaxed);
        for (uint16_t op : removed->GetFilter().rawOpcodes)
            if (op < MaxOpcode)
            {
                _opcodeWants[op].fetch_sub(1, std::memory_order_relaxed);
                _rawWants[op].fetch_sub(1, std::memory_order_relaxed);
            }

        _subscriberCount.fetch_sub(1, std::memory_order_relaxed);
    }

    std::size_t EventBus::SubscriberCount() const
    {
        return static_cast<std::size_t>(std::max(0, _subscriberCount.load(std::memory_order_relaxed)));
    }

    uint64_t EventBus::Dropped() const
    {
        std::lock_guard lock(_subscribersMutex);
        uint64_t total = _droppedOfRemoved + _ingressDropped.load(std::memory_order_relaxed);
        for (Entry const& e : _subscribers)
            total += e.subscriber->TotalDropped();
        return total;
    }
}
