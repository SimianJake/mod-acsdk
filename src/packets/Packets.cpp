#include "Packets.h"

#include "SharedDefines.h"

namespace acsdk::packets
{
    namespace
    {
        // SpellInfo.h SpellCastTargetFlags (game library); duplicated here to keep this file game-free.
        constexpr uint32_t TargetFlagUnit = 0x00000002;
        constexpr uint32_t TargetFlagDestLocation = 0x00000040;

        uint64_t ReadPacked(ByteBuffer& buf)
        {
            uint64_t guid = 0;
            buf.readPackGUID(guid);
            return guid;
        }
    }

    ByteBuffer SetSelection(uint64_t targetGuid)
    {
        ByteBuffer b(8);
        b << uint64(targetGuid);
        return b;
    }

    ByteBuffer AttackSwing(uint64_t victimGuid)
    {
        ByteBuffer b(8);
        b << uint64(victimGuid);
        return b;
    }

    ByteBuffer AttackStop()
    {
        return ByteBuffer(0);
    }

    ByteBuffer CastSpell(uint8_t castCount, uint32_t spellId, CastTarget const& target)
    {
        ByteBuffer b(32);
        b << uint8(castCount);
        b << uint32(spellId);
        b << uint8(0); // castFlags: never 0x02 (would embed a client movement packet)

        uint32_t mask = 0;
        bool dest = target.destX && target.destY && target.destZ;
        if (target.unitGuid)
            mask |= TargetFlagUnit;
        if (dest)
            mask |= TargetFlagDestLocation;

        b << uint32(mask);
        if (mask & TargetFlagUnit)
            b.appendPackGUID(target.unitGuid);
        if (mask & TargetFlagDestLocation)
        {
            b.appendPackGUID(0); // transport guid
            b << float(*target.destX) << float(*target.destY) << float(*target.destZ);
        }
        return b;
    }

    ByteBuffer CancelCast(uint32_t spellId)
    {
        ByteBuffer b(5);
        b << uint8(0);
        b << uint32(spellId);
        return b;
    }

    ByteBuffer CancelChannel(uint32_t spellId)
    {
        ByteBuffer b(4);
        b << uint32(spellId);
        return b;
    }

    ByteBuffer MessageChat(uint32_t chatType, uint32_t language, std::string const& message, std::string const& whisperTo)
    {
        ByteBuffer b(8 + whisperTo.size() + message.size() + 2);
        b << uint32(chatType);
        b << uint32(language);
        if (chatType == CHAT_MSG_WHISPER)
            b << whisperTo;
        b << message;
        return b;
    }

    std::optional<CastFailed> DecodeCastFailed(ByteBuffer buf)
    {
        try
        {
            CastFailed r{};
            r.castCount = buf.read<uint8>();
            r.spellId = buf.read<uint32>();
            r.result = buf.read<uint8>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<SpellStartOrGo> DecodeSpellStartOrGo(ByteBuffer buf)
    {
        try
        {
            SpellStartOrGo r{};
            r.castItemOrCaster = ReadPacked(buf);
            r.caster = ReadPacked(buf);
            r.castCount = buf.read<uint8>();
            r.spellId = buf.read<uint32>();
            r.castFlags = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<SpellFailure> DecodeSpellFailure(ByteBuffer buf)
    {
        try
        {
            SpellFailure r{};
            r.caster = ReadPacked(buf);
            r.castCount = buf.read<uint8>();
            r.spellId = buf.read<uint32>();
            r.result = buf.read<uint8>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<ChannelStart> DecodeChannelStart(ByteBuffer buf)
    {
        try
        {
            ChannelStart r{};
            r.caster = ReadPacked(buf);
            r.spellId = buf.read<uint32>();
            r.durationMs = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<ChannelUpdate> DecodeChannelUpdate(ByteBuffer buf)
    {
        try
        {
            ChannelUpdate r{};
            r.caster = ReadPacked(buf);
            r.remainingMs = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<AttackStart> DecodeAttackStart(ByteBuffer buf)
    {
        try
        {
            AttackStart r{};
            r.attacker = buf.read<uint64>();
            r.victim = buf.read<uint64>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<AttackStopped> DecodeAttackStop(ByteBuffer buf)
    {
        try
        {
            AttackStopped r{};
            r.attacker = ReadPacked(buf);
            r.victim = ReadPacked(buf);
            r.nowDead = buf.read<uint32>();
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    // ChatHandler::BuildChatPacket (Chat/Chat.cpp).
    std::optional<Chat> DecodeChat(ByteBuffer buf)
    {
        try
        {
            Chat r;
            r.type = buf.read<uint8>();
            r.language = buf.read<int32>();
            r.sender = buf.read<uint64>();
            buf.read_skip<uint32>(); // flags

            switch (r.type)
            {
                case CHAT_MSG_MONSTER_SAY:
                case CHAT_MSG_MONSTER_PARTY:
                case CHAT_MSG_MONSTER_YELL:
                case CHAT_MSG_MONSTER_WHISPER:
                case CHAT_MSG_MONSTER_EMOTE:
                case CHAT_MSG_RAID_BOSS_EMOTE:
                case CHAT_MSG_RAID_BOSS_WHISPER:
                case CHAT_MSG_BATTLENET:
                {
                    buf.read_skip<uint32>();
                    r.senderName = buf.ReadCString(false);
                    r.receiver = buf.read<uint64>();
                    uint64_t const highMask = 0xFFFF000000000000ull;
                    bool receiverIsPlayerOrPet = (r.receiver & highMask) == 0 || (r.receiver >> 48) == 0xF140;
                    if (r.receiver && !receiverIsPlayerOrPet)
                    {
                        buf.read_skip<uint32>();
                        buf.ReadCString(false);
                    }
                    break;
                }
                case CHAT_MSG_WHISPER_FOREIGN:
                    buf.read_skip<uint32>();
                    r.senderName = buf.ReadCString(false);
                    r.receiver = buf.read<uint64>();
                    break;
                case CHAT_MSG_CHANNEL:
                    r.channel = buf.ReadCString(false);
                    r.receiver = buf.read<uint64>();
                    break;
                default:
                    r.receiver = buf.read<uint64>();
                    break;
            }

            buf.read_skip<uint32>();          // message length incl. terminator
            r.text = buf.ReadCString(false);
            return r;
        }
        catch (ByteBufferException const&) { return std::nullopt; }
    }

    std::optional<std::string> DecodeChatPlayerNotFound(ByteBuffer buf)
    {
        try { return buf.ReadCString(false); }
        catch (ByteBufferException const&) { return std::nullopt; }
    }
}
