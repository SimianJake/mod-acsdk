/*
 * Packet hooks for headless characters (docs/research/headless-sessions.md §6).
 */

#include "AcsdkModule.h"
#include "Services.h"

#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    class acsdk_server_script : public ServerScript
    {
    public:
        acsdk_server_script() : ServerScript("acsdk_server_script", {
            SERVERHOOK_ON_PACKET_SENT,
            SERVERHOOK_CAN_PACKET_RECEIVE
        }) { }

        // Fires for every WorldSession::SendPacket, on whichever thread sends it (often a map thread).
        // Opcode-first filter: one hash lookup for non-headless sessions, one set lookup for unwanted
        // opcodes, and only then a copy (PLAN.md §4.5).
        void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
        {
            if (!session || !session->IsHeadless())
                return;

            // DebugService.CreateCharacter's temporary session has no sink; it only needs this answer.
            if (packet.GetOpcode() == SMSG_CHAR_CREATE && packet.size())
                acsdk::OnCharCreateResponse(session, packet.contents()[0]);

            std::shared_ptr<acsdk::PacketSink> sink = sAcsdk.Runtime().SinkFor(session);
            if (!sink)
                return;

            uint16 opcode = packet.GetOpcode();
            if (!sink->Wants(opcode))
                return;

            // ByteBuffer::contents() throws on an empty buffer, and many SMSGs have no body
            // (e.g. SMSG_ATTACKSWING_CANCEL_ATTACK). An exception here would take worldserver down.
            sink->Push(opcode, packet.size() ? packet.contents() : nullptr, packet.size());
        }

        // Real clients only reach here on the world thread (CMSG_PLAYER_LOGIN is THREADUNSAFE). Refuse a
        // client login onto an account a headless character is using, which AC itself would not catch
        // (headless-sessions.md §3, duplicate-login hazard).
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (!session || session->IsHeadless() || packet.GetOpcode() != CMSG_PLAYER_LOGIN)
                return true;

            if (!sAcsdk.IsRunning() || !sAcsdk.Runtime().HoldsAccount(session->GetAccountId()))
                return true;

            LOG_INFO("module", "mod-acsdk: refused client login on account {}: a headless character is using it.", session->GetAccountId());
            return false;
        }
    };

    class acsdk_allmap_script : public AllMapScript
    {
    public:
        acsdk_allmap_script() : AllMapScript("acsdk_allmap_script", {
            ALLMAPHOOK_CAN_SEND_OBJECT_UPDATES_TO_PLAYER
        }) { }

        // Never build SMSG_UPDATE_OBJECT for headless characters: state is read from AC objects, and
        // these are the highest-volume packets (PLAN.md §4.7). Map thread.
        bool CanSendObjectUpdatesToPlayer(Map* /*map*/, Player* player) override
        {
            WorldSession* session = player ? player->GetSession() : nullptr;
            return !(session && session->IsHeadless() && sAcsdk.Runtime().IsHeadless(session));
        }
    };
}

void AddSC_acsdk_packets()
{
    new acsdk_server_script();
    new acsdk_allmap_script();
}
