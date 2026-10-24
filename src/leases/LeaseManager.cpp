#include "LeaseManager.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace acsdk
{
    std::string GenerateLeaseToken()
    {
        unsigned char bytes[32];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            throw std::runtime_error("RAND_bytes failed"); // never fall back to a weak RNG

        static constexpr char hex[] = "0123456789abcdef";
        std::string token;
        token.reserve(sizeof(bytes) * 2);
        for (unsigned char b : bytes)
        {
            token += hex[b >> 4];
            token += hex[b & 0x0F];
        }
        return token;
    }

    bool LeaseManager::ConstantTimeEquals(std::string const& a, std::string const& b)
    {
        // Length is not secret (every token has the same length); contents are compared in constant time.
        if (a.size() != b.size() || a.empty())
            return false;
        return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    LeaseManager::Result LeaseManager::Claim(uint64_t guid, std::string const& holderLabel, bool force, Clock::time_point now, Grant& out)
    {
        out = Grant{};
        auto it = _leases.find(guid);
        if (it != _leases.end() && it->second.expiresAt > now)
        {
            if (!force || !_allowForceTakeover)
                return Result::AlreadyLeased;

            out.previousHolder = it->second.holderLabel; // old token is invalidated below
        }

        Lease lease{ _generate(), holderLabel, now + _ttl };
        out.token = lease.token;
        out.expiresAt = lease.expiresAt;
        _leases[guid] = std::move(lease);
        return Result::Ok;
    }

    LeaseManager::Result LeaseManager::Release(uint64_t guid, std::string const& token, Clock::time_point now)
    {
        if (!Validate(guid, token, now))
            return _leases.contains(guid) ? Result::InvalidToken : Result::NotLeased;

        _leases.erase(guid);
        return Result::Ok;
    }

    LeaseManager::Result LeaseManager::Renew(uint64_t guid, std::string const& token, Clock::time_point now, Clock::time_point& expiresAt)
    {
        if (!Validate(guid, token, now))
            return _leases.contains(guid) ? Result::InvalidToken : Result::NotLeased;

        Lease& lease = _leases[guid];
        lease.expiresAt = now + _ttl;
        expiresAt = lease.expiresAt;
        return Result::Ok;
    }

    bool LeaseManager::Validate(uint64_t guid, std::string const& token, Clock::time_point now) const
    {
        auto it = _leases.find(guid);
        if (it == _leases.end() || it->second.expiresAt <= now)
            return false;
        return ConstantTimeEquals(it->second.token, token);
    }

    bool LeaseManager::IsLeased(uint64_t guid, Clock::time_point now) const
    {
        auto it = _leases.find(guid);
        return it != _leases.end() && it->second.expiresAt > now;
    }

    std::optional<LeaseManager::LeaseInfo> LeaseManager::Get(uint64_t guid, Clock::time_point now) const
    {
        auto it = _leases.find(guid);
        if (it == _leases.end() || it->second.expiresAt <= now)
            return std::nullopt;
        return LeaseInfo{ guid, it->second.holderLabel, it->second.expiresAt };
    }

    std::vector<LeaseManager::LeaseInfo> LeaseManager::List(Clock::time_point now) const
    {
        std::vector<LeaseInfo> out;
        for (auto const& [guid, lease] : _leases)
            if (lease.expiresAt > now)
                out.push_back({ guid, lease.holderLabel, lease.expiresAt });
        return out;
    }

    std::vector<uint64_t> LeaseManager::Expire(Clock::time_point now)
    {
        std::vector<uint64_t> expired;
        for (auto it = _leases.begin(); it != _leases.end();)
        {
            if (it->second.expiresAt <= now)
            {
                expired.push_back(it->first);
                it = _leases.erase(it);
            }
            else
                ++it;
        }
        return expired;
    }

    void LeaseManager::Forget(uint64_t guid)
    {
        _leases.erase(guid);
    }
}
