// NPC, quest, vendor, loot, equipment and death packets (Packets.h). Layouts from AzerothCore:
// client reads in Handlers/{NPC,Quest,Item,Loot,Misc}Handler.cpp and Server/Packets/ItemPackets.cpp;
// server writes in PlayerMenu (GossipDef.cpp), WorldSession::SendListInventory, Player::SendLoot / LootView.

#include "Packets.h"

namespace acsdk::packets
{
    ByteBuffer Guid(uint64_t guid)
    {
        ByteBuffer b(8);
        b << uint64(guid);
        return b;
    }

    ByteBuffer GossipSelectOption(uint64_t npc, uint32_t menuId, uint32_t option, std::string const* code)
    {
        ByteBuffer b(16);
        b << uint64(npc) << uint32(menuId) << uint32(option);
        if (code)
            b << *code;
        return b;
    }

    ByteBuffer QuestgiverAcceptQuest(uint64_t npc, uint32_t questId)
    {
        ByteBuffer b(16);
        b << uint64(npc) << uint32(questId) << uint32(1);
        return b;
    }

    ByteBuffer QuestgiverQuest(uint64_t npc, uint32_t questId)
    {
        ByteBuffer b(12);
        b << uint64(npc) << uint32(questId);
        return b;
    }

    ByteBuffer QuestgiverChooseReward(uint64_t npc, uint32_t questId, uint32_t reward)
    {
        ByteBuffer b(16);
        b << uint64(npc) << uint32(questId) << uint32(reward);
        return b;
    }

    ByteBuffer QuestLogRemoveQuest(uint8_t slot)
    {
        ByteBuffer b(1);
        b << uint8(slot);
        return b;
    }

    ByteBuffer BuyItem(uint64_t vendor, uint32_t itemId, uint32_t vendorSlot, uint32_t count)
    {
        ByteBuffer b(21);
        b << uint64(vendor) << uint32(itemId) << uint32(vendorSlot) << uint32(count) << uint8(0);
        return b;
    }

    ByteBuffer SellItem(uint64_t vendor, uint64_t item, uint32_t count)
    {
        ByteBuffer b(20);
        b << uint64(vendor) << uint64(item) << uint32(count);
        return b;
    }

    ByteBuffer AutoEquipItem(uint8_t bag, uint8_t slot)
    {
        ByteBuffer b(2);
        b << uint8(bag) << uint8(slot);
        return b;
    }

    ByteBuffer AutostoreLootItem(uint8_t lootSlot)
    {
        ByteBuffer b(1);
        b << uint8(lootSlot);
        return b;
    }

    ByteBuffer RepopRequest()
    {
        ByteBuffer b(1);
        b << uint8(0);
        return b;
    }

    namespace
    {
        QuestOffer ReadQuestOffer(ByteBuffer& buf)
        {
            QuestOffer q;
            q.questId = buf.read<uint32>();
            q.icon = buf.read<uint32>();
            q.level = buf.read<int32>();
            buf.read_skip<uint32>(); // flags
            buf.read_skip<uint8>();  // repeatable
            q.title = buf.ReadCString(false);
            return q;
        }
    }

    std::optional<GossipMenu> DecodeGossipMessage(ByteBuffer buf)
    {
        try
        {
            GossipMenu m;
            m.npc = buf.read<uint64>();
            m.menuId = buf.read<uint32>();
            buf.read_skip<uint32>(); // title text id
            uint32 options = buf.read<uint32>();
            for (uint32 i = 0; i < options && i < 64; ++i)
            {
                GossipOption o;
                o.index = buf.read<uint32>();
                o.icon = buf.read<uint8>();
                o.coded = buf.read<uint8>() != 0;
                o.boxMoney = buf.read<uint32>();
                o.text = buf.ReadCString(false);
                buf.ReadCString(false); // box text
                m.options.push_back(std::move(o));
            }
            uint32 quests = buf.read<uint32>();
            for (uint32 i = 0; i < quests && i < 64; ++i)
                m.quests.push_back(ReadQuestOffer(buf));
            return m;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<GossipMenu> DecodeQuestgiverQuestList(ByteBuffer buf)
    {
        try
        {
            GossipMenu m;
            m.npc = buf.read<uint64>();
            buf.ReadCString(false);       // greeting
            buf.read_skip<uint32>(); // emote delay
            buf.read_skip<uint32>(); // emote
            uint8 count = buf.read<uint8>();
            for (uint8 i = 0; i < count; ++i)
                m.quests.push_back(ReadQuestOffer(buf));
            return m;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<QuestDetails> DecodeQuestgiverQuestDetails(ByteBuffer buf)
    {
        try
        {
            QuestDetails d;
            d.npc = buf.read<uint64>();
            buf.read_skip<uint64>(); // divider / sharer
            d.questId = buf.read<uint32>();
            d.title = buf.ReadCString(false);
            return d;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<QuestDetails> DecodeQuestgiverGuidQuest(ByteBuffer buf)
    {
        try
        {
            QuestDetails d;
            d.npc = buf.read<uint64>();
            d.questId = buf.read<uint32>();
            d.title = buf.ReadCString(false);
            return d;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<VendorList> DecodeListInventory(ByteBuffer buf)
    {
        try
        {
            VendorList v;
            v.vendor = buf.read<uint64>();
            uint8 count = buf.read<uint8>();
            for (uint8 i = 0; i < count; ++i)
            {
                VendorItem it;
                it.slot = buf.read<uint32>();
                it.itemId = buf.read<uint32>();
                buf.read_skip<uint32>(); // display id
                it.available = buf.read<int32>();
                it.price = buf.read<uint32>();
                buf.read_skip<uint32>(); // max durability
                it.buyCount = buf.read<uint32>();
                it.extendedCost = buf.read<uint32>();
                v.items.push_back(it);
            }
            return v; // count 0 is followed by an error byte ("no inventory"); an empty list says the same
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<LootWindow> DecodeLootWindow(ByteBuffer buf)
    {
        try
        {
            LootWindow w;
            w.lootGuid = buf.read<uint64>();
            w.lootType = buf.read<uint8>();
            if (w.lootType == 0) // LOOT_NONE: Player::SendLootError
            {
                w.error = buf.read<uint8>();
                return w;
            }
            w.gold = buf.read<uint32>();
            uint8 count = buf.read<uint8>();
            for (uint8 i = 0; i < count; ++i)
            {
                LootSlot s;
                s.slot = buf.read<uint8>();
                s.itemId = buf.read<uint32>();
                s.count = buf.read<uint32>();
                buf.read_skip<uint32>(); // display id
                buf.read_skip<uint32>(); // random suffix
                buf.read_skip<uint32>(); // random property id
                s.slotType = buf.read<uint8>();
                w.items.push_back(s);
            }
            return w;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<uint8_t> DecodeLastByte(ByteBuffer buf)
    {
        if (!buf.size())
            return std::nullopt;
        return buf.contents()[buf.size() - 1];
    }

    std::optional<uint8_t> DecodeFirstByte(ByteBuffer buf)
    {
        if (!buf.size())
            return std::nullopt;
        return buf.contents()[0];
    }
}
