# R2 — Headless session mechanics

> Phase 0 research item 5 (PLAN.md §9). Source: `azerothcore/azerothcore-wotlk` master @
> `0540ae8013bc92d93f1f0d3346ccbc1a1a4d6f85`. Every path below is relative to the AC repo root.
> Line numbers are for that SHA. **[inferred]** marks conclusions not directly stated by code.
> Prior art consulted (mechanics only): `mod-playerbots` master, shallow clone in `C:\acsdk-build\ref\mod-playerbots`.

## 0. Headline findings

1. **Stock AC now has first-class "headless session" support.** Two upstream commits (Sept 2026)
   added exactly the hooks/APIs we need, contributed by the playerbots developers:
   - `6792c7de3` *feat(Core/Scripting): add SessionScript and hooks for server-driven characters (#27531)* —
     `ServerScript::OnPacketSent`, `SessionScript::OnSessionUpdate`, `PlayerScript::OnPlayerAfterUpdate`,
     `AllMapScript::CanSendObjectUpdatesToPlayer`, and a few gameplay hooks.
   - `92fed92ea` *feat(Core): support headless sessions and add session-less APIs (#27533)* —
     `WorldSession::_headless`/`IsHeadless()`, `NextQueuedPacket()`, public `ProcessQueryCallbacks()`,
     public `LoginQueryHolder`, `HandlePlayerLoginFromDB` sets `m_playerLoading` itself,
     `MotionMaster::MoveKnockbackFrom(..., allowClientControlled)`, skip of `account.online/totaltime` writes.
2. **A null-socket session must NOT be registered with `WorldSessionMgr`.** `WorldSession::Update` with the
   world filter returns `false` when `m_Socket` is null, and `WorldSessionMgr::UpdateSessions` then deletes it
   on its first tick (§2.2). The module owns and pumps its sessions itself — this is what mod-playerbots does.
3. **AC never processes `_recvQueue` of a null-socket session** (`while (m_Socket && _recvQueue.next(...))`,
   `src/server/game/Server/WorldSession.cpp:414`). Injection = `QueuePacket` + module drains with
   `NextQueuedPacket()` and calls `opcodeTable[op]->Call(session, pkt)` itself. The opcode table's
   processing place (INPLACE/THREADUNSAFE/THREADSAFE) is therefore **not** applied by AC for us; we must
   choose the thread and replicate the session-status checks (§5).
4. **Outgoing capture works with zero patches:** `ServerScript::OnPacketSent` fires for every
   `WorldSession::SendPacket`, *before* the socket check, on whatever thread sent it (§6).
   `ServerScript::CanPacketSend` does **not** fire for null-socket sessions.
5. **`WorldScript::OnUpdate` fires on the world (main) thread after `MapMgr::Update` has joined all map
   worker threads** (`MapUpdater::wait()`), so no map thread is running during it. Touching `Player`s
   there is safe (§8). PLAN §4.1 #2 is confirmed, with the precise wording "after map updates", not "between".
6. **Server-side Player movement works:** `MotionMaster::MovePoint/MoveFollow/MoveChase` have `Player`
   instantiations, use `PathGenerator`, launch a spline broadcast as `SMSG_MONSTER_MOVE`, and the server
   advances the Player's real position each tick in `Unit::UpdateSplineMovement`. Caveats in §7.
7. **Nothing kicks a socketless session for idle/AFK/time-sync.** The only removal paths are explicit
   `KickPlayer` (which for us only sets a flag the module must poll) and our own logout.

---

## 1. Constructing a socketless `WorldSession`

Signature (`src/server/game/Server/WorldSession.h:432-434`, impl `WorldSession.cpp:110-166`):

```cpp
WorldSession(uint32 id, std::string&& name, uint32 accountFlags, std::shared_ptr<WorldSocket> sock,
             AccountTypes sec, uint8 expansion, time_t mute_time, LocaleConstant locale,
             uint32 recruiter, bool isARecruiter, bool skipQueue, uint32 TotalTime);
```

No method of `WorldSession` is `virtual` (grep of `WorldSession.h`): **subclassing cannot override
`Update`/`SendPacket`/`QueuePacket`**, so "HeadlessSession" in PLAN §4 must be a *wrapper/handle* that owns a
plain `WorldSession*`, not a behavioural subclass.

What `sock == nullptr` changes, everywhere `m_Socket` is referenced (complete grep):

| Location | Behaviour with null socket |
|---|---|
| `WorldSession.cpp:148` | `_headless(!sock)` — `IsHeadless()` (`WorldSession.h:1234`) returns true. |
| `WorldSession.cpp:158-165` | No `ResetTimeOutTime`, no `UPDATE account SET online = 1`; `m_Address = "headless"`. |
| `WorldSession.cpp:171,192` (dtor) | Skips `account.totaltime` and `account.online = 0` writes. |
| `WorldSession.cpp:179-183` (dtor) | No socket to close. Dtor still calls `LogoutPlayer(true)` if `_player` set (`:175-176`). |
| `WorldSession.cpp:310-313` `SendPacket` | `sScriptMgr->OnPacketSent(this, *packet)` runs, then returns early — `CanPacketSend` and `m_Socket->SendPacket` skipped. |
| `WorldSession.cpp:390-391` `Update` | Idle-connection close is socket-only → **no idle kick**. |
| `WorldSession.cpp:414` `Update` | **Receive queue never processed** (`while (m_Socket && ...)`). |
| `WorldSession.cpp:600,610` `Update` | Warden not updated (it is null anyway). |
| `WorldSession.cpp:618-620` `Update` | With the world filter (`ProcessUnsafe()==true`) returns **false** ("remove this session"). |
| `WorldSession.cpp:629` `HandleSocketClosed` | Always false (needs a socket) → never moved to `_offlineSessions`. |
| `WorldSession.cpp:641` `IsSocketClosed` | Always **true** for headless. Avoid calling code that treats this as "disconnected". |
| `WorldSession.cpp:781` `LogoutPlayer` | "Leave group on logout" only applies when a socket exists → headless characters **stay in their group** on logout. |
| `WorldSession.cpp:883-888` `KickPlayer` | Only sets `_kicked` (if `setKicked`); nothing else happens (§4.3). |
| `WorldSession.cpp:1654` `SetPacketLogging` | No-op. |

Other members worth setting correctly (load them from `auth.account`/`account_access` with a module-owned
async LoginDatabase query before constructing — playerbots just passes `""`, `SEC_PLAYER`, WotLK):
- `expansion` — used by map-entry checks; pass the account's real value (normally 2).
- `sec` (security) and `name` — used by RBAC (`LoadPermissions`, `WorldSession.cpp:1667-1677`).
- `locale` — DBC/DB string locale.
- RBAC is loaded lazily and **synchronously** by the first `HasPermission()` call (`WorldSession.cpp:1691-1694`).
  For a normal session that first call happens in `WorldSessionMgr::AddSession_` (`WorldSessionMgr.cpp:397`) on the
  world thread. A headless session is never added there, so the first call could come from a map thread
  (e.g. `HandleLogoutRequestOpcode`, `MiscHandler.cpp:423`). **Recommendation:** call `session->HasPermission(0)`
  or `LoadPermissions()` once on the world thread right after construction (or use `LoadPermissionsAsync()`,
  `WorldSession.cpp:1679`, and wait on its `QueryCallback` in a module-owned processor).
- `InitializeSession()` (`WorldSession.cpp:1564-1608`, public) is **optional**: it loads global account data and
  tutorials through the session's own callback processor and sends `SMSG_AUTH_RESPONSE`, addon info, cache
  version and tutorials (all just reach `OnPacketSent`). Without it account data/tutorials stay empty;
  nothing in login/logout requires them **[inferred]**.

Prior art: `mod-playerbots/src/Bot/PlayerbotMgr.cpp:158-160` constructs
`new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING, 0, defaultLocale, 0, false, false, 0)`.

## 2. Registration and ticking

### 2.1 How real sessions are ticked
- `WorldSocket::HandleAuthSessionCallback` → `sWorldSessionMgr->AddSession(_worldSession)`
  (`src/server/game/Server/WorldSocket.cpp:710-724`) → `_addSessQueue` (`WorldSessionMgr.cpp:258-261`).
- `World::Update` → `sWorldSessionMgr->UpdateSessions(diff)` (`src/server/game/World/World.cpp:1225-1228`).
- `WorldSessionMgr::UpdateSessions` (`WorldSessionMgr.cpp:93-216`) drains `_addSessQueue` → `AddSession_`, then
  for each session: `HandleSocketClosed()` → offline list; else `Update(diff, WorldSessionFilter)`; on `false`
  it erases and **deletes** the session (`:179-194`).
- Per map, `Map::Update` calls `session->Update(s_diff, MapSessionFilter)` for every in-world player
  (`src/server/game/Maps/Map.cpp:441-456`) — on the map worker thread.

### 2.2 Why a headless session cannot go into `WorldSessionMgr` (stock)
`Update(diff, WorldSessionFilter)` reaches `if (!m_Socket) return false;` (`WorldSession.cpp:618-620`), so
`UpdateSessions` deletes it the same tick (`WorldSessionMgr.cpp:179-194`); the dtor logs the character out.
Also `AddSession_` would run queue/limit logic and `InitializeSession`. **Conclusion: keep headless sessions in
the module's own `CharacterRegistry`; never call `sWorldSessionMgr->AddSession`.** Side effects of not being
registered **[inferred from the code paths]**:
- `sWorldSessionMgr->FindSession(accountId)` / `GetActiveSessionCount()` / player-limit queue do not see them.
- `WorldSessionMgr::KickAll()` (shutdown, `.server` kick-all) does not reach them → handle `WorldScript::OnShutdown` (§4.4).
- Player-based lookups still work: `HandlePlayerLoginFromDB` calls `ObjectAccessor::AddObject(pCurrChar)`
  (`CharacterHandler.cpp:903`), and `WorldSessionMgr::DoForAllOnlinePlayers` iterates `ObjectAccessor::GetPlayers()`
  (`WorldSessionMgr.cpp:488-503`).

### 2.3 What ticks a headless session anyway
- **Map thread, automatically:** once the Player is in a map, `Map::Update` calls
  `Update(s_diff, MapSessionFilter)` (`Map.cpp:449-450`). With a null socket this does: CAIS check (`:406-410`),
  **no packet processing** (`:414`), **sends `SMSG_TIME_SYNC_REQ` every 10 s** (`:576-590`, `SendTimeSync`
  `:1522-1533`), and `ProcessQueryCallbacks()` (`:592`). It returns `true` (the `ProcessUnsafe()` branch is skipped).
  So DB callbacks queued on the session *after* the Player entered the world (e.g. pet loading) complete on the
  **map thread** — same as for real sessions.
- **Nothing else.** The world-side parts of `Update` (timeout, `OnSessionUpdate`, timed logout via `ShouldLogOut`,
  `:593-622`) never run. The module must, each `WorldScript::OnUpdate` tick, for each headless session:
  1. `session->ProcessQueryCallbacks()` (public since `92fed92ea`, `WorldSession.h:1225`) — required while logging
     in (player not in a map yet) and harmless afterwards. playerbots: `PlayerbotMgr.cpp:202-232` (pending logins)
     and `:286-306` (online bots).
  2. Drain injected packets (§5).
  3. Protocol reflexes (§7.3).
  4. `if (session->IsKicked())` → logout + delete (§4.3).
  5. `if (session->ShouldLogOut(now) && !session->PlayerLoading())` → `LogoutPlayer(true)` (replicates
     `WorldSession.cpp:605-608`; needed if a controller injects `CMSG_LOGOUT_REQUEST` and the logout is not instant).
- Calling `session->Update(diff, worldFilter)` ourselves would do (1), (5), fire `SessionScript::OnSessionUpdate`
  and return `false` (ignorable), but would not drain packets; direct calls are clearer. **[design choice]**

### 2.4 Keep-alive: is a headless session ever dropped?
- Idle/timeout: `IsConnectionIdle()` close is gated on `m_Socket` (`WorldSession.cpp:390`); `UpdateTimeOutTime`
  only matters for that check. → **not kicked**.
- Time sync: `HandleTimeSyncResp` (`src/server/game/Handlers/MovementHandler.cpp:926-958`) only computes a clock
  delta; nothing kicks on missing responses. Side effect: `_pendingTimeSyncRequests` (a `std::map`) grows by one
  entry per 10 s until `ResetTimeSync()` (called on every map add, `Player.cpp:11830`). Bounded and small
  (~360 entries/hour) — optional reflex is to answer with `CMSG_TIME_SYNC_RESP` (§7.3).
- AFK: no server-side AFK kick exists in `PlayerUpdates.cpp` (only "Invalid position" kick, `:276`).
- Warden: never initialised for headless (`InitWarden` only in `WorldSocket.cpp:716-720`).
- Ping/latency: `CMSG_PING` is handled inside `WorldSocket` (`Opcodes.cpp:607` `STATUS_NEVER`), irrelevant.
- **Conclusion:** a headless session stays alive indefinitely until we (or a `KickPlayer` caller) end it.

## 3. Loading and logging in a character

Normal client path: `CMSG_PLAYER_LOGIN` (`Opcodes.cpp:192`, `STATUS_AUTHED`, `PROCESS_THREADUNSAFE`) →
`WorldSession::HandlePlayerLoginOpcode` (`src/server/game/Handlers/CharacterHandler.cpp:675-794`):
realm-login check, `PlayerLoading()/GetPlayer()` re-login guard, `IsLegitCharacterForAccount` (needs a prior
`CMSG_CHAR_ENUM`), offline-session reclaim (`:703-783`), then:

```cpp
auto holder = std::make_shared<LoginQueryHolder>(GetAccountId(), playerGuid);   // :785
if (!holder->Initialize()) return;                                               // :786
m_playerLoading = true;                                                          // :789
AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder))
    .AfterComplete([this](SQLQueryHolderBase const& h){ HandlePlayerLoginFromDB(static_cast<LoginQueryHolder const&>(h)); });
```

**Headless path (no client, zero patches)** — exactly what playerbots does (`PlayerbotMgr.cpp:88-193, 252-284`):

1. World thread: verify the character is not online and its account has no live session:
   `ObjectAccessor::FindConnectedPlayer(guid)`, `sWorldSessionMgr->FindSession(acc)`, `FindOfflineSession(acc)`,
   `FindOfflineSessionForCharacterGUID(low)` (`WorldSessionMgr.cpp:62-91`), plus our own registry. Account id via
   `sCharacterCache->GetCharacterAccountIdByGuid(guid)` (used at `PlayerbotMgr.cpp:98`). → `ACCOUNT_ALREADY_ONLINE`.
2. Construct the session (§1).
3. `std::make_shared<LoginQueryHolder>(accountId, guid)` + `Initialize()` — class is public in
   `WorldSession.h:293-305` (impl `CharacterHandler.cpp:66-...`). It may be subclassed to carry module data
   (playerbots' `PlayerbotLoginQueryHolder`, `PlayerbotMgr.cpp:75-86`).
4. `session->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(cb)`.
   The SQL runs on DB worker threads (async; never blocks the world thread).
5. Each world tick call `session->ProcessQueryCallbacks()` (§2.3). When the holder completes, `cb` runs **on the
   world thread inside that call** and does `session->HandlePlayerLoginFromDB(holder)` (public,
   `CharacterHandler.cpp:796`). It sets `m_playerLoading = true` itself (`:798`, added by `92fed92ea`).
6. `HandlePlayerLoginFromDB` (`:796-1131`): `new Player(this)`, `LoadFromDB` (which re-checks DB account id ==
   session account id, `src/server/game/Entities/Player/PlayerStorage.cpp:5049-5055`, and the ban), sends the login
   packet burst (all go to `OnPacketSent`), `ObjectAccessor::AddObject` (`:903`), `Map::AddPlayerToMap` (`:905`),
   `SendInitialPacketsAfterAddToMap` (`:917`, starts time sync), marks `characters.online` (`:919-924`) and
   `account.online` (`:926-929`), loads pet (`:1103`, async via session processor), fires
   `sScriptMgr->OnPlayerLogin` (`:1126`).
7. Success test after the callback: `session->GetPlayer() != nullptr && player->IsInWorld()`. On
   `LoadFromDB` failure it calls `KickPlayer(...)` (sets `_kicked`), deletes the Player and clears
   `m_playerLoading` (`:806-813`).
8. **Do not delete the session inside its own callback** — the callback processor is on the stack. playerbots flags
   it and tears down later (`PlayerbotMgr.cpp:195-232`, comment at `:263`).

Notes:
- Skipped vs. a real client: `IsLegitCharacterForAccount`, the offline-session reclaim and the realm-login-enabled
  check (`:677`). The module should check `sWorld->getBoolConfig(CONFIG_REALM_LOGIN_ENABLED)` itself if desired.
- `HandlePlayerLoginFromDB` runs `firstLogin` spells, cinematic flag, at-login resets etc. exactly like a client
  login — this is AC's logic, not ours.
- Account DB `account.online` is set at login (`:926-929`) but the headless dtor skips resetting it
  (`WorldSession.cpp:192`); it is cleared at server shutdown by `ClearOnlineAccounts()` (`apps/worldserver/Main.cpp:378-379`).
  Effect on authserver not verified **[open]**.
- **Duplicate-login hazard:** `WorldSocket` performs no "account already online" check (`WorldSocket.cpp:710-724`)
  and `AddSession_`'s `KickSession` only searches `_sessions` (`WorldSessionMgr.cpp:219-233, 337`). A real client
  logging into a headless account could load the same character a second time **[inferred]**. Zero-patch guard:
  `ServerScript::CanPacketReceive` (fires for `STATUS_AUTHED` opcodes, `WorldSession.cpp:505`) → return `false`
  for `CMSG_PLAYER_LOGIN` on a non-headless session whose account/guid we hold. Accounts reserved by
  `ACSDK.HeadlessAccountPrefix` make this rare anyway.

## 4. Logging out and removal

### 4.1 `WorldSession::LogoutPlayer(bool save, bool redirecting = false)` (`WorldSession.cpp:686-878`)
Finishes pending far teleports (`:689-690`, `:795-796`), `OnPlayerBeforeLogout`, loot release, ghost/graveyard
handling, leaves BG and BG queues, guild logout, removes pet, `SaveToDB` (`:810`), leaves channels, social
offline, `OnPlayerLogout` (`:837`), `CleanupsBeforeDelete` + `Map::RemovePlayerFromMap(player, true)` (deletes the
Player, `:851-856`), `SetPlayer(nullptr)`, sends `SMSG_LOGOUT_COMPLETE`, marks characters offline (`:866-871`).
It touches the map directly → **call it on the world thread when no map thread runs** (our `OnUpdate`, §8).
Group: headless characters are *not* removed from their party (socket condition at `:781`).

### 4.2 Safe removal sequence (module)
```
world thread (OnUpdate):
  if (session->PlayerLoading()) -> defer
  session->LogoutPlayer(true);   // saves + removes from map, deletes Player
  registry.erase(guid);
  delete session;                // dtor: _player is null, no socket, skips account writes
```
playerbots: `PlayerbotMgr.cpp:394-456` (`LogoutPlayerBot`: `LogoutPlayer(true)` then `delete` session).

### 4.3 `KickPlayer` and external kicks
`KickPlayer(reason, setKicked=true)` (`WorldSession.cpp:881-893`) just sets `_kicked` for a headless session.
Callers include GM `.kick`, bans (`BanMgr.cpp`), invalid-hyperlink kicks (`WorldSession.cpp:526-539`), worldport
errors (`MovementHandler.cpp:65,153,210`), speed-cheat kick (`:775`), "Invalid position" (`PlayerUpdates.cpp:276`)
and login failure (`CharacterHandler.cpp:809`). Nobody acts on the flag for an unregistered session, so the module
**must poll `IsKicked()` each tick** and run §4.2 (and emit a lifecycle event — a "genuine game event", PLAN §4.3).
`KickPlayer` may be called from a map thread; acting on the flag happens later on the world thread, which is safe.

### 4.4 Server shutdown
`WorldScript::OnShutdown` fires in `apps/worldserver/Main.cpp:426`, after `WorldUpdateLoop()` returns (`:413`) and
**before** the deferred handles run `KickAll()`/`UpdateSessions(1)` (`:370-380`) and `MapMgr::UnloadAll()`
(`:320-329`) — maps still exist and no map thread is updating. Log out all headless characters there
(playerbots: `Playerbots.cpp:553-559`). Also log out pending (still-loading) sessions.

## 5. Packet injection

### 5.1 Queue
`QueuePacket(WorldPacket* p)` (`WorldSession.cpp:360-363`) → `_recvQueue.add(p)`; `_recvQueue` is a
`LockedQueue<WorldPacket*>` (`WorldSession.h:1329`), thread-safe. Ownership passes to the session.
`NextQueuedPacket()` (`WorldSession.cpp:1658-1665`, public since `92fed92ea`) pops the head as
`std::unique_ptr<WorldPacket>` regardless of opcode. There is no public filtered pop.

### 5.2 How AC normally routes (for reference — does not run for us)
`WorldSession::Update` (`WorldSession.cpp:385-625`): `_recvQueue.next(packet, updater)` lets the filter pick:
- `WorldSessionFilter::Process` (`:88-107`, world thread in `WorldSessionMgr::UpdateSessions`): INPLACE, THREADUNSAFE,
  and *everything* when the player is not in world / not set.
- `MapSessionFilter::Process` (`:66-84`, map thread in `Map::Update`): INPLACE and THREADSAFE when player in world.
Then AntiDOS, then the status switch (`:443-519`): `STATUS_LOGGEDIN` requires `_player && _player->IsInWorld()`;
`STATUS_TRANSFER` requires `_player && !IsInWorld()`; `STATUS_AUTHED` blocked if in queue;
`STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT`; `STATUS_NEVER`/`UNHANDLED` logged & dropped. Before each call,
`sScriptMgr->CanPacketReceive(this, *packet)`. Exceptions caught: `InvalidHyperlinkException`,
`IllegalHyperlinkException`, `PacketArrayMaxCapacityException`, `ByteBufferException` (`:521-554`). Max 150 packets per call.

Opcode table: `src/server/game/Server/Protocol/Opcodes.h:1356-1430` (`SessionStatus`, `PacketProcessing`,
`ClientOpcodeHandler{Name, Status, ProcessingPlace, Call(session, packet)}`, `extern OpcodeTable opcodeTable`);
entries in `Opcodes.cpp`. Examples:

| Opcode | Line | Status | Processing |
|---|---|---|---|
| `CMSG_PLAYER_LOGIN` | 192 | AUTHED | THREADUNSAFE |
| `CMSG_LOGOUT_REQUEST` | 206 | LOGGEDIN | THREADUNSAFE |
| `CMSG_MESSAGECHAT` | 280 | LOGGEDIN | THREADUNSAFE |
| `CMSG_LOOT` | 480 | LOGGEDIN | THREADUNSAFE |
| `CMSG_GROUP_ACCEPT` | 245 | LOGGEDIN | THREADUNSAFE |
| `MSG_MOVE_WORLDPORT_ACK` | 351 | TRANSFER | THREADUNSAFE |
| `CMSG_CAST_SPELL` / `CMSG_CANCEL_CAST` | 433-434 | LOGGEDIN | THREADSAFE |
| `CMSG_SET_SELECTION` | 448 | LOGGEDIN | THREADSAFE |
| `CMSG_ATTACKSWING` / `CMSG_ATTACKSTOP` | 452-453 | LOGGEDIN | THREADSAFE |
| `MSG_MOVE_TELEPORT_ACK` | 330 | LOGGEDIN | THREADSAFE |
| `MSG_MOVE_SET_FACING`, `MSG_MOVE_START_FORWARD` | 349, 312 | LOGGEDIN | THREADSAFE |
| `CMSG_TIME_SYNC_RESP` | 1044 | LOGGEDIN | THREADSAFE |
| `CMSG_REPOP_REQUEST`, `CMSG_RESURRECT_RESPONSE` | 477, 479 | LOGGEDIN | THREADSAFE |
| `CMSG_USE_ITEM`, `CMSG_GOSSIP_HELLO`, `CMSG_NAME_QUERY` | 302, 510, 211 | LOGGEDIN | INPLACE |

### 5.3 Injection for headless sessions (zero patches)
Because of `WorldSession.cpp:414`, the module must drain. Proposed `HeadlessSession::DrainPackets()` on the world
thread in `WorldScript::OnUpdate`, mirroring `WorldSession::Update`:

```cpp
while (auto pkt = session->NextQueuedPacket()) {
    auto op = static_cast<OpcodeClient>(pkt->GetOpcode());
    if (op >= NUM_OPCODE_HANDLERS) continue;                      // bounds (table is a raw array)
    ClientOpcodeHandler const* h = opcodeTable[op]; if (!h) continue;
    Player* p = session->GetPlayer();
    bool ok = /* mirror WorldSession.cpp:443-519 */
        h->Status == STATUS_LOGGEDIN  ? (p && p->IsInWorld()) :
        h->Status == STATUS_TRANSFER  ? (p && !p->IsInWorld()) :
        h->Status == STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT ? (p || session->PlayerRecentlyLoggedOut()) :
        h->Status == STATUS_AUTHED;   // NEVER / UNHANDLED -> drop
    if (!ok || !sScriptMgr->CanPacketReceive(session, *pkt)) { /* report REJECTED_STATUS */ continue; }
    try { h->Call(session, *pkt); }
    catch (ByteBufferException const&) { /* malformed: report, never crash */ }
    catch (WorldPackets::PacketArrayMaxCapacityException const&) {}
    catch (WorldPackets::InvalidHyperlinkException const&) {}
    catch (WorldPackets::IllegalHyperlinkException const&) {}
    if (!session->GetPlayer() && /*was logged in*/ ...) break;    // handler may log out (instant CMSG_LOGOUT_REQUEST, MiscHandler.cpp:455-458)
}
```

- playerbots does the same but **without** status checks, `CanPacketReceive` or exception handling
  (`PlayerbotMgr.cpp:308-321`), called from `WorldScript::OnUpdate` (`Playerbots.cpp:545-551`) and from
  `SessionScript::OnSessionUpdate` of the master's real session (`Playerbots.cpp:443-457`, world thread). It only
  drains when `bot->IsInWorld()` and not teleporting (`PlayerbotMgr.cpp:293-304`).
- **Thread choice:** running THREADSAFE handlers on the world thread during `OnUpdate` is safe because no map
  thread is running then (§8) — the world thread has the exclusive access those handlers otherwise get from their
  map thread. THREADUNSAFE handlers are designed for the world thread. **[inferred, consistent with AC practice:
  GM chat commands arrive via THREADUNSAFE `CMSG_MESSAGECHAT` and mutate players/creatures from the world thread]**.
- Deviation from PLAN §4.6.1: "AC's opcode table decides which thread" is **not** achievable without a core patch;
  we read the table's `Status` and replicate the status gate. Record in PLAN §13.
- Alternative (also zero-patch) for strict fidelity: keep THREADSAFE packets in a module per-character inbox and run
  them from `PlayerScript::OnPlayerBeforeUpdate` (map thread, `PlayerUpdates.cpp:58`), i.e. where
  `MapSessionFilter` would have run them. More complex (two queues, ordering across them); not recommended for v1.
- Latency: a packet queued and drained in the same `OnUpdate` executes in that tick.
- AntiDOS (`DosProtection`, `WorldSession.h:1243-1268`) is protected/unreachable → our own rate limits (PLAN §4.6.4).

## 6. Intercepting outgoing packets

- `WorldSession::SendPacket` (`WorldSession.cpp:308-357`) is the single funnel (`Player::SendDirectMessage`,
  `SendMessageToSet` visitors and `Map::SendObjectUpdates` all end here). First line:
  `sScriptMgr->OnPacketSent(this, *packet)` → `ServerScript::OnPacketSent(WorldSession*, WorldPacket const&)`
  (`src/server/game/Scripting/ScriptDefines/ServerScript.h:85`, dispatcher `ServerScript.cpp`, hook id
  `SERVERHOOK_ON_PACKET_SENT`). Documented as firing "also for sessions without a socket".
- `ServerScript::CanPacketSend` (`ServerScript.h:65`) is evaluated **after** the null-socket return
  (`WorldSession.cpp:351`) → never fires for headless; cannot be used.
- **Thread:** whichever thread calls `SendPacket`: map worker threads (spells, combat, movement, object updates —
  most traffic), the world thread (session updates, THREADUNSAFE handlers, BG/LFG/world-state managers, our
  `OnUpdate`), and the LFG worker task scheduled on the map pool (`MapMgr.cpp:256-267`). With
  `MapUpdate.Threads > 1` the hook runs concurrently for different sessions. **PacketSink must be thread-safe**
  (e.g. per-character lock-free/mutexed bounded queue) and must not touch game objects; it should be the cheap
  "opcode first" filter from PLAN §4.5: `if (!session->IsHeadless()) return;` then registry lookup (other modules
  may create headless sessions too, so check our registry, not only `IsHeadless()`).
- The packet is `const&` and freed by the caller → copy the bytes we keep.
- Volume reducer: `AllMapScript::CanSendObjectUpdatesToPlayer(Map*, Player*)` (`Map.cpp:1746-1750`) lets us return
  `false` for headless players so `SMSG_UPDATE_OBJECT` is never even built (playerbots: `Playerbots.cpp:459-476`).
  Fits PLAN §4.7 ("never decode UPDATE_OBJECT"); the opt-in raw-packet mode would need it re-enabled per character.

## 7. Server-side movement of Players

### 7.1 MotionMaster on a Player really moves it
- `MotionMaster::MovePoint` (`src/server/game/Movement/MotionMaster.cpp:487-502`) and `MoveFollow` (`:448-469`),
  `MoveChase` (`:316-...`), `MoveSplinePath` (`:504-518`), `MovePointBackwards` (`:1043-...`) all have an explicit
  `IsPlayer()` branch creating `PointMovementGenerator<Player>` / `FollowMovementGenerator<Player>` /
  `ChaseMovementGenerator<Player>` (instantiated in `PointMovementGenerator.cpp:339-350`,
  `TargetedMovementGenerator.cpp:783-804`). They refuse only on `UNIT_FLAG_DISABLE_MOVE`.
- `PointMovementGenerator<T>::DoInitialize` (`PointMovementGenerator.cpp:30-110`): if `generatePath`,
  `PathGenerator path(unit); path.CalculatePath(x,y,z,forceDest)` (mmaps) and `init.MovebyPath(...)`, else
  straight `MoveTo`; then `MoveSplineInit::Launch()`.
- `MoveSplineInit::Launch` (`src/server/game/Movement/Spline/MoveSplineInit.cpp:35-127`) sets
  `MOVEMENTFLAG_SPLINE_ENABLED`, and broadcasts `SMSG_MONSTER_MOVE` with `SendMessageToSet(&data, true)` (`:114-124`)
  → nearby real clients see the move; our own sink also receives it.
- Each map tick `Player::Update` → `Unit::Update` (`PlayerUpdates.cpp:76`) → `UpdateSplineMovement` +
  `GetMotionMaster()->UpdateMotion` (`Unit.cpp:642-643`); `UpdateSplinePosition` calls `UpdatePosition` for players
  (`Unit.cpp:734-761`) → `Player::UpdatePosition` updates zone/area, group position flag and exploration
  (`PlayerUpdates.cpp:1185-1206`). **Server-authoritative position changes, no client packets needed.**
- Completion: `PointMovementGenerator<Player>::MovementInform` is a **no-op** (`PointMovementGenerator.cpp:315-318`;
  only the `Creature` specialisation informs AI). The MoveTo watcher must poll: generator gone / type idle and
  `movespline->Finalized()`, plus distance to target. `DoUpdate` stops on `UNIT_STATE_NOT_MOVE` / casting
  (`:118-129`) and resumes later.
- Stop: `Unit::StopMoving()` (`Unit.cpp:12666`) / `GetMotionMaster()->Clear()` (playerbots: `PlayerbotMgr.cpp:469-470`).
- Mutating MotionMaster from `WorldScript::OnUpdate` is safe (no map thread running, §8). playerbots does movement
  from `OnPlayerAfterUpdate` on the map thread instead — also valid.

### 7.2 Caveats (Player movement is normally client-driven)
- `Unit::IsClientControlled()` (`Unit.cpp:17033-17058`) is **true** for a headless player (it checks unit flags,
  not the socket). Paths that hand control to "the client" therefore expect a client:
  - Knockback: `Unit::KnockbackFrom` sends `SMSG_MOVE_KNOCK_BACK` to the "client" and moves nothing server-side
    (`Unit.cpp:15040-15059`). `MotionMaster::MoveKnockbackFrom(x, y, speedXY, speedZ, /*allowClientControlled*/true)`
    (`MotionMaster.cpp:620-...`, param added in `92fed92ea` for exactly this) lets us simulate it server-side as a reflex.
  - Root/gravity/fly/hover/feather-fall/water-walk (`Unit::SendMoveRoot` `:14125-14171`, `SetDisableGravity`
    `:16147-16180`, `SetCanFly` `:16207`, `SetFeatherFall` `:16243`, `SetHover` `:16284`, `SetWaterWalking`
    `:16337`): for client-controlled units the `MOVEMENTFLAG_*` is only applied when the client acks the
    `SMSG_FORCE_MOVE_*` / `SMSG_MOVE_*` packet. Gameplay effect of root/stun is still enforced via unit state
    (`UNIT_STATE_ROOT` stops `PointMovementGenerator::DoUpdate`) **[inferred]**; observers may see stale movement flags.
  - Speed changes: `Unit::SetSpeed` applies `m_speed_rate` immediately (`Unit.cpp:11074-11085`) and sends
    `SMSG_FORCE_*_SPEED_CHANGE` incrementing `m_forced_speed_changes` (`:11129`); the ack
    (`HandleForceSpeedChangeAck`, `MovementHandler.cpp:678-778`) only re-broadcasts and validates. **Not required**;
    counter just stays elevated **[inferred harmless]**.
- No client physics: no falling/jumping/swimming simulation beyond what the spline path encodes; `PathGenerator`
  quality depends on mmaps. Never synthesize `MSG_MOVE_*` streams (PLAN §4.6.3).
- Facing: `MSG_MOVE_SET_FACING` goes through `HandleMovementOpcodes` (anti-cheat/`ProcessMovementInfo`) — R3 to
  decide; `Unit::SetFacingTo` is the server-side alternative.
- Multi-map taxi flights need `CMSG_MOVE_SPLINE_DONE` (`TaxiHandler.cpp:204-...`) — out of scope now.

### 7.3 Protocol reflexes needed (feeds PLAN §5.2)
| Situation | Server state | Reflex (world thread, polled in `OnUpdate`) |
|---|---|---|
| Far teleport | `Player::TeleportTo` sends `SMSG_TRANSFER_PENDING`/`SMSG_NEW_WORLD`, removes from map, `SetSemaphoreTeleportFar` (`Player.cpp:1607-1643`) | `if (p->IsBeingTeleportedFar()) session->HandleMoveWorldportAck();` — public "for server-side calls" (`WorldSession.h:803`, impl `MovementHandler.cpp:51-290`). Same as playerbots `PlayerbotAI.cpp:861-863` and AC's own `LogoutPlayer`/login reclaim (`CharacterHandler.cpp:741-749`). |
| Near teleport | Position restored to old until ack; `SetSemaphoreTeleportNear` (`Player.cpp:1534-1548`) | Build `MSG_MOVE_TELEPORT_ACK` {packed guid, u32 flags=0, u32 time=0} and call `session->HandleMoveTeleportAck(pkt)` (`MovementHandler.cpp:292-360`) or queue it. Template: `CharacterHandler.cpp:758-766`, playerbots `PlayerbotAI.cpp:898-913`. |
| Delayed teleport (`MustDelayTeleport`) | Semaphore set, real teleport executed later in the Player update (`Player.cpp:1509-1516, 1575-1583`) | Acking in `OnUpdate` (after map update) sees the executed teleport **[inferred]**. |
| Time sync | `SMSG_TIME_SYNC_REQ` every 10 s | Optional: reply `CMSG_TIME_SYNC_RESP{counter, clientTicks}` to keep `_pendingTimeSyncRequests` empty. |
| Knockback | `SMSG_MOVE_KNOCK_BACK` captured | Optional: `MoveKnockbackFrom(..., true)`. |
| Force root/unroot, gravity, speed | `SMSG_FORCE_*` captured | Optional acks; not needed to stay valid. |

## 8. Threading: where `WorldScript::OnUpdate` runs

`apps/worldserver/Main.cpp:584-618` `WorldUpdateLoop` (main thread) → `World::Update(diff)`
(`src/server/game/World/World.cpp:1131-1382`), strictly sequential:

1. timers, quest/BG/calendar/guild resets, `sAuctionMgr->Update` (`:1136-1223`)
2. **`sWorldSessionMgr->UpdateSessions(diff)`** (`:1225-1228`) — real sessions: THREADUNSAFE packets, logouts
3. `sLFGMgr->Update(diff, 0)` (`:1246-1249`)
4. **`sMapMgr->Update(diff)`** (`:1251-1255`) → `MapMgr::Update` (`src/server/game/Maps/MapMgr.cpp:251-297`): schedules
   the LFG task and every map on `MapUpdater` worker threads (`:259-277`), then **`m_updater.wait()`** (`:279-280`,
   blocks until `pending_requests == 0`, `MapUpdater.cpp:124-132`), then `DelayedUpdate` on the world thread
   (`:282-289`). Without worker threads maps update inline on the world thread (`:276`).
5. BG, outdoor PvP, world state, battlefields, LFG step 2 (`:1257-1290`)
6. `ProcessQueryCallbacks()` (world-level DB callbacks, `:1292-1296`), uptime, game events, DB ping, instance resets
7. `ProcessCliCommands()` (`:1347-1351`)
8. **`sScriptMgr->OnWorldUpdate(diff)` → `WorldScript::OnUpdate`** (`:1353-1356`)
9. TC9 cluster tasks (only if cluster mode), metrics.

**Conclusion:** `OnUpdate` runs on the main/world thread, after session updates and after all map updates have
completed and been joined. No map worker is active, so reading/mutating `Player`, `Unit`, `Map`, `MotionMaster`
and calling `LogoutPlayer` / `HandleMoveWorldportAck` / opcode handlers there is safe **[the join is verified; the
"no other game-object thread" part is inferred from the absence of other updaters — network threads only enqueue
packets, DB threads only run SQL]**. Effect on latency: a command dispatched in `OnUpdate` is executed in the same
tick; its SMSG results generated by the next map update arrive in the following tick.

Other relevant hook threads:
| Hook | Thread |
|---|---|
| `WorldScript::OnUpdate`, `OnShutdown`, `OnStartup` | world (main) |
| `SessionScript::OnSessionUpdate` | world, inside `UpdateSessions` (real sessions only, before maps) — `WorldSession.cpp:598` |
| `PlayerScript::OnPlayerBeforeUpdate/OnPlayerUpdate/OnPlayerAfterUpdate` | map worker of the player's map (`PlayerUpdates.cpp:58, 314, 431`) |
| `AllMapScript::OnMapUpdate`, `CanSendObjectUpdatesToPlayer` | map worker |
| `ServerScript::OnPacketSent` | any sender thread (§6) |
| `ServerScript::CanPacketReceive` | thread processing the packet (world/map; ours: world) |
| `PlayerScript::OnPlayerLogin` (our path) | world (inside our `ProcessQueryCallbacks` pump) |

## 9. End-to-end recipe (Spike B outline)
1. `WorldScript` (hooks `ON_UPDATE`, `ON_SHUTDOWN`), `ServerScript` (`ON_PACKET_SENT`, `CAN_PACKET_RECEIVE`),
   optional `AllMapScript` (`CAN_SEND_OBJECT_UPDATES_TO_PLAYER`).
2. Login request → checks (§3 step 1) → async account query → `new WorldSession(..., nullptr, ...)` → `HasPermission`
   warm-up → `LoginQueryHolder` + `DelayQueryHolder` → pump `ProcessQueryCallbacks` in `OnUpdate` →
   `HandlePlayerLoginFromDB` → verify in world → `ONLINE`.
3. `OnUpdate` per session: callbacks → reflexes → drain injected packets → kicked / timed-logout check → watchers.
4. Inject `CMSG_MESSAGECHAT` {u32 type=CHAT_MSG_SAY, u32 lang, string msg} (layout to be confirmed in R3) → `QueuePacket` → drain → handler
   (THREADUNSAFE) → `SMSG_MESSAGECHAT` arrives in `OnPacketSent` for our session (and nearby players').
5. `player->GetMotionMaster()->MovePoint(id, x, y, z, FORCED_MOVEMENT_NONE, 0.f, 0.f, /*generatePath*/true)` in
   `OnUpdate`; poll for arrival.
6. Logout: `LogoutPlayer(true)` + `delete` in `OnUpdate`; on shutdown in `OnShutdown`.

## 10. Prior-art notes (mod-playerbots)
- Its README (`README.md:37`) still says a fork (`mod-playerbots/azerothcore-wotlk`, branch `Playerbot`) is required.
  However every mechanic cited in this document is present in **stock** AC at our SHA (verified by grep in the AC
  tree: `NextQueuedPacket`, `IsHeadless`, `OnPacketSent`, `OnSessionUpdate`, `OnPlayerAfterUpdate`,
  `CanSendObjectUpdatesToPlayer`, public `LoginQueryHolder`/`ProcessQueryCallbacks`,
  `MoveKnockbackFrom(..., allowClientControlled)`, public `HandleMoveWorldportAck`/`HandleMoveTeleportAck`).
  Other playerbots features may still rely on fork-only changes; we do not use them.
- playerbots does not replicate status checks or catch `ByteBufferException` when draining — we should.
