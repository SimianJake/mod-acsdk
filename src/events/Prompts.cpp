#include "Prompts.h"

#include "Packets.h"

#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"

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

        void Inject(HeadlessRuntime& runtime, ObjectGuid character, uint16 opcode, ByteBuffer const& body)
        {
            auto packet = std::make_unique<WorldPacket>(opcode, body.size());
            if (body.size())
                packet->append(body.contents(), body.size());
            runtime.Inject(character, std::move(packet));
        }
    }

    std::vector<uint16_t> PromptManager::PromptOpcodes()
    {
        return {
            SMSG_GROUP_INVITE, SMSG_TRADE_STATUS, SMSG_DUEL_REQUESTED, SMSG_DUEL_COMPLETE, SMSG_RESURRECT_REQUEST,
            SMSG_SUMMON_REQUEST, SMSG_LOOT_START_ROLL, SMSG_LOOT_ROLL_WON, SMSG_LOOT_ALL_PASSED, MSG_RAID_READY_CHECK,
            MSG_RAID_READY_CHECK_FINISHED,
        };
    }

    void PromptManager::OnPacket(ObjectGuid character, CapturedPacket const& packet)
    {
        uint64_t const self = character.GetRawValue();
        Prompt p;
        p.character = character;

        switch (packet.opcode)
        {
            case SMSG_GROUP_INVITE:
                if (auto name = packets::DecodeGroupInvite(Buf(packet)))
                {
                    CloseKind(character, v1::GROUP_INVITE, "REPLACED"); // AC keeps one pending invite per player
                    p.kind = v1::GROUP_INVITE;
                    p.fromName = *name;
                    Open(std::move(p));
                }
                break;
            case SMSG_TRADE_STATUS:
                if (auto s = packets::DecodeTradeStatus(Buf(packet)))
                {
                    if (s->status == 1) // TRADE_STATUS_BEGIN_TRADE: someone asks to trade
                    {
                        CloseKind(character, v1::TRADE_REQUEST, "REPLACED");
                        p.kind = v1::TRADE_REQUEST;
                        p.fromGuid = s->trader;
                        Open(std::move(p));
                    }
                    else
                        CloseKind(character, v1::TRADE_REQUEST, "TRADE_STATUS_CHANGED");
                }
                break;
            case SMSG_DUEL_REQUESTED:
                if (auto d = packets::DecodeDuelRequested(Buf(packet)); d && d->initiator != self) // also sent to the initiator
                {
                    CloseKind(character, v1::DUEL_REQUEST, "REPLACED");
                    p.kind = v1::DUEL_REQUEST;
                    p.fromGuid = d->initiator;
                    p.lootGuid = d->arbiter; // the duel flag object; echoed in the answer
                    Open(std::move(p));
                }
                break;
            case SMSG_DUEL_COMPLETE:
                CloseKind(character, v1::DUEL_REQUEST, "DUEL_ENDED");
                break;
            case SMSG_RESURRECT_REQUEST:
                if (auto r = packets::DecodeResurrectRequest(Buf(packet)))
                {
                    CloseKind(character, v1::RESURRECT, "REPLACED");
                    p.kind = v1::RESURRECT;
                    p.fromGuid = r->guid;
                    p.fromName = r->name;
                    Open(std::move(p));
                }
                break;
            case SMSG_SUMMON_REQUEST:
                if (auto s = packets::DecodeSummonRequest(Buf(packet)))
                {
                    CloseKind(character, v1::SUMMON, "REPLACED");
                    p.kind = v1::SUMMON;
                    p.fromGuid = s->guid;
                    p.zoneId = s->value;
                    Open(std::move(p));
                }
                break;
            case SMSG_LOOT_START_ROLL:
                if (auto r = packets::DecodeLootStartRoll(Buf(packet)))
                {
                    p.kind = v1::LOOT_ROLL; // several rolls can be open at once, keyed by loot guid + slot
                    p.lootGuid = r->lootGuid;
                    p.lootSlot = r->slot;
                    p.itemId = r->itemId;
                    Open(std::move(p));
                }
                break;
            case SMSG_LOOT_ROLL_WON:
            case SMSG_LOOT_ALL_PASSED:
                if (auto r = packets::DecodeLootRollEnd(Buf(packet)))
                {
                    std::vector<uint64_t> done;
                    for (auto const& [id, pr] : _prompts)
                        if (pr.character == character && pr.kind == v1::LOOT_ROLL && pr.lootGuid == r->lootGuid && pr.lootSlot == r->slot)
                            done.push_back(id);
                    for (uint64_t id : done)
                        Close(id, "ROLL_ENDED");
                }
                break;
            case MSG_RAID_READY_CHECK:
                if (auto g = packets::DecodeGuid(Buf(packet)); g && *g != self)
                {
                    CloseKind(character, v1::READY_CHECK, "REPLACED");
                    p.kind = v1::READY_CHECK;
                    p.fromGuid = *g;
                    Open(std::move(p));
                }
                break;
            case MSG_RAID_READY_CHECK_FINISHED:
                CloseKind(character, v1::READY_CHECK, "READY_CHECK_FINISHED");
                break;
            default:
                break;
        }
    }

    uint64_t PromptManager::Open(Prompt prompt)
    {
        prompt.id = _nextId++;
        prompt.deadline = std::chrono::steady_clock::now() + _timeout;
        PublishPrompt(prompt, false, "");
        uint64_t id = prompt.id;
        _prompts.emplace(id, std::move(prompt));
        return id;
    }

    void PromptManager::PublishPrompt(Prompt const& p, bool closed, char const* reason)
    {
        if (!_bus.HasSubscribers())
            return;

        auto event = std::make_unique<v1::Event>();
        v1::PromptEvent* e = event->mutable_prompt();
        e->set_prompt_id(p.id);
        e->set_kind(p.kind);
        e->set_from_guid(p.fromGuid);
        e->set_from_name(p.fromName);
        e->set_timeout_ms(static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(_timeout).count()));
        if (p.kind == v1::LOOT_ROLL)
        {
            e->set_loot_guid(p.lootGuid);
            e->set_loot_slot(p.lootSlot);
            e->set_item_id(p.itemId);
        }
        e->set_zone_id(p.zoneId);
        e->set_closed(closed);
        e->set_close_reason(reason);
        _bus.Publish(p.character.GetRawValue(), std::move(event));
    }

    void PromptManager::Close(uint64_t id, char const* reason)
    {
        auto it = _prompts.find(id);
        if (it == _prompts.end())
            return;
        PublishPrompt(it->second, true, reason);
        _prompts.erase(it);
    }

    void PromptManager::CloseKind(ObjectGuid character, v1::PromptKind kind, char const* reason)
    {
        std::vector<uint64_t> ids;
        for (auto const& [id, p] : _prompts)
            if (p.character == character && p.kind == kind)
                ids.push_back(id);
        for (uint64_t id : ids)
            Close(id, reason);
    }

    // The exact client answers (docs/research/opcode-map.md §2.8).
    void PromptManager::SendAnswer(Prompt const& p, bool accept, uint32_t rollVote)
    {
        ByteBuffer b;
        switch (p.kind)
        {
            case v1::GROUP_INVITE:
                if (accept)
                {
                    b << uint32(0); // unused by the handler
                    Inject(_runtime, p.character, CMSG_GROUP_ACCEPT, b);
                }
                else
                    Inject(_runtime, p.character, CMSG_GROUP_DECLINE, b);
                break;
            case v1::TRADE_REQUEST:
                Inject(_runtime, p.character, accept ? CMSG_BEGIN_TRADE : CMSG_CANCEL_TRADE, b);
                break;
            case v1::DUEL_REQUEST:
                b << uint64(p.lootGuid); // arbiter
                Inject(_runtime, p.character, accept ? CMSG_DUEL_ACCEPTED : CMSG_DUEL_CANCELLED, b);
                break;
            case v1::RESURRECT:
                b << uint64(p.fromGuid) << uint8(accept ? 1 : 0);
                Inject(_runtime, p.character, CMSG_RESURRECT_RESPONSE, b);
                break;
            case v1::SUMMON:
                b << uint64(p.fromGuid) << uint8(accept ? 1 : 0);
                Inject(_runtime, p.character, CMSG_SUMMON_RESPONSE, b);
                break;
            case v1::LOOT_ROLL:
                b << uint64(p.lootGuid) << uint32(p.lootSlot) << uint8(rollVote);
                Inject(_runtime, p.character, CMSG_LOOT_ROLL, b);
                break;
            case v1::READY_CHECK:
                b << uint8(accept ? 1 : 0);
                Inject(_runtime, p.character, MSG_RAID_READY_CHECK, b);
                break;
            default:
                break;
        }
    }

    PromptManager::RespondResult PromptManager::Respond(ObjectGuid character, uint64_t promptId, bool accept, uint32_t rollVote)
    {
        auto it = _prompts.find(promptId);
        if (it == _prompts.end())
            return RespondResult::UnknownPrompt;
        if (it->second.character != character)
            return RespondResult::WrongCharacter;

        SendAnswer(it->second, accept, rollVote);
        Close(promptId, accept ? "ACCEPTED" : "DECLINED");
        return RespondResult::Ok;
    }

    void PromptManager::Update()
    {
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> expired, gone;
        for (auto const& [id, p] : _prompts)
        {
            if (!_runtime.FindOnlinePlayer(p.character))
                gone.push_back(id);
            else if (now >= p.deadline)
                expired.push_back(id);
        }

        for (uint64_t id : gone)
            Close(id, "CHARACTER_OFFLINE");

        for (uint64_t id : expired)
        {
            // Config-declared mechanical behaviour (ACSDK.Prompt.TimeoutSec): decline / pass, never accept.
            SendAnswer(_prompts[id], false, 0);
            Close(id, "TIMEOUT");
        }
    }
}
