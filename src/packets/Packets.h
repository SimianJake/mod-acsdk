#ifndef ACSDK_PACKETS_H
#define ACSDK_PACKETS_H

// Client packet builders and server packet decoders (PLAN.md §4.6). Layouts come from the AzerothCore
// handler read code and Send* write code cited in docs/research/opcode-map.md. Only ByteBuffer is used
// (no game types), so tests/ can check exact bytes.

#include "ByteBuffer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acsdk::packets
{
    // ---- client -> server bodies (opcode is set by the caller) ------------------------------------

    // CMSG_SET_SELECTION: uint64 guid (full). 0 clears.
    ByteBuffer SetSelection(uint64_t targetGuid);

    // CMSG_ATTACKSWING: uint64 victim guid (full).
    ByteBuffer AttackSwing(uint64_t victimGuid);

    // CMSG_ATTACKSTOP: empty.
    ByteBuffer AttackStop();

    struct CastTarget
    {
        uint64_t unitGuid = 0;                 // TARGET_FLAG_UNIT when set
        std::optional<float> destX, destY, destZ; // TARGET_FLAG_DEST_LOCATION when all set
    };

    // CMSG_CAST_SPELL: uint8 castCount, uint32 spellId, uint8 castFlags(0), SpellCastTargets.
    // SpellCastTargets::Write: uint32 mask, [packed unit guid], [packed transport guid(0) + float x,y,z].
    ByteBuffer CastSpell(uint8_t castCount, uint32_t spellId, CastTarget const& target);

    // CMSG_CANCEL_CAST: uint8 counter (ignored by AC), uint32 spellId (0 = any).
    ByteBuffer CancelCast(uint32_t spellId);

    // CMSG_CANCEL_CHANNELLING: uint32 spellId (must match the current channel).
    ByteBuffer CancelChannel(uint32_t spellId);

    // CMSG_MESSAGECHAT: uint32 type, uint32 language, [cstring to for WHISPER], cstring message.
    ByteBuffer MessageChat(uint32_t chatType, uint32_t language, std::string const& message, std::string const& whisperTo = "");

    // ---- server -> client decoders. Return nullopt on malformed input; never throw. ---------------

    struct CastFailed        { uint8_t castCount; uint32_t spellId; uint8_t result; };
    struct SpellStartOrGo    { uint64_t castItemOrCaster; uint64_t caster; uint8_t castCount; uint32_t spellId; uint32_t castFlags; };
    struct SpellFailure      { uint64_t caster; uint8_t castCount; uint32_t spellId; uint8_t result; };
    struct ChannelStart      { uint64_t caster; uint32_t spellId; uint32_t durationMs; };
    struct ChannelUpdate     { uint64_t caster; uint32_t remainingMs; };
    struct AttackStart       { uint64_t attacker; uint64_t victim; };
    struct AttackStopped     { uint64_t attacker; uint64_t victim; uint32_t nowDead; };

    struct Chat
    {
        uint8_t type = 0;
        int32_t language = 0;
        uint64_t sender = 0;
        uint64_t receiver = 0;
        std::string senderName;   // NPC/monster and some system types
        std::string channel;      // CHAT_MSG_CHANNEL
        std::string text;
    };

    std::optional<CastFailed>     DecodeCastFailed(ByteBuffer buf);      // SMSG_CAST_FAILED
    std::optional<SpellStartOrGo> DecodeSpellStartOrGo(ByteBuffer buf);  // SMSG_SPELL_START / SMSG_SPELL_GO
    std::optional<SpellFailure>   DecodeSpellFailure(ByteBuffer buf);    // SMSG_SPELL_FAILURE / SMSG_SPELL_FAILED_OTHER
    std::optional<ChannelStart>   DecodeChannelStart(ByteBuffer buf);    // MSG_CHANNEL_START
    std::optional<ChannelUpdate>  DecodeChannelUpdate(ByteBuffer buf);   // MSG_CHANNEL_UPDATE
    std::optional<AttackStart>    DecodeAttackStart(ByteBuffer buf);     // SMSG_ATTACKSTART
    std::optional<AttackStopped>  DecodeAttackStop(ByteBuffer buf);      // SMSG_ATTACKSTOP
    std::optional<Chat>           DecodeChat(ByteBuffer buf);            // SMSG_MESSAGECHAT / SMSG_GM_MESSAGECHAT
    std::optional<std::string>    DecodeChatPlayerNotFound(ByteBuffer buf); // SMSG_CHAT_PLAYER_NOT_FOUND

    // SMSG_SPELL_START int32 timer (cast time) after the cast flags.
    std::optional<uint32_t> DecodeSpellStartTimer(ByteBuffer buf);

    // ---- combat log (Unit::SendAttackStateUpdate / Unit::SendSpellNonMeleeDamageLog) ----
    struct Damage
    {
        uint64_t attacker = 0, victim = 0;
        uint32_t amount = 0, overkill = 0, schoolMask = 0, spellId = 0;
        uint32_t absorbed = 0, resisted = 0, blocked = 0;
        bool critical = false;
    };
    std::optional<Damage> DecodeAttackerStateUpdate(ByteBuffer buf);    // SMSG_ATTACKERSTATEUPDATE
    std::optional<Damage> DecodeSpellNonMeleeDamageLog(ByteBuffer buf); // SMSG_SPELLNONMELEEDAMAGELOG

    // ---- loot ----
    struct LootResponse { uint64_t lootGuid; uint8_t lootType; uint8_t error; };
    struct ItemPush     { uint64_t player; uint32_t itemId; uint32_t count; };
    std::optional<LootResponse> DecodeLootResponse(ByteBuffer buf);     // SMSG_LOOT_RESPONSE
    std::optional<ItemPush>     DecodeItemPushResult(ByteBuffer buf);   // SMSG_ITEM_PUSH_RESULT
    std::optional<uint32_t>     DecodeLootMoneyNotify(ByteBuffer buf);  // SMSG_LOOT_MONEY_NOTIFY

    // ---- prompts (docs/research/opcode-map.md §2.8) ----
    struct GuidAndName { uint64_t guid = 0; std::string name; uint32_t value = 0; };
    std::optional<std::string> DecodeGroupInvite(ByteBuffer buf);       // SMSG_GROUP_INVITE: inviter name
    std::optional<GuidAndName> DecodeResurrectRequest(ByteBuffer buf);  // resurrector guid, name
    std::optional<GuidAndName> DecodeSummonRequest(ByteBuffer buf);     // summoner guid, value = zone id
    struct DuelRequested { uint64_t arbiter; uint64_t initiator; };
    std::optional<DuelRequested> DecodeDuelRequested(ByteBuffer buf);
    struct TradeStatus { uint32_t status; uint64_t trader; };
    std::optional<TradeStatus> DecodeTradeStatus(ByteBuffer buf);
    std::optional<uint64_t> DecodeGuid(ByteBuffer buf);                 // MSG_RAID_READY_CHECK (initiator)
    struct LootRoll { uint64_t lootGuid; uint32_t slot; uint32_t itemId; };
    std::optional<LootRoll> DecodeLootStartRoll(ByteBuffer buf);        // SMSG_LOOT_START_ROLL
    std::optional<LootRoll> DecodeLootRollEnd(ByteBuffer buf);          // SMSG_LOOT_ROLL_WON / SMSG_LOOT_ALL_PASSED

    // ---- NPCs, quests, vendors, loot, equipment, death (InteractionPackets.cpp) ----------------------
    // Client bodies. Layouts: AC handlers in Handlers/{NPC,Quest,Item,Loot,Misc}Handler.cpp.
    ByteBuffer Guid(uint64_t guid);                                       // CMSG_GOSSIP_HELLO, CMSG_QUESTGIVER_HELLO,
                                                                          // CMSG_LIST_INVENTORY, CMSG_LOOT, CMSG_LOOT_RELEASE,
                                                                          // CMSG_RECLAIM_CORPSE, CMSG_SPIRIT_HEALER_ACTIVATE
    ByteBuffer GossipSelectOption(uint64_t npc, uint32_t menuId, uint32_t option, std::string const* code);
    ByteBuffer QuestgiverAcceptQuest(uint64_t npc, uint32_t questId);    // guid, quest, uint32 unk (1)
    ByteBuffer QuestgiverQuest(uint64_t npc, uint32_t questId);          // COMPLETE_QUEST / REQUEST_REWARD: guid, quest
    ByteBuffer QuestgiverChooseReward(uint64_t npc, uint32_t questId, uint32_t reward);
    ByteBuffer QuestLogRemoveQuest(uint8_t slot);
    ByteBuffer BuyItem(uint64_t vendor, uint32_t itemId, uint32_t vendorSlot, uint32_t count); // slot counts from 1
    ByteBuffer SellItem(uint64_t vendor, uint64_t item, uint32_t count);
    ByteBuffer AutoEquipItem(uint8_t bag, uint8_t slot);
    ByteBuffer AutostoreLootItem(uint8_t lootSlot);
    ByteBuffer RepopRequest();                                            // uint8 0

    struct GossipOption { uint32_t index = 0; uint8_t icon = 0; bool coded = false; uint32_t boxMoney = 0; std::string text; };
    struct QuestOffer   { uint32_t questId = 0; uint32_t icon = 0; int32_t level = 0; std::string title; };
    struct GossipMenu   { uint64_t npc = 0; uint32_t menuId = 0; std::vector<GossipOption> options; std::vector<QuestOffer> quests; };
    struct VendorItem   { uint32_t slot = 0; uint32_t itemId = 0; int32_t available = -1; uint32_t price = 0; uint32_t buyCount = 1; uint32_t extendedCost = 0; };
    struct VendorList   { uint64_t vendor = 0; std::vector<VendorItem> items; };
    struct LootSlot     { uint8_t slot = 0; uint32_t itemId = 0; uint32_t count = 0; uint8_t slotType = 0; };
    struct LootWindow   { uint64_t lootGuid = 0; uint8_t lootType = 0; uint8_t error = 0; uint32_t gold = 0; std::vector<LootSlot> items; };
    struct QuestDetails { uint64_t npc = 0; uint32_t questId = 0; std::string title; };

    std::optional<GossipMenu>   DecodeGossipMessage(ByteBuffer buf);     // SMSG_GOSSIP_MESSAGE
    std::optional<GossipMenu>   DecodeQuestgiverQuestList(ByteBuffer buf); // SMSG_QUESTGIVER_QUEST_LIST (quests only)
    std::optional<QuestDetails> DecodeQuestgiverQuestDetails(ByteBuffer buf); // SMSG_QUESTGIVER_QUEST_DETAILS
    std::optional<QuestDetails> DecodeQuestgiverGuidQuest(ByteBuffer buf); // SMSG_QUESTGIVER_REQUEST_ITEMS / OFFER_REWARD
    std::optional<VendorList>   DecodeListInventory(ByteBuffer buf);     // SMSG_LIST_INVENTORY
    std::optional<LootWindow>   DecodeLootWindow(ByteBuffer buf);        // SMSG_LOOT_RESPONSE incl. items
    // The reason byte that ends SMSG_BUY_FAILED / SMSG_SELL_ITEM, or the first byte of SMSG_INVENTORY_CHANGE_FAILURE.
    std::optional<uint8_t>      DecodeLastByte(ByteBuffer buf);
    std::optional<uint8_t>      DecodeFirstByte(ByteBuffer buf);
}


#endif
