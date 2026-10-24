// Interact channel (Commands.h): death recovery, NPC dialogs, quests, vendors, loot and equipment.
//
// Same rules as every other command: the client's own CMSG is injected so AzerothCore's handler applies
// every game rule, and the result comes from the server's reply (or, for handlers that answer silently,
// from the game state the handler changed). Nothing here acts on the game directly.

#include "Commands.h"

#include "EnumNames.generated.h"
#include "Packets.h"

#include "Corpse.h"
#include "Creature.h"
#include "GameTime.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "WorldPacket.h"

namespace acsdk
{
    namespace
    {
        constexpr uint8 LootSlotAllow = 0; // LOOT_SLOT_TYPE_ALLOW_LOOT
        constexpr uint8 LootSlotOwner = 4; // LOOT_SLOT_TYPE_OWNER

        void Inject(HeadlessRuntime& runtime, ObjectGuid character, uint16 opcode, ByteBuffer const& body)
        {
            auto packet = std::make_unique<WorldPacket>(opcode, body.size());
            if (body.size())
                packet->append(body.contents(), body.size());
            runtime.Inject(character, std::move(packet));
        }

        std::string NameOr(char const* name, char const* prefix, uint32 value)
        {
            return name ? name : std::string(prefix) + std::to_string(value);
        }

        bool IsInteractionType(CommandType type)
        {
            return type >= CommandType::ReleaseSpirit;
        }

        bool OpensWindow(CommandType type)
        {
            return type == CommandType::Talk || type == CommandType::GossipSelect || type == CommandType::OpenVendor;
        }

        bool UsesNpc(CommandType type)
        {
            switch (type)
            {
                case CommandType::SpiritHealer: case CommandType::Talk: case CommandType::GossipSelect:
                case CommandType::AcceptQuest: case CommandType::CompleteQuest: case CommandType::OpenVendor:
                case CommandType::BuyItem: case CommandType::SellItem: case CommandType::Loot:
                    return true;
                default:
                    return false;
            }
        }

        std::shared_ptr<Interaction> Window(char const* kind, uint64 npc)
        {
            auto w = std::make_shared<Interaction>();
            w->kind = kind;
            w->npc = npc;
            return w;
        }
    }

    bool CommandManager::BeginInteraction(Player* player, std::shared_ptr<Command> const& cmd, std::string& failReason)
    {
        CommandSpec const& spec = cmd->spec;
        ObjectGuid character = cmd->character;
        uint64 npc = spec.npcGuid;

        switch (spec.type)
        {
            case CommandType::ReleaseSpirit:
                Inject(_runtime, character, CMSG_REPOP_REQUEST, packets::RepopRequest());
                return true;
            case CommandType::ReclaimCorpse:
                Inject(_runtime, character, CMSG_RECLAIM_CORPSE, packets::Guid(player->GetGUID().GetRawValue()));
                return true;
            case CommandType::SpiritHealer:
                Inject(_runtime, character, CMSG_SPIRIT_HEALER_ACTIVATE, packets::Guid(npc));
                return true;
            case CommandType::Talk:
            {
                // What a client's right-click sends depends on the NPC's flags.
                Creature* creature = ObjectAccessor::GetCreature(*player, ObjectGuid(npc));
                if (!creature)
                {
                    failReason = "TARGET_GONE";
                    return false;
                }
                uint16 opcode = creature->IsGossip() ? CMSG_GOSSIP_HELLO
                              : creature->IsQuestGiver() ? CMSG_QUESTGIVER_HELLO
                              : creature->IsVendor() ? CMSG_LIST_INVENTORY
                              : CMSG_GOSSIP_HELLO;
                Inject(_runtime, character, opcode, packets::Guid(npc));
                return true;
            }
            case CommandType::GossipSelect:
                Inject(_runtime, character, CMSG_GOSSIP_SELECT_OPTION,
                       packets::GossipSelectOption(npc, spec.menuId, spec.optionIndex, spec.text.empty() ? nullptr : &spec.text));
                return true;
            case CommandType::AcceptQuest:
                cmd->questStatusAtStart = player->GetQuestStatus(spec.questId);
                Inject(_runtime, character, CMSG_QUESTGIVER_ACCEPT_QUEST, packets::QuestgiverAcceptQuest(npc, spec.questId));
                return true;
            case CommandType::CompleteQuest:
                Inject(_runtime, character, CMSG_QUESTGIVER_COMPLETE_QUEST, packets::QuestgiverQuest(npc, spec.questId));
                return true;
            case CommandType::AbandonQuest:
            {
                uint16 slot = player->FindQuestSlot(spec.questId);
                if (slot >= MAX_QUEST_LOG_SIZE)
                {
                    failReason = "QUEST_NOT_IN_LOG";
                    return false;
                }
                Inject(_runtime, character, CMSG_QUESTLOG_REMOVE_QUEST, packets::QuestLogRemoveQuest(uint8(slot)));
                return true;
            }
            case CommandType::OpenVendor:
                Inject(_runtime, character, CMSG_LIST_INVENTORY, packets::Guid(npc));
                return true;
            case CommandType::BuyItem:
            {
                // The client names the vendor slot it saw in SMSG_LIST_INVENTORY (counting from 1).
                Creature* creature = ObjectAccessor::GetCreature(*player, ObjectGuid(npc));
                if (!creature)
                {
                    failReason = "TARGET_GONE";
                    return false;
                }
                VendorItemData const* items = sObjectMgr->GetNpcVendorItemList(creature->GetEntry());
                uint32 slot = 0;
                for (uint32 i = 0; items && i < items->GetItemCount(); ++i)
                    if (VendorItem const* v = items->GetItem(i); v && v->item == spec.itemId)
                    {
                        slot = i + 1;
                        break;
                    }
                if (!slot)
                {
                    failReason = "BUY_ERR_CANT_FIND_ITEM";
                    return false;
                }
                Inject(_runtime, character, CMSG_BUY_ITEM, packets::BuyItem(npc, spec.itemId, slot, std::max<uint32>(spec.count, 1)));
                return true;
            }
            case CommandType::SellItem:
            {
                Item* item = player->GetItemByGuid(ObjectGuid(spec.objectGuid));
                if (!item)
                {
                    failReason = "ITEM_NOT_FOUND";
                    return false;
                }
                cmd->startCount = item->GetCount();
                Inject(_runtime, character, CMSG_SELL_ITEM, packets::SellItem(npc, spec.objectGuid, spec.count));
                return true;
            }
            case CommandType::EquipItem:
            {
                Item* item = player->GetItemByGuid(ObjectGuid(spec.objectGuid));
                if (!item)
                {
                    failReason = "ITEM_NOT_FOUND";
                    return false;
                }
                Inject(_runtime, character, CMSG_AUTOEQUIP_ITEM, packets::AutoEquipItem(item->GetBagSlot(), item->GetSlot()));
                return true;
            }
            case CommandType::Loot:
                Inject(_runtime, character, CMSG_LOOT, packets::Guid(npc));
                return true;
            default:
                failReason = "UNSUPPORTED";
                return false;
        }
    }

    void CommandManager::HandleInteractionPacket(Player* player, std::shared_ptr<Command> const& cmd, CapturedPacket const& packet, ByteBuffer& buf)
    {
        CommandType type = cmd->spec.type;
        uint64 npc = cmd->spec.npcGuid;

        auto window = [&](std::shared_ptr<Interaction> w)
        {
            if (!OpensWindow(type))
                return;
            cmd->interaction = std::move(w);
            Finish(cmd, CommandStatus::Completed, "");
        };

        switch (packet.opcode)
        {
            // ---- windows ----
            case SMSG_GOSSIP_MESSAGE:
                if (auto m = packets::DecodeGossipMessage(buf))
                {
                    auto w = Window("GOSSIP", m->npc);
                    w->menuId = m->menuId;
                    w->options = std::move(m->options);
                    w->quests = std::move(m->quests);
                    window(w);
                }
                break;
            case SMSG_QUESTGIVER_QUEST_LIST:
                if (auto m = packets::DecodeQuestgiverQuestList(buf))
                {
                    auto w = Window("QUEST_LIST", m->npc);
                    w->quests = std::move(m->quests);
                    window(w);
                }
                break;
            case SMSG_QUESTGIVER_QUEST_DETAILS:
            case SMSG_QUESTGIVER_REQUEST_ITEMS:
            case SMSG_QUESTGIVER_OFFER_REWARD:
            {
                auto d = packet.opcode == SMSG_QUESTGIVER_QUEST_DETAILS ? packets::DecodeQuestgiverQuestDetails(buf)
                                                                        : packets::DecodeQuestgiverGuidQuest(buf);
                if (!d)
                    break;

                if (type == CommandType::CompleteQuest && d->questId == cmd->spec.questId)
                {
                    // Continue the turn-in as the client's quest dialog would.
                    if (packet.opcode == SMSG_QUESTGIVER_REQUEST_ITEMS)
                    {
                        if (player->GetQuestStatus(d->questId) != QUEST_STATUS_COMPLETE && !player->CanCompleteQuest(d->questId))
                            return Finish(cmd, CommandStatus::Failed, "QUEST_NOT_COMPLETE");
                        Inject(_runtime, cmd->character, CMSG_QUESTGIVER_REQUEST_REWARD, packets::QuestgiverQuest(npc, d->questId));
                    }
                    else if (packet.opcode == SMSG_QUESTGIVER_OFFER_REWARD)
                    {
                        cmd->started = true;
                        Inject(_runtime, cmd->character, CMSG_QUESTGIVER_CHOOSE_REWARD,
                               packets::QuestgiverChooseReward(npc, d->questId, cmd->spec.rewardIndex));
                    }
                    break;
                }

                char const* kind = packet.opcode == SMSG_QUESTGIVER_QUEST_DETAILS ? "QUEST_DETAILS"
                                 : packet.opcode == SMSG_QUESTGIVER_REQUEST_ITEMS ? "QUEST_REQUEST_ITEMS"
                                 : "QUEST_REWARD";
                auto w = Window(kind, d->npc);
                packets::QuestOffer offer;
                offer.questId = d->questId;
                offer.title = d->title;
                if (Quest const* q = sObjectMgr->GetQuestTemplate(d->questId))
                    offer.level = q->GetQuestLevel();
                w->quests.push_back(std::move(offer));
                window(w);
                break;
            }
            case SMSG_LIST_INVENTORY:
                if (auto v = packets::DecodeListInventory(buf))
                {
                    auto w = Window("VENDOR", v->vendor);
                    w->vendorItems = std::move(v->items);
                    window(w);
                }
                break;
            case SMSG_TRAINER_LIST:  window(Window("TRAINER", npc)); break;
            case SMSG_SHOWTAXINODES: window(Window("TAXI", npc)); break;
            case SMSG_SHOW_BANK:     window(Window("BANK", npc)); break;
            case SMSG_BINDER_CONFIRM: window(Window("BINDER", npc)); break;
            case SMSG_GOSSIP_COMPLETE: window(Window("CLOSED", npc)); break;

            // ---- quests ----
            case SMSG_QUESTGIVER_QUEST_INVALID:
                if (type == CommandType::AcceptQuest || type == CommandType::CompleteQuest)
                {
                    uint32 reason = buf.size() >= 4 ? buf.read<uint32>() : 0;
                    Finish(cmd, CommandStatus::Failed, NameOr(QuestFailedReasonName(reason), "INVALIDREASON_", reason));
                }
                break;
            case SMSG_QUESTGIVER_QUEST_FAILED:
                if (type == CommandType::AcceptQuest || type == CommandType::CompleteQuest)
                {
                    uint32 reason = buf.size() >= 8 ? (buf.read_skip<uint32>(), buf.read<uint32>()) : 0;
                    Finish(cmd, CommandStatus::Failed, NameOr(QuestFailedReasonName(reason), "INVALIDREASON_", reason));
                }
                break;

            // ---- vendor ----
            case SMSG_BUY_ITEM:
                if (type == CommandType::BuyItem)
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            case SMSG_BUY_FAILED:
                if (type == CommandType::BuyItem)
                    if (auto code = packets::DecodeLastByte(buf))
                        Finish(cmd, CommandStatus::Failed, NameOr(BuyResultName(*code), "BUY_ERR_", *code));
                break;
            case SMSG_SELL_ITEM:
                if (type == CommandType::SellItem)
                    if (auto code = packets::DecodeLastByte(buf))
                        Finish(cmd, CommandStatus::Failed, NameOr(SellResultName(*code), "SELL_ERR_", *code));
                break;

            // ---- items ----
            case SMSG_INVENTORY_CHANGE_FAILURE:
                if (auto code = packets::DecodeFirstByte(buf); code && *code != 0)
                {
                    std::string reason = NameOr(InventoryResultName(*code), "EQUIP_ERR_", *code);
                    if (type == CommandType::Loot)
                    {
                        if (cmd->lootFailure.empty())
                            cmd->lootFailure = reason;
                    }
                    else if (type == CommandType::EquipItem || type == CommandType::BuyItem || type == CommandType::CompleteQuest)
                        Finish(cmd, CommandStatus::Failed, reason);
                }
                break;

            // ---- loot: open, take everything allowed, release ----
            case SMSG_LOOT_RESPONSE:
                if (type != CommandType::Loot || cmd->started)
                    break;
                if (auto w = packets::DecodeLootWindow(buf))
                {
                    if (w->lootType == 0)
                        return Finish(cmd, CommandStatus::Failed, NameOr(LootErrorName(w->error), "LOOT_ERROR_", w->error));

                    cmd->started = true;
                    cmd->lootGuid = w->lootGuid;
                    auto result = Window("LOOT", w->lootGuid);
                    result->lootMoney = w->gold;
                    if (w->gold)
                        Inject(_runtime, cmd->character, CMSG_LOOT_MONEY, ByteBuffer(0));
                    for (packets::LootSlot const& slot : w->items)
                    {
                        result->loot.push_back({ slot.slot, slot.itemId, slot.count, false });
                        if (slot.slotType == LootSlotAllow || slot.slotType == LootSlotOwner)
                            Inject(_runtime, cmd->character, CMSG_AUTOSTORE_LOOT_ITEM, packets::AutostoreLootItem(slot.slot));
                    }
                    cmd->interaction = result;
                    Inject(_runtime, cmd->character, CMSG_LOOT_RELEASE, packets::Guid(w->lootGuid));
                }
                break;
            case SMSG_LOOT_REMOVED:
                if (type == CommandType::Loot && cmd->interaction && buf.size() >= 1)
                {
                    uint8 slot = buf.read<uint8>();
                    for (Interaction::Looted& l : cmd->interaction->loot)
                        if (l.slot == slot)
                            l.taken = true;
                }
                break;
            case SMSG_LOOT_RELEASE_RESPONSE:
                if (type == CommandType::Loot && cmd->started)
                {
                    if (cmd->lootFailure.empty())
                        Finish(cmd, CommandStatus::Completed, "");
                    else
                        Finish(cmd, CommandStatus::Failed, cmd->lootFailure);
                }
                break;
            default:
                break;
        }
    }

    void CommandManager::PollInteraction(Player* player, std::shared_ptr<Command> const& cmd)
    {
        CommandSpec const& spec = cmd->spec;
        switch (spec.type)
        {
            case CommandType::ReleaseSpirit:
                if (player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            case CommandType::ReclaimCorpse:
            case CommandType::SpiritHealer:
                if (player->IsAlive() && !cmd->aliveAtStart)
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            case CommandType::AcceptQuest:
                if (cmd->questStatusAtStart == QUEST_STATUS_NONE && player->GetQuestStatus(spec.questId) != QUEST_STATUS_NONE)
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            case CommandType::CompleteQuest:
                if (player->GetQuestRewardStatus(spec.questId))
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            case CommandType::AbandonQuest:
                if (player->FindQuestSlot(spec.questId) >= MAX_QUEST_LOG_SIZE)
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            case CommandType::SellItem:
            {
                // AzerothCore answers a sale only when it fails.
                Item* item = player->GetItemByGuid(ObjectGuid(spec.objectGuid));
                if (!item || item->GetCount() < cmd->startCount)
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            }
            case CommandType::EquipItem:
            {
                Item* item = player->GetItemByGuid(ObjectGuid(spec.objectGuid));
                if (!item)
                    Finish(cmd, CommandStatus::Failed, "ITEM_NOT_FOUND");
                else if (item->IsEquipped())
                    Finish(cmd, CommandStatus::Completed, "");
                break;
            }
            default:
                break;
        }
    }

    std::string CommandManager::DiagnoseNoResponse(Player* player, Command const& cmd) const
    {
        CommandSpec const& spec = cmd.spec;
        if (!IsInteractionType(spec.type))
            return "";

        switch (spec.type)
        {
            case CommandType::ReleaseSpirit:
                if (player->IsAlive())
                    return "NOT_DEAD";
                break;
            case CommandType::ReclaimCorpse:
            {
                if (player->IsAlive())
                    return "NOT_DEAD";
                if (!player->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                    return "NOT_RELEASED";
                Corpse* corpse = player->GetCorpse();
                if (!corpse)
                    return "NO_CORPSE";
                if (time_t(corpse->GetGhostTime() + player->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP)) > time_t(GameTime::GetGameTime().count()))
                    return "RECLAIM_DELAY";
                if (!corpse->IsInMap(player) || !corpse->IsWithinDist(player, CORPSE_RECLAIM_RADIUS, true))
                    return "TOO_FAR_FROM_CORPSE";
                break;
            }
            case CommandType::GossipSelect:
                if (player->PlayerTalkClass->GetGossipMenu().GetSenderGUID() != ObjectGuid(spec.npcGuid) ||
                    !player->PlayerTalkClass->GetGossipMenu().GetItem(spec.optionIndex))
                    return "NO_SUCH_OPTION"; // Talk to the NPC first; menus change after each choice
                break;
            case CommandType::AcceptQuest:
                if (cmd.questStatusAtStart != QUEST_STATUS_NONE)
                    return "QUEST_ALREADY_IN_LOG";
                if (Object* giver = ObjectAccessor::GetWorldObject(*player, ObjectGuid(spec.npcGuid)); giver && !giver->hasQuest(spec.questId))
                    return "QUEST_NOT_OFFERED";
                break;
            case CommandType::CompleteQuest:
                if (player->GetQuestStatus(spec.questId) == QUEST_STATUS_NONE)
                    return "QUEST_NOT_IN_LOG";
                break;
            default:
                break;
        }

        if (UsesNpc(spec.type) && spec.npcGuid)
        {
            WorldObject* target = ObjectAccessor::GetWorldObject(*player, ObjectGuid(spec.npcGuid));
            if (!target)
                return "TARGET_GONE";
            float range = spec.type == CommandType::Loot ? INTERACTION_DISTANCE + 2.0f : INTERACTION_DISTANCE;
            if (!player->IsWithinDistInMap(target, range))
                return "TOO_FAR";
            if (spec.type != CommandType::SpiritHealer && !player->IsAlive())
                return "DEAD";
            if (spec.type == CommandType::Loot)
                if (Creature* creature = target->ToCreature(); creature && creature->IsAlive())
                    return "TARGET_ALIVE";
        }
        return "";
    }
}
