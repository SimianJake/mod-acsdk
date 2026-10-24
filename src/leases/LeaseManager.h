#ifndef ACSDK_LEASE_MANAGER_H
#define ACSDK_LEASE_MANAGER_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acsdk
{
    // Per-character controller ownership (PLAN.md §4.4). Pure logic over character guids (no game types),
    // so it can be unit tested. World thread only.
    //
    // Tokens are bearer capabilities: >=128 bits from a CSPRNG, compared in constant time, never logged and
    // never returned to anyone but the claimant.
    class LeaseManager
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TokenGenerator = std::function<std::string()>;

        enum class Result
        {
            Ok,
            AlreadyLeased,      // held by someone else and takeover not requested/allowed
            InvalidToken,       // missing, wrong or expired
            NotLeased,
        };

        struct Grant
        {
            std::string token;
            Clock::time_point expiresAt;
            std::optional<std::string> previousHolder; // set when a forced takeover replaced someone
        };

        struct LeaseInfo
        {
            uint64_t characterGuid = 0;
            std::string holderLabel;
            Clock::time_point expiresAt;
        };

        LeaseManager(TokenGenerator generator, std::chrono::seconds ttl, bool allowForceTakeover)
            : _generate(std::move(generator)), _ttl(ttl), _allowForceTakeover(allowForceTakeover) { }

        Result Claim(uint64_t guid, std::string const& holderLabel, bool force, Clock::time_point now, Grant& out);
        Result Release(uint64_t guid, std::string const& token, Clock::time_point now);
        Result Renew(uint64_t guid, std::string const& token, Clock::time_point now, Clock::time_point& expiresAt);

        // True only for the current, unexpired token of `guid`.
        bool Validate(uint64_t guid, std::string const& token, Clock::time_point now) const;
        bool IsLeased(uint64_t guid, Clock::time_point now) const;
        std::optional<LeaseInfo> Get(uint64_t guid, Clock::time_point now) const;
        std::vector<LeaseInfo> List(Clock::time_point now) const;

        // Removes leases whose TTL ran out; returns the affected characters.
        std::vector<uint64_t> Expire(Clock::time_point now);
        // Drop a lease without a token (character logged out / removed).
        void Forget(uint64_t guid);

        std::chrono::seconds Ttl() const { return _ttl; }

        static bool ConstantTimeEquals(std::string const& a, std::string const& b);

    private:
        struct Lease
        {
            std::string token;
            std::string holderLabel;
            Clock::time_point expiresAt;
        };

        TokenGenerator _generate;
        std::chrono::seconds _ttl;
        bool _allowForceTakeover;
        std::unordered_map<uint64_t, Lease> _leases;
    };

    // 32 random bytes from OpenSSL's CSPRNG, hex encoded (256 bits).
    std::string GenerateLeaseToken();
}

#endif
