// Golden-byte tests for mod-acsdk's packet builders and decoders (PLAN.md Phase 3).
// Expected bytes are derived by hand from the AzerothCore read/write code cited in
// docs/research/opcode-map.md. Built as acsdk-packet-tests (mod-acsdk.cmake, -DACSDK_BUILD_TESTS=ON).

#include "Packets.h"
#include "SharedDefines.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace acsdk::packets;

static int failures = 0;

static std::vector<uint8_t> Bytes(ByteBuffer const& b)
{
    return b.size() ? std::vector<uint8_t>(b.contents(), b.contents() + b.size()) : std::vector<uint8_t>{};
}

static void ExpectBytes(char const* name, ByteBuffer const& actual, std::initializer_list<uint8_t> expected)
{
    std::vector<uint8_t> a = Bytes(actual);
    std::vector<uint8_t> e(expected);
    if (a == e)
        return;

    ++failures;
    std::printf("FAIL %s\n  expected:", name);
    for (uint8_t v : e) std::printf(" %02x", v);
    std::printf("\n  actual:  ");
    for (uint8_t v : a) std::printf(" %02x", v);
    std::printf("\n");
}

static void Expect(char const* name, bool ok)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

static ByteBuffer From(std::initializer_list<uint8_t> bytes)
{
    ByteBuffer b;
    for (uint8_t v : bytes) b << v;
    return b;
}

int main()
{
    // CMSG_SET_SELECTION: uint64 guid, little endian.
    ExpectBytes("SetSelection", SetSelection(0xF130000000001234ull), { 0x34, 0x12, 0, 0, 0, 0, 0x30, 0xF1 });

    // CMSG_ATTACKSWING: uint64 victim guid. CMSG_ATTACKSTOP: empty.
    ExpectBytes("AttackSwing", AttackSwing(0x0102030405060708ull), { 8, 7, 6, 5, 4, 3, 2, 1 });
    ExpectBytes("AttackStop", AttackStop(), {});

    // CMSG_CAST_SPELL, self cast: castCount, spellId (133 = 0x85), castFlags 0, targetMask 0.
    ExpectBytes("CastSpell/self", CastSpell(7, 133, {}), { 7, 0x85, 0, 0, 0, 0, 0, 0, 0, 0 });

    // Unit target: mask 0x2 then packed guid. Packed guid of 0xF130000000001234:
    // mask byte has bits for non-zero bytes 0,1,6,7 -> 0b11000011 = 0xC3, then 0x34 0x12 0x30 0xF1.
    {
        CastTarget t;
        t.unitGuid = 0xF130000000001234ull;
        ExpectBytes("CastSpell/unit", CastSpell(1, 133, t), { 1, 0x85, 0, 0, 0, 0, 2, 0, 0, 0, 0xC3, 0x34, 0x12, 0x30, 0xF1 });
    }

    // Dest target: mask 0x40, packed transport guid 0 (single 0x00 mask byte), floats x,y,z (1.0f = 00 00 80 3f).
    {
        CastTarget t;
        t.destX = 1.0f;
        t.destY = 1.0f;
        t.destZ = 1.0f;
        ExpectBytes("CastSpell/dest", CastSpell(2, 1, t),
            { 2, 1, 0, 0, 0, 0, 0x40, 0, 0, 0, 0x00, 0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3f });
    }

    // CMSG_CANCEL_CAST: uint8 counter, uint32 spellId. CMSG_CANCEL_CHANNELLING: uint32 spellId.
    ExpectBytes("CancelCast", CancelCast(133), { 0, 0x85, 0, 0, 0 });
    ExpectBytes("CancelChannel", CancelChannel(5143), { 0x17, 0x14, 0, 0 });

    // CMSG_MESSAGECHAT SAY: type 1, language 7 (Common), "hi\0".
    ExpectBytes("MessageChat/say", MessageChat(CHAT_MSG_SAY, LANG_COMMON, "hi"), { 1, 0, 0, 0, 7, 0, 0, 0, 'h', 'i', 0 });
    // WHISPER: type 7, language, "Bob\0", "yo\0".
    ExpectBytes("MessageChat/whisper", MessageChat(CHAT_MSG_WHISPER, LANG_COMMON, "yo", "Bob"),
        { 7, 0, 0, 0, 7, 0, 0, 0, 'B', 'o', 'b', 0, 'y', 'o', 0 });

    // SMSG_CAST_FAILED: castCount, spellId, result.
    {
        auto r = DecodeCastFailed(From({ 9, 0x85, 0, 0, 0, 85 }));
        Expect("DecodeCastFailed", r && r->castCount == 9 && r->spellId == 133 && r->result == 85);
        Expect("DecodeCastFailed/short", !DecodeCastFailed(From({ 9, 0x85 })));
    }

    // SMSG_SPELL_GO: packed castItemOrCaster, packed caster, castCount, spellId, castFlags.
    {
        auto r = DecodeSpellStartOrGo(From({ 0x01, 0x05, 0x01, 0x05, 3, 0x85, 0, 0, 0, 0x00, 0x01, 0, 0 }));
        Expect("DecodeSpellGo", r && r->caster == 5 && r->castItemOrCaster == 5 && r->castCount == 3 && r->spellId == 133 && r->castFlags == 0x100);
    }

    // SMSG_SPELL_FAILURE: packed caster, castCount, spellId, result.
    {
        auto r = DecodeSpellFailure(From({ 0x01, 0x05, 4, 0x85, 0, 0, 0, 40 }));
        Expect("DecodeSpellFailure", r && r->caster == 5 && r->castCount == 4 && r->spellId == 133 && r->result == 40);
    }

    // MSG_CHANNEL_UPDATE: packed caster, uint32 remaining.
    {
        auto r = DecodeChannelUpdate(From({ 0x01, 0x05, 0, 0, 0, 0 }));
        Expect("DecodeChannelUpdate", r && r->caster == 5 && r->remainingMs == 0);
    }

    // SMSG_ATTACKSTART: uint64 attacker, uint64 victim. SMSG_ATTACKSTOP: packed, packed, uint32 nowDead.
    {
        auto s = DecodeAttackStart(From({ 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0 }));
        Expect("DecodeAttackStart", s && s->attacker == 1 && s->victim == 2);
        auto t = DecodeAttackStop(From({ 0x01, 0x01, 0x01, 0x02, 1, 0, 0, 0 }));
        Expect("DecodeAttackStop", t && t->attacker == 1 && t->victim == 2 && t->nowDead == 1);
    }

    // SMSG_MESSAGECHAT SAY: type, lang, sender, flags, receiver, len, text, chatTag.
    {
        auto c = DecodeChat(From({ 1, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 'h', 'i', 0, 0 }));
        Expect("DecodeChat/say", c && c->type == 1 && c->language == 7 && c->sender == 1 && c->receiver == 1 && c->text == "hi");
        Expect("DecodeChat/garbage", !DecodeChat(From({ 1, 2, 3 })));
    }

    // ---- interactions (InteractionPackets.cpp) ----

    // CMSG_BUY_ITEM: vendor guid, uint32 item, uint32 slot (from 1), uint32 count, uint8 0.
    ExpectBytes("BuyItem", BuyItem(2, 159, 3, 1), { 2, 0, 0, 0, 0, 0, 0, 0, 159, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0 });
    // CMSG_QUESTGIVER_ACCEPT_QUEST: npc guid, uint32 quest, uint32 1.
    ExpectBytes("QuestgiverAcceptQuest", QuestgiverAcceptQuest(5, 33), { 5, 0, 0, 0, 0, 0, 0, 0, 33, 0, 0, 0, 1, 0, 0, 0 });
    // CMSG_GOSSIP_SELECT_OPTION: npc guid, uint32 menu, uint32 option, [cstring code].
    {
        std::string code = "ab";
        ExpectBytes("GossipSelectOption/code", GossipSelectOption(1, 2, 3, &code),
                    { 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 'a', 'b', 0 });
        ExpectBytes("GossipSelectOption", GossipSelectOption(1, 2, 3, nullptr), { 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0 });
    }

    // SMSG_GOSSIP_MESSAGE: guid, menu, title, nOptions, {index, icon, coded, money, text, box}, nQuests,
    // {id, icon, level, flags, repeatable, title}.
    {
        auto m = DecodeGossipMessage(From({ 7, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,  // guid, menu, title, 1 option
                                            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 'h', 'i', 0, 0,             // index 0, icon 1, "hi", ""
                                            1, 0, 0, 0, 33, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'Q', 0 }));
        Expect("DecodeGossipMessage", m && m->npc == 7 && m->menuId == 9 && m->options.size() == 1 && m->options[0].index == 0 &&
                                      m->options[0].icon == 1 && m->options[0].text == "hi" && m->quests.size() == 1 &&
                                      m->quests[0].questId == 33 && m->quests[0].level == 1 && m->quests[0].title == "Q");
    }

    // SMSG_LOOT_RESPONSE: guid, type, gold, count, {slot, item, count, display, suffix, property, slotType}.
    {
        auto w = DecodeLootWindow(From({ 3, 0, 0, 0, 0, 0, 0, 0, 1, 25, 0, 0, 0, 1,
                                         0, 0x65, 0x0B, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }));
        Expect("DecodeLootWindow", w && w->lootGuid == 3 && w->lootType == 1 && w->gold == 25 && w->items.size() == 1 &&
                                   w->items[0].itemId == 2917 && w->items[0].count == 2 && w->items[0].slotType == 0);
        auto e = DecodeLootWindow(From({ 3, 0, 0, 0, 0, 0, 0, 0, 0, 4 }));
        Expect("DecodeLootWindow/error", e && e->lootType == 0 && e->error == 4);
    }

    // SMSG_LIST_INVENTORY: guid, count, {slot, item, display, avail, price, durability, buyCount, extCost}.
    {
        auto v = DecodeListInventory(From({ 2, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 159, 0, 0, 0, 0, 0, 0, 0,
                                            0xFF, 0xFF, 0xFF, 0xFF, 25, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0 }));
        Expect("DecodeListInventory", v && v->vendor == 2 && v->items.size() == 1 && v->items[0].slot == 1 && v->items[0].itemId == 159 &&
                                      v->items[0].available == -1 && v->items[0].price == 25 && v->items[0].buyCount == 5);
    }

    if (failures == 0)
        std::printf("all packet tests passed\n");
    return failures == 0 ? 0 : 1;
}
