#ifndef ACSDK_CONFIG_H
#define ACSDK_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace acsdk
{
    // Values from mod_acsdk.conf (PLAN.md §8). Loaded on the world thread; gRPC threads only read the
    // copy made before the listener starts.
    struct Config
    {
        bool enable = true;
        std::string bindAddress = "127.0.0.1";
        uint16_t port = 50051;
        std::string authToken;
        uint32_t maxCommandsPerTick = 200;
        uint32_t maxTickBudgetMs = 5;
        uint32_t commandQueueSize = 4096;
        std::string headlessAccountPrefix = "ACSDKHC"; // AzerothCore stores usernames upper-case
        uint32_t maxListResults = 1000;
        uint32_t maxHeadlessCharacters = 50;
        bool reflexTeleportAck = true;
        bool reflexKnockback = true;
        bool debugService = false;              // Phase 0 Spike B helpers (DebugService); dev only
        uint32_t packetSinkSize = 256;          // captured packets kept per character
        uint32_t leaseTtlSec = 15;
        bool leaseAllowForceTakeover = true;
        uint32_t leaseLogoutOnOrphanSec = 0;    // 0 = never log out unleased characters
        std::vector<std::string> autostartCharacters;
        uint32_t eventQueueSize = 4096;         // per subscriber; overflow drops oldest + Lagged
        uint32_t promptTimeoutSec = 60;         // unanswered prompts are declined (PLAN.md §5.2)
        bool rawPacketsEnable = false;          // §4.6.4 escape hatch (SendClientPacket + RAW_PACKET events)
        std::vector<uint16_t> rawPacketsAllow;  // client opcodes SendClientPacket may send; empty = any not denied
        uint32_t rawPacketsPerSecond = 20;      // per character

        static Config Load();
    };
}

#endif
