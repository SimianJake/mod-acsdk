#ifndef ACSDK_HEADLESS_RUNTIME_H
#define ACSDK_HEADLESS_RUNTIME_H

#include "ObjectGuid.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class WorldPacket;
class WorldSession;
class Player;

namespace acsdk
{
    // PLAN.md §4.3. Online sub-flags (dead, in combat, ...) are read from the Player, not stored here.
    enum class Lifecycle : uint8_t
    {
        Offline = 0,
        LoggingIn,
        Online,
        LoggingOut,
    };

    char const* LifecycleName(Lifecycle state);

    // Copy of a server->"client" packet captured for a headless character (PLAN.md §4.5: only opcodes
    // somebody asked for are kept; everything else is dropped after reading the opcode).
    struct CapturedPacket
    {
        uint16_t opcode = 0;
        std::vector<uint8_t> bytes;
    };

    // Thread-safe sink for one character's outgoing packets. OnPacketSent can fire on any thread.
    class PacketSink
    {
    public:
        explicit PacketSink(std::size_t capacity) : _capacity(capacity) { }

        // Opcodes some other consumer (the EventBus) needs; consulted on every packet, any thread.
        using ExtraWants = bool (*)(uint16_t opcode);
        static void SetExtraWants(ExtraWants fn) { s_extraWants.store(fn, std::memory_order_relaxed); }

        void Want(uint16_t opcode);
        bool Wants(uint16_t opcode) const;
        void Push(uint16_t opcode, uint8_t const* data, std::size_t size);
        std::vector<CapturedPacket> Take();
        uint64_t Dropped() const;

    private:
        static inline std::atomic<ExtraWants> s_extraWants{nullptr};

        mutable std::mutex _mutex;
        std::unordered_set<uint16_t> _wanted;
        std::deque<CapturedPacket> _packets;
        std::size_t _capacity;
        uint64_t _dropped = 0;
    };

    struct LoginResult
    {
        bool ok = false;
        std::string reason;  // e.g. "CHARACTER_NOT_FOUND", "ACCOUNT_ALREADY_ONLINE"
        ObjectGuid guid;
        std::string name;
    };

    using LoginDone = std::function<void(LoginResult const&)>;
    using LogoutDone = std::function<void(bool ok, std::string const& reason)>;

    // One headless character: a WorldSession with no socket that mod-acsdk owns and pumps itself
    // (docs/research/headless-sessions.md §2). WorldSession has no virtual methods, so this wraps it.
    struct HeadlessCharacter
    {
        ObjectGuid guid;
        uint32_t accountId = 0;
        std::string name;
        Lifecycle state = Lifecycle::Offline;
        WorldSession* session = nullptr;
        std::shared_ptr<PacketSink> sink;
        bool sinkWired = false;        // CommandManager registered the opcodes it watches
        uint64_t loginId = 0;          // distinguishes async callbacks of successive logins
        bool loginCallbackDone = false;
        bool logoutRequested = false;
        std::chrono::steady_clock::time_point unleasedSince; // for ACSDK.Lease.LogoutOnOrphanSec
        std::vector<LoginDone> loginWaiters;
        std::vector<LogoutDone> logoutWaiters;
    };

    // Owns all headless characters. World thread only, except IsHeadless()/SinkFor(), which the
    // packet hook calls from any thread.
    class HeadlessRuntime
    {
    public:
        HeadlessRuntime();
        ~HeadlessRuntime();

        void Configure(std::string accountPrefix, uint32_t maxCharacters, std::size_t sinkCapacity);

        // World thread. `done` is always called exactly once, on the world thread.
        void Login(std::string const& name, LoginDone done);
        void Logout(ObjectGuid guid, LogoutDone done);

        // World thread, every tick (WorldScript::OnUpdate).
        void Update();

        // World thread, WorldScript::OnShutdown: log every headless character out.
        void ShutdownAll();

        // World thread.
        HeadlessCharacter* Find(ObjectGuid guid);
        Player* FindOnlinePlayer(ObjectGuid guid);
        bool Inject(ObjectGuid guid, std::unique_ptr<WorldPacket> packet);
        std::vector<HeadlessCharacter const*> All() const;
        bool HoldsAccount(uint32_t accountId) const;

        // Any thread.
        bool IsHeadless(WorldSession const* session) const;
        std::shared_ptr<PacketSink> SinkFor(WorldSession const* session) const;

    private:
        void StartSessionAndLoad(ObjectGuid guid, uint64_t loginId, uint32_t accountId, std::string accountName,
                                 uint8_t security, uint8_t expansion, int64_t muteTime, uint8_t locale);
        void FinishLogin(HeadlessCharacter& c, LoginResult const& result);
        void DrainInjectedPackets(HeadlessCharacter& c);
        void RunReflexes(HeadlessCharacter& c);
        void DestroySession(HeadlessCharacter& c, bool save);
        void Remove(ObjectGuid guid);

        std::unordered_map<ObjectGuid, std::unique_ptr<HeadlessCharacter>> _characters;
        uint64_t _nextLoginId = 1;
        bool _stopping = false;

        std::string _accountPrefix;
        uint32_t _maxCharacters = 50;
        std::size_t _sinkCapacity = 256;

        // Read by the packet hook on map threads; written on the world thread.
        mutable std::shared_mutex _sessionsMutex;
        std::unordered_map<WorldSession const*, std::shared_ptr<PacketSink>> _sessions;
    };
}

#endif
