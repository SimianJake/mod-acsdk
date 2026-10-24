#ifndef ACSDK_PACKET_EVENTS_H
#define ACSDK_PACKET_EVENTS_H

#include "EventBus.h"
#include "HeadlessRuntime.h"
#include "ObjectGuid.h"

#include <functional>
#include <vector>

namespace acsdk
{
    // Server opcodes that are the official source of an event type (docs/events.md). Only copied by the
    // PacketSink while someone is subscribed.
    std::vector<uint16_t> EventSourceOpcodes();

    // Translates one captured packet of `character` into its typed event (if it is an event source) and/or a
    // RAW_PACKET copy (if a subscriber asked for that opcode). World thread.
    // `commandForCast` maps an echoed cast count to the CastSpell command it belongs to (0 if none).
    void PublishPacketEvents(EventBus& bus, ObjectGuid character, CapturedPacket const& packet,
                             std::function<uint64_t(uint8_t castCount)> const& commandForCast);
}

#endif
