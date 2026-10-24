// Decoders for event and prompt packets (see Packets.h). Kept separate from Packets.cpp for size only.

#include "Packets.h"

namespace acsdk::packets
{
    namespace
    {
        uint64_t ReadPacked(ByteBuffer& buf)
        {
            uint64_t guid = 0;
            buf.readPackGUID(guid);
            return guid;
        }

        // Unit.h HitInfo / SpellHitType bits (game library), duplicated to keep this file game-free.
        constexpr uint32_t HitFullAbsorb = 0x20, HitPartialAbsorb = 0x40, HitFullResist = 0x80, HitPartialResist = 0x100;
        constexpr uint32_t HitCritical = 0x200, HitBlock = 0x2000;
        constexpr uint32_t SpellHitCrit = 0x2;
    }

    std::optional<uint32_t> DecodeSpellStartTimer(ByteBuffer buf)
    {
        try
        {
            ReadPacked(buf);           // cast item or caster
            ReadPacked(buf);           // caster
            buf.read_skip<uint8>();    // cast count
            buf.read_skip<uint32>();   // spell id
            buf.read_skip<uint32>();   // cast flags
            return buf.read<uint32>(); // timer
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<Damage> DecodeAttackerStateUpdate(ByteBuffer buf)
    {
        try
        {
            Damage d;
            uint32_t hitInfo = buf.read<uint32>();
            d.attacker = ReadPacked(buf);
            d.victim = ReadPacked(buf);
            d.amount = buf.read<uint32>();
            d.overkill = buf.read<uint32>();
            uint8_t count = buf.read<uint8>();
            for (uint8_t i = 0; i < count; ++i)
            {
                d.schoolMask |= buf.read<uint32>();
                buf.read_skip<float>();
                buf.read_skip<uint32>();
            }
            if (hitInfo & (HitFullAbsorb | HitPartialAbsorb))
                for (uint8_t i = 0; i < count; ++i)
                    d.absorbed += buf.read<uint32>();
            if (hitInfo & (HitFullResist | HitPartialResist))
                for (uint8_t i = 0; i < count; ++i)
                    d.resisted += buf.read<uint32>();
            buf.read_skip<uint8>();   // target state
            buf.read_skip<uint32>();
            buf.read_skip<uint32>();  // melee spell id (always 0 in AC)
            if (hitInfo & HitBlock)
                d.blocked = buf.read<uint32>();
            d.critical = (hitInfo & HitCritical) != 0;
            return d;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<Damage> DecodeSpellNonMeleeDamageLog(ByteBuffer buf)
    {
        try
        {
            Damage d;
            d.victim = ReadPacked(buf);
            d.attacker = ReadPacked(buf);
            d.spellId = buf.read<uint32>();
            d.amount = buf.read<uint32>();
            d.overkill = buf.read<uint32>();
            d.schoolMask = buf.read<uint8>();
            d.absorbed = buf.read<uint32>();
            d.resisted = buf.read<uint32>();
            buf.read_skip<uint8>();   // physical log
            buf.read_skip<uint8>();   // unused
            d.blocked = buf.read<uint32>();
            d.critical = (buf.read<uint32>() & SpellHitCrit) != 0;
            return d;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<LootResponse> DecodeLootResponse(ByteBuffer buf)
    {
        try
        {
            LootResponse r{};
            r.lootGuid = buf.read<uint64>();
            r.lootType = buf.read<uint8>();
            r.error = r.lootType == 0 ? buf.read<uint8>() : 0; // LOOT_NONE carries a LootError
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<ItemPush> DecodeItemPushResult(ByteBuffer buf)
    {
        try
        {
            ItemPush r{};
            r.player = buf.read<uint64>();
            buf.read_skip<uint32>(); // received
            buf.read_skip<uint32>(); // created
            buf.read_skip<uint32>(); // show in chat
            buf.read_skip<uint8>();  // bag
            buf.read_skip<uint32>(); // slot
            r.itemId = buf.read<uint32>();
            buf.read_skip<uint32>(); // suffix factor
            buf.read_skip<int32>();  // random property
            r.count = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<uint32_t> DecodeLootMoneyNotify(ByteBuffer buf)
    {
        try { return buf.read<uint32>(); }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<std::string> DecodeGroupInvite(ByteBuffer buf)
    {
        try
        {
            buf.read_skip<uint8>(); // 1 = can accept (0 when already in a group)
            return buf.ReadCString(false);
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<GuidAndName> DecodeResurrectRequest(ByteBuffer buf)
    {
        try
        {
            GuidAndName r;
            r.guid = buf.read<uint64>();
            buf.read_skip<uint32>(); // name length
            r.name = buf.ReadCString(false);
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<GuidAndName> DecodeSummonRequest(ByteBuffer buf)
    {
        try
        {
            GuidAndName r;
            r.guid = buf.read<uint64>();
            r.value = buf.read<uint32>(); // zone id
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<DuelRequested> DecodeDuelRequested(ByteBuffer buf)
    {
        try
        {
            DuelRequested r{};
            r.arbiter = buf.read<uint64>();
            r.initiator = buf.read<uint64>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<TradeStatus> DecodeTradeStatus(ByteBuffer buf)
    {
        try
        {
            TradeStatus r{};
            r.status = buf.read<uint32>();
            if (r.status == 1) // TRADE_STATUS_BEGIN_TRADE
                r.trader = buf.read<uint64>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<uint64_t> DecodeGuid(ByteBuffer buf)
    {
        try { return buf.read<uint64>(); }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<LootRoll> DecodeLootStartRoll(ByteBuffer buf)
    {
        try
        {
            LootRoll r{};
            r.lootGuid = buf.read<uint64>();
            buf.read_skip<uint32>(); // map id
            r.slot = buf.read<uint32>();
            r.itemId = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<LootRoll> DecodeLootRollEnd(ByteBuffer buf)
    {
        try
        {
            LootRoll r{};
            r.lootGuid = buf.read<uint64>();
            r.slot = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }
}
