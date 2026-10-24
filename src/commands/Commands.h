#ifndef ACSDK_COMMANDS_H
#define ACSDK_COMMANDS_H

#include "HeadlessRuntime.h"
#include "Packets.h"
#include "ObjectGuid.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace acsdk
{
    // PLAN.md §4.2.
    enum class CommandStatus : uint8_t
    {
        Started = 1,
        Completed = 2,
        Failed = 3,
        Timeout = 4,
        Cancelled = 5,
    };

    // Per-character command channels (docs/research/opcode-map.md §5). A channel holds at most one
    // command; a new one supersedes it.
    enum class Channel : uint8_t
    {
        Move,
        Cast,
        Melee,
        Chat,
        Interact,   // death recovery, NPC dialogs, quests, vendors, loot, equipment
    };

    enum class CommandType : uint8_t
    {
        MoveTo,
        Stop,
        Follow,
        Face,
        SetTarget,
        Attack,
        StopAttack,
        CastSpell,
        CancelCast,
        Say,
        Whisper,
        // Interact channel
        ReleaseSpirit,
        ReclaimCorpse,
        SpiritHealer,
        Talk,
        GossipSelect,
        AcceptQuest,
        CompleteQuest,
        AbandonQuest,
        OpenVendor,
        BuyItem,
        SellItem,
        EquipItem,
        Loot,
    };

    // What an interaction window showed (CommandResult.interaction).
    struct Interaction
    {
        std::string kind;                    // GOSSIP, QUEST_LIST, QUEST_DETAILS, VENDOR, LOOT, ...
        uint64_t npc = 0;
        uint32_t menuId = 0;
        std::vector<packets::GossipOption> options;
        std::vector<packets::QuestOffer> quests;
        std::vector<packets::VendorItem> vendorItems;
        uint32_t lootMoney = 0;
        struct Looted { uint8_t slot = 0; uint32_t itemId = 0; uint32_t count = 0; bool taken = false; };
        std::vector<Looted> loot;
    };

    struct CommandResult
    {
        uint64_t id = 0;
        CommandStatus status = CommandStatus::Started;
        std::string reason;
        uint32_t spellId = 0;
        std::shared_ptr<Interaction const> interaction;

        bool Terminal() const { return status != CommandStatus::Started; }
    };

    using CommandDone = std::function<void(CommandResult const&)>;

    struct CommandSpec
    {
        CommandType type = CommandType::Stop;
        std::chrono::milliseconds timeout{30000};

        // MoveTo
        float x = 0, y = 0, z = 0, tolerance = 1.0f;
        // Follow / Face(target) / SetTarget / Attack / CastSpell(unit)
        uint64_t targetGuid = 0;
        float distance = 2.0f, angle = 0.0f;
        // Face(orientation)
        std::optional<float> orientation;
        // Attack
        bool untilStopped = false;
        // CastSpell / CancelCast
        uint32_t spellId = 0;
        std::optional<float> destX, destY, destZ;
        // Say / Whisper; GossipSelect code
        std::string text, whisperTo;
        // Interact: npcGuid = NPC / loot target; objectGuid = item
        uint64_t npcGuid = 0, objectGuid = 0;
        uint32_t questId = 0, rewardIndex = 0, menuId = 0, optionIndex = 0, itemId = 0, count = 0;
    };

    // Owns every in-flight command. World thread only.
    //
    // Life of a command: Submit() validates nothing about game rules; it performs the action (inject the
    // client packet, or the whitelisted MotionMaster call) and registers a watcher. Each tick, Update()
    // feeds the character's captured SMSGs to its watchers and polls state; the first terminal result
    // answers every waiter. Results come from AzerothCore's replies, never from our own checks (§4.2).
    class CommandManager
    {
    public:
        explicit CommandManager(HeadlessRuntime& runtime) : _runtime(runtime) { }

        void SetKnockbackReflex(bool enabled) { _knockbackReflex = enabled; }

        // Every captured packet of every headless character passes here once per tick, before the watchers
        // see it (event translation and prompts hook in here). World thread.
        using PacketObserver = std::function<void(ObjectGuid character, CapturedPacket const& packet)>;
        void SetPacketObserver(PacketObserver observer) { _packetObserver = std::move(observer); }
        // Called with every terminal result (CommandResult events). World thread.
        using ResultObserver = std::function<void(ObjectGuid character, CommandResult const& result)>;
        void SetResultObserver(ResultObserver observer) { _resultObserver = std::move(observer); }

        // The running CastSpell command of `character` that used `castCount`, or 0.
        uint64_t CommandForCast(ObjectGuid character, uint8_t castCount) const;

        // Returns the new command's id. `onStarted` (optional) gets the STARTED result once the command is
        // underway; `onDone` gets the terminal result. Both are called on the world thread.
        uint64_t Submit(ObjectGuid character, CommandSpec const& spec, CommandDone onDone);

        bool Cancel(ObjectGuid character, uint64_t commandId, std::string const& reason);
        void CancelAll(ObjectGuid character, std::string const& reason);

        // Answers `done` with the command's terminal result now (if finished recently) or when it ends.
        // Returns false if the id is unknown.
        bool Wait(uint64_t commandId, CommandDone done);
        // The terminal result if the command already finished (and is still remembered).
        std::optional<CommandResult> Finished(uint64_t commandId) const;

        void Update();

        std::size_t InFlight() const;

    private:
        struct Command
        {
            uint64_t id = 0;
            ObjectGuid character;
            CommandSpec spec;
            Channel channel = Channel::Move;
            std::chrono::steady_clock::time_point deadline;
            std::vector<CommandDone> waiters;

            // Watcher state
            bool started = false;            // STARTED-worthy reply seen (SPELL_START, ATTACKSTART, ...)
            bool injectedOnce = false;
            uint8_t castCount = 0;
            bool channeled = false;
            bool cancelRequested = false;    // our CancelCast targets this cast
            uint32_t actualSpellId = 0;
            float lastX = 0, lastY = 0;
            std::chrono::steady_clock::time_point lastProgress;
            bool aliveAtStart = true;         // MoveTo/Attack/Cast fail with DIED only if the character dies
            // Interact
            std::shared_ptr<Interaction> interaction;
            uint32_t startCount = 0;          // SellItem: stack size before
            int questStatusAtStart = 0;       // AcceptQuest/AbandonQuest
            uint64_t lootGuid = 0;
            std::string lootFailure;          // first EQUIP_ERR_* while taking loot
        };

        struct CharacterCommands
        {
            std::unordered_map<uint8_t, std::shared_ptr<Command>> byChannel;
            uint8_t nextCastCount = 1;
        };

        static Channel ChannelOf(CommandType type);
        std::shared_ptr<Command> Begin(Player* player, CommandSpec const& spec, uint64_t id, ObjectGuid character, CommandDone onDone);
        void Finish(std::shared_ptr<Command> const& cmd, CommandStatus status, std::string reason);
        void HandlePacket(Player* player, CharacterCommands& cc, CapturedPacket const& packet);
        void KnockbackReflex(Player* player, CapturedPacket const& packet);
        void Poll(Player* player, Command& cmd);
        bool BeginInteraction(Player* player, std::shared_ptr<Command> const& cmd, std::string& failReason);
        void HandleInteractionPacket(Player* player, std::shared_ptr<Command> const& cmd, CapturedPacket const& packet, ByteBuffer& buf);
        void PollInteraction(Player* player, std::shared_ptr<Command> const& cmd);
        std::string DiagnoseNoResponse(Player* player, Command const& cmd) const;
        void Remember(CommandResult const& result);

        HeadlessRuntime& _runtime;
        bool _knockbackReflex = true;
        PacketObserver _packetObserver;
        ResultObserver _resultObserver;
        uint64_t _nextId = 1;
        std::unordered_map<ObjectGuid, CharacterCommands> _characters;
        std::unordered_map<uint64_t, std::shared_ptr<Command>> _active;
        std::deque<CommandResult> _recent;    // bounded history for late Wait() calls
        std::unordered_map<uint64_t, CommandResult> _recentById;
    };

    char const* CommandStatusName(CommandStatus status);
}

#endif
