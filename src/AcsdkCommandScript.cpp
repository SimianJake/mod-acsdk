/*
 * mod-acsdk GM/console commands (ARCHITECTURE.md §4.3):
 *   .acsdk session list               live SDK sessions (account, label, age, idle time; never tokens)
 *   .acsdk session revoke <account>   end an account's session (name or id); its leases and streams end
 * Administrator level; both work from the worldserver console.
 */

#include "AcsdkModule.h"

#include "Chat.h"
#include "CommandScript.h"
#include "Common.h"

#include <algorithm>
#include <cctype>

using namespace Acore::ChatCommands;

namespace
{
    long long SecondsSince(acsdk::SessionManager::Clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::seconds>(acsdk::SessionManager::Clock::now() - t).count();
    }

    class acsdk_commandscript : public CommandScript
    {
    public:
        acsdk_commandscript() : CommandScript("acsdk_commandscript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable sessionTable =
            {
                { "list",   HandleSessionList,   SEC_ADMINISTRATOR, Console::Yes },
                { "revoke", HandleSessionRevoke, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable acsdkTable =
            {
                { "session", sessionTable },
            };
            static ChatCommandTable commandTable =
            {
                { "acsdk", acsdkTable },
            };
            return commandTable;
        }

        static bool HandleSessionList(ChatHandler* handler)
        {
            std::vector<acsdk::SessionManager::SessionInfo> sessions = sAcsdk.Sessions().List();
            if (sessions.empty())
            {
                handler->SendSysMessage("No SDK sessions.");
                return true;
            }

            std::sort(sessions.begin(), sessions.end(), [](auto const& a, auto const& b) { return a.identity.username < b.identity.username; });
            handler->PSendSysMessage("{} SDK session(s):", sessions.size());
            for (acsdk::SessionManager::SessionInfo const& s : sessions)
                handler->PSendSysMessage("  {} (account {}){} label '{}' session {} age {}s idle {}s",
                    s.identity.username, s.identity.accountId, s.identity.admin ? " [admin]" : "", s.identity.label,
                    s.identity.sessionId, SecondsSince(s.identity.issuedAt), SecondsSince(s.lastUsedAt));
            return true;
        }

        static bool HandleSessionRevoke(ChatHandler* handler, std::string account)
        {
            // Match a live session by account name (any case) or id; no DB lookup needed.
            std::string upper = account;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            uint32 accountId = 0;
            for (acsdk::SessionManager::SessionInfo const& s : sAcsdk.Sessions().List())
                if (s.identity.username == upper || std::to_string(s.identity.accountId) == account)
                    accountId = s.identity.accountId;

            if (!accountId || !sAcsdk.Sessions().EndAccount(accountId, "REVOKED"))
            {
                handler->PSendSysMessage("No SDK session for account '{}'.", account);
                handler->SetSentErrorMessage(true);
                return false;
            }

            handler->PSendSysMessage("Ended the SDK session of account '{}'; its leases and event streams end on the next tick.", account);
            return true;
        }
    };
}

void AddSC_acsdk_commands()
{
    new acsdk_commandscript();
}
