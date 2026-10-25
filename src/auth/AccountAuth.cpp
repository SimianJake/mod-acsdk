#include "AccountAuth.h"

#include "AcsdkModule.h"

#include "BigNumber.h"
#include "CryptoHash.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "QueryResult.h"
#include "Realm.h"
#include "SRP6.h"
#include "StringFormat.h"
#include "World.h"

#include <openssl/crypto.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace acsdk
{
    std::string AccountRowSql(std::string const& where)
    {
        return Acore::StringFormat(
            "SELECT a.id, a.username, a.salt, a.verifier, a.locked, "
            "CAST(EXISTS(SELECT 1 FROM account_banned ab WHERE ab.id = a.id AND ab.active = 1 AND (ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate)) AS UNSIGNED), "
            "CAST(COALESCE((SELECT MAX(aa.gmlevel) FROM account_access aa WHERE aa.id = a.id AND (aa.RealmID = -1 OR aa.RealmID = {})), 0) AS UNSIGNED) "
            "FROM account a WHERE {}",
            realm.Id.Realm, where);
    }

    AccountRow ReadAccountRow(Field* fields)
    {
        AccountRow row;
        row.id = fields[0].Get<uint32>();
        row.username = fields[1].Get<std::string>();
        row.salt = fields[2].Get<Binary>();
        row.verifier = fields[3].Get<Binary>();
        row.locked = fields[4].Get<uint8>() != 0;
        row.banned = fields[5].Get<uint64>() != 0;
        row.gmLevel = static_cast<uint32_t>(fields[6].Get<uint64>());
        return row;
    }

    bool MatchesAccountPrefix(std::string username, std::string const& prefix)
    {
        std::transform(username.begin(), username.end(), username.begin(), ::toupper);
        return prefix.empty() || username.rfind(prefix, 0) == 0;
    }

    std::string AccountRefusal(AccountRow const& row, Config const& config)
    {
        if (row.banned)
            return "BANNED";
        // AzerothCore's 'locked' pins an account to its last IP. The SDK has no client IP to compare, so a locked
        // account is refused outright.
        if (row.locked)
            return "LOCKED";
        if (row.gmLevel > 0 && !config.authAllowGmAccounts)
            return "GM_ACCOUNT";
        if (!MatchesAccountPrefix(row.username, config.headlessAccountPrefix))
            return "ACCOUNT_NOT_ELIGIBLE";
        return "";
    }

    bool IsAdmin(AccountRow const& row, Config const& config)
    {
        return row.gmLevel >= config.authAdminGmLevel;
    }

    bool VerifyPassword(std::string const& username, std::string const& password,
                        std::vector<uint8_t> const& salt, std::vector<uint8_t> const& verifier)
    {
        using Acore::Crypto::SHA1;
        using Acore::Crypto::SRP6;

        bool wellFormed = salt.size() == SRP6::SALT_LENGTH && verifier.size() == SRP6::VERIFIER_LENGTH;
        std::array<uint8, SRP6::SALT_LENGTH> s{};
        std::array<uint8, SRP6::VERIFIER_LENGTH> v{};
        if (wellFormed)
        {
            std::copy(salt.begin(), salt.end(), s.begin());
            std::copy(verifier.begin(), verifier.end(), v.begin());
        }

        // Same computation as SRP6::CalculateVerifier (private there), so the comparison can be constant-time.
        BigNumber x(SHA1::GetDigestOf(s, SHA1::GetDigestOf(username, ":", password)));
        SRP6::Verifier computed = BigNumber(SRP6::g).ModExp(x, BigNumber(SRP6::N)).ToByteArray<SRP6::VERIFIER_LENGTH>();
        bool equal = CRYPTO_memcmp(computed.data(), v.data(), computed.size()) == 0;
        return wellFormed && equal;
    }

    std::string VerifierTag(AccountRow const& row)
    {
        std::string material(row.salt.begin(), row.salt.end());
        material.append(row.verifier.begin(), row.verifier.end());
        return SessionManager::HashToken(material);
    }

    void RecheckSessions()
    {
        std::vector<SessionManager::SessionInfo> sessions = sAcsdk.Sessions().List();
        if (sessions.empty())
            return;

        std::string ids;
        for (SessionManager::SessionInfo const& s : sessions)
        {
            if (!ids.empty())
                ids += ',';
            ids += std::to_string(s.identity.accountId);
        }

        sAcsdk.Queries().AddCallback(LoginDatabase.AsyncQuery(AccountRowSql("a.id IN (" + ids + ")")).WithCallback(
            [sessions = std::move(sessions)](QueryResult result)
            {
                std::unordered_map<uint32_t, AccountRow> rows;
                if (result)
                {
                    do
                    {
                        AccountRow row = ReadAccountRow(result->Fetch());
                        rows.emplace(row.id, std::move(row));
                    } while (result->NextRow());
                }

                Config const& config = sAcsdk.GetConfig();
                for (SessionManager::SessionInfo const& s : sessions)
                {
                    std::string reason;
                    auto it = rows.find(s.identity.accountId);
                    if (it == rows.end())
                        reason = "DELETED";
                    else if (std::string refusal = AccountRefusal(it->second, config); !refusal.empty())
                        reason = refusal;
                    else if (VerifierTag(it->second) != s.verifierTag)
                        reason = "PASSWORD_CHANGED";
                    else if (IsAdmin(it->second, config) != s.identity.admin)
                        reason = "GM_LEVEL_CHANGED";

                    // EndSession is a no-op if the session ended (or was replaced) while the query ran.
                    if (!reason.empty())
                        sAcsdk.Sessions().EndSession(s.identity.sessionId, "REVOKED_" + reason);
                }
            }));
    }
}
