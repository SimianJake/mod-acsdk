#ifndef ACSDK_ACCOUNT_AUTH_H
#define ACSDK_ACCOUNT_AUTH_H

#include "DatabaseEnvFwd.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acsdk
{
    struct Config;

    // The auth-DB facts an SDK login depends on (ARCHITECTURE.md §4.3). Read by AuthService.Login and by the
    // periodic session recheck with the same query, so both apply the same rules.
    struct AccountRow
    {
        uint32_t id = 0;
        std::string username;
        std::vector<uint8_t> salt;
        std::vector<uint8_t> verifier;
        bool locked = false;  // account.locked (AzerothCore's IP lock)
        bool banned = false;  // an active, unexpired account_banned row
        uint32_t gmLevel = 0; // highest account_access.gmlevel for this realm or all realms
    };

    // SELECT for AccountRow columns; `where` is an already-escaped condition on table alias `a`.
    std::string AccountRowSql(std::string const& where);
    AccountRow ReadAccountRow(Field* fields);

    // True if `username` starts with ACSDK.HeadlessAccountPrefix (case-insensitive); an empty prefix allows any.
    bool MatchesAccountPrefix(std::string username, std::string const& prefix);

    // Why this account may not hold an SDK session (BANNED, LOCKED, GM_ACCOUNT, ACCOUNT_NOT_ELIGIBLE),
    // or "" if it may.
    std::string AccountRefusal(AccountRow const& row, Config const& config);
    bool IsAdmin(AccountRow const& row, Config const& config);

    // SRP6 check with AzerothCore's own parameters (v = g ^ H(s | H(USER:PASS)) mod N, as SRP6::CheckLogin),
    // compared in constant time. Does the same work for malformed rows, then returns false. Both names must
    // already be upper-cased (Utf8ToUpperOnlyLatin).
    bool VerifyPassword(std::string const& username, std::string const& password,
                        std::vector<uint8_t> const& salt, std::vector<uint8_t> const& verifier);

    // Fingerprint of the account's salt and verifier at login; a different one later means the password changed.
    std::string VerifierTag(AccountRow const& row);

    // World thread: re-read every live session's account (async) and end the sessions whose account was
    // deleted, banned, locked, changed password or GM level, or no longer matches the prefix.
    void RecheckSessions();
}

#endif
