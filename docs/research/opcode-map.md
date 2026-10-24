# R3 — Opcode map for mod-acsdk primitives

- **AC source:** azerothcore-wotlk master @ `0540ae8013bc92d93f1f0d3346ccbc1a1a4d6f85`
- **Scope:** every primitive planned in PLAN.md Phases 2–4, plus the protocol reflexes of §5.2.
- **Method:** read the handler code and the opcode table. The handler's read code is the packet layout spec (§4.6.1).
- **Evidence markers:** *(verified)* = read in source at the cited location. *(inferred)* = reasoned from code nearby or from naming, but not traced end to end. Each inferred item is something Spike B or Phase 3 tests should confirm.

Paths are relative to `src/server/game/`.

---

## 0. Cross-cutting findings (read first)

### 0.1 Stock `WorldSession::Update` does NOT drain the receive queue of a socketless session *(verified)*

`Server/WorldSession.cpp`, `WorldSession::Update`:

```cpp
while (m_Socket && _recvQueue.next(packet, updater))   // line 414
...
if (updater.ProcessUnsafe()) { ...  if (!m_Socket) return false; }   // line 618-621
```

- `QueuePacket()` (line 360) only does `_recvQueue.add()`, so injected packets **are queued but never processed** when `m_Socket == nullptr`.
- A socketless session registered in `WorldSessionMgr::_sessions` returns `false` from its first world-thread `Update`. `WorldSessionMgr::UpdateSessions` (`Server/WorldSessionMgr.cpp:179-194`) then erases and **deletes** it.
- AC already has a partial headless concept: `WorldSession::_headless = !sock` (`WorldSession.cpp:148`, `IsHeadless()` in `WorldSession.h:1234`). Today it only skips the account `online`/`totaltime` DB writes, and `m_Address` becomes `"headless"`.
- `ServerScript::OnPacketSent(WorldSession*, WorldPacket const&)` (`Scripting/ScriptDefines/ServerScript.h:85`) is called in `WorldSession::SendPacket` **before** the socket check (`WorldSession.cpp:310`). Its doc comment says it exists for client-less sessions. **Outgoing capture works on stock AC.**

**Consequence for §4.6.1 ("prefer queueing into the session").** Queueing alone is impossible on stock AC. The two options, for decision D2:

| Option | What | Thread rules honoured? |
|---|---|---|
| **A — core patch** (2–3 lines) | `while ((m_Socket \|\| _headless) && ...)` and `if (!m_Socket && !_headless) return false;` in `WorldSession::Update`. Then `QueuePacket` works unchanged. AC's own filters (`MapSessionFilter`/`WorldSessionFilter`), status checks, AntiDOS and exception handling all apply. | Yes, by construction. |
| **B — module-side dispatch** | Call `opcodeTable[op]->Call(session, pkt)` ourselves. The module must replicate the status switch (`WorldSession.cpp:443-519`: LOGGEDIN needs `_player->IsInWorld()`, TRANSFER needs `_player && !IsInWorld()`), the `sScriptMgr->CanPacketReceive` call, and the `try/catch` for `ByteBufferException`/hyperlink exceptions. Run PROCESS_THREADSAFE opcodes from a per-character inbox drained in `PlayerScript::OnPlayerBeforeUpdate`: that runs on the map thread, right after `Map::Update` would have called `session->Update(MapSessionFilter)` (`Maps/Map.cpp:447-454`). Run PROCESS_THREADUNSAFE / PROCESS_INPLACE / STATUS_TRANSFER opcodes from `WorldScript::OnUpdate`, on the world thread. | Yes if built as described. It duplicates about 40 lines of dispatch glue, but no game logic. |

Option B is the only one possible with zero core edits. It is allowed by §4.6.1's fallback clause. This finding belongs to R1/R2; it is recorded here because every row below depends on it.

### 0.2 Processing places in Opcodes.cpp *(verified)*

From `Server/Protocol/Opcodes.h:1368-1370` and the filters in `WorldSession.cpp:66-107`:

| ProcessingPlace | Where AC runs it | Notes |
|---|---|---|
| `PROCESS_INPLACE` | Both `Map::Update` (map thread) and `World::UpdateSessions` (world thread). The first filter to see it wins. | Usually map thread when the player is in world. |
| `PROCESS_THREADUNSAFE` | Only `WorldSessionMgr::UpdateSessions` → world thread. | |
| `PROCESS_THREADSAFE` | `Map::Update` → map worker thread, only while `player->IsInWorld()`. When not in world it falls to the world thread. | |

**Ordering hazard *(verified from the filters)*:** packets of different processing places are drained at different points of the tick. World-thread UpdateSessions runs at `World.cpp:1227`, map updates at `World.cpp:1254`. So **injection order is not preserved across processing classes**. Example: `CMSG_SET_SELECTION` (THREADSAFE) followed by `CMSG_LOOT` (THREADUNSAFE) can run in the opposite order. The module must not rely on two injected packets of different classes running in order within one tick. Option B can enforce order explicitly.

`WorldScript::OnUpdate` fires at `World.cpp:1355`, after `sMapMgr->Update` has joined the map threads (`MapMgr.cpp:279-280`, `m_updater.wait()`). So it runs with no map thread active, which confirms §4.1 #2's premise.

### 0.3 Status codes

Every primitive below is `STATUS_LOGGEDIN` (needs `_player` and `_player->IsInWorld()`) unless noted otherwise. Packets with an unmet status are silently ignored. They are not requeued; the requeue is commented out at `WorldSession.cpp:453`.

### 0.4 Serializers available

| Need | AC serializer |
|---|---|
| Packed GUID | `ObjectGuid::WriteAsPacked()` / `Object::GetPackGUID()` / `ByteBuffer::appendPackGUID()` |
| Full GUID (uint64) | `data << ObjectGuid` |
| `SpellCastTargets` | `SpellCastTargets::Write(ByteBuffer&)` (`Spells/Spell.cpp:216`), which mirrors `Read` (`:163`). Caveat: item targets write `m_itemTarget->GetPackGUID()`, so `SetItemTarget(Item*)` must be called with a live item on the world/map thread; otherwise it writes `0`. |
| MovementInfo with packed GUID prefix (MSG_MOVE_* body) | `WorldSession::WriteMovementInfo(WorldPacket*, MovementInfo*)` (`Server/WorldSession.cpp:1203`). It writes `mi->guid` packed, then the body. |
| MovementInfo body only (used after `guid + counter` in acks) | `Unit::BuildMovementPacket(ByteBuffer*)` (`Entities/Unit/Unit.h:2074`). AC uses it this way in `Player::SendTeleportAckPacket` (`Entities/Player/Player.cpp:1391-1399`). |
| Chat | None for CMSG; hand-build (layout below). |

### 0.5 Silent no-reply paths are common

Many handlers `return` without sending anything: wrong state, unknown spell, spell not in spellbook, etc. **Every watcher needs a server-side timeout.** A timeout with no matching reply should be reported as `NO_RESPONSE`, not guessed. Pre-checking game rules is forbidden by §4.6.1, so the plumbing cannot tell the controller *why* AC stayed silent.

### 0.6 `KickPlayer` on a headless session only sets a flag *(verified)*

`WorldSession::KickPlayer` (`WorldSession.cpp:881-893`) closes the socket only when one exists; otherwise it just sets `_kicked`. Anti-cheat and handler kicks therefore do not remove a headless character, including:

- `KickPlayer("Incorrect speed")` in `HandleForceSpeedChangeAck`
- the double-jump check
- `!MapMgr::IsValidMapCoord` in worldport ack

The runtime must poll `IsKicked()` and run its own logout path. That is a lifecycle transition (→ `CharacterStateChanged`), not autonomy.

### 0.7 Client-side state a headless character lacks: summary

| Handler / mechanic | Missing client state | Effect for headless | Resolution |
|---|---|---|---|
| `HandleMovementOpcodes` (all `MSG_MOVE_*`, incl. SET_FACING) | Client-simulated position/flags. Packet MovementInfo **overwrites** `m_movementInfo` wholesale (`HandleMoverRelocation`, `MovementHandler.cpp:434-435`). | Any field we fill wrong becomes truth. The packet is rejected while a server spline is active (`!mover->movespline->Finalized()`, `:543-547`). | Build MovementInfo from the server's current `m_movementInfo`. Only send when the spline is finalized. |
| `SynchronizeMovement` (`:416-428`) | Time-sync clock delta | `_timeSyncClockDelta == 0` logs `LOG_INFO("misc", ...)` on **every** movement packet and falls back to `getMSTime()`. | Time-sync reflex (§3.4), or accept the log spam. |
| Facing (`HasInArc` checks in `Spell::CheckCast`, melee `BADFACING`) | Client turns the character | Nothing turns a headless character except movement splines, `Face`, or `ChaseMovementGenerator`. | Controller must `Face` before casting or meleeing. Report AC's `SPELL_FAILED_UNIT_NOT_INFRONT` / `SMSG_ATTACKSWING_BADFACING` as-is. |
| `Spell::prepare` moving check (`Spells/Spell.cpp:3671`) | — | `isMoving()` reads `m_movementInfo` flags (`Unit.h:1720`). `MoveSplineInit::Launch` sets `MOVEMENTFLAG_FORWARD` (`Movement/Spline/MoveSplineInit.cpp:69-111`), so casting during `MoveTo` yields `SPELL_FAILED_MOVING`, as for a real client. | Correct behaviour; document it. |
| Root / water-walk / hover / feather-fall / can-fly / gravity | For client-controlled units, AC sets the server-side `m_movementInfo` flag **only when the client acks** (`Unit::SetCanFly` etc., `Unit.cpp:16207-16370`; `SendMoveRoot` `:14125-14147`). | Without acks the server-side flag never changes, which inconsistently affects `IsRooted()`, `IsFlying()`, `CanFly`. | Movement-flag acks are needed as reflexes (§3.3). |
| Knockback (`Unit::KnockbackFrom`, `Unit.cpp:15023-15060`) | Client simulates the arc and reports the landing | AC sends `SMSG_MOVE_KNOCK_BACK` and does nothing server-side. A headless character stays put. | No non-reinventing fix within the whitelist. Ack with current position, or ignore. `MotionMaster::MoveKnockbackFrom` (used by AC when not client-controlled) would need a §4.6.3 whitelist entry. **Owner decision.** |
| Area triggers (`CMSG_AREATRIGGER`, client-detected) | Client detects trigger volumes | Headless characters moved by MotionMaster never fire instance portals, tavern rest, or exploration triggers. | Out of Phase 2–4 scope; flag for later. |
| Chat `.commands` | — | `ChatHandler::ParseCommands` runs for any message (`Handlers/ChatHandler.cpp:307`). A Say of `.xyz` executes a GM command if the account's security allows. | Headless accounts must be player-level. Consider rejecting a leading `.`/`!` in plumbing as server protection (not a game rule). |

---

## 1. Movement primitives (whitelisted direct calls, §4.6.3)

### 1.1 MoveTo / Stop / Follow — `MotionMaster` (no opcode)

- **Why not opcodes *(verified)*:** `MSG_MOVE_START_FORWARD` etc. (0x0B5–0x0C3, THREADSAFE, `HandleMovementOpcodes`) carry a client-simulated position. Synthesizing them means reimplementing client physics. Also, `VerifyMovementInfo` rejects them while a server spline runs.
- **API:** `Player::GetMotionMaster()->MovePoint(id, x, y, z, ...)`, `MoveFollow(target, dist, angle)`, `Clear()` / `MoveIdle()` plus `StopMoving()`.
  - Player instantiations exist: `PointMovementGenerator<Player>` (`Movement/MovementGenerators/PointMovementGenerator.cpp:339-350`), `FollowMovementGenerator<Player>` / `ChaseMovementGenerator<Player>` (`TargetedMovementGenerator.cpp:783-804`).
- **Thread:** world thread inside our `WorldScript::OnUpdate`. Map threads are idle there (§0.2).
- **Server packets:** `SMSG_MONSTER_MOVE` (0x0DD) is broadcast to the set. **Do not use it for completion**: it is high-volume and is a visibility packet.
- **Completion (watcher, state-polled):**
  - **MoveTo:** COMPLETED when `movespline->Finalized()`, the top generator is not POINT, and distance ≤ tolerance.
  - **MoveTo FAILED cases:**
    - no path (`PathGenerator` result type)
    - no progress for N s
    - died (`!IsAlive()`)
    - teleported (`IsBeingTeleported()` or map change)
    - timeout
  - `PointMovementGenerator::MovementInform` only informs creature AI *(verified, `:316`)*, so polling is required.
- **Correlation:** not packet-based. There is exactly one MotionMaster active slot per character, so this is **single-in-flight by construction**.
- **Stop:** instant (COMPLETED after `Clear()` + `StopMoving()`). `MSG_MOVE_STOP` (0x0B7) is not usable; it is rejected while a spline is active.

### 1.2 Face — two options

| | `MSG_MOVE_SET_FACING` (0x0DA) | `Unit::SetFacingTo(float)` / `SetFacingToObject` |
|---|---|---|
| Handler | `Handlers/MovementHandler.cpp: WorldSession::HandleMovementOpcodes` (`:362`). THREADSAFE, LOGGEDIN. | `Entities/Unit/Unit.cpp:16096` / `:16106`. Facing-only spline via `MoveSplineInit` (`init.MoveTo(current pos); init.SetFacing(o); Launch()`). |
| Layout | packed mover guid · `MovementInfo` body: `uint32 flags, uint16 flags2, uint32 time, float x,y,z,o`, then `[transport block if ONTRANSPORT]`, `[float pitch if SWIMMING\|FLYING or flags2 ALWAYS_ALLOW_PITCHING]`, `uint32 fallTime`, `[jump block if FALLING]`, `[float splineElevation if SPLINE_ELEVATION]` (`WorldSession::ReadMovementInfo`, `WorldSession.cpp:1089-1122`). Build with `WorldSession::WriteMovementInfo` from a copy of `player->m_movementInfo` with only `pos.o` changed and `time = getMSTime()`. | n/a |
| Preconditions | Not teleporting (`:373`); guid == `m_mover` guid (`:384`); spline finalized (`VerifyMovementInfo :543`); not rooted with moving flags; anticheat hooks pass. | `SetFacingToObject` returns early if `!IsStopped()` (`:16109`). `SetFacingTo` always launches, which **replaces any running spline (stops MoveTo/Follow)**. |
| Effect | `HandleMoverRelocation` sets `m_movementInfo = packet` and `UpdatePosition`, then broadcasts `MSG_MOVE_SET_FACING` to the set (`:411-413`). Orientation is immediate. | Orientation updates when the spline finalizes (next update). `SMSG_MONSTER_MOVE` is broadcast incl. self. |
| Headless risks | Overwrites all of `m_movementInfo` (flags, fallTime…). Wrong values corrupt state. Silently dropped while moving. | Uses the creature-style spline path on a player. Used by AC core for players too *(inferred)*. |
| SMSG reply to self | none | `SMSG_MONSTER_MOVE` (not used) |

- **Recommendation:** use `MSG_MOVE_SET_FACING` when stationary. It is the client's own path and the plan prefers opcodes.
  - If a spline is active, the Face command must either fail (`MOVING`) or be put on the `move` channel so it supersedes and stops movement first. In that case `SetFacingTo` is the cleaner whitelisted call. **Owner pick.**
- **Completion:** both are state-polled (orientation within ε, read on the world thread).
- **Correlation:** single-in-flight on the `move` channel.

---

## 2. Action primitives (injected client opcodes)

Column key: **Op** = opcode/hex · **H** = handler · **S/P** = status / processing place.

### 2.1 SetTarget — `CMSG_SET_SELECTION` 0x13D

- **H:** `Handlers/MiscHandler.cpp:534 HandleSetSelectionOpcode`. **S/P:** LOGGEDIN / THREADSAFE.
- **Layout:** `uint64 guid` (full, not packed). `0` clears.
- **Effect:** `_player->SetSelection(guid)` and retargets auto-repeat if valid.
- **Replies:** none. Only `UNIT_FIELD_TARGET` changes, which shows up in `SMSG_UPDATE_OBJECT` and is never decoded.
- **Correlation:** **fire-and-forget**. COMPLETED = packet processed. A watcher may confirm by reading `GetTarget()` on the next tick (whitelisted read).
- **Note:** spells with no explicit target fall back to this selection. `Spell::InitExplicitTargets` uses `playerCaster->GetTarget()` (`Spells/Spell.cpp:787-806`), so ordering vs `CMSG_CAST_SPELL` matters. Both are THREADSAFE, so they keep their order (§0.2).

### 2.2 Attack — `CMSG_ATTACKSWING` 0x141 · StopAttack — `CMSG_ATTACKSTOP` 0x142

- **H:** `Handlers/CombatHandler.cpp:28 HandleAttackSwingOpcode`, `:68 HandleAttackStopOpcode`. **S/P:** LOGGEDIN / THREADSAFE (both).
- **Layout:** ATTACKSWING = `uint64 victim guid` (full). ATTACKSTOP = empty.
- **Effect:** `Player::Attack(victim, true)` (`Entities/Unit/Unit.cpp:7084`). ATTACKSTOP → `AttackStop()`.
- **Success:** `SMSG_ATTACKSTART` 0x143 = `uint64 attacker, uint64 victim` (`Unit.cpp:3261-3270`), broadcast to set incl. self. Target death → `Death` event (§events) / `SMSG_ATTACKSTOP` with `NowDead`.
- **Failures:**

| Case | Reply |
|---|---|
| victim not found | `SMSG_ATTACKSTOP` 0x144 = `packed attacker, packed victim, uint32 nowDead` (`Server/Packets/CombatPackets.cpp:25-34`), via `SendMeleeAttackStop(nullptr)` |
| `!IsValidAttackTarget` | `SMSG_ATTACKSTOP` |
| vehicle seat can't attack | `SMSG_ATTACKSTOP` |
| mounted / dead / same target / GM victim / evading | **silent** (`Unit::Attack` returns false) |
| out of range, per swing | `SMSG_ATTACKSWING_NOTINRANGE` 0x145, empty, sent once per error streak (`m_swingErrorMsg`, `Entities/Player/PlayerUpdates.cpp:175-188`) |
| bad facing | `SMSG_ATTACKSWING_BADFACING` 0x146, empty |
| dead target | `SMSG_ATTACKSWING_DEADTARGET` 0x148, empty |
| can't attack | `SMSG_ATTACKSWING_CANT_ATTACK` 0x149, empty |
| combat cancelled (CC, evade…) | `SMSG_CANCEL_COMBAT` 0x14E, empty |

- **Correlation:** `SMSG_ATTACKSTART` / `SMSG_ATTACKSTOP` carry attacker+victim, so **matching keys** (attacker == self and victim == requested). The `ATTACKSWING_*` errors are payload-less and only ever about our own swing, so they need **single-in-flight on the `melee` channel**.
- **Headless flag:** BADFACING is likely unless the controller `Face`s first. AC does not auto-face players.
- **StopAttack:** fire-and-forget (AC sends `SMSG_ATTACKSTOP` when it was attacking; it can be matched by attacker == self).

### 2.3 CastSpell — `CMSG_CAST_SPELL` 0x12E

- **H:** `Handlers/SpellHandler.cpp:386 HandleCastSpellOpcode`. **S/P:** LOGGEDIN / THREADSAFE.
- **Layout (read order):**
  1. `uint8 castCount`
  2. `uint32 spellId`
  3. `uint8 castFlags`
  4. `SpellCastTargets`: `uint32 targetMask`, then `[packed objectGuid if UNIT|UNIT_MINIPET|GAMEOBJECT|CORPSE_ENEMY|CORPSE_ALLY]`, `[packed itemGuid if ITEM|TRADE_ITEM]`, `[packed transportGuid + float x,y,z if SOURCE_LOCATION]`, `[same if DEST_LOCATION]`, `[cstring if STRING]` (`Spell.cpp:163-214`)
  5. `[if castFlags & 0x02: float elevation, float speed, uint8 hasMovementData, (uint32 opcode + MSG_MOVE body)]` (`HandleClientCastFlags`, `SpellHandler.cpp:35-56`). **Always send `castFlags = 0`.** Flag 0x02 re-enters `HandleMovementOpcodes`.
- **Serializer:** `SpellCastTargets::Write`.
- **Flow:**
  1. Unknown spell → silent.
  2. Not in spellbook / passive → **silent** return (`:458-483`).
  3. `SpellQueue` handling (`:419-447`): if GCD/cooldown/cast-in-progress is within `CONFIG_SPELL_QUEUE_WINDOW`, the packet is **copied into `_player->SpellQueue` and processed later** by `Player::ProcessSpellQueue` (`Entities/Player/PlayerUpdates.cpp:2435`) with the same bytes. **No immediate reply.**
  4. Rank auto-selection for friendly targets may change `spellInfo` (`:543-552`). `ValidateSpellAtCastSpell` scripts may change `spellId` (`:515-518`).
  5. `Spell::prepare` (`Spell.cpp:3557`).
- **Replies (all carry the echoed `castCount`, verified):**

| Outcome | Packet | Layout |
|---|---|---|
| Fail (any CheckCast result, disabled, in progress, moving, …) | `SMSG_CAST_FAILED` 0x130, **to caster only** | `uint8 castCount, uint32 spellId, uint8 SpellCastResult, [extra per result, Spell.cpp:4711-4808]` (`WriteCastResultInfo :4706`, `SendCastResult :4811`) |
| Cast start (cast-time / channel) | `SMSG_SPELL_START` 0x131, broadcast incl. self | `packed castItemOrCaster, packed caster, uint8 castCount, uint32 spellId, uint32 castFlags, int32 timer, SpellCastTargets…` (`:4916-4928`) |
| Cast go (success) | `SMSG_SPELL_GO` 0x132, broadcast incl. self | `packed castItemOrCaster, packed caster, uint8 castCount, uint32 spellId, uint32 castFlags, uint32 msTime, targets…` (`:5015-5028`) |
| Interrupted | `SMSG_SPELL_FAILURE` 0x133 **and** `SMSG_SPELL_FAILED_OTHER` 0x2A6, both broadcast | `packed caster, uint8 castCount, uint32 spellId, uint8 result` (`SendInterrupted :5327-5342`) |
| Cancel during PREPARING | `SMSG_CAST_FAILED(SPELL_FAILED_INTERRUPTED)`, then SendInterrupted (`Spell::cancel :3822-3834`) | |
| Channel start / end | `MSG_CHANNEL_START` 0x139 = `packed caster, uint32 spellId, uint32 duration`; `MSG_CHANNEL_UPDATE` 0x13A = `packed caster, uint32 time` (time 0 = ended) (`:5344-5389`) | **no castCount** |
| Cancel-in-progress race | `SMSG_CAST_FAILED(SPELL_FAILED_DONT_REPORT)` (`SpellHandler.cpp:422-429`) | |

- **Reason enum:** `SpellCastResult` (`SharedDefines.h`). Report by name, e.g. `SPELL_FAILED_OUT_OF_RANGE`.
- **Correlation — echoed field (confirmed):** `castCount` sent in CMSG_CAST_SPELL is copied to `spell->m_cast_count` (`:558`). It is echoed by CAST_FAILED, SPELL_START, SPELL_GO, SPELL_FAILURE/FAILED_OTHER. Matching key = `caster guid == self && castCount == ours`.
  - Use **non-zero** counts (1..255, wrapping, skip 0). AC-triggered spells use `m_cast_count = 0` (see the `!m_cast_count` checks at `:4877`, `:4960`), so 0 would collide with them.
  - Do **not** require `spellId` equality: rank auto-selection and `ValidateSpellAtCastSpell` can change it. Record the actual id.
  - Spells cast *on* the character by others carry a different caster guid and are ignored. This satisfies the Phase 3 acceptance test.
  - Channel end (`MSG_CHANNEL_UPDATE time=0`) has no castCount. Attribute it to the in-flight channel with caster == self. Only one channel can be active (`CURRENT_CHANNELED_SPELL`).
- **Completion:**
  - Instant / cast-time spell: `SMSG_SPELL_GO`.
  - Channel: `MSG_CHANNEL_UPDATE(0)` without a preceding `SPELL_FAILURE` → COMPLETED; with one → FAILED(INTERRUPTED).
  - No reply within timeout → `NO_RESPONSE`. Covers silent paths: not in spellbook, passive, possessing, autorepeat already active.
- **Queued casts:** while the request sits in `SpellQueue` nothing is sent. `CMSG_CANCEL_CAST` clears the queue silently (`SpellHandler.cpp:569`). `setDeathState` also clears it (`PlayerUpdates.cpp:2451-2454`). The watcher must treat our own CancelCast and death as terminal for a queued cast.

### 2.4 CancelCast — `CMSG_CANCEL_CAST` 0x12F · CancelChannel — `CMSG_CANCEL_CHANNELLING` 0x13B

**CMSG_CANCEL_CAST**

- **H:** `SpellHandler.cpp:562 HandleCancelCastOpcode`. **S/P:** LOGGEDIN / THREADSAFE.
- **Layout:** `uint8 counter` (skipped), `uint32 spellId`.
- **Effect:**
  - `SpellQueue.clear()`
  - `InterruptSpell(CURRENT_MELEE_SPELL)`
  - if casting, `InterruptNonMeleeSpells(false, spellId, /*withInstant*/false, /*bySelf*/true)`. `spellId` is a filter; 0 = any (`Unit.h:1606`).
- **Replies:**
  - Preparing cast: `SMSG_CAST_FAILED(INTERRUPTED)` + `SMSG_SPELL_FAILURE`, carrying the **cancelled cast's** castCount.
  - Channel in CASTING state with `bySelf = true`: `Spell::cancel` sends **no** channel-update/interrupt packet (`Spell.cpp:3835-3848`). Whether `finish()` later sends `MSG_CHANNEL_UPDATE(0)` is *(inferred, verify)*.

**CMSG_CANCEL_CHANNELLING**

- **H:** `SpellHandler.cpp:661 HandleCancelChanneling`. **S/P:** LOGGEDIN / **INPLACE**.
- **Layout:** `uint32 spellId`. Must equal the current channel's id, else silent.
- **Effect:** `mover->InterruptSpell(CURRENT_CHANNELED_SPELL)` with `bySelf = false`, which gives `MSG_CHANNEL_UPDATE(0)` + `SMSG_SPELL_FAILURE`.

**Correlation:** both commands are **fire-and-forget**. Their effect resolves the *target* cast's watcher, matched by castCount, as `CANCELLED`. They must be on the `cast` channel so the runtime knows which cast they target.

### 2.5 Say / Whisper / Party — `CMSG_MESSAGECHAT` 0x095

- **H:** `Handlers/ChatHandler.cpp:61 HandleMessagechatOpcode`. **S/P:** LOGGEDIN / **THREADUNSAFE** (world thread).
- **Layout:**

| Type | Bytes |
|---|---|
| common prefix | `uint32 ChatMsg type`, `uint32 Language lang` |
| SAY / YELL / EMOTE / PARTY / PARTY_LEADER / RAID* / GUILD / OFFICER / BG* | `cstring msg` |
| WHISPER | `cstring to`, `cstring msg` |
| CHANNEL | `cstring channel`, `cstring msg` |

- **Validation, all AC's (do not duplicate):**
  - `LANG_UNIVERSAL` is rejected for non-AFK/DND types (`:76-82`). Use a racial language or `LANG_COMMON`/`LANG_ORCISH`; GMs are forced to universal.
  - Message ≤ 255 bytes.
  - No `|0`.
  - Hyperlink validation can throw; that is why Option B needs the try/catch (§0.1).
- **Success echoes (to self):**
  - **Say:** `Player::Say` → `SendDirectMessage(SMSG_MESSAGECHAT CHAT_MSG_SAY)` to self (`Entities/Player/Player.cpp:9594-9597`) plus range broadcast.
  - **Whisper:** `SMSG_MESSAGECHAT CHAT_MSG_WHISPER_INFORM` (or `CHAT_MSG_FILTERED`) to the sender, with `receiver = target` (`Player.cpp:9711-9716`).
  - **Party:** group broadcast incl. sender *(inferred: `Group::BroadcastPacket(..., ignore = Empty)`)*.
- **SMSG_MESSAGECHAT 0x096 layout:** `uint8 type, int32 lang, uint64 senderGuid, uint32 0, [type-specific names/channel], uint64 receiverGuid, uint32 len, cstring msg, uint8 chatTag, [uint32 achievementId]` (`Chat/Chat.cpp:273-348`). `SMSG_GM_MESSAGECHAT` 0x3B3 is used when the sender is GM-tagged.
- **Failures:**

| Cause | Reply |
|---|---|
| whisper target not found / not accepting | `SMSG_CHAT_PLAYER_NOT_FOUND` 0x2A9 = `cstring name` (`ChatHandler.cpp:814`) |
| cross-faction | `SMSG_CHAT_WRONG_FACTION` 0x219, empty |
| trial restriction | `SMSG_MESSAGECHAT CHAT_MSG_RESTRICTED` |
| unknown language, not learned, muted, level requirement, GM silence | `SMSG_NOTIFICATION` 0x1CB (localized text) *(inferred: `ChatHandler::SendNotification`)* |
| chat filter | `CHAT_MSG_SYSTEM` |
| dead (Say/Yell/Emote), not in group (Party), empty msg, bad char, spectator | **silent** |

- **Correlation — matching keys:**
  - echo with `sender == self && type == requested && msg == sent`
  - for whisper, `INFORM && receiver name/guid`
  - `PLAYER_NOT_FOUND` carries the name.
  - `WRONG_FACTION` / `NOTIFICATION` have no key, so they need single-in-flight on `chat`.
  - PLAN §4.2 allows "COMPLETED immediately after send". The recommendation is the echo-based completion above, with a short timeout fallback to `COMPLETED_UNCONFIRMED`.

### 2.6 UseItem — `CMSG_USE_ITEM` 0x0AB

- **H:** `SpellHandler.cpp:58 HandleUseItemOpcode`. **S/P:** LOGGEDIN / **INPLACE**.
- **Layout:**
  1. `uint8 bagIndex`
  2. `uint8 slot`
  3. `uint8 castCount`
  4. `uint32 spellId` (the item's on-use spell; must exist in SpellInfo or the packet is silently dropped)
  5. `uint64 itemGuid` (full)
  6. `uint32 glyphIndex`
  7. `uint8 castFlags`
  8. `SpellCastTargets`
  9. `[castFlags 0x02 block]`
- **Requirements:** `m_mover == player`. `itemGuid` must match the item at `bag/slot`.
- **Pre-spell failures:** `SMSG_INVENTORY_CHANGE_FAILURE` 0x112 = `uint8 InventoryResult, [uint64 item1, uint64 item2, uint8 bagSubclass, extra]` (`Entities/Player/PlayerStorage.cpp:4131-4171`). Values include:
  - `EQUIP_ERR_ITEM_NOT_FOUND`
  - `EQUIP_ERR_NOT_DURING_ARENA_MATCH`
  - `EQUIP_ERR_NOT_IN_COMBAT`
  - any `CanUseItem` result
  - `EQUIP_ERR_NONE` (spell-queue cancel race)
  - Item guid is often **Empty** (`SendEquipError(..., nullptr, nullptr)`).
- **Spell phase:** `Player::CastItemUseSpell` (`Player.cpp:7595`) sets `spell->m_cast_count = castCount`. All CastSpell replies (§2.3) therefore apply, with `castItemOrCaster` = the item's packed guid in SPELL_START/GO. A spell on cooldown → `SMSG_CAST_FAILED(SPELL_FAILED_NOT_READY)` with castCount (`:7647-7653`).
- **Spell queue:** item casts can also be queued silently (`:110-122`).
- **Correlation:**
  - spell part: **echoed castCount**, from the same per-character counter as CastSpell.
  - pre-spell part: `SMSG_INVENTORY_CHANGE_FAILURE` has **no echo**, and the item guid is unreliable. It is shared with loot/equip errors, so it needs **single-in-flight on the `item` channel**.
- **Completion:**
  - `SPELL_GO` with our castCount → COMPLETED.
  - CAST_FAILED / INVENTORY_CHANGE_FAILURE → FAILED.
  - Items with no on-use spell → silent → timeout.

### 2.7 Loot

| Step | Opcode | H / S / P | Layout | Success | Failure |
|---|---|---|---|---|---|
| Open (creature) | `CMSG_LOOT` 0x15D | `Handlers/LootHandler.cpp:287 HandleLootOpcode` / LOGGEDIN / **THREADUNSAFE** | `uint64 guid` (must be creature/vehicle; silent if dead looter or other type) | `SMSG_LOOT_RESPONSE` 0x160 = `uint64 lootGuid, uint8 LootType, LootView…` (`Player.cpp:8386-8391`) | `SMSG_LOOT_RESPONSE` = `uint64 guid, uint8 LOOT_NONE, uint8 LootError` (`SendLootError :8403`). Errors: `LOOT_ERROR_DIDNT_KILL`, `TOO_FAR`, `PLAY_TIME_EXCEEDED`, … Some paths send `SMSG_LOOT_RELEASE_RESPONSE`. Opening interrupts current non-melee casts. |
| Take item | `CMSG_AUTOSTORE_LOOT_ITEM` 0x108 | `LootHandler.cpp:33` / LOGGEDIN / **INPLACE** | `uint8 lootSlot` (uses server-stored `GetLootGUID()`) | `SMSG_LOOT_REMOVED` 0x162 = `uint8 slot` (to all looters or self), then `SMSG_ITEM_PUSH_RESULT` 0x166 = `uint64 playerGuid, uint32 received, uint32 created, uint32 showChat, uint8 bag, uint32 slot, uint32 itemId, uint32 suffix, int32 randomProp, uint32 count, uint32 invCount` (`PlayerStorage.cpp:4855-4878`; may be group-broadcast) | `SMSG_INVENTORY_CHANGE_FAILURE` (`EQUIP_ERR_ALREADY_LOOTED`, bag full…), `SMSG_LOOT_RELEASE_RESPONSE` (blocked / roll item / not allowed), `SMSG_LOOT_RESPONSE` error (creature too far) (`Player.cpp:13862-13910`) |
| Take money | `CMSG_LOOT_MONEY` 0x15E | `LootHandler.cpp:122` / LOGGEDIN / THREADUNSAFE | empty | `SMSG_LOOT_MONEY_NOTIFY` 0x163 = `uint32 amount, uint8 soleLooter` (to each sharing member); `SMSG_LOOT_CLEAR_MONEY` 0x165 to looters | `SMSG_LOOT_RESPONSE` error; silent if no loot guid |
| Release | `CMSG_LOOT_RELEASE` 0x15F | `LootHandler.cpp:313` / LOGGEDIN / THREADUNSAFE | `uint64 guid` (must equal current loot guid, else silent) | `SMSG_LOOT_RELEASE_RESPONSE` 0x161 = `uint64 guid, uint8 1` (`DoLootRelease`, `LootHandler.cpp:327-333` → `Player::SendLootRelease`, `Player.cpp:7987`) | silent |

- **Correlation:**
  - Open and Release: **matching key** = loot guid.
  - Item: `LOOT_REMOVED.slot` plus `ITEM_PUSH_RESULT.playerGuid == self`. Errors via `INVENTORY_CHANGE_FAILURE` have no key, so **single-in-flight**.
  - Money: `LOOT_MONEY_NOTIFY` has no key, so single-in-flight.
  - Mixed processing places (INPLACE vs THREADUNSAFE) mean Open → TakeItem → Release must be **sequenced one at a time** (§0.2 ordering hazard). A single `loot` command channel enforces that. Because inventory errors are shared with UseItem, the plan merges both into one `item` channel (§5).

### 2.8 RespondToPrompt

All prompts arrive as SMSG (see `docs/events.md` → `Prompt`). Each response is a separate opcode:

| Prompt (SMSG in) | Response opcode | H / S / P | Layout | Result observed | Correlation |
|---|---|---|---|---|---|
| Group invite `SMSG_GROUP_INVITE` 0x06F (`uint8 1, cstring inviterName, uint32, uint8, uint32`; **no guid**) | accept `CMSG_GROUP_ACCEPT` 0x072 / decline `CMSG_GROUP_DECLINE` 0x073 | `Handlers/GroupHandler.cpp:240` / `:332`; LOGGEDIN / THREADUNSAFE | accept: `uint32` (skipped); decline: empty | accept → `SMSG_GROUP_LIST` 0x07D *(inferred: `BroadcastGroupUpdate`)*. Errors: `SMSG_PARTY_COMMAND_RESULT` 0x07F (`ERR_GROUP_FULL`, `ERR_INVITE_RESTRICTED`) or silent. Decline → `SMSG_GROUP_DECLINE` goes to the leader only (nothing to self). | One pending invite per player (`GetGroupInvite()`). **Matching key:** prompt kind. Decline is fire-and-forget. |
| Trade request `SMSG_TRADE_STATUS` 0x120 `TRADE_STATUS_BEGIN_TRADE` (+ `uint64 traderGuid`) | accept `CMSG_BEGIN_TRADE` 0x117; decline `CMSG_CANCEL_TRADE` 0x11C (or `CMSG_BUSY_TRADE` 0x118 / `CMSG_IGNORE_TRADE` 0x119); confirm `CMSG_ACCEPT_TRADE` 0x11A; unconfirm `CMSG_UNACCEPT_TRADE` 0x11B | `Handlers/TradeHandler.cpp:692/714/69/64/237/683`; LOGGEDIN / THREADUNSAFE; CANCEL_TRADE is `STATUS_LOGGEDIN_OR_RECENTLY_LOGGOUT` | all empty (ACCEPT_TRADE body not read) | `SMSG_TRADE_STATUS OPEN_WINDOW`; `CLOSE_WINDOW`(InventoryResult)/`TARGET_TO_FAR`/`CANCELLED`/`COMPLETE`… (`TradeHandler.cpp:35-62`) | One trade per player. **Matching key:** prompt kind + trader guid. Statuses arrive in order. |
| Duel `SMSG_DUEL_REQUESTED` 0x167 = `uint64 arbiterGuid, uint64 initiatorGuid` (also sent to the initiator, `Spells/SpellEffects.cpp:4359-4363`) | accept `CMSG_DUEL_ACCEPTED` 0x16C / decline `CMSG_DUEL_CANCELLED` 0x16D | `Handlers/DuelHandler.cpp:25/52`; LOGGEDIN / THREADUNSAFE | `uint64 arbiterGuid` (accept checks it; cancel ignores it) | accept → `SMSG_DUEL_COUNTDOWN` 0x2B7; decline → `SMSG_DUEL_COMPLETE` 0x16A *(inferred: `DuelComplete(DUEL_INTERRUPTED)`)* | **Matching key:** arbiter guid. Note CANCELLED during a running duel = forfeit. |
| Resurrect `SMSG_RESURRECT_REQUEST` 0x15B = `uint64 resurrector, uint32 nameLen, cstring name, uint8 sickness, [uint32 0]` (`Spell.cpp:5391-5414`) | `CMSG_RESURRECT_RESPONSE` 0x15C | `MiscHandler.cpp:668`; LOGGEDIN / THREADSAFE | `uint64 resurrectorGuid, uint8 status` (0 = decline) | accept → `ResurectUsingRequestData` (state: alive). Decline → nothing. Silent if alive, prevent-rez aura, or guid mismatch. | **Matching key:** resurrector guid. Completion state-polled (`IsAlive()`). |
| Summon `SMSG_SUMMON_REQUEST` 0x2AB = `uint64 summoner, uint32 zone, uint32 timeoutMs` (`SpellEffects.cpp:4449-4453`) | `CMSG_SUMMON_RESPONSE` 0x2AC | `MovementHandler.cpp:870`; LOGGEDIN / THREADUNSAFE | `uint64 summonerGuid, uint8 agree` | accept → `SummonIfPossible` → teleport (reflex). **Silent if dead or in combat** (`:872`). | **Matching key:** summoner guid. Completion = teleport completed. |
| Loot roll `SMSG_LOOT_START_ROLL` 0x2A1 = `uint64 itemGuid(loot source), uint32 mapId, uint32 slot, uint32 itemId, …` (`Groups/Group.cpp:950-`) | `CMSG_LOOT_ROLL` 0x2A0 | `GroupHandler.cpp:549`; LOGGEDIN / THREADUNSAFE | `uint64 lootGuid, uint32 slot, uint8 RollVote (0 pass, 1 need, 2 greed, 3 disenchant)` | `SMSG_LOOT_ROLL` 0x2A2 (`source, slot, rollerGuid, …, rollNumber, rollType`), later `SMSG_LOOT_ROLL_WON` 0x29F / `SMSG_LOOT_ALL_PASSED` 0x29E | **Matching keys:** lootGuid + slot (+ roller == self). Several rolls can be open at once. |
| Ready check `MSG_RAID_READY_CHECK` 0x322 = `uint64 initiatorGuid` (`GroupHandler.cpp:785-787`) | `MSG_RAID_READY_CHECK` 0x322 **with body** | `GroupHandler.cpp:760`; LOGGEDIN / THREADUNSAFE | `uint8 state` (1 ready, 0 not ready). An empty body means "start a check". | `MSG_RAID_READY_CHECK_CONFIRM` 0x3AE = `uint64 guid, uint8 state` broadcast | **Matching key:** guid == self. One per group. |

Prompt timeouts (`ACSDK.Prompt.TimeoutSec` → decline) are config-declared mechanical behaviour, §5.2. The group invite has no guid; key it by inviter name plus arrival order.

### 2.9 Release spirit / reclaim corpse

| | `CMSG_REPOP_REQUEST` 0x15A | `CMSG_RECLAIM_CORPSE` 0x1D2 |
|---|---|---|
| H / S / P | `MiscHandler.cpp:59` / LOGGEDIN / THREADSAFE | `MiscHandler.cpp:633` / LOGGEDIN / THREADSAFE |
| Layout | `uint8` (skipped) | `uint64 guid` (read, unused) |
| Preconditions (silent if unmet) | dead and not ghost; no prevent-rez aura | ghost, not arena, corpse exists, reclaim delay elapsed, within `CORPSE_RECLAIM_RADIUS` |
| Effect / packets | `BuildPlayerRepop` → `SMSG_PRE_RESURRECT` 0x494 (`Player.cpp:4517-4519`), ghost aura, `PlayerScript::OnPlayerReleasedGhost`; then `RepopAtGraveyard` → `SMSG_DEATH_RELEASE_LOC` 0x378 and a near/far teleport (reflex) | `ResurrectPlayer` → `SMSG_DEATH_RELEASE_LOC` (clear), state alive; `SpawnCorpseBones` |
| Completion | state-polled: `PLAYER_FLAGS_GHOST` set **and** teleport finished | state-polled: `IsAlive()` |
| Correlation | single-in-flight (`death` channel); no keyed reply | same |

Note: AC itself auto-repops after `m_deathTimer` (6 min, `Player.cpp:4663`). That is game logic, not SDK autonomy. Surface it as a state change.

---

## 3. Protocol reflexes (§5.2) — mechanical acks

**Trigger:** all reflexes key off `ServerScript::OnPacketSent`. That hook fires **inside `SendPacket`**, on whatever thread is sending (usually a map thread, often in the middle of a handler or `TeleportTo`).

**Rule:** reflexes must only record "ack needed" in the per-character inbox. Never re-enter a handler from inside the hook. Deliver the ack later on the thread the table specifies.

### 3.1 Far teleport — `MSG_MOVE_WORLDPORT_ACK` 0x0DC

- **Trigger:** `SMSG_NEW_WORLD` 0x03E = `uint32 mapId, float x,y,z,o` (`Player.cpp:1630-1637`). `SMSG_TRANSFER_PENDING` 0x03F precedes it and needs no reply.
- **H:** `MovementHandler.cpp:45 HandleMoveWorldportAckOpcode` → public `HandleMoveWorldportAck()` (`:51`). **S/P:** **STATUS_TRANSFER** / THREADUNSAFE. The player is out of every map (`TeleportTo` calls `RemovePlayerFromMap`), so only the world thread can process it.
- **Layout:** empty.
- **Guard:** ignored unless `IsBeingTeleportedFar()`.
- **Completion signal:** `PlayerScript::OnPlayerMapChanged` fires *(inferred)*; position and map are read from state.
- **Correlation:** single (one far teleport at a time).
- **Headless note:** with Option B, call `HandleMoveWorldportAck()` from `WorldScript::OnUpdate`. Delayed teleports (`MustDelayTeleport`) send `SMSG_NEW_WORLD` later; the hook still catches them.

### 3.2 Near teleport — `MSG_MOVE_TELEPORT_ACK` 0x0C7 (same opcode both directions)

- **Trigger (S→C):** `MSG_MOVE_TELEPORT_ACK` = `packed guid, uint32 orderCounter, MovementInfo body` (`Player::SendTeleportAckPacket`, `Player.cpp:1391-1399`).
- **Reply (C→S):** `packed guid, uint32 flags (echo the counter), uint32 time` (`HandleMoveTeleportAck`, `MovementHandler.cpp:292-360`). **S/P:** LOGGEDIN / THREADSAFE.
- **Guards:** `m_mover` is a player, `IsBeingTeleportedNear()`, guid == mover. The handler ignores counter and time.
- **Note:** while `IsBeingTeleported()`, every `MSG_MOVE_*` is dropped (`:373-377`), and MotionMaster is reinitialized on ack (`:355`).

### 3.3 Movement acks

All of these are LOGGEDIN / THREADSAFE.

| Server sends | Client ack | Handler | Layout | Does AC need it? |
|---|---|---|---|---|
| `SMSG_FORCE_{WALK,RUN,RUN_BACK,SWIM,SWIM_BACK,TURN_RATE,FLIGHT,FLIGHT_BACK,PITCH_RATE}_SPEED_CHANGE` (0x2DA, 0x0E2, 0x0E4, 0x0E6, 0x2DC, 0x2DE, 0x381, 0x383, 0x45C) = `packed guid, uint32 counter, [uint8 0 for RUN], float speed` (`Unit::SendSpeedToController`, `Unit.cpp:11124-11140`) | `CMSG_FORCE_*_SPEED_CHANGE_ACK` (0x2DB, 0x0E3, 0x0E5, 0x0E7, 0x2DD, 0x2DF, 0x382, 0x384, 0x45D) | `MovementHandler.cpp:678 HandleForceSpeedChangeAck` | `packed guid, uint32 counter, MovementInfo body, float newSpeed` | **Speed is already applied server-side** (`m_speed_rate`, `Unit.cpp:11083`). The ack only (a) broadcasts `MSG_MOVE_SET_*_SPEED` to observers and (b) decrements `m_forced_speed_changes`. Without it, real players watching see the wrong speed *(inferred)*. A `newSpeed` higher than the server's → `KickPlayer` (flag-only for headless, §0.6). Echo the server speed exactly. Ignored while a spline runs or if `counter <= mapChangeOrderCounter`. |
| `SMSG_FORCE_MOVE_ROOT` 0x0E8 / `UNROOT` 0x0EA = `packed guid, uint32 counter` | `CMSG_FORCE_MOVE_ROOT_ACK` 0x0E9 / `UNROOT_ACK` 0x0EB | `MovementHandler.cpp:997 HandleMoveRootAck` | `packed guid, uint32 counter, MovementInfo body` | **Yes.** `MOVEMENTFLAG_ROOT` is applied to `m_movementInfo` only via the ack for client-controlled players (`SendMoveRoot`, `Unit.cpp:14125-14147`). `UNIT_STATE_ROOT` is set regardless. |
| `SMSG_MOVE_WATER_WALK`/`LAND_WALK` (0x0DE/0x0DF), `SMSG_MOVE_FEATHER_FALL`/`NORMAL_FALL` (0x0F2/0x0F3), `SMSG_MOVE_SET_HOVER`/`UNSET_HOVER` (0x0F4/0x0F5), `SMSG_MOVE_SET_CAN_FLY`/`UNSET_CAN_FLY` (0x343/0x344), `SMSG_MOVE_GRAVITY_DISABLE`/`ENABLE` (0x4CE/0x4D0) = `packed guid, uint32 counter` | `CMSG_MOVE_WATER_WALK_ACK` 0x2D0, `CMSG_MOVE_FEATHER_FALL_ACK` 0x2CF, `CMSG_MOVE_HOVER_ACK` 0x0F6, `CMSG_MOVE_SET_CAN_FLY_ACK` 0x345, `CMSG_MOVE_GRAVITY_DISABLE_ACK` 0x4CF / `ENABLE_ACK` 0x4D1 | `MiscHandler.cpp:1496 HandleMoveFlagChangeOpcode` | `packed guid, uint32 counter, MovementInfo body, [uint32 isApplied — not for gravity]` | **Yes.** These flags change server-side only via the ack (`Unit.cpp:16207-16370`). The handler assigns `mover->m_movementInfo.flags = packet flags` **before** its checks (`MiscHandler.cpp:1527`). The reflex must send current flags ± exactly the toggled flag. CAN_FLY ack also clears `PendingFlightChange`, which `HandleMoveWorldportAck` consults (`MovementHandler.cpp:110-114`). |
| `SMSG_MOVE_KNOCK_BACK` 0x0EF = `packed guid, uint32 counter, float vcos, vsin, speedXY, speedZ` | `CMSG_MOVE_KNOCK_BACK_ACK` 0x0F0 | `MovementHandler.cpp:824` | `packed guid, uint32 counter (skipped), MovementInfo body` | Ack relocates the mover to the reported position and broadcasts `MSG_MOVE_KNOCK_BACK`. A headless character cannot simulate the arc (§0.7). **Owner decision:** ack with current position (no movement), or seek a whitelist entry for `MotionMaster::MoveKnockbackFrom`. |

- **All movement acks share these constraints:**
  - They go through `ProcessMovementInfo` → `HandleMoverRelocation`, which **overwrites** `m_movementInfo` and position. Always build the body from the server's current state (`BuildMovementPacket`), with `guid = player guid`.
  - They are dropped while `!movespline->Finalized()`, except root/unroot acks during boarding.
  - Anticheat hooks (`AnticheatCheckMovementInfo`) run if an anticheat module is installed.

### 3.4 Time sync — `CMSG_TIME_SYNC_RESP` 0x391

- **Trigger:** `SMSG_TIME_SYNC_REQ` 0x390 = `uint32 counter`. Sent at 5 s, then every 10 s from the **map** session update (`WorldSession.cpp:576-590`, `SendTimeSync :1522-1533`).
- **H:** `MovementHandler.cpp:926 HandleTimeSyncResp`. **S/P:** LOGGEDIN / THREADSAFE.
- **Layout:** `uint32 counter, uint32 clientTicks`.
- **Important:** the handler uses `recvData.GetReceivedTime()` for RTT. `m_receivedTime` is only set by the `WorldPacket(WorldPacket&&, TimePoint)` constructor (`Server/WorldPacket.h:38`, `:78-82`; `WorldSocket.cpp:459-461`). The injected packet must be built with `GameTime::Now()`.
- **Need:**
  - Not required for liveness. The idle timeout only applies when `m_Socket` exists (`WorldSession.cpp:390`), and there is no keepalive requirement for socketless sessions *(verified)*.
  - Without replies `_pendingTimeSyncRequests` grows by one entry per 10 s, unbounded (`:1528`).
  - `_timeSyncClockDelta` stays 0, so every movement/ack packet logs an INFO line (§0.7).
  - **Recommend:** enable, with `clientTicks = getMSTime()` (delta ≈ 0 + lag).
- `CMSG_KEEP_ALIVE` / `CMSG_PING` are socket-level (`STATUS_NEVER`, handled in `WorldSocket`). Not needed.

---

## 4. Summary table

| Primitive | Opcode (hex) | Proc. place | Success signal | Failure signal(s) | Correlation |
|---|---|---|---|---|---|
| MoveTo / Follow / Stop | — (MotionMaster) | world thread (ours) | state poll | state poll / timeout | single (MotionMaster slot) |
| Face | `MSG_MOVE_SET_FACING` 0x0DA (or `SetFacingTo`) | THREADSAFE | state poll | silently dropped while moving → timeout/precondition | single, `move` channel |
| SetTarget | `CMSG_SET_SELECTION` 0x13D | THREADSAFE | none | none | fire-and-forget |
| Attack | `CMSG_ATTACKSWING` 0x141 | THREADSAFE | `SMSG_ATTACKSTART` 0x143 | `SMSG_ATTACKSTOP` 0x144, `SMSG_ATTACKSWING_*` 0x145/146/148/149, `SMSG_CANCEL_COMBAT` 0x14E, silent | keys (attacker, victim) + single for `ATTACKSWING_*` |
| StopAttack | `CMSG_ATTACKSTOP` 0x142 | THREADSAFE | `SMSG_ATTACKSTOP` | — | fire-and-forget (keyed) |
| CastSpell | `CMSG_CAST_SPELL` 0x12E | THREADSAFE | `SMSG_SPELL_GO` 0x132 / `MSG_CHANNEL_UPDATE(0)` 0x13A | `SMSG_CAST_FAILED` 0x130, `SMSG_SPELL_FAILURE` 0x133, silent | **echoed castCount** (+ caster guid) |
| CancelCast | `CMSG_CANCEL_CAST` 0x12F | THREADSAFE | target cast's failure packets | — | fire-and-forget; resolves target cast |
| CancelChannel | `CMSG_CANCEL_CHANNELLING` 0x13B | INPLACE | `MSG_CHANNEL_UPDATE(0)` + `SMSG_SPELL_FAILURE` | silent | fire-and-forget; resolves target channel |
| Say / Whisper / Party | `CMSG_MESSAGECHAT` 0x095 | THREADUNSAFE | `SMSG_MESSAGECHAT` 0x096 echo | `SMSG_CHAT_PLAYER_NOT_FOUND` 0x2A9, `SMSG_CHAT_WRONG_FACTION` 0x219, `SMSG_NOTIFICATION` 0x1CB, silent | keys (type, text, receiver) + single |
| UseItem | `CMSG_USE_ITEM` 0x0AB | INPLACE | `SMSG_SPELL_GO` | `SMSG_INVENTORY_CHANGE_FAILURE` 0x112, `SMSG_CAST_FAILED` | castCount for spell part; single for inventory errors |
| Loot open | `CMSG_LOOT` 0x15D | THREADUNSAFE | `SMSG_LOOT_RESPONSE` 0x160 | `SMSG_LOOT_RESPONSE`(error), silent | key: loot guid |
| Loot item | `CMSG_AUTOSTORE_LOOT_ITEM` 0x108 | INPLACE | `SMSG_LOOT_REMOVED` 0x162 + `SMSG_ITEM_PUSH_RESULT` 0x166 | `SMSG_INVENTORY_CHANGE_FAILURE`, `SMSG_LOOT_RELEASE_RESPONSE` 0x161 | single |
| Loot money | `CMSG_LOOT_MONEY` 0x15E | THREADUNSAFE | `SMSG_LOOT_MONEY_NOTIFY` 0x163 | `SMSG_LOOT_RESPONSE`(error), silent | single |
| Loot release | `CMSG_LOOT_RELEASE` 0x15F | THREADUNSAFE | `SMSG_LOOT_RELEASE_RESPONSE` | silent | key: loot guid |
| Prompt responses | 0x072/0x073, 0x117/0x11C/0x11A, 0x16C/0x16D, 0x15C, 0x2AC, 0x2A0, 0x322 | THREADUNSAFE (rez: THREADSAFE) | see §2.8 | see §2.8 | keys per prompt kind |
| Release spirit | `CMSG_REPOP_REQUEST` 0x15A | THREADSAFE | state (ghost) + teleport | silent | single |
| Reclaim corpse | `CMSG_RECLAIM_CORPSE` 0x1D2 | THREADSAFE | state (alive) | silent | single |
| Reflex: far teleport | `MSG_MOVE_WORLDPORT_ACK` 0x0DC | THREADUNSAFE, STATUS_TRANSFER | — | — | internal |
| Reflex: near teleport | `MSG_MOVE_TELEPORT_ACK` 0x0C7 | THREADSAFE | — | — | internal |
| Reflex: speed / flag / root acks | see §3.3 | THREADSAFE | — | — | internal |
| Reflex: time sync | `CMSG_TIME_SYNC_RESP` 0x391 | THREADSAFE | — | — | internal |

---

## 5. Proposed per-character command channels

Derived from the correlation constraints. Rule (§4.2): two commands whose replies cannot be told apart must share a channel, so at most one is in flight on it. A new command on a channel supersedes the old one, as requested by the controller.

| Channel | Commands | In-flight limit | Why |
|---|---|---|---|
| **`move`** | MoveTo, Follow, Stop, Face | 1 | One MotionMaster active slot. `MSG_MOVE_SET_FACING` is dropped while a spline runs, so Face must sequence with movement. All completion is state-polled. |
| **`cast`** | CastSpell, CancelCast, CancelChannel, *and the spell phase of UseItem* | >1 allowed by correlation (castCount namespace 1..255) | Echoed castCount disambiguates casts, including queued ones. AC itself allows one generic cast + one queued request + one channel. Superseding a cast should not auto-send `CMSG_CANCEL_CAST`; that would be a gameplay decision. **Owner to confirm** whether "supersede" only drops the old watcher (→ CANCELLED) or also injects a cancel. The castCount counter is per character and shared with UseItem. |
| **`melee`** | Attack, StopAttack | 1 | `SMSG_ATTACKSWING_*` errors have no payload. |
| **`item`** | UseItem (pre-spell phase), Loot open / item / money / release | 1 | `SMSG_INVENTORY_CHANGE_FAILURE` carries no request echo and is shared by item use and loot store. Loot steps mix INPLACE and THREADUNSAFE processing, so they must be serialized (§0.2). Later Equip/Swap/Destroy (out of scope) join this channel. |
| **`chat`** | Say, Whisper, PartySay | 1 | `SMSG_CHAT_WRONG_FACTION` and `SMSG_NOTIFICATION` have no key. Echo matching handles success. |
| **`prompt`** | RespondToPrompt | 1 per prompt kind (group, trade, duel, resurrect, summon, ready-check); N for loot rolls (keyed by lootGuid + slot) | AC holds at most one pending prompt of each kind per player. Replies are keyed by the prompt's guid. |
| **`death`** | ReleaseSpirit, ReclaimCorpse | 1 | No keyed replies; completion is state-polled; mutually exclusive by game state. |
| *(none — instant)* | SetTarget | n/a | Fire-and-forget; no reply exists. |
| *(internal, not a channel)* | Reflexes (worldport/teleport/speed/flag/root/knockback/time-sync acks) | n/a | Mechanical; not controller commands. Must not race a `move` command. Both run on world/map thread inboxes in FIFO order. |

**Cross-channel ordering:** commands on different channels can still interact through shared server state. Examples: SetTarget then CastSpell with no explicit target; Loot open interrupting a cast. The runtime should preserve per-character FIFO order of injection across channels, which Option B dispatch can guarantee (§0.2). Commands needing a specific target state should carry explicit targets (`SpellCastTargets` with the unit guid) instead of relying on SetTarget ordering.
