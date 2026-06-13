# Megaserver scaling plan — core architecture to 10–12k concurrent

North-star: the **core** must not be the bottleneck. Given enough cores/RAM it should scale to Nostalrius/vmangos-class numbers (10–12k). This plan covers the four remaining architectural items, grounded in this fork's actual code.

## Current state (what's already parallel — don't redo)

- **Across-map parallelism**: `MapUpdater` runs one worker thread per Map; `MapManager::Update` enqueues all maps, barriers on `wait()`, then runs `DelayedUpdate` serially. `MapUpdate.Threads=5` today. One **continent = one thread** — the wall.
- **Movement/cast packets already run on map threads**: TC splits opcodes into `PROCESS_THREADSAFE` (~60: movement, cast, acks — handled inside `Map::Update` via `MapSessionFilter`, `Map.cpp` session loop) vs `PROCESS_THREADUNSAFE` (~271: chat, guild, group, trade, social, login — main thread, `World::UpdateSessions`). So per-player movement handling is *already* off the main thread.
- **Network I/O already parallel**: Boost.Asio threads (`Network.Threads`) fill per-session `_recvQueue` (LockedQueue); only *dispatch* is the question.
- **Player concurrency model is PHASE-SEPARATION, not locks**: the same Player is touched by the map thread (`Player::Update` + THREADSAFE packets) and the main thread (THREADUNSAFE packets), but in **different phases of `World::Update`** (UpdateSessions runs, completes; then map dispatch+barrier). There is no lock protecting Player — correctness depends on these never overlapping in time. **Any new parallelism must preserve this invariant or add real locking.**
- **Already built (per-player cost reducers — megaserver plumbing, complete)**: MovementBroadcaster (relay off map thread, mutex-safe listener model), interest throttling, adaptive per-observer player-visibility cap, async auth, MySQL tuning.

## The real dependency order (NOT 1→2→3→4)

```
Phase A  FOUNDATION (item 4 essentials)        ── gates B, C, D
  A1  fix unprotected data races               (must-fix even at 5 threads)
  A2  shard HashMapHolder<Player>              (every FindPlayer)
  A3  tswow lua-mutex strategy                 (gates D; server-wide serializer)
Phase B  PARALLEL VISIBILITY (item 1)          ── highest value / effort; needs only A1
Phase C  PARALLEL SESSIONS  (item 2)           ── needs A2 + chat/social sharding
Phase D  PARALLEL COMBAT    (item 3)           ── needs A3 + deep combat thread-safety
```

---

## Phase A — Foundation (item 4): global contention & correctness

Ranked by the audit. The first two are also **latent bugs today** (unprotected maps already raced by the 5 map threads).

### A1. Unprotected global maps — DATA RACES, fix first (low effort, high urgency)
- **`PlayerNameMapHolder`** (`ObjectAccessor.cpp` ~84-108): name→Player* with **no lock**. Read on whisper/invite/friend from main thread; written on login/logout. Already racy.
- **`sCharacterCache`** (`Cache/CharacterCache.cpp`): guid/name caches, **no lock**; `UpdateCharacterLevel/GuildId/ArenaTeamId` write while others read.
- **Fix**: give each a sharded lock (N buckets by guid/name hash, `std::shared_mutex` per bucket — read-mostly). Mechanical, isolated, testable. Do this regardless of the rest.

### A2. `HashMapHolder<Player>` — the player registry (medium effort, high value)
- `ObjectAccessor.cpp` ~40-60: single global `std::shared_mutex` over guid→Player*. Read on **every** `FindPlayer`/`FindConnectedPlayer` (tens of thousands/sec at scale), write on login/logout.
- **Fix**: shard into K buckets (`guid.GetCounter() % K`), each its own `std::shared_mutex`. `Find` locks one bucket; iteration (`SaveAllPlayers`, who-list) locks all (rare). Keeps the API identical. Unblocks running handlers on worker threads (Phase C).

### A3. The tswow global lua mutex — the megaserver serializer (high effort, gates Phase D)
- `tswow-core/Public/TSLua.h`: one `recursive_mutex` (`TSWOW_LUA_GUARD`) around the single global `sol::state`. **Every** livescript hook — `FIRE_ID(Creature,OnUpdate/OnCast/...)`, map OnUpdate, spell/aura hooks — takes it. At 10k players this is ~100k acquisitions/sec, and crucially it **serializes scripted creature AI ticks across all map threads**, so Phase D (parallel combat) gains little while it stands.
- **Options** (decision required):
  1. **Deferred lua execution** — map/worker threads enqueue lua events into per-thread queues; a single lua thread (or the main thread at a barrier) drains them serially. Keeps one lua state; decouples worker threads from the lock; adds latency + ordering constraints (a hook that must mutate game state synchronously can't defer). Best fit for the read-heavy hooks; the synchronous ones need care.
  2. **Per-thread lua states** — N independent `sol::state`s, one per worker. True parallelism, but: module-level shared lua state (globals, caches) must be replicated or made thread-local; memory ×N; cross-state references forbidden. Biggest change.
  3. **Minimise hot-path hooks** — keep the lock but ensure per-tick creature hooks are rare/cheap (audit which scripts subscribe to OnUpdate). Cheap mitigation, not a real fix; buys time.
- **Recommendation**: start with (3) as a stopgap (already partly done — we moved azeroth_quests off OnUpdate), design toward (1) deferred execution as the real answer. (2) only if profiling shows lua is the wall after (1).

### A4. Lower-priority contention (do as needed)
- `MapManager::_mapsLock` `DoForAllMaps` (~5×/tick, blocks all maps) → RCU/snapshot copy of the map list.
- Social/guild/group/LFG global locks → shard per-subsystem; mostly relevant once Phase C moves their handlers to workers.

---

## Phase B — Item 1: player-sharded parallel visibility (highest value / tractable)

**Target**: `Map::ProcessRelocationNotifies` (`Map.cpp` ~985) → `DelayedUnitRelocation` → `PlayerRelocationNotifier` → `Player::UpdateVisibilityOf`. This is the O(n²) crowd cost (each player checks/serves visibility of ~N others, builds UpdateData packets).

**Why it's shardable**: a player's visibility writes are its own (`m_clientGUIDs`, the `UpdateData` packet, `i_visibleNow`); broadcaster listener registration is already mutex-safe (we built it that way); reads of other objects (positions/auras/phase via `CanSeeOrDetect`) are read-only.

**The three collisions to resolve (this is the actual work):**
1. **Symmetric visibility update** — `PlayerRelocationNotifier::Visit(PlayerMapType)` does `i_player.UpdateVisibilityOf(player)` **and** `player->UpdateVisibilityOf(&i_player)` (writes the *other* player's `m_clientGUIDs`). That cross-player write breaks sharding. **Fix**: remove the symmetric optimization — each player computes only its own visibility in its own shard-pass. Cost: each pair is visited from both sides instead of once (more cell visits), but writes become disjoint. Net win at scale.
2. **AI relocation (`CreatureUnitRelocationWorker` → `MoveInLineOfSight`)** — the same pass also fires creature AI noticing players, which mutates creature AI/threat (cross-object). **Fix**: split the phase — sharded threads do *pure player visibility* (disjoint); collect AI-relocation work and run it in a separate serial pass, or shard it by **creature** (each creature's AI touched by one thread). First cut: keep AI relocation serial (cheaper than packet-building), parallelize only the player-visibility packet construction.
3. **`marked_cells` bitset** — written in the earlier entity-update phase, only **read** during ProcessRelocationNotifies → read-only within this phase, safe. `ResetNotifier` (clears `NOTIFY_VISIBILITY_CHANGED`) → run as a serial pass after the sharded part, or make the flag atomic.

**Shape**: inside `ProcessRelocationNotifies`, partition the map's players across a worker pool (reuse `MapUpdater`'s threads or a sub-pool), each thread runs the player-visibility notifier for its slice. Barrier, then the serial AI-relocation + ResetNotifier pass. Gated by a config (`Visibility.ParallelThreads`) and a player-count threshold so small maps stay single-threaded.

**Composes with**: the broadcaster (listener writes are locked), the per-observer cap (each player's radius is local), throttling — all already thread-safe.

**Risk**: medium-high (hot path; visibility bugs = invisible/ghost players). Heavy testing needed. **Effort**: ~1–2 weeks.

---

## Phase C — Item 2: parallel session/packet processing

**The real bottleneck** (not what I first assumed): movement/cast are already on map threads. What's left on the **main thread** is the ~271 `PROCESS_THREADUNSAFE` handlers — chat, guild, group, trade, mail, auction, social — plus iterating 10k sessions/tick, plus logout/save.

**Design:**
- **C1. Async-ify logout/save** — `LogoutPlayer`/`SaveToDB` on the main thread block the loop per logout. Push fully to `CharacterDatabase` async transactions; drain results off-thread. (Partly async already.)
- **C2. Reclassify + make-safe the high-frequency THREADUNSAFE handlers** — chat is the big one (`SendMessageToSet` is already distance-based and could go through a worker; whisper needs A1's safe name lookup). Move chat/social handlers to per-session workers once their global state (A1, A4) is sharded.
- **C3. Shard session dispatch** — partition `m_sessions` across a worker pool for packet dispatch; each worker runs its sessions' THREADSAFE + now-safe handlers. **Genuinely-global rare ops** (guild create/disband, arena team, channel admin) stay on a serialized queue — they're infrequent, so serializing them is fine.
- **Invariant to preserve**: the phase-separation between session handling and map update (no Player touched by two threads at once). Sharded session dispatch must still complete before (or be fenced from) the map-update phase, OR the handlers must only touch player-local + properly-locked global state.

**Depends on**: A2 (player lookup), A1 (name/cache), A4 (social/guild sharding). **Effort**: ~2–3 weeks, much of it the A-dependencies. **Risk**: medium.

---

## Phase D — Item 3: parallel object/combat updates (the deep one)

**Target**: the entity-update phase (`Map.cpp` ~826-888) — `VisitNearbyCellsOf` → `ObjectUpdater` → `Creature::Update` → AI/`_UpdateSpells`/`CombatManager`/`ThreatManager`/`MotionMaster`. Cross-object mutation everywhere (damage, threat, auras, chase reading target positions).

**vmangos model**: **safe-distance cell scheduling** — partition cells into independent sets farther apart than the max interaction range; update each set on a thread; multiple passes cover the map. Distant objects provably can't interact → no locks for the common case. Plus deep thread-safety for the cases that exceed safe distance (long-range spells, pets/summons, area auras).

**The hard sub-problems in THIS fork:**
- **`ThreatManager`/`CombatManager`** — mutated cross-creature, no locks. Needs per-unit locking or the safe-distance guarantee.
- **Aura application / `_UpdateSpells`** — cross-unit `AddAura`/periodic ticks.
- **`Spell::DoAllEffectOnTarget`** — effects land on targets possibly in another thread's region.
- **`MotionMaster` chase** — reads a target's position while another thread moves it.
- **GATED BY A3 (lua mutex)** — every scripted creature's AI tick fires a livescript hook under the global lua lock. Until A3 is solved, parallel object update **serializes on lua** and gains little. This is why A3 must precede D.

**Design**: safe-distance scheduler over cells; per-region threads update objects; cross-region interactions either fall outside safe distance (rare → lock the few objects) or are queued (spell-effect events applied at a barrier). **The dense-blob caveat stands**: a single packed zone is all within safe distance → serializes onto few threads (that's why the visibility cap/throttle/broadcaster matter — they make the one thread carrying the blob survive).

**Depends on**: A3. **Effort**: the big one — weeks-to-months, highest risk (combat correctness, races). Do last, after profiling confirms it's the remaining wall.

---

## Sequencing & effort summary

| Phase | Item | Effort | Risk | Depends on | Value |
|---|---|---|---|---|---|
| A1 | fix data races (name map, char cache) | days | low | — | correctness now + foundation |
| A2 | shard player HashMapHolder | ~1 wk | low-med | — | unblocks C; every FindPlayer |
| A3 | lua-mutex strategy (stopgap→deferred) | wks | high | — | unblocks D; server-wide |
| B  | parallel visibility (item 1) | 1-2 wk | med-high | A1 | **biggest tractable unlock** |
| C  | parallel sessions (item 2) | 2-3 wk | med | A1,A2,A4 | main-thread relief |
| D  | parallel combat (item 3) | mo. | high | A3 | full-continent scaling |

## Recommended start
1. **A1** (fix the unprotected races) — small, fixes latent bugs that already exist at 5 threads, foundation.
2. **B** (parallel visibility) in parallel with A2 — the highest value-per-effort and the thing that actually gets the heaviest crowd cost off the single continent thread.
3. Re-profile, then A3 → D as the final, biggest lift.

Everything stays behind config flags + kill switches, built and tested incrementally as we've been doing, with the same backup/test discipline.
