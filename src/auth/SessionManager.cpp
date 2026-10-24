#include "SessionManager.h"

#include <openssl/evp.h>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <stdexcept>

namespace acsdk
{
    std::string SessionManager::HashToken(std::string const& token)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        if (EVP_Digest(token.data(), token.size(), digest, &size, EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("SHA-256 failed");
        return std::string(reinterpret_cast<char const*>(digest), size);
    }

    bool SessionManager::Expired(Session const& s, Clock::time_point now) const
    {
        Clock::time_point lastUsed{ Clock::duration(s.lastUsed.load(std::memory_order_relaxed)) };
        return now >= lastUsed + _idle || now >= s.identity.issuedAt + _max;
    }

    std::string SessionManager::Create(uint32_t accountId, std::string username, std::string label, bool admin,
                                       std::string verifierTag, Clock::time_point now, uint64_t* sessionId)
    {
        std::string token = _generate();
        std::string hash = HashToken(token);

        std::unique_lock lock(_mutex);
        if (auto it = _idByAccount.find(accountId); it != _idByAccount.end())
            if (auto h = _hashById.find(it->second); h != _hashById.end())
                EndLocked(_byHash.find(h->second), "REPLACED");

        auto session = std::make_unique<Session>();
        session->identity = Identity{ _nextId++, accountId, std::move(username), std::move(label), admin, now };
        session->verifierTag = std::move(verifierTag);
        session->lastUsed.store(now.time_since_epoch().count(), std::memory_order_relaxed);

        uint64_t id = session->identity.sessionId;
        _hashById[id] = hash;
        _idByAccount[accountId] = id;
        _byHash.emplace(std::move(hash), std::move(session));
        if (sessionId)
            *sessionId = id;
        return token;
    }

    std::optional<SessionManager::Identity> SessionManager::Resolve(std::string const& token, Clock::time_point now)
    {
        if (token.empty())
            return std::nullopt;

        // The lookup key is a hash of the secret, so the map lookup leaks nothing useful about the token.
        std::string hash = HashToken(token);
        std::shared_lock lock(_mutex);
        auto it = _byHash.find(hash);
        if (it == _byHash.end() || Expired(*it->second, now))
            return std::nullopt;

        it->second->lastUsed.store(now.time_since_epoch().count(), std::memory_order_relaxed);
        return it->second->identity;
    }

    bool SessionManager::IsActive(uint64_t sessionId, Clock::time_point now) const
    {
        std::shared_lock lock(_mutex);
        auto h = _hashById.find(sessionId);
        if (h == _hashById.end())
            return false;
        auto it = _byHash.find(h->second);
        return it != _byHash.end() && !Expired(*it->second, now);
    }

    std::optional<SessionManager::Clock::time_point> SessionManager::ExpiresAt(uint64_t sessionId) const
    {
        std::shared_lock lock(_mutex);
        auto h = _hashById.find(sessionId);
        if (h == _hashById.end())
            return std::nullopt;
        Session const& s = *_byHash.at(h->second);
        Clock::time_point lastUsed{ Clock::duration(s.lastUsed.load(std::memory_order_relaxed)) };
        return std::min(lastUsed + _idle, s.identity.issuedAt + _max);
    }

    void SessionManager::EndLocked(std::unordered_map<std::string, std::unique_ptr<Session>>::iterator it, std::string const& reason)
    {
        if (it == _byHash.end())
            return;

        Identity const& id = it->second->identity;
        _ended.push_back({ id.sessionId, id.accountId, id.username, reason });
        _hashById.erase(id.sessionId);
        if (auto a = _idByAccount.find(id.accountId); a != _idByAccount.end() && a->second == id.sessionId)
            _idByAccount.erase(a);
        _byHash.erase(it);
    }

    bool SessionManager::EndToken(std::string const& token, std::string const& reason)
    {
        std::string hash = HashToken(token);
        std::unique_lock lock(_mutex);
        auto it = _byHash.find(hash);
        if (it == _byHash.end())
            return false;
        EndLocked(it, reason);
        return true;
    }

    bool SessionManager::EndSession(uint64_t sessionId, std::string const& reason)
    {
        std::unique_lock lock(_mutex);
        auto h = _hashById.find(sessionId);
        if (h == _hashById.end())
            return false;
        EndLocked(_byHash.find(h->second), reason);
        return true;
    }

    std::size_t SessionManager::EndAccount(uint32_t accountId, std::string const& reason)
    {
        std::unique_lock lock(_mutex);
        auto a = _idByAccount.find(accountId);
        if (a == _idByAccount.end())
            return 0;
        auto h = _hashById.find(a->second);
        if (h == _hashById.end())
            return 0;
        EndLocked(_byHash.find(h->second), reason);
        return 1;
    }

    void SessionManager::Expire(Clock::time_point now)
    {
        std::unique_lock lock(_mutex);
        for (auto it = _byHash.begin(); it != _byHash.end();)
        {
            auto next = std::next(it);
            if (Expired(*it->second, now))
                EndLocked(it, "EXPIRED");
            it = next;
        }
    }

    std::vector<SessionManager::Ended> SessionManager::TakeEnded()
    {
        std::unique_lock lock(_mutex);
        std::vector<Ended> out;
        out.swap(_ended);
        return out;
    }

    std::vector<SessionManager::SessionInfo> SessionManager::List() const
    {
        std::shared_lock lock(_mutex);
        std::vector<SessionInfo> out;
        out.reserve(_byHash.size());
        for (auto const& [hash, s] : _byHash)
            out.push_back({ s->identity, Clock::time_point{ Clock::duration(s->lastUsed.load(std::memory_order_relaxed)) }, s->verifierTag });
        return out;
    }

    std::size_t SessionManager::Count() const
    {
        std::shared_lock lock(_mutex);
        return _byHash.size();
    }
}
