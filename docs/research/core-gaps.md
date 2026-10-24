# R1 — Core capability gaps

> Phase 0 research item 4 (PLAN.md §5.3, §9). Source: `azerothcore/azerothcore-wotlk` master @
> `0540ae8013bc92d93f1f0d3346ccbc1a1a4d6f85`; paths relative to the AC repo root. Mechanics and evidence are
> detailed in [`headless-sessions.md`](headless-sessions.md) (R2); this document classifies each capability.
> **[inferred]** = conclusion drawn from code paths, not stated by the code.

Classification: **(a)** possible with existing AC APIs / script hooks · **(b)** needs a core change.

## Context: what changed upstream

Stock AC gained explicit support for server-driven characters in September 2026:
- `6792c7de3` (#27531) — `ServerScript::OnPacketSent`, `SessionScript::OnSessionUpdate`,
  `PlayerScript::OnPlayerAfterUpdate`, `AllMapScript::CanSendObjectUpdatesToPlayer` (+ gameplay hooks).
- `92fed92ea` (#27533) — `WorldSession::IsHeadless()`, `NextQueuedPacket()`, public `ProcessQueryCallbacks()`,
  public `LoginQueryHolder`, `HandlePlayerLoginFromDB` self-sets `m_playerLoading`,
  `MotionMaster::MoveKnockbackFrom(..., allowClientControlled)`, no `account` writes for headless sessions.

Without these two commits most of the table below would be (b). **Our minimum supported AC revision is
therefore `92fed92ea` or later**; pin the SHA in PLAN §13.

## Capability table

| # | Capability | Class | How (existing API / hook) | Evidence | Notes / residual risk |
|---|---|---|---|---|---|
| 1 | Create a socketless session | **a** | `new WorldSession(accId, name, flags, nullptr, sec, expansion, mute, locale, 0, false, false, 0)` | ctor `src/server/game/Server/WorldSession.cpp:110-166`; `_headless(!sock)` `:148`; `IsHeadless()` `WorldSession.h:1234` | No virtual methods → "HeadlessSession" is a wrapper owning a `WorldSession*`, not a subclass. Load real security/expansion/locale from `auth.account` first. Warm RBAC on the world thread (`HasPermission` lazily does a sync query, `WorldSession.cpp:1691-1694`). |
| 2 | Register / tick the session | **a** (module-owned) | Keep it in our registry; **do not** call `sWorldSessionMgr->AddSession`. Each `WorldScript::OnUpdate`: `session->ProcessQueryCallbacks()`. Maps tick it automatically once in world (`Map.cpp:449-450`, `MapSessionFilter`). | Registered null-socket session returns `false` from `Update` (`WorldSession.cpp:618-620`) and is deleted (`WorldSessionMgr.cpp:179-194`) | Making it registrable would need patch P2 (below) — not needed. Unregistered ⇒ invisible to `FindSession`/`KickAll`/session counters **[inferred, harmless]**. |
| 3 | Keep-alive (no disconnect/idle/AFK/time-sync kick) | **a** (nothing to do) | — | Idle close gated on `m_Socket` (`WorldSession.cpp:390`); time-sync resp only computes clock delta (`MovementHandler.cpp:926-958`); no AFK kick in `PlayerUpdates.cpp` | `_pendingTimeSyncRequests` grows ~1 entry/10 s until next map change (`WorldSession.cpp:1522-1533`, reset `Player.cpp:11830`). Optional `CMSG_TIME_SYNC_RESP` reflex. |
| 4 | Character load + login (async DB) | **a** | `LoginQueryHolder` (public, `WorldSession.h:293-305`) + `CharacterDatabase.DelayQueryHolder` + `session->AddQueryHolderCallback(...).AfterComplete(cb)`; `cb` calls `session->HandlePlayerLoginFromDB(holder)` (public, `CharacterHandler.cpp:796`) | Client path `CharacterHandler.cpp:785-793`; playerbots `src/Bot/PlayerbotMgr.cpp:151-193, 252-284` | Callback runs on world thread inside our pump. Account ownership re-checked in `PlayerStorage.cpp:5049-5055`. Never delete the session inside its own callback. |
| 5 | Pre-login exclusivity ("already online") | **a** | `ObjectAccessor::FindConnectedPlayer`, `sWorldSessionMgr->FindSession/FindOfflineSession/FindOfflineSessionForCharacterGUID` (`WorldSessionMgr.cpp:62-91`), own registry | — | Reverse direction (real client logs into a headless account) is unguarded in core (`WorldSocket.cpp:710-724`) **[inferred duplicate-load hazard]** → guard with `ServerScript::CanPacketReceive` rejecting `CMSG_PLAYER_LOGIN` (`WorldSession.cpp:505`). |
| 6 | Logout / removal | **a** | World thread: `session->LogoutPlayer(true)` (`WorldSession.cpp:686-878`) then `delete session` | playerbots `PlayerbotMgr.cpp:394-456` | Headless chars are **not** removed from their party on logout (socket check `WorldSession.cpp:781`) — matches "no autonomy", document it. |
| 7 | Timed logout (injected `CMSG_LOGOUT_REQUEST`) | **a** | Poll `session->ShouldLogOut(now) && !PlayerLoading()` → `LogoutPlayer(true)` | Core only checks in world-side `Update` (`WorldSession.cpp:605-608`), which never runs for us | Instant logout happens inside the handler (`MiscHandler.cpp:455-458`) → drain loop must re-check `GetPlayer()`. |
| 8 | React to external kicks (GM `.kick`, ban, cheat/invalid-position kicks, login failure) | **a** | Poll `session->IsKicked()` each tick → logout + delete + lifecycle event | `KickPlayer` only sets `_kicked` without socket (`WorldSession.cpp:881-893`) | Must be implemented, otherwise a kicked headless char stays in world forever. |
| 9 | Server shutdown | **a** | `WorldScript::OnShutdown` — fires after the update loop, before `KickAll` and `MapMgr::UnloadAll` | `apps/worldserver/Main.cpp:413, 426` vs `:320-329, :370-380` | `KickAll` would not reach unregistered sessions anyway. |
| 10 | Packet injection (CMSG into the session) | **a**, with a documented deviation | `session->QueuePacket(new WorldPacket(...))` (`WorldSession.cpp:360-363`); module drains on the world thread in `OnUpdate`: `NextQueuedPacket()` (`:1658-1665`) → bounds/`opcodeTable[op]` (`Opcodes.h:1386-1430`) → replicate status gate (`WorldSession.cpp:443-519`) → `sScriptMgr->CanPacketReceive` → `h->Call(session, pkt)` in try/catch (`ByteBufferException` etc., `:521-554`) | Core never drains a null-socket queue: `while (m_Socket && _recvQueue.next(...))` (`WorldSession.cpp:414`); playerbots `PlayerbotMgr.cpp:308-321` | AC's own filter/thread routing (`MapSessionFilter`/`WorldSessionFilter`, `:66-107`) is **not** used; we execute THREADSAFE opcodes on the world thread while no map thread runs (safe — see #15). AntiDOS is unreachable (protected). Needs a PLAN §4.6.1/§13 amendment. For exact core routing see optional patch P1. |
| 11 | Outgoing packet capture | **a** | `ServerScript::OnPacketSent(WorldSession*, WorldPacket const&)` (`ServerScript.h:85`), called first thing in `WorldSession::SendPacket` (`WorldSession.cpp:310`) | `CanPacketSend` is after the null-socket return (`:312-313, :351`) → unusable | Fires on any thread (mostly map workers) and concurrently across maps → PacketSink must be thread-safe, copy bytes, no game-object access. Filter by our registry first (cost on every server packet for every session). |
| 12 | Suppress unwanted high-volume packets | **a** | `AllMapScript::CanSendObjectUpdatesToPlayer` → `false` for headless players | `Map.cpp:1746-1750`; playerbots `Playerbots.cpp:459-476` | Saves building `SMSG_UPDATE_OBJECT`; re-enable per character for opt-in raw mode. |
| 13 | Server-side movement (MovePoint/Follow/Chase + PathGenerator) | **a** (whitelisted direct call) | `player->GetMotionMaster()->MovePoint(id, x, y, z, FORCED_MOVEMENT_NONE, 0, 0, true)`, `MoveFollow`, `MoveChase`, `Clear`, `Unit::StopMoving` | Player branches `MotionMaster.cpp:448-502`; path `PointMovementGenerator.cpp:61-69`; broadcast `MoveSplineInit.cpp:114-124`; server position `Unit.cpp:642-643, 734-761`; `Player::UpdatePosition` `PlayerUpdates.cpp:1185-1206` | `MovementInform` is a no-op for players (`PointMovementGenerator.cpp:315-318`) → poll for completion. `IsClientControlled()` is true for headless (`Unit.cpp:17033-17058`), so knockback/root/gravity/hover/etc. flags are "sent to the client" (`Unit.cpp:14125-14171, 15023-15059, 16147-16337`); see #14. |
| 14 | Protocol-reflex acks | **a** | Far teleport: `session->HandleMoveWorldportAck()` (public, `WorldSession.h:803`, `MovementHandler.cpp:51-290`). Near teleport: build `MSG_MOVE_TELEPORT_ACK` and `session->HandleMoveTeleportAck(pkt)` (`MovementHandler.cpp:292-360`). Knockback: `MoveKnockbackFrom(..., true)` (`MotionMaster.cpp:620-...`). Time sync: inject `CMSG_TIME_SYNC_RESP`. Speed/root/gravity acks: optional injected `CMSG_*_ACK`. | Teleport semaphores `Player.cpp:1501-1647`; same calls used by core itself (`CharacterHandler.cpp:741-767`, `WorldSession.cpp:689-690`) and playerbots (`PlayerbotAI.cpp:847-920`) | Detect by polling `IsBeingTeleportedFar/Near()` in `OnUpdate` (or by opcode in PacketSink → flag for the world thread). Speed acks not required: speed applies immediately (`Unit.cpp:11074-11085`). |
| 15 | World-thread tick hook | **a** | `WorldScript::OnUpdate(diff)` (`WorldScript.h:71`) | `World::Update` order: sessions `:1227` → maps `:1254` (joined by `MapUpdater::wait()`, `MapMgr.cpp:279-280`, `MapUpdater.cpp:124-132`) → … → `OnWorldUpdate` `:1355` | Runs on main thread with no map worker active → safe to touch Players (PLAN §4.1 #2 confirmed: "after map updates"). |
| 16 | Per-character map-thread hook (future, PLAN §5.4) | **a** | `PlayerScript::OnPlayerBeforeUpdate / OnPlayerUpdate / OnPlayerAfterUpdate` | `PlayerUpdates.cpp:58, 314, 431` | Map worker thread of the player's map. |
| 17 | Game events for EventBus | **a** (mostly) | Existing `PlayerScript`/`UnitScript`/… hooks + PacketSink | — | Per-event source table is R3/`docs/events.md` scope. |

**Result: no capability requires a core change.** Two optional patches are listed for fidelity/ergonomics only.

## Optional patches (not required)

### P1 — Let AC route packets for headless sessions (fidelity)
Purpose: make injected packets follow AC's exact `MapSessionFilter` / `WorldSessionFilter` threading, status
switch, AntiDOS and exception handling, instead of our replica (#10).
```diff
--- src/server/game/Server/WorldSession.cpp  (Update, line 414)
-    while (m_Socket && _recvQueue.next(packet, updater))
+    while ((m_Socket || _headless) && _recvQueue.next(packet, updater))
```
Effect: `Map::Update` would process THREADSAFE packets on the map thread for in-world headless players
(`Map.cpp:449-450`). THREADUNSAFE/INPLACE and not-in-world packets still need a world-filter `Update` call:
either the module calls `session->Update(diff, worldFilter)` from `OnUpdate` (ignoring its `false` return,
`WorldSession.cpp:618-620`), or patch P2. Risk: low; 1 line; upstreamable ("headless sessions may receive
injected packets"). Note `Update(worldFilter)` would also tick timed logout and fire `OnSessionUpdate`.

### P2 — Allow registering headless sessions with `WorldSessionMgr` (ergonomics)
```diff
--- src/server/game/Server/WorldSession.cpp  (Update, line 618)
-        if (!m_Socket)
+        if (!m_Socket && !_headless)
             return false;
```
plus guards so `AddSession_` skips queueing/`InitializeSession` for headless and `KickAll` → `LogoutPlayer`
(`KickPlayer` alone only sets a flag for headless; the session would then also need an exit path, e.g.
`if (_headless && IsKicked()) return false;`). Gains: AC ticks, kicks, counts and shuts down headless sessions
itself. Cost: several touched functions in `WorldSessionMgr.cpp` (`AddSession_` `:331-409`, `UpdateSessions`
`:137-195`), larger blast radius, changes player-limit/queue accounting. **Not recommended** — the module-owned
model (#2, #6-#9) already covers this and is what playerbots does on stock.

## Things to watch (not core gaps, but runtime obligations)
1. Exceptions from handlers (`ByteBufferException` on malformed injected packets) must be caught in our drain —
   an uncaught exception on the world thread crashes worldserver (PLAN goal 6).
2. RBAC warm-up on the world thread (#1) to avoid a synchronous LoginDatabase query on a map thread.
3. DB callbacks queued on the session after the player is in world may complete on a **map thread**
   (`WorldSession.cpp:592` via `Map.cpp:450`). Module-owned queries (account lookup, etc.) should use a
   module-owned callback processor drained in `OnUpdate`, not the session's.
4. `account.online` stays set after a headless logout (`WorldSession.cpp:192` skips the reset; cleared at shutdown,
   `Main.cpp:378-379`) — effect on authserver not verified **[open]**.
5. Other modules see headless characters as players (`OnPlayerLogin`, etc.). If mod-playerbots is co-installed,
   both modules create `IsHeadless()` sessions — always filter by our registry, never by `IsHeadless()` alone.
6. PLAN §4.6.1 says "AC's opcode table decides which thread processes it". Under D2(a) that becomes: "we read the
   opcode table's `Status` and execute the handler on the world thread inside `OnUpdate`". Record in §13.

## Recommendation for D2

**Choose D2 (a): stock AzerothCore, zero core patches**, pinned to a master revision at or after `92fed92ea`
(currently `0540ae8013bc92d93f1f0d3346ccbc1a1a4d6f85`).

Rationale:
- Every capability in PLAN §5.3 maps to an existing public API or script hook at this SHA (table above);
  none is blocked.
- The hooks were upstreamed specifically for "server-driven characters" by the playerbots team, so they are
  likely to stay stable and tested by a large user base; mod-playerbots' current code uses the same mechanics on
  these APIs.
- The only cost is that our module, not AC, routes injected packets (#10). The replica is ~40 lines mirroring
  `WorldSession::Update:443-554`, runs on the world thread while maps are joined, and is the established playerbots
  pattern. Keep patch P1 documented as a fallback if Spike B or Phase 3 reveals a THREADSAFE handler that misbehaves
  outside its map thread **[none expected; inferred]**.
- Spike B (Phase 0 item 8) should confirm empirically: 10-minute online, MovePoint, injected `CMSG_MESSAGECHAT` with
  captured `SMSG_MESSAGECHAT`, `.tele` near/far completing via reflexes, GM `.kick` handled, clean logout and
  clean shutdown. If any of these fails on stock, revisit D2 with P1 as the first candidate patch.
