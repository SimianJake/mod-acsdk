#include "PacketEvents.h"

#include "EnumNames.generated.h"
#include "Packets.h"

#include "Opcodes.h"
#include "SharedDefines.h"

namespace acsdk
{
    namespace
    {
        ByteBuffer Buf(CapturedPacket const& p)
        {
            ByteBuffer b;
            if (!p.bytes.empty())
                b.append(p.bytes.data(), p.bytes.size());
            return b;
        }

        std::string CastResult(uint8_t r)
        {
            char const* name = SpellCastResultName(r);
            return name ? name : "SPELL_FAILED_" + std::to_string(r);
        }

        void FillDamage(v1::DamageEvent* e, packets::Damage const& d)
        {
            e->set_attacker_guid(d.attacker);
            e->set_victim_guid(d.victim);
            e->set_amount(d.amount);
            e->set_overkill(d.overkill);
            e->set_school_mask(d.schoolMask);
            e->set_spell_id(d.spellId);
            e->set_absorbed(d.absorbed);
            e->set_resisted(d.resisted);
            e->set_blocked(d.blocked);
            e->set_critical(d.critical);
        }
    }

    std::vector<uint16_t> EventSourceOpcodes()
    {
        return {
            SMSG_MESSAGECHAT, SMSG_GM_MESSAGECHAT,                                   // Chat
            SMSG_ATTACKERSTATEUPDATE, SMSG_SPELLNONMELEEDAMAGELOG,                    // DamageDealt / DamageTaken
            SMSG_SPELL_START, MSG_CHANNEL_START,                                     // SpellCastStart
            SMSG_SPELL_GO, SMSG_CAST_FAILED, SMSG_SPELL_FAILURE, MSG_CHANNEL_UPDATE,  // SpellCastFinish
            SMSG_LOOT_RESPONSE, SMSG_ITEM_PUSH_RESULT, SMSG_LOOT_MONEY_NOTIFY,       // Loot
        };
    }

    void PublishPacketEvents(EventBus& bus, ObjectGuid character, CapturedPacket const& packet,
                             std::function<uint64_t(uint8_t castCount)> const& commandForCast)
    {
        if (!bus.HasSubscribers())
            return;

        uint64_t const self = character.GetRawValue();

        if (bus.WantsRaw(packet.opcode))
        {
            auto raw = std::make_unique<v1::Event>();
            raw->mutable_raw_packet()->set_opcode(packet.opcode);
            raw->mutable_raw_packet()->set_payload(std::string(packet.bytes.begin(), packet.bytes.end()));
            bus.Publish(self, std::move(raw));
        }

        auto event = std::make_unique<v1::Event>();
        switch (packet.opcode)
        {
            case SMSG_MESSAGECHAT:
            case SMSG_GM_MESSAGECHAT:
            {
                auto c = packets::DecodeChat(Buf(packet));
                if (!c)
                    return;
                v1::ChatEvent* e = event->mutable_chat();
                e->set_chat_type(c->type);
                e->set_language(static_cast<uint32_t>(c->language));
                e->set_sender_guid(c->sender);
                e->set_sender_name(c->senderName);
                e->set_receiver_guid(c->receiver);
                e->set_text(c->text);
                e->set_channel(c->channel);
                e->set_self(c->sender == self || c->type == CHAT_MSG_WHISPER_INFORM); // own say / own whisper echo
                break;
            }
            case SMSG_ATTACKERSTATEUPDATE:
            case SMSG_SPELLNONMELEEDAMAGELOG:
            {
                auto d = packet.opcode == SMSG_ATTACKERSTATEUPDATE ? packets::DecodeAttackerStateUpdate(Buf(packet))
                                                                   : packets::DecodeSpellNonMeleeDamageLog(Buf(packet));
                if (!d)
                    return;
                // Broadcast to everyone in range; only damage this character dealt or took is its event.
                if (d->attacker == self)
                {
                    FillDamage(event->mutable_damage_dealt(), *d);
                    if (d->victim == self) // self-damage: also a DamageTaken (a different event type)
                    {
                        bus.Publish(self, std::move(event));
                        event = std::make_unique<v1::Event>();
                        FillDamage(event->mutable_damage_taken(), *d);
                    }
                }
                else if (d->victim == self)
                    FillDamage(event->mutable_damage_taken(), *d);
                else
                    return;
                break;
            }
            case SMSG_SPELL_START:
            {
                auto s = packets::DecodeSpellStartOrGo(Buf(packet));
                if (!s || s->caster != self)
                    return;
                v1::SpellCastEvent* e = event->mutable_spell_cast_start();
                e->set_caster_guid(s->caster);
                e->set_spell_id(s->spellId);
                e->set_cast_count(s->castCount);
                e->set_command_id(s->castCount ? commandForCast(s->castCount) : 0);
                e->set_outcome(v1::SpellCastEvent::STARTED);
                e->set_cast_time_ms(packets::DecodeSpellStartTimer(Buf(packet)).value_or(0));
                break;
            }
            case MSG_CHANNEL_START:
            {
                auto s = packets::DecodeChannelStart(Buf(packet));
                if (!s || s->caster != self)
                    return;
                v1::SpellCastEvent* e = event->mutable_spell_cast_start();
                e->set_caster_guid(s->caster);
                e->set_spell_id(s->spellId);
                e->set_outcome(v1::SpellCastEvent::STARTED);
                e->set_cast_time_ms(s->durationMs);
                break;
            }
            case SMSG_SPELL_GO:
            {
                auto s = packets::DecodeSpellStartOrGo(Buf(packet));
                if (!s || s->caster != self)
                    return;
                v1::SpellCastEvent* e = event->mutable_spell_cast_finish();
                e->set_caster_guid(s->caster);
                e->set_spell_id(s->spellId);
                e->set_cast_count(s->castCount);
                e->set_command_id(s->castCount ? commandForCast(s->castCount) : 0);
                e->set_outcome(v1::SpellCastEvent::SUCCEEDED);
                break;
            }
            case SMSG_CAST_FAILED: // sent to the caster only
            {
                auto f = packets::DecodeCastFailed(Buf(packet));
                if (!f)
                    return;
                v1::SpellCastEvent* e = event->mutable_spell_cast_finish();
                e->set_caster_guid(self);
                e->set_spell_id(f->spellId);
                e->set_cast_count(f->castCount);
                e->set_command_id(f->castCount ? commandForCast(f->castCount) : 0);
                e->set_outcome(v1::SpellCastEvent::FAILED);
                e->set_reason(CastResult(f->result));
                break;
            }
            case SMSG_SPELL_FAILURE: // SMSG_SPELL_FAILED_OTHER duplicates it and is deliberately not a source
            {
                auto f = packets::DecodeSpellFailure(Buf(packet));
                if (!f || f->caster != self)
                    return;
                v1::SpellCastEvent* e = event->mutable_spell_cast_finish();
                e->set_caster_guid(self);
                e->set_spell_id(f->spellId);
                e->set_cast_count(f->castCount);
                e->set_command_id(f->castCount ? commandForCast(f->castCount) : 0);
                e->set_outcome(v1::SpellCastEvent::INTERRUPTED);
                e->set_reason(CastResult(f->result));
                break;
            }
            case MSG_CHANNEL_UPDATE:
            {
                auto u = packets::DecodeChannelUpdate(Buf(packet));
                if (!u || u->caster != self || u->remainingMs != 0)
                    return;
                v1::SpellCastEvent* e = event->mutable_spell_cast_finish();
                e->set_caster_guid(self);
                e->set_outcome(v1::SpellCastEvent::CHANNEL_ENDED);
                break;
            }
            case SMSG_LOOT_RESPONSE:
            {
                auto r = packets::DecodeLootResponse(Buf(packet));
                if (!r)
                    return;
                v1::LootEvent* e = event->mutable_loot();
                e->set_loot_guid(r->lootGuid);
                e->set_kind(r->lootType == 0 ? v1::LootEvent::ERROR : v1::LootEvent::WINDOW_OPENED);
                e->set_error(r->error);
                break;
            }
            case SMSG_ITEM_PUSH_RESULT:
            {
                auto r = packets::DecodeItemPushResult(Buf(packet));
                if (!r || r->player != self) // may be group-broadcast
                    return;
                v1::LootEvent* e = event->mutable_loot();
                e->set_kind(v1::LootEvent::ITEM_RECEIVED);
                e->set_item_id(r->itemId);
                e->set_count(r->count);
                break;
            }
            case SMSG_LOOT_MONEY_NOTIFY:
            {
                auto amount = packets::DecodeLootMoneyNotify(Buf(packet));
                if (!amount)
                    return;
                v1::LootEvent* e = event->mutable_loot();
                e->set_kind(v1::LootEvent::MONEY);
                e->set_money_copper(*amount);
                break;
            }
            default:
                return;
        }

        bus.Publish(self, std::move(event));
    }
}
