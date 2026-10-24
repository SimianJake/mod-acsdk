#include "LoginThrottle.h"

#include <algorithm>

namespace acsdk
{
    LoginThrottle::Clock::duration LoginThrottle::DelayFor(uint32_t failures) const
    {
        if (failures < _free)
            return Clock::duration::zero();

        uint32_t shift = std::min<uint32_t>(failures - _free, 20);
        auto delay = std::chrono::duration_cast<Clock::duration>(_base) * (int64_t(1) << shift);
        return std::min<Clock::duration>(delay, _max);
    }

    std::chrono::milliseconds LoginThrottle::RetryAfter(std::vector<std::string> const& keys, Clock::time_point now) const
    {
        std::lock_guard lock(_mutex);
        Clock::duration wait = Clock::duration::zero();
        for (std::string const& key : keys)
        {
            auto it = _entries.find(key);
            if (it == _entries.end())
                continue;
            Clock::time_point until = it->second.lastFailure + DelayFor(it->second.failures);
            if (until > now)
                wait = std::max(wait, until - now);
        }
        return std::chrono::ceil<std::chrono::milliseconds>(wait);
    }

    void LoginThrottle::Failure(std::vector<std::string> const& keys, Clock::time_point now)
    {
        std::lock_guard lock(_mutex);
        if (_entries.size() >= _maxKeys)
            Prune(now);

        for (std::string const& key : keys)
        {
            if (_entries.size() >= _maxKeys && !_entries.contains(key))
                continue; // bounded memory; the peer key still throttles a flood of fresh usernames
            Entry& e = _entries[key];
            if (e.failures && now - e.lastFailure >= _forget)
                e.failures = 0;
            ++e.failures;
            e.lastFailure = now;
        }
    }

    void LoginThrottle::Success(std::string const& key)
    {
        std::lock_guard lock(_mutex);
        _entries.erase(key);
    }

    void LoginThrottle::Prune(Clock::time_point now)
    {
        for (auto it = _entries.begin(); it != _entries.end();)
            it = now - it->second.lastFailure >= _forget ? _entries.erase(it) : std::next(it);
    }
}
