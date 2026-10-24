# Event sources — single source of truth (PLAN.md §4.7)

Every event type in `event.proto` (`EventService.Subscribe`, PLAN §7) has **exactly one** official source:

- an AzerothCore script hook, **or**
- captured SMSG packet(s) via `PacketSink`, **or**
- the SDK runtime itself (SDK-internal),

and never more than one of these. This table is mirrored as a comment in `event.proto`. Changing a row is a §13 decision-log entry.

- **AC source:** azerothcore-wotlk master @ `0540ae8013bc92d93f1f0d3346ccbc1a1a4d6f85`. Paths are relative to `src/server/game/`.
- **Evidence markers:** *(verified)* = read in source. *(inferred)* = reasoned, not traced; confirm in Phase 4.
- **"One source" means one *kind* of source.** An SMSG source may be a small family of opcodes that together describe one event. Each opcode in a family maps to exactly one event type.

## How each source kind works

### SMSG source

- **Hook:** `ServerScript::OnPacketSent(WorldSession*, WorldPacket const&)` (`Scripting/ScriptDefines/ServerScript.h:85`, hook id `SERVERHOOK_ON_PACKET_SENT`).
- **When it runs:** synchronously inside `WorldSession::SendPacket`, before the socket check (`Server/WorldSession.cpp:310`). So it fires for socketless sessions.
- **Filtering:** PacketSink checks "is this a headless session?" and then "does any subscription / watcher / reflex need this opcode?" before reading anything (§4.5).
- **Thread:** the thread of whoever sent the packet. Mostly map worker threads (in-world actions, combat, spells). The world thread for THREADUNSAFE handlers, world-level broadcasts and the far-teleport path *(inferred from §0.2 of the opcode map)*. Possibly others (DB async callbacks run inside session updates) *(inferred)*.

### Hook source

- **Signature / thread:** given per row.
- **Registration:** AC hooks are opt-in per script object via the `enabledHooks` vector, e.g. `PLAYERHOOK_ON_LEVEL_CHANGED`, `UNITHOOK_*`. They must be registered at module load.
- **UnitScript hooks fire for every unit on the server** (creatures included). The first statement must be a cheap "is this guid indexed?" lookup (§4.5, zero cost when unused).

### Thread rule for the EventBus (consequence)

Both packet and hook sources fire on **map worker threads as well as the world thread**. EventBus ingress must therefore be a lock-free MPSC enqueue of an already-built, self-contained event (copied bytes or POD fields).

Never do the following inside a hook:

- dereference game objects later, outside the hook
- block on a lock
- re-enter a handler

---

## Table

| Event (proto oneof) | Official source | Exact hook / opcode(s) | Thread (when known) | Rationale (§4.7) |
|---|---|---|---|---|
| **Chat** | SMSG | `SMSG_MESSAGECHAT` 0x096, `SMSG_GM_MESSAGECHAT` 0x3B3 (`Chat/Chat.cpp:273-348`). Layout: `uint8 type, int32 lang, uint64 sender, uint32, [names/channel by type], uint64 receiver, uint32 len, cstring text, uint8 tag, [uint32 achievement]`. | sender's thread. Say: map thread. Whisper/party/guild: world thread (CMSG_MESSAGECHAT is THREADUNSAFE). | "What the player was told". It includes text exactly as delivered (language, filters, GM tag) and covers NPC/monster/system/whisper-inform. Hooks (`OnPlayerBeforeSendChatMessage`, `OnPlayerCanUseChat`) fire on the *sender* side and before validation, so they would report messages AC later drops. The character's own Say/Whisper echoes also arrive here (`Player::Say` → `SendDirectMessage`, `Entities/Player/Player.cpp:9594-9597`; `WHISPER_INFORM` `:9711-9716`), so a `self` flag is included. |
| **DamageDealt** | SMSG | `SMSG_ATTACKERSTATEUPDATE` 0x14A (melee) and `SMSG_SPELLNONMELEEDAMAGELOG` 0x250 (spells), filtered to `attacker == self` (`Entities/Unit/Unit.cpp` `SendAttackStateUpdate` / `SendSpellNonMeleeDamageLog`). | sender's thread | **Changed 2026-10-22 (owner decision, PLAN §13):** the combat-log packets carry spell id, school, crit, absorb/resist/block, which `UnitScript::OnDamage` lacks. Periodic (`SMSG_PERIODICAURALOG`) and environmental logs are not decoded yet. |
| **DamageTaken** | SMSG | Same packets, filtered to `victim == self`. Self-damage yields one DamageDealt and one DamageTaken (two event types, not a duplicate). | as above | Same source as DamageDealt. |
| **Death** | Hook | `UnitScript::OnUnitDeath(Unit* unit, Unit* killer)` (`UnitScript.h:111`). Called at the end of `Unit::Kill` (`Unit.cpp:13938`). Emit for the headless character's stream when `unit` (it died) **or** `killer` (it killed something) is a headless character. | map thread (`Unit::Kill` from damage) *(inferred)* | Carries the killer. Covers the character's own death and kills it scores, which the `Attack` "target dead" completion uses. `PlayerScript::OnPlayerJustDied(Player*)` (`PlayerScript.h:248`, called from `Player::KillPlayer`, `Player.cpp:4672`) has no killer and does not cover creature deaths, so it is not used. **Known gap:** paths that call `Player::KillPlayer()` directly without `Unit::Kill` do not fire OnUnitDeath. These are the GM void-fall fallback (`Handlers/MovementHandler.cpp:520`), Spirit-of-Redemption on logout (`Server/WorldSession.cpp:715`) and the repop race (`Handlers/MiscHandler.cpp:80`). The `dead` sub-flag of **CharacterStateChanged** still reports them. |
| **AuraApplied** | Hook | `UnitScript::OnAuraApply(Unit* unit, Aura* aura)` (`UnitScript.h:84`). Two call sites: `Unit::_ApplyAura` (new application on a target, `Unit.cpp:4588`) and `Unit::_TryStackingOrRefreshingExistingAura` (stack/refresh on the owner, `Unit.cpp:4447`). Include an `is_refresh` field so the two are distinguishable. | map thread *(inferred)* | `SMSG_AURA_UPDATE` 0x496 / `SMSG_AURA_UPDATE_ALL` 0x495 are slot-based. Decoding removals would need per-unit slot→spell state, which is state reconstruction from packets, forbidden by §4.7. They are also sent for every visible unit. The hook gives `Aura*` (spell id, caster, duration, stacks) directly. **Note:** passive/talent auras apply at login, so expect a burst then. |
| **AuraRemoved** | Hook | `UnitScript::OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode)` (`UnitScript.h:86`). Called at the end of `Unit::_UnapplyAura` (`Unit.cpp:4681`). | map thread *(inferred)* | Same reason as AuraApplied. `AuraRemoveMode` (expire / cancel / dispel / death / interrupt…) comes from AC directly. |
| **SpellCastStart** | SMSG | `SMSG_SPELL_START` 0x131 (`Spells/Spell.cpp:4916-4928`) and, for channels, `MSG_CHANNEL_START` 0x139 (`:5374-5379`). Filter caster guid (second packed guid) == self. | caster's map thread | These are the **same packets the CastSpell watcher uses**, and `SMSG_SPELL_START` carries the echoed `castCount`, so events correlate with commands (`command_id` field). `UnitScript`/`AllSpellScript::OnSpellPrepare` (`Spell.cpp:3805`) fires even for spells that are then not sent to the client (`IsNeedSendToClient`). The packet is "what the player was told". |
| **SpellCastFinish** | SMSG | `SMSG_SPELL_GO` 0x132 (success), `SMSG_CAST_FAILED` 0x130 (fail, `SpellCastResult`), `SMSG_SPELL_FAILURE` 0x133 (interrupt), `MSG_CHANNEL_UPDATE` 0x13A with `time == 0` (channel end). Layouts in `docs/research/opcode-map.md` §2.3. **Ignore** `SMSG_SPELL_FAILED_OTHER` 0x2A6 (a duplicate of SPELL_FAILURE sent alongside it, `Spell.cpp:5336-5341`). | caster's map thread (CAST_FAILED from a THREADUNSAFE context may be world thread) *(inferred)* | Carries the echoed castCount and AC's own reason enum. Hooks `AllSpellScript::OnSpellCast` / `PlayerScript::OnPlayerSpellCast` (`Spell.cpp:4209`, `:3945`) see only successes and give no failure reason. |
| **LevelUp** | Hook | `PlayerScript::OnPlayerLevelChanged(Player* player, uint8 oldlevel)` (`PlayerScript.h:285`, hook id `PLAYERHOOK_ON_LEVEL_CHANGED`). Called at the end of `Player::GiveLevel` (`Player.cpp:2583`), after the new level is applied. | thread of the XP grant (map thread for kills/quests; world thread for GM commands) *(inferred)* | Gives old and new level in one place. `SMSG_LEVELUP_INFO` 0x1D4 exists (stat deltas) but needs decoding for no extra value. Current name verified: `OnPlayerLevelChanged`. |
| **Loot** | SMSG | `SMSG_LOOT_RESPONSE` 0x160 (window opened, or error `LOOT_NONE + LootError`), `SMSG_LOOT_REMOVED` 0x162, `SMSG_LOOT_MONEY_NOTIFY` 0x163, `SMSG_LOOT_CLEAR_MONEY` 0x165, `SMSG_LOOT_RELEASE_RESPONSE` 0x161, `SMSG_ITEM_PUSH_RESULT` 0x166 (filter `playerGuid == self`; it may be group-broadcast, `Entities/Player/PlayerStorage.cpp:4875-4878`). | THREADUNSAFE loot handlers → world thread; autostore (INPLACE) → map thread *(inferred)* | "Loot window" is explicitly a packet event per §4.7. Same packets as the Loot command watcher. Hooks (`OnPlayerLootItem`, `OnPlayerBeforeSendLoot`, `OnLootMoney`) cover only parts of the flow and not errors. |
| **Prompt** | SMSG | `SMSG_GROUP_INVITE` 0x06F; `SMSG_TRADE_STATUS` 0x120 (`TRADE_STATUS_BEGIN_TRADE` opens, later statuses close); `SMSG_DUEL_REQUESTED` 0x167 (only when initiator ≠ self; it is also sent to the initiator, `Spells/SpellEffects.cpp:4362-4363`), closed by `SMSG_DUEL_COMPLETE` 0x16A; `SMSG_RESURRECT_REQUEST` 0x15B; `SMSG_SUMMON_REQUEST` 0x2AB; `SMSG_LOOT_START_ROLL` 0x2A1 (closed by `SMSG_LOOT_ROLL_WON` 0x29F / `SMSG_LOOT_ALL_PASSED` 0x29E); `MSG_RAID_READY_CHECK` 0x322 (closed by `MSG_RAID_READY_CHECK_FINISHED` 0x3C6). | mixed: world thread (group/trade/duel handlers are THREADUNSAFE), map thread (spell-driven rez/summon/duel request) *(inferred)* | "Prompts" are what the player was told. Hooks exist only for some kinds (`PlayerScript::OnPlayerDuelRequest`, `GroupScript::OnInviteMember`) and none for rez/summon/ready-check/roll. One uniform packet source avoids mixing. Prompt ids are assigned by the runtime. The response opcodes are in `opcode-map.md` §2.8. |
| **LeaseLost** | **SDK-internal** | Lease manager (§4.4): force takeover, TTL expiry, explicit release by a newer holder. | world thread (dispatcher) | No AC concept. |
| **CharacterStateChanged** | **SDK-internal** | Runtime lifecycle state machine (§4.3): `OFFLINE → LOGGING_IN → ONLINE → LOGGING_OUT → OFFLINE`, plus ONLINE sub-flags `dead`, `ghost`, `in_combat`, `teleporting`. Produced in one place: the world-thread per-tick diff of each registered character's state, read directly from `Player` (whitelisted read, §4.6.3). | world thread | It summarises transitions the runtime already tracks. Login/logout complete, GM kick (`IsKicked()`, opcode-map §0.6), teleport start/finish from reflex bookkeeping and death/ghost flags. Polling AC state once per tick makes it robust to paths that bypass hooks (e.g. `KillPlayer` without `Unit::Kill`). AC hooks such as `OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerReleasedGhost`, `OnPlayerResurrect`, `OnPlayerEnterCombat` / `OnPlayerLeaveCombat` and `OnPlayerMapChanged` may be used **internally** to trigger an immediate diff. They are never exposed as their own events, so no duplicates arise. |
| **CommandResult** | **SDK-internal** | Command dispatcher / completion watchers (§4.2). The *reasons* inside it come from AC packets per `opcode-map.md`, but the event is emitted by the watcher state machine. | world thread | One event per terminal command state. Mirrors the unary RPC result. |
| **RawPacket** (opt-in) | SMSG | Any opcode on the subscriber's allowlist, via `ServerScript::OnPacketSent`, gated by `ACSDK.RawPackets.Enable` (§4.6.4). | sender's thread | Escape hatch. It is a distinct event type, so a packet also used for Chat/Loot/etc. appearing as RawPacket for an explicit raw subscriber is not a duplicate of the typed event. Opcodes PacketSink discards by default (`SMSG_UPDATE_OBJECT` 0x0A9, `SMSG_COMPRESSED_UPDATE_OBJECT` 0x1F6, `SMSG_MONSTER_MOVE` 0x0DD…) are only decoded/copied for raw subscribers. |
| **Lagged** | **SDK-internal** | EventBus per-subscriber bounded queue overflow, drop-oldest, `Lagged{dropped=N}` (§4.1 #4). | gRPC / world thread (queue owner) | No AC concept. |

---

## Notes and open items

1. **Hook names verified against current headers.** All of these exist with the signatures shown:
   - `Scripting/ScriptDefines/PlayerScript.h`: `OnPlayerLevelChanged`, `OnPlayerJustDied`, `OnPlayerReleasedGhost`, `OnPlayerResurrect`, `OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerMapChanged`, `OnPlayerEnterCombat`, `OnPlayerLeaveCombat`, `OnPlayerDuelRequest`, `OnPlayerLootItem`
   - `UnitScript.h`: `OnDamage`, `OnUnitDeath`, `OnAuraApply`, `OnAuraRemove`
   - `ServerScript.h`: `OnPacketSent`
   - `GroupScript.h`: `OnInviteMember`
   - `AllSpellScript.h`: `OnSpellPrepare`, `OnSpellCast`, `OnSpellCastCancel`

   The PlayerScript hooks all use the `OnPlayer…` prefix in this revision.
2. **Damage source:** decided by the owner on 2026-10-22: combat-log SMSGs (see the table).
3. **Death row scope.** Death is emitted for the character dying **and** for kills it makes, distinguished by a `role` field. If the owner prefers "Death = own death only", the hook stays the same and only the filter changes.
4. **Dedup across characters.** When two headless characters are near each other, both PacketSinks see broadcast packets (`SMSG_SPELL_GO`, chat in range). Each copy belongs to a different character's stream ("what *this* character was told"), so it is not a duplicate under §4.7. Hook events are attributed by guid role (attacker/victim/unit/killer), so each occurrence appears at most once per character stream.
5. **Never decode** `SMSG_UPDATE_OBJECT` / `SMSG_COMPRESSED_UPDATE_OBJECT` / `SMSG_MONSTER_MOVE` / aura-update packets for typed events (§4.7).
