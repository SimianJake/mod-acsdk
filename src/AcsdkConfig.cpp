#include "AcsdkConfig.h"

#include "Config.h"
#include "Define.h"
#include "Tokenize.h"

#include <algorithm>
#include <cctype>

namespace acsdk
{
    Config Config::Load()
    {
        Config c;
        c.enable = sConfigMgr->GetOption<bool>("ACSDK.Enable", true);
        c.bindAddress = sConfigMgr->GetOption<std::string>("ACSDK.BindAddress", "127.0.0.1");
        c.port = static_cast<uint16_t>(sConfigMgr->GetOption<uint32>("ACSDK.Port", 50051));
        c.authToken = sConfigMgr->GetOption<std::string>("ACSDK.AuthToken", "", false);
        c.maxCommandsPerTick = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.MaxCommandsPerTick", 200));
        c.maxTickBudgetMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.MaxTickBudgetMs", 5));
        c.commandQueueSize = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.CommandQueueSize", 4096));
        c.headlessAccountPrefix = sConfigMgr->GetOption<std::string>("ACSDK.HeadlessAccountPrefix", "acsdkhc");
        std::transform(c.headlessAccountPrefix.begin(), c.headlessAccountPrefix.end(), c.headlessAccountPrefix.begin(), ::toupper);
        c.maxListResults = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.MaxListResults", 1000));
        c.maxHeadlessCharacters = sConfigMgr->GetOption<uint32>("ACSDK.MaxHeadlessCharacters", 50);
        c.reflexTeleportAck = sConfigMgr->GetOption<bool>("ACSDK.Reflex.TeleportAck", true);
        c.reflexKnockback = sConfigMgr->GetOption<bool>("ACSDK.Reflex.Knockback", true);
        c.debugService = sConfigMgr->GetOption<bool>("ACSDK.Debug.Enable", false);
        c.packetSinkSize = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.PacketSinkSize", 256));
        c.leaseTtlSec = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.Lease.TtlSec", 15));
        c.leaseAllowForceTakeover = sConfigMgr->GetOption<bool>("ACSDK.Lease.AllowForceTakeover", true);
        c.leaseLogoutOnOrphanSec = sConfigMgr->GetOption<uint32>("ACSDK.Lease.LogoutOnOrphanSec", 0);
        std::string const autostart = sConfigMgr->GetOption<std::string>("ACSDK.Autostart.Characters", "");
        for (std::string_view name : Acore::Tokenize(autostart, ',', false))
        {
            std::string trimmed(name);
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
            if (!trimmed.empty())
                c.autostartCharacters.push_back(std::move(trimmed));
        }
        c.eventQueueSize = std::max<uint32>(16, sConfigMgr->GetOption<uint32>("ACSDK.EventQueueSize", 4096));
        c.promptTimeoutSec = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.Prompt.TimeoutSec", 60));
        c.rawPacketsEnable = sConfigMgr->GetOption<bool>("ACSDK.RawPackets.Enable", false);
        std::string const allow = sConfigMgr->GetOption<std::string>("ACSDK.RawPackets.Allow", "");
        for (std::string_view op : Acore::Tokenize(allow, ',', false))
        {
            std::string s(op);
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            if (!s.empty())
                c.rawPacketsAllow.push_back(static_cast<uint16_t>(std::stoul(s, nullptr, 0)));
        }
        c.rawPacketsPerSecond = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("ACSDK.RawPackets.PerSecond", 20));
        return c;
    }
}
