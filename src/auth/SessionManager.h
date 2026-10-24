#ifndef ACSDK_SESSION_MANAGER_H
#define ACSDK_SESSION_MANAGER_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acsdk
{
    // SDK sessions of AzerothCore accounts (ARCHITECTURE.md §4.3). Pure logic over account ids (no game
    // types), so it can be unit tested.
    //
    // Thread-safe: gRPC threads resolve tokens under a shared lock; Create/End/Expire take it exclusively.
    // Tokens are 256-bit bearer secrets; only their SHA-256 is kept, so they are never stored or logged.
    // One session per account: a new Create ends the account's previous session.
    class SessionManager
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TokenGenerator = std::function<std::string()>;

        // What an RPC handler knows about its caller.
        struct Identity
        {
            uint64_t sessionId = 0;
            uint32_t accountId = 0;
            std::string username;
            std::string label;
            bool admin = false;
            Clock::time_point issuedAt;
        };

        struct SessionInfo
        {
            Identity identity;
            Clock::time_point lastUsedAt;
            std::string verifierTag; // fingerprint of the account's verifier at login (revocation recheck)
        };

        struct Ended
        {
            uint64_t sessionId = 0;
            uint32_t accountId = 0;
            std::string username;
            std::string reason; // LOGOUT, REPLACED, EXPIRED, REVOKED, BANNED, LOCKED, DELETED, PASSWORD_CHANGED, ...
        };

        SessionManager(TokenGenerator generator, std::chrono::seconds idleTimeout, std::chrono::seconds maxLifetime)
            : _generate(std::move(generator)), _idle(idleTimeout), _max(maxLifetime) { }

        // Starts a session and returns its token. Ends the account's previous session (reason REPLACED).
        std::string Create(uint32_t accountId, std::string username, std::string label, bool admin,
                           std::string verifierTag, Clock::time_point now, uint64_t* sessionId = nullptr);

        // The live session for `token`; refreshes its idle timer. nullopt if unknown, ended or expired.
        std::optional<Identity> Resolve(std::string const& token, Clock::time_point now);

        bool IsActive(uint64_t sessionId, Clock::time_point now) const;
        // When the session ends unless it is used again: the earlier of the idle and absolute limits.
        std::optional<Clock::time_point> ExpiresAt(uint64_t sessionId) const;

        bool EndToken(std::string const& token, std::string const& reason);
        bool EndSession(uint64_t sessionId, std::string const& reason);
        // Returns the number of sessions ended (0 or 1).
        std::size_t EndAccount(uint32_t accountId, std::string const& reason);
        // Ends sessions past their idle or absolute limit.
        void Expire(Clock::time_point now);

        // Sessions ended since the last call, for the world thread to release their leases and end their streams.
        std::vector<Ended> TakeEnded();
        std::vector<SessionInfo> List() const;
        std::size_t Count() const;

        std::chrono::seconds IdleTimeout() const { return _idle; }
        std::chrono::seconds MaxLifetime() const { return _max; }

        static std::string HashToken(std::string const& token);

    private:
        struct Session
        {
            Identity identity;
            std::string verifierTag;
            std::atomic<Clock::rep> lastUsed{0}; // updated under the shared lock
        };

        bool Expired(Session const& s, Clock::time_point now) const;
        // Exclusive lock held.
        void EndLocked(std::unordered_map<std::string, std::unique_ptr<Session>>::iterator it, std::string const& reason);

        TokenGenerator _generate;
        std::chrono::seconds _idle;
        std::chrono::seconds _max;

        mutable std::shared_mutex _mutex;
        std::unordered_map<std::string, std::unique_ptr<Session>> _byHash; // SHA-256(token) -> session
        std::unordered_map<uint64_t, std::string> _hashById;
        std::unordered_map<uint32_t, uint64_t> _idByAccount;
        std::vector<Ended> _ended;
        uint64_t _nextId = 1;
    };
}

#endif
