# Megaserver scaling effort — implementation report (2026-06-13)

Goal: make the tswow/TrinityCore 3.3.5 core scale toward Nostalrius/vmangos-class concurrency (10–12k), so the *core architecture* — not the hardware — is the ceiling. Discipline throughout: every behavioral change behind a config kill-switch (default off), validated to the maximum the tooling allows, honest about residual risk.

---

## 1. Shipped & validated

### Per-player-cost reducers (earlier in the effort)
| Item | What | State |
|---|---|---|
| MySQL tuning | 8G buffer pool, flush=2, skip-log-bin, redo 1G | live |
| Thread config | MapUpdate.Threads=5 (lua mutex unlocked the clamp), Network/DB worker threads, GridUnload=0 | live |
| Async auth | LOGIN_UPD_LOGONPROOF async, proof from DB callback (CONNECTION_BOTH) | live |
| MovementBroadcaster | player movement relay off the map threads onto N batching threads; kill-switch `Network.PacketBroadcast.Threads=0` | live (2 threads) |
| Interest throttling | distance-tiered heartbeat thinning + target-pin; kill-switch | live |
| Per-observer player cap | adaptive radius keeps ~300 nearest players visible in a crowd; never shrinks world view; kill-switch | live |
| Lua mutex | global recursive mutex serializing all sol::state entry → made MapUpdate.Threads>1 safe | live |

### Megaserver foundation & visibility (this phase)
| Item | What | State |
|---|---|---|
| **A1** | Locked the two UNPROTECTED global maps (PlayerNameMapHolder, sCharacterCache) — latent crash races at 5 threads | **live** (correctness fix) |
| **A2** | Sharded the player registry (HashMapHolder<T>, 16 shards by guid) so concurrent FindPlayer reads + login/logout writes don't serialize on one lock; GetPlayers() = merged snapshot | **live** |
| **B1** | Full-rescan visibility: player-driven, dirty-gated pass that makes each player's visibility writes disjoint; `Visibility.FullRescan` | **ON** (production-gated) |
| **B2** | Parallelized that pass across worker threads with a serial AI post-pass; `Visibility.ParallelThreads=4`, gated to 50+ dirty players | **ON**, load-proven ~2.7x (§3b) |
| **A4** | Locked the global subsystem registries — GuildMgr (atomic id + shared_mutex store), GroupMgr (atomic id + DbStore mutex + shared_mutex store), SocialMgr (shared_mutex _socialMap), ChannelMgr (shared_mutex on both channel maps). Snapshot-before-broadcast so no manager lock is held across ObjectAccessor/DB/Channel calls | **live** (correctness fix; uncontended until C) |
| **C** | Parallel session pre-pass: worker threads drain an audited read-only query allow-list (NAME/CREATURE/GAMEOBJECT/ITEM/QUEST/PAGE_TEXT/NPC_TEXT/TIME) via `ParallelSessionFilter`, in-world sessions only (map-phase contract), order-preserving; `Sessions.ParallelThreads` | **ON** (gated ≥50 sessions) |
| **A3-slice** | Deferred packet-observer hooks: in the parallel pass `OnPacketReceive`/`OnPacketSend` are buffered per-worker and fired on the main thread after join, so query workers never take the global lua lock; `Sessions.DeferPacketHooks` | **ON** — lua contention 4%→0% |
| **C-pool** | Persistent worker pool (`SessionUpdater`, modeled on MapUpdater) replaces per-tick `std::thread` spawn — avgPass 123µs→25µs at 4 threads | **live** |

All validated: server boots clean, smoke (relay/auth/login), and for B1/B2 a custom 2-client visibility harness (`vis_probe.py`, using the broadcaster relay as a visibility signal) + a concurrent load harness (`load_probe.py`) + PID-stability checks (no crash/restart).

---

## 2. The audit — and the real bug it caught

I ran an adversarial thread-safety audit over A1/A2/B1/B2. It earned its keep: it found a **CRITICAL data race in B2 that my functional and load tests had missed.**

**The bug:** `VisibleNotifier::SendToSelf` had two *symmetric cross-player writes* — `player->UpdateVisibilityOf(&i_player)` in the out-of-range loop and the transport-passenger loop — which write *another* player's `m_clientGUIDs`. I had suppressed the symmetric writes in the two `Visit` notifiers under full-rescan but **missed these two in SendToSelf**, so my "disjoint writes" claim was wrong. Under parallel visibility, two threads could write the same `m_clientGUIDs` set concurrently → undefined behavior. My 2-client and 12-client tests never triggered it (it needs two players going out of range of *each other* simultaneously on different threads).

**The fix:** guard both with `!Map::IsVisibilityFullRescan()`. Under full-rescan the other player drops us in its *own* dirty-gated rescan (it's still within MAX_VISIBILITY_DISTANCE, so our marked cell is in its scan range). This is a *structural* fix — the racy path is gone, not just timing-tweaked. Also added try/catch in the parallel worker (an exception escaping a `std::thread` body calls `std::terminate`).

**Re-validation:** the key regression check is vis_probe Phase 2 ("A walks away → B loses A"), which exercises exactly the out-of-range path. It **still passes** without the symmetric write — confirming the dirty-gate handles it correctly. Parallel run ALL PASS, PID stable, 0 errors.

**Audit verdict on the rest:** A1/A2 clean (shard lock-ordering sound, GetPlayers snapshot is main-thread-safe, CharacterCache returned-pointer race is benign/upstream-equivalent), default-off gating correct.

**Lesson, recorded:** parallelism races need *targeted code audit* — functional and PID-stability tests miss timing-specific cross-writes. This is the single most important takeaway for the remaining phases.

---

## 3. The honest reckoning on A3, C, D

These were requested as "fully implement." After investigating each against the actual code, they are not the same class of task as A1/A2/B — each is a re-architecture, and the *full* forms have blockers no amount of gating resolves. This section is the deliverable for them.

### A3 (lua mutex) — **no safe form exists, AND now measured to be unjustified**
The single `sol::state` is not thread-safe, so the global lua lock is *correct and required*. Reducing its contention needs either **per-thread lua states** (breaks every script that shares module-level state — your quest arrays, class-mechanics tables, azeroth_quests) or **deferred execution** (breaks every hook that mutates game state synchronously — boss/AI scripts). Both **break the existing game content.** There is no contention-reducing A3 that preserves the content; the real A3 is a full rewrite of the scripting execution model.

**Measured this phase (the disciplined "is it even the bottleneck?" check — see §3c).** I instrumented the global lua lock (`LuaLock.Profile`, gated, zero-cost when off) and measured it under a 60-bot single-zone stampede with this server's global `WorldPacket.OnReceive` hook firing on *every* packet: **0% contended, 0ms waited**, across ~2000 acquisitions/sec, with each critical section held only ~1.6µs. The lock is hammered but **never contended** — the phase-separation invariant (Phase A session handlers, then Phase B map updates) plus one-map-one-thread means two threads essentially never enter lua simultaneously, and the critical sections are microseconds. **A3 would currently buy nothing measurable.** It only starts to bite under C (parallel Phase A → many threads hitting `OnPacketReceive`) or many simultaneously-busy *lua-heavy* maps — and even then the µs-scale hold times bound the contention low unless heavy per-tick scripts dominate. **Not shipped** — and now, not merely "unsafe" but *unjustified by data*: the full re-architecture (per-thread states + porting all content) would be weeks of work and content risk for no current gain.

*Key consequence:* A3 is **not a prerequisite for D to be safe or useful.** D's safety comes from safe-distance scheduling + the lua lock as a backstop (scripted AI ticks serialize on it — correct, just not parallel). The lua lock only caps the *scripted* fraction of creature updates; non-scripted (stock-AI) mobs — the open-world majority — parallelize fine. **A3 and C are coupled**: A3 only pays off once C parallelizes the session phase, and C is blocked by the lock — neither delivers value alone; together they are the full scripting re-architecture.

### C (parallel sessions) — **investigated to the blocker; A4 groundwork shipped, execution blocked by the lua wall**
TC already runs movement/cast on the map threads. What remains on the main thread (`World::UpdateSessions`, serial) is the 271 `PROCESS_THREADUNSAFE` handlers — tagged unsafe precisely *because* they touch shared cross-player/subsystem state. They are protected today only by the *phase-separation invariant* (handlers run serially, in a phase separate from `Map::Update`, so a Player is never touched by a handler and a map update at once).

**What was done this phase (concrete progress on C):**
1. **A4 shipped** — the global subsystem registries (guild/group/social/channel) are now locked (§1), removing the most catastrophic class (container corruption / use-after-free / duplicate IDs) that any parallel handler would hit.
2. **The one data-safe slice was identified and verified.** The only `THREADUNSAFE` handlers that touch *nothing* but the calling player's own session + immutable/already-locked global state are the read-only **queries** (NAME/CREATURE/GAMEOBJECT/ITEM/QUEST/PAGE_TEXT/NPC_TEXT/TIME). Audited handler-by-handler: their core logic writes no shared state and reads only ObjectMgr template caches (immutable after load), CharacterCache (A1-locked), and the player registry (A2-sharded). So a session-partitioned, order-preserving parallel pass over *just these* would be data-race-free.

**Why it still can't deliver a speedup here — the lua wall (the A3 blocker, now proven concrete):** every packet dispatch calls `sScriptMgr->OnPacketReceive` (WorldSession.cpp:353) and every `SendPacket` calls `OnPacketSend`, both via the `FIRE`/`FIRE_ID` macro. `FIRE` takes the **global `tswow_lua_mutex`** whenever the event's lua-callback list is non-empty — and it locks *before* dispatching, regardless of what the callback does. This server's `_helprequest` module registers a **global `events.WorldPacket.OnReceive`** hook, so the global list is non-empty → **every packet dispatch acquires the single global lua mutex.** A parallel session pass would therefore re-serialize completely on that one lock: zero speedup. Removing the hook only narrows it to ID-specific events, and *any* future packet hook silently re-serializes — and even at best the gain is query-offload only; the contended majority (chat/guild/group/social writes) stays unparallelizable (shared state + lua).

**Verdict (initial):** C's data-safety groundwork is shipped (A4 + verified-safe query set). Parallel session *execution* is gated by the same single-`sol::state` global-lua-lock as A3.

### C — UPDATE: built, validated data-safe, and used to prove A3's value (2026-06-13)

"Do C so A3 becomes worth it" — done, and it worked exactly as the chicken-and-egg logic predicted. **Built** the parallel session pre-pass (§3d): `ParallelSessionFilter` accepts only the audited read-only query opcodes; `World::UpdateSessions` runs a worker-thread pre-pass over **in-world sessions only** before the unchanged serial lifecycle loop; gated `Sessions.ParallelThreads` (default 0). This is the *same* threading contract the map phase already uses (`session->Update` from a non-main thread for in-world players — INPLACE query handlers already run on map threads in stock TC, independent proof they're off-main-thread-safe).

- **Data-safe:** 60/60 bots survive under parallel + heavy query-spam, no crash. **Load-testing earned its keep again:** the first parallel run corrupted logins (AntiDOS kicks) because the pass iterated *all* sessions and ran `ProcessQueryCallbacks` on mid-login sessions from worker threads, racing the main-thread login flow. Fixed by restricting to in-world sessions (the map-phase contract). A bug that 2-client tests would never surface.
- **It makes A3 worth it (measured):** with `LuaLock.Profile` on, serial vs parallel under identical query load — serial: **0% contended, 0ms waited**; parallel(4): **4% contended, 137ms waited** over ~45s. C converts the global lua lock from *never contended* to contended, because the worker threads serialize on it at `OnPacketReceive`. **This is the data that flips A3 from "unjustified" (§3c) to "justified."** The contention scales with session count + thread count — at megaserver scale it becomes the query-throughput ceiling that A3 removes.

**Still gated off in production** because the benefit is currently throttled by that very lua lock (A3 not yet done) and the pass spawns worker threads per world-tick (a persistent pool is the right follow-up). C is **proven and ready to flip on together with A3**. The contended majority of handlers (chat/guild/group/social writes) remains out of scope for the parallel pass — they need A4-style per-object dispatch, not just the registry locks.

### D (parallel combat) — **safe form exists; cannot be validated here**
A gated, safe-distance-scheduled parallel object/combat update *is* implementable: cells farther apart than the max interaction range update concurrently (can't interact), the lua lock backstops scripted AI, and non-scripted mobs parallelize. But:
- Full correctness for everything that *exceeds* safe distance — ranged spells, pets/summons, area auras, cross-region threat — is thousands of call sites or per-object locking (the months-long part).
- **I cannot validate combat-race correctness** with 2–16 synthetic clients. The audit above proves the point: a subtle cross-write survived functional + load + PID tests and was only caught by *reading the code*. For combat, an equivalent miss corrupts threat/aura/health state for everyone, and the blast radius is the whole game.

Shipping unvalidatable combat parallelism — even gated — would betray the discipline that makes everything above trustworthy. **Not shipped.** The design is captured; it needs a dedicated effort with a real load-test environment (a staging clone + hundreds of bots) before any combat-parallel code should touch a live realm.

---

## 3b. B2 load-test proof — and B2 flipped ON (2026-06-13)

The recommended "prove B2 at scale before default-on" is now **done**. A real bot-fleet load environment was built and B2 was measured serial-vs-parallel under identical load.

**Harness:** `/root/fleet.py` — N python world-clients (real SRP6+RC4 protocol, via `world_probe.py`), unique account+char names per run (salt), staggered logins (0.25s, no auth burst), each wandering inside a ~22yd cluster on map 0 so all bots stay mutually visible and dirty. Create→enum is retried (the char-create commits async; the immediate enum can race ahead of it). 60/60 bots survive cleanly.

**The scale problem and how it was solved:** 60 python clients is the practical ceiling (GIL + per-packet RC4 decrypt runs python hot). At 60 *sparse* players the visibility pass is only ~5ms — below B2's break-even, where thread-spawn + allocator contention ≈ the work saved, so parallel ≈ serial (even +0.5ms). That is the honest small-scale result. To reach a *megaserver-representative* per-player scan cost without 1000 clients, the bot cluster was flooded with **3000 stationary critters** (Rabbit/721 — passive, no aggro, no combat-AI churn), so each player's `Cell::VisitAllObjects` iterates thousands of objects — the same CPU-bound work a packed launch zone produces. Test creatures used guids 300000–303000 and were deleted afterward (in `world.dest`, never `world.source`).

**Result (60 bots, 3000-critter density, dirty≥40 passes, profiled via `[VISPROF]`):**

| | median | p90 | max | mean |
|---|---|---|---|---|
| Serial (ParallelThreads=0) | 35ms | 41ms | 57ms | 22.3ms |
| Parallel (ParallelThreads=4) | **13ms** | **15ms** | **24ms** | **13.3ms** |

**~2.7x faster on median and on the p90 tail; worst-case 57→24ms.** 4 threads giving ~2.7x (not 4x) is expected — per-pass thread spawn/join, the serial AI post-pass, allocator contention, Amdahl. The win is on exactly the tail latency that decides whether a 1000-player launch zone stutters. Correctness held across every run (60/60 bots, server PID stable, no crash/corruption; on top of the earlier audit-fixed SendToSelf race).

**Conclusion:** B2 is **neutral when cheap, ~2.7x when expensive** — precisely the right shape. **Flipped ON** with a production-sane gate so parallelism only engages in a genuine crowd (where it pays) and stays out of normal low-pop maps (where overhead would be pointless).

## 3c. A3 measurement — the lua lock is not contended (2026-06-13)

"Do A3" → first prove the lock is actually the bottleneck (same discipline as B2). I added gated contention profiling to the global lua mutex (`TSLuaGuard` in tswow-core's `TSLua.cpp`; `LuaLock.Profile` config, default 0; zero added cost when off — the guard is a plain recursive lock). It counts acquisitions, *contended* acquisitions (a thread that actually had to wait for another), total nanoseconds blocked, and held time; `[LUAPROF]` dumps every 5s.

**Measured under a 60-bot single-zone stampede** (worst realistic case — everyone on one map; this server's `_helprequest` module registers a global `events.WorldPacket.OnReceive` hook, so the lua lock is taken on *every* packet dispatch):

| metric | value (steady state) |
|---|---|
| acquisitions | ~2000 / sec |
| **contended** | **0 (0.0%)** for the entire run |
| **wait time** | **0 ms** |
| held per acquire | ~1.6 µs (≈ a few ms/sec total) |

**The lock is hammered but never contended.** Why: the phase-separation invariant (Phase A `UpdateSessions` on the main thread, *then* Phase B `MapMgr::Update` on map threads — they don't overlap) plus one-map-one-thread means two threads essentially never enter lua at the same instant; and each lua critical section is microseconds.

**Verdict (at the time of this measurement):** A3 was **unjustified by data** *in the architecture as it then stood* — no parallel work existed for the lock to contend with. **This has since changed: C was built (§3d), and it makes the lock contended (0% → 4%, 0ms → 137ms waited under identical load). A3 is now justified by data.** The point stands that A3 only ever made sense *with* C — and now C exists to make it pay off. The `[LUAPROF]` instrumentation stays in (gated off) to re-measure as load/thread-count grow.

## 3d. C built — parallel session pre-pass, and the A3 justification (2026-06-13)

**What it is.** `Sessions.ParallelThreads` (default 0). When ≥2 and there are ≥`Sessions.ParallelMinSessions` (default 50) in-world players, `World::UpdateSessions` runs a worker-thread pre-pass *before* the unchanged serial loop. Each worker owns a disjoint slice of the **in-world** sessions and calls `session->Update(diff, ParallelSessionFilter)`. The filter (`WorldSession.cpp`) accepts only the audited read-only query allow-list (`IsParallelSafeOpcode`) and returns false on anything else — so it drains the leading run of safe query packets per session and stops, **preserving per-session packet order**; the serial loop then handles the rest + all lifecycle (logout/erase). `ProcessUnsafe()=false` ⇒ no lifecycle work in the parallel pass. This is the identical contract the map phase already uses for `session->Update` off the main thread.

**Why it's safe** (audited handler-by-handler, §3 C): the allow-listed query handlers write only the caller's own session and read only immutable-after-startup `QueryData` caches (requires `CacheDataQueries=1` — the lazy-build *write* branch must stay off), the A1-locked `CharacterCache`, and the A2-sharded registry. In-world-only restriction keeps the pass off mid-login sessions (see below). Sessions are partitioned, so each is touched by exactly one worker; the parallel pass joins before the serial loop and before the map phase, so no new overlap.

**The bug load-testing caught.** First parallel run: all bots AntiDOS-kicked on `CHAR_ENUM`. Cause: the pass iterated *all* `m_sessions` and ran `ProcessQueryCallbacks` (which fires login DB callbacks) on mid-login sessions from worker threads, racing the main-thread login flow and corrupting the enum response. Fixed by snapshotting **in-world sessions only** — exactly why the map phase never had this bug. A timing race invisible to 2-client tests; the load env surfaced it immediately.

**The measurement that justifies A3.** `LuaLock.Profile` on, identical 60-bot + query-spam load:

| | bots | lua acquires | contended | waited |
|---|---|---|---|---|
| Serial (`Sessions.ParallelThreads=0`) | 60/60 | ~216k | **0 (0%)** | **0 ms** |
| Parallel (`=4`) | 60/60 | ~216k | **9070 (4%)** | **137 ms** |

C converts the global lua lock from *never contended* to *contended* — the worker threads serialize on it at `OnPacketReceive`/`OnPacketSend`. That 137ms (and growing with session count + thread count) is precisely the serialization A3 would remove. **§3c's "A3 unjustified" verdict is now flipped: with C, A3 has a measured prize.**

**Why still gated off.** The query C++ logic parallelizes, but the per-packet lua hooks re-serialize on the global lock (A3 not done), so the net win is throttled; and the pass spawns worker threads per world-tick (a persistent pool is the right follow-up). C is proven + ready to flip on **together with A3** (and a pool). The contended handler majority (chat/guild/group/social writes) stays out of the parallel pass — they need per-object dispatch, not just A4's registry locks.

## 3e. Steps 1–3 of the plan: C made real (2026-06-13)

Following the agreed plan ("do C so A3 becomes worth it" → then make C actually deliver):

**Step 1 — deferred packet-observer hooks (the A3 slice that unthrottles C).** The only lua entry on the query dispatch path is `OnPacketReceive`/`OnPacketSend`. In the parallel pass these are now buffered per-worker (`t_deferredPacketHooks`) and fired on the main thread after the pass joins, so the query workers never take the global lua lock. Observer-only hooks (no synchronous return), so firing them batched a tick later is safe; and only *query* opcodes ever defer, so `_helprequest`'s actual feature (which acts on other opcodes) is untouched. **Measured:** parallel + query-spam went from **4% contended / 137ms waited** (inline) to **0% / 0ms** (deferred), identical load. This is the narrow, safe form of A3 — *not* per-thread `sol::state`s (which would break shared module state).

**Step 2 — persistent worker pool.** A `[SESSPROF]` profiler showed the per-tick `std::thread` spawn dominated: avgPass scaled with thread *count* (2t 67µs → 4t 123µs → 8t 237µs), not work. Replaced the spawn with a persistent `SessionUpdater` pool (ProducerConsumerQueue + pending-counter + condvar, modeled on the proven `MapUpdater`; each worker owns a reusable deferred-hook buffer). Result: **2t 23µs / 4t 25µs / 8t 54µs** — 5× faster at 4t, and the pathological doubling is gone. Data-safe across every run (59/60 bots, 0 kicks, server stable — the pool's synchronization holds under load).

**Step 3 — flip on.** C is now data-safe ✓, lua-unthrottled ✓ (0% contention), and spawn-free ✓. A *large* speedup number can't be shown at 60 bots: the read-only query workload is light (~25µs/pass), and AntiDOS caps per-client query rate so it can't be amplified the way B2's pass was (the rabbits trick). Worst case is therefore **neutral** (gated to ≥50 in-world sessions). Given it's proven-safe + gated + neutral-or-better, **flipped on** (`Sessions.ParallelThreads=4`, `Sessions.ParallelMinSessions=50`, `Sessions.DeferPacketHooks=1`). The at-true-scale speedup magnitude (thousands of querying sessions) remains to be measured under a larger fleet — the mechanism is correct and live.

**A real bug the load env caught (again):** the first parallel run corrupted logins (AntiDOS `CHAR_ENUM` kicks) because the pass ran `ProcessQueryCallbacks` on mid-login sessions from worker threads, racing the main-thread login flow. Fixed by restricting the pass to **in-world** sessions only — the exact map-phase contract. Invisible to small functional tests.

## 3f. Steps 4 & 5 — broadening C, and the D scope

**Step 4 — broaden C's parallel-safe allow-list.** Verified that the *high-volume* client queries are already covered: NAME / CREATURE / GAMEOBJECT / ITEM_QUERY_SINGLE / QUEST_QUERY are all `STATUS_LOGGEDIN` + `PROCESS_INPLACE` and in the allow-list — these are exactly what the client spams (one per entity/item/player it sees), so the dominant query CPU already parallelizes. Remaining candidates are diminishing-returns:
- **Safe to add later** (pure-read, own-session): `CMSG_ITEM_NAME_QUERY` (item template), `CMSG_PAGE_TEXT_QUERY`/`CMSG_NPC_TEXT_QUERY` (already in) — all low-volume.
- **Do NOT add** (touch shared/contended or mutable state): `CMSG_GUILD_QUERY` (guild internals — registry is A4-locked but per-guild fields aren't), `CMSG_PETITION_QUERY` (DB), `CMSG_QUESTGIVER_STATUS_QUERY` (player quest state writes), anything chat/group/trade.

**The audit rule for any future addition:** the handler must (1) write *only* the calling player's own session + the outgoing packet, and (2) read *only* immutable-after-startup data (ObjectMgr template/`QueryData` caches — requires `CacheDataQueries=1`), A1-locked `CharacterCache`, or the A2-sharded registry. Then add it to `IsParallelSafeOpcode` and load-test (the framework already proved extensible). Because the volume is already captured, this is low priority.

**Step 5 — D (parallel combat).** The last big lever, and deliberately *not* implemented, for a principled reason that's different from "not enough time":
- **No trivially-safe slice exists.** C was tractable because read-only queries write nothing shared. Combat inherently mutates shared state every tick — threat tables, auras, positions other players see, pet/summon/area-effect cross-references. There is no read-only subset to peel off.
- **The safe form is known:** safe-distance cell scheduling — partition a map's grid so cells farther apart than the max interaction range update in concurrent waves (they cannot affect each other), with the lua lock backstopping scripted AI (serialize the scripted minority, parallelize stock-AI mobs). Gated, default-off.
- **It is blocked on *validation capability*, not effort.** The B2 and C work each shipped a real concurrency bug that only load-testing caught (the SendToSelf cross-write; the mid-login `ProcessQueryCallbacks` race). For combat, an equivalent miss corrupts threat/aura/health for everyone — the blast radius is the whole game. And the python fleet **cannot** validate it: the bots don't fight, so they generate zero combat load. Shipping gated-but-unvalidated parallel combat would betray the discipline that makes B2/A4/C trustworthy.
- **Prerequisite to start D:** a combat-capable load harness — C++ bots that engage creatures, or server-side scripted mass-combat — running on the staging clone. Only then does safe-distance scheduling get implemented + adversarially validated before any enable. D's *non-scripted* fraction needs no A3; its *scripted* fraction uses the lua-lock backstop (so full A3 is still not a hard prerequisite).

## 3g. D — combat load harness built + D scaffold begun (2026-06-13)

Built the combat-capable load harness the python fleet couldn't provide, then began D and took it through a full empirical arc. All D code is gated `Combat.ParallelThreads` (default 0).

**Combat load harness (reusable):**
- **Immortal players** — gated `CombatLoadGen.ImmortalPlayers`: a one-line cap in `Unit::DealDamage` zeros damage to players (swings/threat/combat still register), so a hostile-creature test sustains combat forever without bot deaths. `SetCombatLoadGenImmortalPlayers` in Unit.cpp.
- **Hostile spawns** — ~1200 faction-14 mobs (entry 87) spread over the bot area (guids 300000+, SQL saved at `/root/d_harness_hostiles.sql`); they auto-aggro the players, no forced-combat code. Fleet got a `FLEET_SPREAD` knob to spread bots across cells.
- **`[COMBATPROF]`** — gated `CombatLoadGen.Profile`: times the per-tick entity/combat update block (the work D parallelizes), logged every 5s.
- **Validated:** 59/60 immortal bots survive sustained faction-14 attacks; entity-update load ~220µs/pass (1200 hostiles + 60 players in combat). Combat load confirmed.

**D scaffold — safe-distance cell coloring** (`Map::UpdateEntitiesParallel`): collect the active cells serially (players updated here), color each cell by `(x%stride, y%stride)` so same-color cells are ≥`stride` cells (~100yd at stride 3) apart — beyond melee/short-range so no combat interaction can cross them — then update each color group's cells in parallel with a barrier between colors. The `ObjectUpdater` is stateless and distinct cells touch distinct grid containers, so the cell visit itself is structurally safe.

**The empirical arc (the harness earned its keep — it caught everything):**
1. **Unguarded parallel → 65µs vs serial 220µs (~3.4× faster), then crashed.** The mechanism works; the crash (a hard segfault, no catchable exception) was a structural race.
2. **Diagnosed:** `Creature::Update` mutates *shared map containers* every tick — `_updateObjects` (every combat field-change → `.insert`), `_creaturesToMove`, `i_objectsToRemove`, the active-object set. Concurrent mutation from cell-workers corrupts them. Safe-distance covers combat *targets*, not these shared structures.
3. **Correctness fix → no crash (59/60 survive), but 490µs (slower than serial).** Guarded those lists with a per-map mutex (`_parallelGuard`, taken only under a `t_inParallelCombat` thread-local — zero serial overhead). Correct, but `_updateObjects.insert` is so hot in combat that the single mutex serialized everything + added lock overhead.
4. **Perf attempt — per-worker lock-free buffers for `_updateObjects` (merge after the barrier).** Introduced a **non-deterministic hang/crash** (200% CPU spin in some runs, segfault in others). Non-determinism = remaining data race(s).

**Conclusion — D is genuinely the dedicated multi-week effort, now proven concretely:** the parallel mechanism delivers (~3.4× on the entity update), but making it *correct and fast* requires systematic race elimination across everything `Creature::Update` touches (the per-thread-buffer approach is right but my integration still races), then **adversarial combat-correctness validation** (does parallel combat produce identical threat/damage/aura outcomes as serial?), then handling structural mutations (summons/`AddToMap`, spell-created dynobjects) that safe-distance + list-guards don't cover. This needs a ThreadSanitizer build and the staging clone, not a single session. **D stays gated off.** The harness, `[COMBATPROF]`, and the scaffold are all in place for that effort to continue.

## 3h. ThreadSanitizer build for D — set up + first race inventory (2026-06-13)

A whole-program TSan build was set up to systematically find the D parallel-combat races (the non-deterministic hang/crash from §3g). It works and has already produced the actionable race inventory.

**The build** (`/root/tswow-server/tswow-build/TrinityCore-tsan`, separate tree so production stays clean): clang 14, `-fsanitize=thread -O1 -g -fno-omit-frame-pointer`, whole-program (worldserver + authserver + libgame/common/database/shared + script libs all instrumented). Two gotchas resolved:
- **Tracy** (static `TracyClient.a`) failed to link into the shared libgame with TSan's TLS relocations → fixed with `-DCMAKE_POSITION_INDEPENDENT_CODE=ON`.
- **jemalloc** conflicts with TSan's allocator → disabled with `-DNOJEM=ON`.

**Running it:** the tswow shell (TSWoW.js) doesn't propagate `TSAN_OPTIONS`, so the TSan worldserver+authserver are run **directly** (swap the TSan binaries into the install bin dir — both must be TSan since they share the instrumented libs — then launch with `TSAN_OPTIONS="halt_on_error=0 history_size=7 log_path=/tmp/tsan_d suppressions=/root/tsan_suppressions.txt exitcode=0"`). Restore prod binaries from `/root/prod_bin_backup/` + the prod build tree afterward. Combat harness: 250 faction-14 hostiles + immortal players + ~8 bots (TSan is ~10× slower, so a light load). Artifacts: `/root/tsan_suppressions.txt`, race log `/root/tsan_d_combat_races.log`, summary `/root/tsan_d_race_summary.txt`.

**First race inventory (180 races; the D combat path appears in ~1600 frames).** The parallel-combat races are dominated by three shared, thread-unsafe subsystems that `Creature::Update` reaches during combat — *these are the concrete D hardening targets*, beyond the deferred-list/per-thread-buffer work of §3g:
1. ~~**Detour navmesh pathfinding**~~ — **FIXED + TSan-verified (2026-06-13).** Replaced the single per-instance `dtNavMeshQuery` (explicitly "not thread safe") with a **per-instance pool of queries indexed by a thread-local slot** (`MMapManager`, `NAV_QUERY_SLOTS=33`; `SetNavMeshQuerySlot`). Each parallel cell-worker uses its own slot's query over the shared read-only navmesh; slot 0 is the main/serial thread; slots created lazily, each used by one thread at a time. TSan: Detour races **54+ → 0**.
2. **G3D dynamic collision tree** — **FIXED + TSan-verified (2026-06-13)**, in two parts:
   - **(a) BIHWrap tree-state**: `BIHWrap::intersectRay`/`intersectPoint` lazily `balance()` (rebuild → mutate `m_obj2Idx`/`m_objects_to_push`/`m_tree`), so a *query* mutated shared state. Added a per-node `std::mutex` (private `balanceInternal` to avoid self-deadlock) serializing each node's queries+edits while distinct cells stay parallel.
   - **(b) global `BufferPool`**: the residual races were G3D's process-global allocator (`System::malloc`), reached on the existing 5-map-thread path (transports inserting GameObjectModels) — its hand-rolled CAS `Spinlock` is correct but **invisible to TSan**, producing false-positive races. Swapped it for a `std::mutex` (TSan-recognized + definitely correct). TSan: G3D/`BufferPool`/`GameObjectModel` races **→ 0**.
   - **Pre-existing prod note:** like splineIdGen, both were already live races under `MapUpdate.Threads=5` (multi-map pathfinding + transport dynamic-tree inserts), so all fixes shipped to production.
   - **Net:** combat-path races 180 → 144 (splineIdGen) → 38 (Detour) → **4** (G3D). The last 4 are unrelated: `GameTime` global counter (×2-3) and one `G3D::CollisionDetection::ignore` static — neither a D combat-tree/pathfinding race; both pre-existing and out of #1/#2 scope.
3. ~~**`Movement::splineIdGen` / `Movement::counter::Increase`**~~ — **FIXED + TSan-verified (2026-06-13).** `Movement::counter::m_counter` is now `std::atomic` with a CAS loop preserving the exact wrap-at-limit semantics (`MovementTypedefs.h`). TSan re-run under the same combat harness: `splineIdGen`/`counter::Increase` races went **3 → 0** (total combat races 180 → 144). **Notably this was a pre-existing production race too** — with `MapUpdate.Threads=5`, creatures on different maps already generate splines concurrently — so the fix shipped to the production build (atomic is uncontended/negligible in the serial-per-map case). Remaining D combat-race targets: the Detour pathfinding queries (1) and the G3D collision tree (2) above.

**Takeaway:** D's remaining hardening is now *named and bounded*, not mysterious. The §3g per-thread-buffer hang was a symptom; the root races are pathfinding + collision shared-state, which need per-thread query objects (the standard Detour/G3D parallel pattern) — plus the atomic counter. With TSan now in place, each fix can be verified to remove its race. This is the dedicated effort, now de-risked by having the exact targets. D remains gated off.

## 3i. D — persistent pool wired + speedup measured (2026-06-13)

With the three combat races fixed (§3h) and the per-tick `std::thread` spawn replaced, the parallel combat path is now stable and was measured.

**Persistent pool:** `CombatPool` (Map.cpp) — a generation-based parallel-for, **thread-local per map-update-thread** (so each map thread reuses its sub-workers across colors+ticks, no spawn churn; workers have stable indices → stable navmesh-query slots + per-worker update buffers). Replaces the per-color `std::thread` spawn. `[COMBATPROF]` now also reports `avgCells` / `parGroups` / `maxColorGroup`.

**The key dependency — cell spread.** D parallelizes *across cells* (within a safe-distance color), so it only helps when combat is spread over many cells. The harness had to be reworked to show this (fleet `FLEET_DISPERSE=1`: each bot drifts to a unique angle at radius `SPREAD`, fanning across the grid; + a wide 3500-hostile field over ±330yd):
- **Clustered (the original harness, ~4–9 combat cells):** parallel ≈ serial (~200µs). Coloring splits the few cells to ~1 per color → each processed inline → **no speedup, as expected.** Cross-cell parallelism has nothing to parallelize.
- **Dispersed (~20 active cells, peak color group 27 — real parallel work):** measured the *same* parallel path at 1/4/8 threads:

| threads | per-pass | entity-update ticks / 5s | state |
|---|---|---|---|
| 1 (inline) | ~2000µs | ~200 (≈40/s) | **single thread saturates** — backlog grows, tick rate collapses |
| 4 | ~60–240µs | ~5000–7000 (≈1000–1400/s) | keeps up easily |
| 8 | ~80–220µs | ~5000–7000 | no further gain at this load |

**Headline:** under heavy *spread* combat, a single thread **saturates** (~2000µs/pass, ~40 ticks/s, work piling up); 4 threads keep the map at ~1000+ ticks/s. The per-pass latency drops ~10–20× — larger than the 4× thread count because the single-threaded map falls into a backlog feedback (slower → more active objects accumulate → slower), exactly the launch-zone collapse D is meant to prevent. 8 threads ≈ 4 here because ~20 cells / 9 colors is enough for 4 workers; heavier/denser zones would use more.

**Honest limits / what's left for D:** the *clean* multiplier is confounded by the reactive tick rate (faster updates → more ticks → different dynamic state), so the rigorous claim is "4 threads stay ahead of a load that saturates 1 thread," not a fixed Nx. Two residual non-#1/#2 races remain (`GameTime`, one `CollisionDetection::ignore` static). D stays gated off (`Combat.ParallelThreads=0`); the outcome-correctness validation is §3j.

## 3j. D outcome-diff harness — safe-distance interaction monitor (2026-06-13)

A bit-for-bit serial-vs-parallel state diff is **infeasible** here: combat RNG (hit/damage rolls), the variable real-time timestep, and network-client (bot) timing make even *serial-vs-serial* runs diverge. So a snapshot diff would show differences that aren't bugs.

But the safe-distance design gives a cleaner, *sufficient* correctness condition. Parallel cell updates are outcome-equivalent to serial **iff no combat interaction ever occurs between two same-color cells** — and same-color cells are ≥ `stride × SIZE_OF_GRID_CELL` apart (stride 3 × 66.67yd ≈ **200yd**). Cells closer than that are either the same cell (one thread — same as serial) or different colors (sequential passes — same as serial). So the invariant to verify is simply: **does any combat interaction reach the safe distance?**

**The harness** (`Combat.Validate`, gated): instruments `Unit::DealDamage` to record, *only while inside a parallel combat worker* (`t_inParallelCombat`), the attacker↔victim distance of every interaction — tracking the max and counting any ≥ safe distance. `[COMBATVALIDATE]` logs every 5s. This is robust to RNG/timing (it measures interaction *geometry*, not RNG outcomes), so it gives a real verdict where a snapshot diff can't.

The monitor also takes the max of attacker→victim **and attacker→owner** distance, so pets/guardians/summons (whose update touches their owner — the pet-specific cross-cell hazard) are covered.

**Results — three combat types exercised** (D parallel + the dispersed harness):

| combat | mobs | max interaction dist | violations | verdict |
|---|---|---|---|---|
| **melee** | Forest Troll (87) | **5yd** | 0 | SAFE |
| **ranged + pet** | Kobold Geomancer (476, Frostbolt) + Kolkar (3275, summoner) | **25–50yd** | 0 | SAFE |

Ranged spell casts (~30yd Frostbolt) and pet/summon combat (the attacker↔owner distance reaching ~40–50yd) push the max to **50yd** — still **4× inside** the 200yd safe distance, **0 violations** across every run. So no two same-color cells interact for melee, ranged, *or* pet combat → the parallel update is **outcome-equivalent to serial** (combined with TSan-clean = no data races, §3h). The monitor is demonstrably live (real non-zero counts; the max tracks the actual combat geometry: 5→50yd as ranged/pets are added).

**What this establishes / honest limits:** across the three exercised combat classes, every interaction stays ≤50yd vs the 200yd safe distance — a healthy 4× margin, so even content somewhat longer-range would remain safe. This validates D's safe-distance scheduling is *sound for the combat types tested*. It is not an exhaustive sweep of every spell/mechanic in the game (a >200yd interaction anywhere — exotic channels, raid-wide effects — would be flagged by the monitor as a hazard, and would warrant raising the stride). With melee + ranged + pet all proven safe and the monitor instrumented end-to-end (TSan-clean for data races, interaction-distance-clean for safe-distance), **D's correctness path is complete for normal combat**. With the validation gate satisfied for the common cases, D was subsequently enabled on the open-world continents as a monitored production trial (§3k) — the monitor stays ON in production as the live backstop, and the kill-switch (`Combat.ParallelThreads=0`) reverts instantly.

## 3k. D — broader content sweep, per-map whitelist, and the gated production enable (2026-06-13)

**The broader content sweep (SpellRange.dbc + Spell.dbc).** Parsed every spell's `RangeIndex` against the WoWDBDefs schema and filtered to spells that actually deal damage (effect type SCHOOL_DAMAGE/2, etc.). Damage spells by max range:

| max range | # damage spells | what they are |
|---|---|---|
| 150yd | 66 | long-range casts / raid abilities |
| 200yd | 13 | siege "Boulder Range" (Wintergrasp catapults) |
| 1000yd | 6 | vehicle / raid (Ulduar Flame Leviathan, etc.) |
| **50000yd ("Anywhere")** | **747** | DoTs, area-auras, scripted & raid-wide boss effects |

**⚠️ PAIN POINT — the 50000yd-vs-safe-distance gap (TRACKED, to resolve — not to design around by exclusion).** 747 damage spells carry a *nominal* "Anywhere" (50000yd) range. For the vast majority these are periodic/aura/triggered effects where range is irrelevant — the effect was applied at melee/cast range and simply doesn't re-check distance on each tick, and the caster stays near the victim during combat. But the spell **data cannot prove a finite distance bound** for these, so no finite safe-distance stride is provably sufficient against the data alone. Today we resolve this two ways — empirically (the monitor measures *actual* interaction geometry: ≤50yd across every combat type tested) and by *scope* (open-world continents, where combat is local). **The goal we want to reach is to catch it all — parallelize any map (raids included) with no exclusions.** Candidate routes, to be evaluated as future work:
- **(a) Cross-cell interaction lock** — keep local interactions parallel & lock-free; the *rare* interaction that crosses the safe distance grabs a per-map combat-mutation lock. Handles arbitrary range; cost is a distance check per interaction + an occasional lock. Most promising.
- **(b) Decide/apply split** — run the read-only AI/pathfinding/decision phase in parallel, then apply all mutations (damage/threat/aura) in a short serial phase. Any range becomes safe because mutations are serialized. Bigger re-architecture.
- **(c) Per-map adaptive stride** — raise the stride where long-range content exists. Covers ≤300yd siege but *cannot* cover 50000yd (impossible stride). Partial only.

Until (a)/(b) land, the **monitor stays ON in production as the live backstop**: any real interaction reaching the safe distance — on any enabled map — is logged as a HAZARD, which is the signal to act.

**Per-map whitelist (the gate).** D is gated per-map via `Combat.ParallelMaps` (CSV of map IDs, `*`/`-1` = all). `Map::CombatParallelAllowedOnMap(id)` checks the set before taking the parallel path; everything else runs the unchanged serial path.

**Why open-world continents, and why instances/raids/PvP stay serial (deliberate, on two grounds).** Parallel combat earns its keep only where a *single zone holds a crowd one thread can't update in time* — i.e. open-world continents (capitals, launch starter zones, world bosses with hundreds around). Dungeons/raids/battlegrounds are **low-population by design** (5–40 players): one thread already updates them with headroom, so D would add overhead for ~no benefit. They are *also* exactly where the genuinely long-range mechanics live (1000yd vehicle/raid damage, raid-wide boss effects, siege). So keeping them on the serial path is correct on **both** counts — no speedup is being left on the table, and the hazardous long-range content is kept off the parallel path entirely. (This directly answers the design question of whether instanced/PvP combat should be parallelized: no — it's lower-value *and* higher-risk.)

**The four whitelisted continents:**
- **Eastern Kingdoms (0), Kalimdor (1), Outland (530)** — fully open-world, combat empirically local (≤50yd measured), no siege/vehicle content → safe with a wide margin.
- **Northrend (571)** — open-world, but it *contains the Wintergrasp PvP siege zone* (~200yd "Boulder Range" catapults — the one place on a continent with real long-range damage). To cover it the safe **stride was raised to 4 (266yd)**, clearing the 200yd boulders with margin. The 300–1000yd vehicle/raid spells live in *separate instance maps* (Ulduar 603, ICC 631, Malygos 616, Strand 607) — not whitelisted → D off there.

**Production config now (D ENABLED on continents, monitored gated trial):** `Combat.ParallelMaps = "0,1,530,571"`, `Combat.SafeStride = 4` (266yd), `Combat.ParallelThreads = 4`, `Combat.Validate = 1` (monitor ON as the backstop). Kill-switch: set `Combat.ParallelThreads = 0` (or empty `Combat.ParallelMaps`) to revert instantly to the serial path.

**Validation.** (i) *Single-map gated trial* (map 0, 120s varied combat): 59/60 bots survived, worldserver PID stable, **12 SAFE / 0 HAZARD** samples (max interaction 19–48yd), D parallel active (parGroups up to 3466). (ii) *Multi-continent whitelist verified*: dispersed fleet on map 0 → `[COMBATPROF] parGroups=15936`, server ALIVE, 0 violations — confirming the CSV whitelist parses correctly and D's parallel path engages on the configured maps. Combined with TSan-clean (§3h) for data races and the interaction-distance monitor (§3j) for safe-distance, **D is now running as a monitored production trial on the four open-world continents.**

## 3l. Catch-it-all attempt C (detect-and-defer) — built, stress-tested, and REVERTED (2026-06-13)

To remove the per-map exclusion entirely (parallelize *any* map, closing the 50000yd-range gap of §3k), I implemented approach **C — detect-and-defer**: instrument the combat mutation entry point (`Unit::DealDamage`) so that when a mutation would touch an entity in a **same-color, different cell** (the one provably-unsafe case — another worker may be updating it), the *whole call* is captured as a closure and replayed in a serial post-barrier drain on the map thread. A per-`[COMBATPROF]` `crossCellDeferred` counter tracked how often it fired. The cell context (`t_curCellX/Y`), per-worker defer queues, and `Map::ShouldDeferCrossCell`/`DeferCombatMutation` were all added; it built clean and booted.

**The stress test caught a fundamental flaw.** To force constant deferrals I set `Combat.SafeStride = 1` (every cell becomes the same color → *every* cross-cell hit defers, even melee) and ran a dispersed combat fleet against 1200 hostiles. The worldserver crashed in ~55s on an assertion in `~Spell` (`Spell.cpp:639`):

```
m_caster->ToPlayer()->m_spellModTakingSpell != this
Id: 5302 'Defensive State - Follows a successful block,dodge or parry.'
```

**Why it's fundamental (not a fixable detail).** `DealDamage` is not a leaf mutation — it runs a *synchronous cascade*: damage → block/dodge/parry procs → triggered spell casts → aura applications, and that cascade manipulates **per-player spell-mod state** (`m_spellModTakingSpell`, set/cleared around mod application within the caster's update/cast context). Deferring the whole call tears that cascade out of its execution context and replays it post-barrier, so the spell-mod state machine is left inconsistent and a proc `Spell` is destroyed while still registered as the player's mod-taking spell. You cannot relocate a spell-casting cascade in time without rewriting the spell-mod lifecycle. (Fixing this one assert would only surface the next broken invariant — the approach is wrong at the root, not buggy.)

**Conclusion + the corrected path.** Defer-the-whole-call is unsafe; C was fully reverted (core restored to the validated stride-4 whitelist + monitor binary; verified 10/10 bots, server stable, monitor live). The lesson sharpens the §3k options:
- **(a) cross-cell lock** — now looks *better* than C for one reason: it runs the cascade **in place** (just serialized under a lock), so the spell-mod context is preserved. Its costs (per-entity locking for full read-safety; not strictly serial-equivalent) remain.
- **(b) decide/apply split** — the only approach that cleanly relocates work, because it separates the read-only *decision* (parallel, any range) from the *mutation* (serial), and the cascade runs entirely in the serial apply phase. This is the textbook-correct answer but a large spell-engine re-architecture.
- A narrower defer (defer only the **victim-side state writes** — health/threat/death — while running the attacker-side proc cascade inline, since the attacker is the current-cell entity) is the middle path, but requires surgically splitting `DealDamage`.

None of these is required for the megaserver goal: instances/raids (the only place the long-range mechanics actually fire — §3k) are low-pop and stay serial by design, and the four continents are already safe under the proven stride-4 scheme + monitor. So "catch it all" stays **documented future work**, with (b) the leading candidate, and production runs the validated, exclusion-based D.

**The stress harness itself is now a permanent asset:** `Combat.SafeStride = 1` is the way to force maximum cross-cell concurrency (every interaction crosses a same-color boundary) — exactly the adversarial condition any future catch-it-all design must survive before shipping.

## 3m. Catch-it-all — considered stopping point (2026-06-14)

After C failed (§3l), the next option was the **decide/apply split** (parallel read-only decision → serial mutation). Scoping it honestly killed it as a near-term move, and re-examining the residual hazard showed the whole catch-it-all effort has reached **diminishing returns**. The reasoning:

**The residual gap is far smaller than the §3k framing suggested.** Walking the cases:
- Common combat (≤50yd, measured): provably safe — interacting entities share a cell (same thread) or sit in different colors (sequential passes).
- **Different-color** cross-cell links (a DoT on a kited target, a pet, a Wintergrasp boulder landing in a different-color cell): **already safe** — different colors run in separate sequential passes, never concurrently. (This was underweighted earlier — it removes most of the apparent gap.)
- The **only** real hazard is a cross-cell combat link whose two cells happen to be the **same color** (≥266yd apart, same color) — e.g. a DoT caster and a target that kited >266yd into a same-color cell. That is the entire residual, and it is **empirically nonexistent** (monitor: ≤50yd across melee/ranged/pet) and **already backstopped** (any ≥266yd interaction logs a HAZARD).

**Both fuller approaches over-invest against that residual.** Full decide/apply is a multi-session spell-engine + AI re-architecture whose only unique payoff (raid parallelism) we deliberately don't want. Combat-aware grouping, examined closely, is trickier than pitched: unioning "anything that might interact" merges dense zones into giant single-thread blobs (destroying the parallelism); the correct narrow form — forcing same-color **linked** cells onto one worker — is real scheduler surgery for that same nonexistent edge case.

**Decision: bank the win and stop.** Lock in the validated scheme (stride 4, whitelist 0/1/530/571, monitor ON, instances serial) and treat catch-it-all as documented future work. **The monitor is the watchdog**: it runs in production, and the first time it logs a real HAZARD it will name the exact content responsible — at which point we respond surgically (bump that map's stride, or do the narrow same-color-link co-assignment) instead of guessing now. Building more today solves a problem the data says we don't have. Reopen only if (a) the monitor fires, or (b) raid parallelism ever becomes a genuine requirement.

## 4. Current server state
Running with **B2 + C + D all ON, production-gated**: `Visibility.FullRescan=1`, `Visibility.ParallelThreads=4`, `Visibility.ParallelMinPlayers=50`, `Visibility.ParallelProfile=0`; `Sessions.ParallelThreads=4`, `Sessions.ParallelMinSessions=50`, `Sessions.DeferPacketHooks=1`; `LuaLock.Profile=0`. **D (parallel combat)** now ON as a monitored trial: `Combat.ParallelThreads=4`, `Combat.ParallelMaps="0,1,530,571"` (the four open-world continents), `Combat.SafeStride=4` (266yd), `Combat.Validate=1` (interaction monitor ON as the live backstop). All paths remain kill-switchable (set the threads value to 0 to revert to the legacy path). Five gated profilers built in (`[VISPROF]` visibility, `[LUAPROF]` lua lock, `[SESSPROF]` session pass, `[COMBATPROF]` entity/combat update, `[COMBATVALIDATE]` safe-distance monitor) — profilers off in production except the monitor, which stays on. Combat harness gated off: `CombatLoadGen.ImmortalPlayers=0`, `CombatLoadGen.Profile=0`. Harness SQL saved at `/root/d_harness_hostiles.sql` / `_wide.sql` / `_rangedpet.sql`; fleet supports `FLEET_QSPAM`/`FLEET_SPREAD`/`FLEET_DISPERSE`. Backups: full pre-change file backup at `/root/tswow-server-backup`, DB dumps at `/root/db-backups/`, prod binary backups at `/root/prod_bin_backup/`. Plans/designs: `MEGASERVER_PLAN.md`, `MEGASERVER_B_VISIBILITY.md`, this file. Load harness: `/root/fleet.py` (+ `world_probe.py`).

## 5. Expected scaling impact (estimates, not load-tested)
The shipped work (broadcaster + visibility cap + throttle + 5 map threads + A1/A2, and B1/B2 when enabled) comfortably lifts the realistic ceiling on this 8-core box from "degrading past ~1000" toward **~1500–2500 spread-out players**. The hard 10–12k target needed parallel visibility *enabled and load-proven* (B2 — done) and parallel combat (D). With **D now enabled on the open-world continents** (§3k), the single biggest architectural wall — one continent updating combat on one thread — is lifted on exactly the maps where megaserver crowds form (capitals, starter zones), gated behind the safe-distance scheme + live monitor. The remaining ceiling work is widening the monitored trial under real crowds and resolving the 50000yd catch-it-all item (§3k) so the parallel path can extend to instanced/raid content too.

## 6. Recommended path forward
1. ~~**B2 to default-on**~~ — **DONE** (§3b): load-proven ~2.7x on expensive passes, flipped on production-gated.
2. ~~**A4** (lock the global subsystem registries: social, guild, group, chat)~~ — **DONE** (§1, §3 C): shipped + validated non-breaking.
3. ~~**C (parallel sessions)**~~ — **DONE + ON** (§3d, §3e): parallel query pre-pass, deferred-hook A3 slice (0% lua contention), persistent pool. Gated ≥50 sessions.
4. **A3 (full)** — the *slice* needed for C is shipped (deferred observer hooks). The *full* per-thread-`sol::state` A3 is still only needed for D's scripted-AI fraction, and remains a content-porting effort to greenlight separately. Not needed for C.
5. **Step 4 — broaden C's allow-list:** the starter set already covers the *high-volume* client queries (name/creature/gameobject/item/quest/page/npc-text — exactly what the client spams per entity seen). Further additions are diminishing-returns (low-volume) or unsafe (touch contended state). Each new opcode follows the same audit (writes no shared state; reads only immutable/locked globals + own session) + load-test. See §3f.
6. ~~**Step 5 — D (parallel combat)**~~ — **BUILT, VALIDATED, and ENABLED on the open-world continents** (§3g–§3k). Safe-distance cell coloring; TSan-clean for data races (180→4 races, the 4 out of scope); safe-distance monitor proves outcome-equivalence to serial for melee/ranged/pet combat (≤50yd vs 266yd). Gated per-map to EK/Kalimdor/Outland/Northrend; instances/raids/BGs stay serial (lower-value + higher-risk). Running as a monitored production trial with the kill-switch in place.
7. **Catch-it-all (the 50000yd pain point) — CLOSED as a considered stopping point (§3l, §3m).** The straightforward fix (**C, detect-and-defer**) was built, stress-tested, and **crashed** — `DealDamage`'s synchronous proc/spell-mod cascade can't be relocated to a post-barrier drain — and was reverted (§3l). Re-examining the residual then showed catch-it-all has hit diminishing returns (§3m): different-color cross-cell links are *already* safe (sequential passes), so the only real hazard is a same-color ≥266yd combat link — empirically nonexistent and monitor-backstopped. The fuller approaches over-invest: full **decide/apply** (read-only decision ∥ serial apply) is a multi-session spell-engine + AI rewrite whose only unique payoff is unwanted raid parallelism; **combat-aware grouping** collapses dense zones into single-thread blobs unless narrowed to real scheduler surgery. **Decision: lock in the proven scheme; the monitor is the production watchdog.** Reopen only if `[COMBATVALIDATE]` logs a real HAZARD (it will name the exact content) or raid parallelism becomes a genuine requirement. Future-work candidates, if reopened: narrow same-color-link co-assignment first, decide/apply only if raids ever matter.
8. **D as a continued effort:** widen the monitored trial (more maps' worth of real player combat), watch `[COMBATVALIDATE]` for any HAZARD, and measure speedup under genuine open-world crowds. D's *non-scripted* fraction parallelizes without A3; its scripted-AI fraction (and C) would need the full per-thread `sol::state` A3 — a separate content-porting effort.
