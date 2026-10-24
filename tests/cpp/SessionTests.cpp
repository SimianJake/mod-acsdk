// Unit tests for account sessions (ARCHITECTURE.md §4.3): SessionManager, LoginThrottle and the binding of
// leases to the session that claimed them. Built as acsdk-session-tests (mod-acsdk.cmake, -DACSDK_BUILD_TESTS=ON).

#include "LeaseManager.h"
#include "LoginThrottle.h"
#include "SessionManager.h"

#include <cstdio>
#include <string>

using namespace acsdk;
using namespace std::chrono_literals;

static int failures = 0;

static void Expect(char const* name, bool ok)
{
    if (!ok)
    {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}

static std::string Counter()
{
    static int n = 0;
    return "token-" + std::to_string(++n);
}

static void SessionBasics()
{
    SessionManager sm(Counter, 60s, 600s);
    auto t0 = SessionManager::Clock::time_point{} + 1000s;

    uint64_t idA = 0;
    std::string a = sm.Create(7, "ALICE", "bot-1", false, "tag", t0, &idA);
    auto who = sm.Resolve(a, t0 + 1s);
    Expect("resolve/known", who && who->accountId == 7 && who->username == "ALICE" && who->label == "bot-1" && who->sessionId == idA);
    Expect("resolve/unknown", !sm.Resolve("nope", t0));
    Expect("resolve/empty", !sm.Resolve("", t0));
    Expect("active", sm.IsActive(idA, t0 + 1s));

    // Tokens are stored hashed: listing exposes no token.
    auto list = sm.List();
    Expect("list/one", list.size() == 1 && list[0].identity.sessionId == idA && list[0].verifierTag == "tag");

    // Sliding idle timeout: each use pushes expiry out.
    Expect("idle/used-at-59", bool(sm.Resolve(a, t0 + 59s)));
    Expect("idle/still-ok-at-118", bool(sm.Resolve(a, t0 + 118s)));
    Expect("idle/expired-after-60s-unused", !sm.Resolve(a, t0 + 178s));
    Expect("idle/inactive", !sm.IsActive(idA, t0 + 178s));
    sm.Expire(t0 + 178s);
    auto ended = sm.TakeEnded();
    Expect("expire/ended", ended.size() == 1 && ended[0].sessionId == idA && ended[0].reason == "EXPIRED" && ended[0].accountId == 7);
    Expect("expire/taken-once", sm.TakeEnded().empty());
    Expect("expire/gone", sm.Count() == 0);
}

static void AbsoluteLifetime()
{
    SessionManager sm(Counter, 60s, 120s);
    auto t0 = SessionManager::Clock::time_point{} + 1000s;
    std::string a = sm.Create(1, "A", "", false, "", t0);
    for (int s = 30; s < 120; s += 30)
        Expect("max/kept-alive", bool(sm.Resolve(a, t0 + std::chrono::seconds(s))));
    Expect("max/cap-reached", !sm.Resolve(a, t0 + 120s));
    auto exp = sm.ExpiresAt(1);
    Expect("max/expires-at-cap", exp && *exp == t0 + 120s);
}

static void OneSessionPerAccount()
{
    SessionManager sm(Counter, 60s, 600s);
    auto t0 = SessionManager::Clock::time_point{} + 1000s;
    uint64_t first = 0, second = 0, other = 0;
    std::string a1 = sm.Create(5, "A", "", false, "", t0, &first);
    std::string b = sm.Create(6, "B", "", false, "", t0, &other);
    std::string a2 = sm.Create(5, "A", "", false, "", t0 + 1s, &second);

    Expect("replace/old-token-dead", !sm.Resolve(a1, t0 + 2s));
    Expect("replace/new-token-live", bool(sm.Resolve(a2, t0 + 2s)));
    Expect("replace/other-account-untouched", bool(sm.Resolve(b, t0 + 2s)));
    auto ended = sm.TakeEnded();
    Expect("replace/reported", ended.size() == 1 && ended[0].sessionId == first && ended[0].reason == "REPLACED");

    Expect("logout/token", sm.EndToken(a2, "LOGOUT") && !sm.Resolve(a2, t0 + 3s));
    Expect("logout/twice", !sm.EndToken(a2, "LOGOUT"));
    Expect("revoke/account", sm.EndAccount(6, "REVOKED") == 1 && !sm.Resolve(b, t0 + 3s));
    Expect("revoke/none-left", sm.EndAccount(6, "REVOKED") == 0);
    ended = sm.TakeEnded();
    Expect("ended/two", ended.size() == 2 && ended[0].reason == "LOGOUT" && ended[1].reason == "REVOKED" && ended[1].sessionId == other);
    Expect("end-session/unknown", !sm.EndSession(second, "REVOKED"));
}

static void Throttle()
{
    LoginThrottle t(3, 1s, 8s, 900s);
    auto t0 = LoginThrottle::Clock::time_point{} + 1000s;
    std::vector<std::string> keys{ "user:A", "peer:x" };

    for (int i = 0; i < 3; ++i)
    {
        Expect("throttle/free", t.RetryAfter(keys, t0).count() == 0);
        t.Failure(keys, t0);
    }
    // 3 failures (free) -> 1 s, then 2 s, 4 s, capped at 8 s.
    Expect("throttle/1s", t.RetryAfter(keys, t0).count() == 1000);
    Expect("throttle/1s-elapsed", t.RetryAfter(keys, t0 + 1s).count() == 0);
    t.Failure(keys, t0 + 1s);
    Expect("throttle/2s", t.RetryAfter(keys, t0 + 1s).count() == 2000);
    for (int i = 0; i < 5; ++i)
        t.Failure(keys, t0 + 2s);
    Expect("throttle/capped", t.RetryAfter(keys, t0 + 2s).count() == 8000);

    // A different user from the same peer is still held back by the peer key.
    Expect("throttle/peer-key", t.RetryAfter({ "user:B", "peer:x" }, t0 + 2s).count() == 8000);
    // Success clears the user key only.
    t.Success("user:A");
    Expect("throttle/user-cleared", t.RetryAfter({ "user:A" }, t0 + 2s).count() == 0);
    Expect("throttle/peer-kept", t.RetryAfter({ "peer:x" }, t0 + 2s).count() == 8000);
    // Forgotten after 15 min without failures.
    t.Failure({ "user:C" }, t0);
    t.Failure({ "user:C" }, t0);
    t.Failure({ "user:C" }, t0);
    t.Failure({ "user:C" }, t0 + 901s);
    Expect("throttle/forgotten", t.RetryAfter({ "user:C" }, t0 + 901s).count() == 0);
}

static void LeasesBoundToSession()
{
    LeaseManager lm(Counter, 15s, true);
    auto t0 = LeaseManager::Clock::time_point{} + 1000s;
    constexpr uint64_t S1 = 1, S2 = 2;

    LeaseManager::Grant g;
    Expect("lease/claim", lm.Claim(100, S1, "a", false, t0, g) == LeaseManager::Result::Ok);
    Expect("lease/valid-own-session", lm.Validate(100, S1, g.token, t0 + 1s));
    Expect("lease/token-useless-in-other-session", !lm.Validate(100, S2, g.token, t0 + 1s));
    LeaseManager::Clock::time_point exp;
    Expect("lease/renew-other-session", lm.Renew(100, S2, g.token, t0 + 1s, exp) == LeaseManager::Result::InvalidToken);
    Expect("lease/release-other-session", lm.Release(100, S2, g.token, t0 + 1s) == LeaseManager::Result::InvalidToken);
    Expect("lease/renew-own", lm.Renew(100, S1, g.token, t0 + 1s, exp) == LeaseManager::Result::Ok);
    Expect("lease/info-session", lm.Get(100, t0 + 1s) && lm.Get(100, t0 + 1s)->sessionId == S1);

    LeaseManager::Grant g2, g3;
    Expect("lease/claim-2", lm.Claim(101, S1, "a", false, t0, g2) == LeaseManager::Result::Ok);
    Expect("lease/claim-3", lm.Claim(102, S2, "b", false, t0, g3) == LeaseManager::Result::Ok);

    // Ending a session ends exactly its leases.
    auto ended = lm.EndSession(S1);
    Expect("lease/end-session", ended.size() == 2 && !lm.IsLeased(100, t0 + 2s) && !lm.IsLeased(101, t0 + 2s));
    Expect("lease/other-session-kept", lm.Validate(102, S2, g3.token, t0 + 2s));
    Expect("lease/end-session-again", lm.EndSession(S1).empty());
    Expect("lease/old-token-dead", !lm.Validate(100, S1, g.token, t0 + 2s));
}

int main()
{
    SessionBasics();
    AbsoluteLifetime();
    OneSessionPerAccount();
    Throttle();
    LeasesBoundToSession();

    if (failures == 0)
        std::printf("all session tests passed\n");
    return failures == 0 ? 0 : 1;
}
