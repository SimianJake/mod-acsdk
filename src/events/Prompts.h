#ifndef ACSDK_PROMPTS_H
#define ACSDK_PROMPTS_H

#include "EventBus.h"
#include "HeadlessRuntime.h"
#include "ObjectGuid.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace acsdk
{
    // Server prompts a client would show a dialog for (group invite, trade, duel, resurrect, summon, loot roll,
    // ready check). PLAN.md §5.2: they are surfaced as Prompt events and the controller decides through
    // RespondToPrompt; an unanswered prompt is declined after ACSDK.Prompt.TimeoutSec (config-declared, not
    // a gameplay decision). World thread only.
    class PromptManager
    {
    public:
        PromptManager(HeadlessRuntime& runtime, EventBus& bus) : _runtime(runtime), _bus(bus) { }

        void SetTimeout(std::chrono::seconds timeout) { _timeout = timeout; }

        // A captured server packet for `character` (opcodes listed in PromptOpcodes()).
        void OnPacket(ObjectGuid character, CapturedPacket const& packet);

        // Expire unanswered prompts (auto-decline) and forget prompts of characters that went offline.
        void Update();

        enum class RespondResult { Ok, UnknownPrompt, WrongCharacter };
        RespondResult Respond(ObjectGuid character, uint64_t promptId, bool accept, uint32_t rollVote);

        static std::vector<uint16_t> PromptOpcodes();

    private:
        struct Prompt
        {
            uint64_t id = 0;
            ObjectGuid character;
            v1::PromptKind kind = v1::PROMPT_KIND_UNSPECIFIED;
            uint64_t fromGuid = 0;
            std::string fromName;
            uint64_t lootGuid = 0;
            uint32_t lootSlot = 0;
            uint32_t itemId = 0;
            uint32_t zoneId = 0;
            std::chrono::steady_clock::time_point deadline;
        };

        uint64_t Open(Prompt prompt);
        void Close(uint64_t id, char const* reason);
        void SendAnswer(Prompt const& p, bool accept, uint32_t rollVote);
        void PublishPrompt(Prompt const& p, bool closed, char const* reason);
        void CloseKind(ObjectGuid character, v1::PromptKind kind, char const* reason);

        HeadlessRuntime& _runtime;
        EventBus& _bus;
        std::chrono::seconds _timeout{60};
        uint64_t _nextId = 1;
        std::unordered_map<uint64_t, Prompt> _prompts;
    };
}

#endif
