#ifndef ACSDK_LOGIN_THROTTLE_H
#define ACSDK_LOGIN_THROTTLE_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acsdk
{
    // In-memory backoff for failed AuthService.Login attempts, keyed by username and by peer address
    // (ARCHITECTURE.md §4.3). Never touches the auth database. Pure logic; thread-safe.
    //
    // The first `freeAttempts` failures of a key cost nothing; after that each further attempt must wait
    // baseDelay * 2^(failures - freeAttempts), capped at maxDelay, counted from the last failure. A key is
    // forgotten after `forgetAfter` without failures.
    class LoginThrottle
    {
    public:
        using Clock = std::chrono::steady_clock;

        LoginThrottle(uint32_t freeAttempts = 3, std::chrono::seconds baseDelay = std::chrono::seconds(1),
                      std::chrono::seconds maxDelay = std::chrono::seconds(300),
                      std::chrono::seconds forgetAfter = std::chrono::seconds(900), std::size_t maxKeys = 10000)
            : _free(freeAttempts), _base(baseDelay), _max(maxDelay), _forget(forgetAfter), _maxKeys(maxKeys) { }

        // How long the caller must still wait before trying any of `keys`; zero means go ahead.
        std::chrono::milliseconds RetryAfter(std::vector<std::string> const& keys, Clock::time_point now) const;
        void Failure(std::vector<std::string> const& keys, Clock::time_point now);
        void Success(std::string const& key);

    private:
        struct Entry
        {
            uint32_t failures = 0;
            Clock::time_point lastFailure;
        };

        Clock::duration DelayFor(uint32_t failures) const;
        void Prune(Clock::time_point now);

        uint32_t _free;
        std::chrono::seconds _base;
        std::chrono::seconds _max;
        std::chrono::seconds _forget;
        std::size_t _maxKeys;

        mutable std::mutex _mutex;
        std::unordered_map<std::string, Entry> _entries;
    };
}

#endif
