# Phase B — Parallel visibility: detailed implementation scope

Goal: get the O(n²) per-tick visibility cost (each player computing/serving what it can see) off the single continent thread, so a busy continent uses many cores. Target function: `Map::ProcessRelocationNotifies` (`Map.cpp` ~985) and the notifiers it drives (`GridNotifiers.cpp`).

## The enabling insight (why this is tractable)

Visibility runs in its **own phase**, *after* the entity-update phase that moves objects and ticks spells/auras. So during the visibility phase **all inputs are stable** — positions are already final, auras already applied, nobody is writing them. Visibility is therefore a *read-mostly* computation whose only writes are **per-entity-local**: each player's `m_clientGUIDs`, its `UpdateData` packet, its `i_visibleNow`, and broadcaster listener registration (already mutex-guarded, built that way). This is what makes lock-free parallelism possible — the hard part is not the reads, it's making sure each player's writes happen on exactly one thread.

## The design decision: player-sharded *full-rescan* (lock-free), not the obvious shardings

Three candidate models; the analysis killed the first two for our goal:

1. **Naive player-shard of the current code** — BROKEN. Today `PlayerRelocationNotifier::Visit(PlayerMapType)` does the *symmetric* update `player->UpdateVisibilityOf(&i_player)` (writes the **other** player's `m_clientGUIDs`), and the creature-relocation path pushes updates into *nearby players'* `m_clientGUIDs`. So a player's visible-set is written from multiple other entities' processing → cross-shard writes → needs a per-player lock on `m_clientGUIDs`. Reject (locking overhead, contention in crowds).

2. **Region-sharded with safe-distance** (vmangos `VisibilityUpdate.MaxThreads` model) — shares the scheduler with Phase D; no per-player locks because non-adjacent regions run concurrently. BUT in a dense blob everyone is within one safe-distance region → serializes onto one thread. Helps a *spread* continent, not the blob. Viable but doesn't fully parallelize.

3. **Player-sharded full-rescan** — CHOSEN. Restructure so a player's `m_clientGUIDs` is written **only** during that player's own visibility pass, and that pass **re-scans everything in range** (players + creatures, moved or not). Then writes are strictly disjoint per player → **lock-free**, fully parallel (no safe-distance serialization), and it helps the dense blob too. The cost is more *redundant scanning* (a stationary player near movement re-scans its surroundings instead of getting a targeted push) — which we pay for with parallelism, and which is **bounded by the per-observer cap we already built** (a player scans only its adaptive radius, ≤ ~300 players). At 10-12k on many cores, trading redundant work for lock-free parallelism is the right trade.

### What changes to make writes disjoint
- **Remove the symmetric update**: in `PlayerRelocationNotifier::Visit(PlayerMapType)`, drop `player->UpdateVisibilityOf(&i_player)`. Each player updates only its own view, in its own pass.
- **Remove the creature→nearby-player push** for the *player-visibility* part: today creature relocation calls `player->UpdateVisibilityOf(creature)` for nearby players. Drop that write; instead the player picks up the creature's new state in its own rescan.
- **Keep correctness for stationary observers** via the dirty-neighborhood gate (below): a stationary player whose neighborhood had movement is still scheduled for a rescan, so it notices the creature/player that moved up to it.

### What stays serial (Phase B does NOT parallelize these)
- **AI relocation** (`CreatureUnitRelocationWorker` → `MoveInLineOfSight_Safe`): creature AI noticing players mutates threat/AI (cross-creature). Pull it out of the visibility notifiers into a **separate serial pass** after the parallel visibility barrier (or hand to Phase D later). It's far cheaper than the packet-building visibility cost.
- **`ResetNotifier`** (clears `NOTIFY_VISIBILITY_CHANGED`, resets grid relocation timers): serial pass after the barrier.

## The dirty-player gate (correctness + avoids re-scanning everyone)

A player P needs a visibility rescan this tick iff:
- P itself moved enough (its own relocation flag / `m_lastNotifyPosition` delta — the gate we already added), **OR**
- any object within P's visibility radius moved (a `marked_cell` intersects P's cells).

`marked_cells` is set during the entity-update phase and **read-only** during visibility → safe to test concurrently. So: build the **dirty-player list** = players who moved OR have a marked cell in range. Only those are scheduled. In a quiet zone almost nobody is dirty (no work); in a busy zone most are (but that's the work we're parallelizing). This replaces the cell-marked iteration with a player-centric one.

## Worker-pool integration

- Reuse the existing `MapUpdater` thread pool, or add a dedicated visibility sub-pool sized by `Visibility.ParallelThreads` (default = MapUpdate.Threads, 0/1 = serial).
- Inside `ProcessRelocationNotifies`: build the dirty-player vector → partition across N workers → each worker runs, for its slice, `Cell::VisitAllObjects(viewPoint, PlayerRelocationNotifier(player), GetSightRange-equivalent)` which updates only that player's view → **barrier** → serial AI-relocation pass → serial `ResetNotifier`.
- **Per-map**: this parallelizes within a single map's visibility phase. It composes with the existing across-map `MapUpdater` (a map's visibility sub-pool runs while that map holds its update slot). Watch total thread count (across-map threads × visibility sub-threads) vs cores — likely make visibility reuse the same pool rather than nest.
- **Gate**: only parallelize when the dirty-player count exceeds a threshold (e.g. 200); below it, run serial (no thread-dispatch overhead, no behavior change).

## Thread-safety ledger (what each worker touches)

| Access | Site | Safe because |
|---|---|---|
| read other objects' position/auras/phase | `CanSeeOrDetect` | stable in this phase (written in prior entity-update phase) |
| write P's `m_clientGUIDs`, `UpdateData`, `i_visibleNow` | `Player::UpdateVisibilityOf` | disjoint per player (only P's own pass writes them) |
| register/unregister listener in **other** player's broadcaster | `StartListeningTo`/`StopListeningTo` | already mutex-guarded (PlayerBroadcaster) |
| read `marked_cells` | dirty-gate + visit | read-only in this phase |
| send packets to P's session | `SendDirectMessage` | P's own socket; per-player |
| `m_visiblePlayerCount` / cap radius | the cap (Player.cpp) | P-local, written on P's thread only |
| write `NOTIFY_VISIBILITY_CHANGED` flags | **ResetNotifier** | moved to serial post-barrier pass |
| creature AI `MoveInLineOfSight` | **CreatureUnitRelocationWorker** | moved to serial post-barrier pass |

The only cross-thread writes are broadcaster listener ops (locked) — everything else is per-player-disjoint or read-only.

## Synergy with what's already built
- **Per-observer cap** bounds each player's scan to its adaptive radius (≤ ~300 players) → makes full-rescan affordable exactly where it'd otherwise be expensive (crowds).
- **Broadcaster** listener registration is already the only locked cross-player op, and it's lock-sharded per broadcaster.
- **tswow lua mutex**: does the visibility path fire livescript hooks? Audit `Player::UpdateVisibilityOf` / `CanSeeOrDetect` for `FIRE`/`FIRE_ID`. If yes (e.g. an OnVisibility hook), those serialize on the lua lock and cap the parallelism — must confirm the visibility path is lua-free (likely is; lua hooks are mostly on combat/AI/use). **This is the first thing to verify before building.**

## Incremental sub-steps (each builds + tests green before the next)

1. **B0 — DONE 2026-06-13: visibility path is lua-free → B is NOT gated by A3.** The only `FIRE_ID` in the whole path is `Creature,OnMoveInLOS` in `CreatureUnitRelocationWorker` (GridNotifiers.cpp:147) — i.e. inside the AI-relocation we already move to the SERIAL post-barrier pass. `Player::UpdateVisibilityOf` and `CanSeeOrDetect` have NO FIRE. So the *parallelized* player-visibility part never touches the lua mutex. Green light.
2. **B1 — restructure to full-rescan, still SERIAL.** Remove the symmetric update + creature→player push; add the dirty-player gate; move AI-relocation + ResetNotifier to explicit post-passes. Ship it single-threaded first. This is a *behavioral* change (visibility timing) with zero threading risk — validate correctness here (players see each other, moving creatures noticed, no ghosts, stealth/phase still work) before adding threads.
3. **B2 — parallelize the dirty-player loop** behind `Visibility.ParallelThreads` + the count threshold. Now purely a threading change on top of the already-validated B1 logic.
4. **B3 — tune**: threshold, pool sizing vs across-map threads, profile.

## Testing strategy
- **Correctness (B1, serial)**: multi-client visibility test — clients at varying distances see/unsee each other correctly as they move; a moving client is noticed by a stationary one (dirty-gate works); stealth/invis/GM-visibility/phase still gate correctly; no ghost objects (destroyed entities removed); broadcaster relay still works (depends on visibility). Extend `world_probe.py` to assert A appears/disappears in B's object set as A crosses B's range.
- **Parallel safety (B2)**: run with `Visibility.ParallelThreads=5`; soak with synthetic load (wandering NPCBots as crowd) under Tracy; watch for visibility races (intermittent invisible players), crashes, and the `_clientGUIDs`/UpdateData integrity. ASan build for a soak if feasible.
- **Perf**: Tracy zone around `ProcessRelocationNotifies` — confirm wall-time drops ~linearly with threads under spread load; confirm serial-gate no-op at low pop.
- **Regression**: existing relay + auth + cap unit tests stay green; low-pop = unchanged behavior.

## Risks & mitigations
- **Behavioral change from full-rescan** (visibility *timing* differs slightly): isolate it in B1 (serial) and validate before any threading. Biggest risk is a subtle "stationary observer misses a fast-mover" if the dirty-gate is wrong — test explicitly.
- **More CPU from redundant scanning**: bounded by the per-observer cap; gate parallelization so quiet maps don't pay; profile.
- **Hidden cross-writes**: the ledger above must be verified by code-read before B2 — especially any custom (tswow/Duskhaven) addition to `UpdateVisibilityOf` that writes shared state. Audit pass required.
- **Thread count blowup** (across-map × visibility sub-pool): prefer reusing one pool; cap total at cores−2.

## Effort
- B0+B1 (restructure + serial validation): ~3-5 days.
- B2 (parallelize + soak): ~3-5 days.
- B3 (tune/profile): ~2-3 days.
- Total ~1.5-2 weeks, most of the risk front-loaded in B1's behavioral validation.

## Recommended first action
**B0** (10 minutes: confirm the visibility path is lua-free), then **B1** (the serial restructure) — because it de-risks the whole thing: the hard correctness work happens single-threaded where it's debuggable, and B2 becomes a pure, well-bounded threading change on validated logic.


---

## B1 progress + a complication found while reverse-engineering the real code (2026-06-13)

**Validation harness built & baselined**: `/root/vis_probe.py` uses the broadcaster relay as a free visibility signal (B receives A's movement IFF B can see A). Tests the 3 properties B1 must preserve with 2 clients (B stationary): co-located→B sees A; A walks 130yd away→B loses A; A walks back→B re-sees A (the stationary-observer dirty-gate case). **Current code baselines ALL PASS.** This is the tool that validates the restructure.

**Plan refinement — implement behind a config gate**: `Visibility.FullRescan` (default 0 = current path, zero risk). Land the new path off-by-default, validate with the harness (flag on vs off), only then consider default-on. Same kill-switch discipline as everything else.

**The complication (not in the original scope): grid-relocation-timer coupling.** `ProcessRelocationNotifies` throttles visibility via a *per-grid relocation timer* (the DynamicVisibility notify-period): a grid is only processed when its timer passes, and the timer is `TUpdate`'d inline in the cell loop and `TReset` in the ResetNotifier pass. A naive player-driven pass that iterates all players every tick would (a) bypass this throttle → visibility recomputed every tick (perf + behavior change) and (b) read stale timer state if it runs before the cell loop's `TUpdate`. So the restructure must first **refactor the grid-relocation-timer management out of the cell loop into a shared "advance timers / which grids are due this tick" step** that both the new player-driven pass and the (kept) creature/AI cell loop consult. This is the real work of B1 — not just removing two cross-write lines.

**Precise restructure (gated)**:
1. Refactor timer advance into a pre-step; expose "is grid G due this tick".
2. Player-driven pass (only for due grids): for each player P, dirty = (P moved past the dist-gate) OR `Map::HasMarkedCellInRange(P, MAX_VISIBILITY_DISTANCE)`; if dirty, run `PlayerRelocationNotifier` (symmetric cross-write suppressed in fullrescan) + `SendToSelf`.
3. `DelayedUnitRelocation::Visit(PlayerMapType)` early-returns in fullrescan (players handled by the new pass); the creature `Visit(CreatureMapType)` (AI + creature-seen-by-players) stays as the cell loop.
4. `ResetNotifier` unchanged (after both).
5. AI worker still fires inside the player pass for now (serial-safe; B2 separates it for the lua-free guarantee — already confirmed the only lua hook is OnMoveInLOS there).
6. `Map::HasMarkedCellInRange` helper (ComputeCellCoord bounding box over `marked_cells`).

**Validation gap to close before default-on**: the harness covers player↔player visibility. It does NOT cover creature aggro (MoveInLineOfSight) — need a creature-aggro check (hostile mob notices a moving player) and ultimately real in-game testing, since the restructure touches the AI-relocation path too.


---

## B1 IMPLEMENTED & VALIDATED (gated, default off) — 2026-06-13

Done, behind `Visibility.FullRescan` (default 0 = legacy path, zero risk):
- **Map.cpp ProcessRelocationNotifies**: player-driven pass inserted between the cell loop's timer `TUpdate` and the `ResetNotifier`'s `TReset` (so grid timers are current — the throttle is preserved). Each player processed iff its grid timer is due AND it's dirty (moved past the dist-gate OR `HasMarkedCellInRange(MAX_VISIBILITY_DISTANCE)`); runs its own `PlayerRelocationNotifier` + `SendToSelf`. Cell loop does creatures only under the flag.
- **GridNotifiers.cpp**: in full-rescan, `PlayerRelocationNotifier::Visit(PlayerMapType)` skips the symmetric cross-write; `CreatureRelocationNotifier::Visit(PlayerMapType)` skips the player-sees-creature push (player picks it up in its own rescan); `DelayedUnitRelocation::Visit(PlayerMapType)` early-returns. New `i_playerMoved` flag gates the creature-AI MoveInLineOfSight so it still only fires for genuinely-moved players (AI behaviour unchanged).
- **Map::HasMarkedCellInRange** + `Map::s_visibilityFullRescan` + config wiring (World.cpp).
- **Defensive coord clamping**: `Trinity::ComputeGridCoord`/`ComputeCellCoord` do NOT clamp (a negative becomes a huge uint32). Added `gc.IsCoordValid()` guard before `getNGrid` (which ASSERTs) and clamp+bounds-check in `HasMarkedCellInRange` (else `marked_cells.test` throws). These are correct regardless.

**Validation** (vis_probe.py, broadcaster relay = visibility signal):
- FullRescan=0 → ALL PASS (legacy unchanged): co-located sees / 130yd away loses / returns re-sees; object-blocks A=444 B=194.
- FullRescan=1 → ALL PASS (the dirty-gate stationary-observer case works): object-blocks A=630 B=286 (healthy — creature/object visibility preserved, actually higher from fuller rescans). Server STABLE 160s, 0 errors, 5 map threads.
- A spurious "crash" during testing was traced to my own overlapping pkill/restart commands SIGKILLing the worldserver mid-test, NOT a code fault. A single disciplined restart passes cleanly and the server survives.

**Left default OFF** (kill-switch discipline). Remaining before default-on: load-scale validation, a direct creature-aggro test (only an object-block proxy so far), and real in-game testing. Next: **B2** = parallelize the player-driven loop (the writes are now disjoint per player).

---

## B2 IMPLEMENTED & VALIDATED (gated, default off) — 2026-06-13

Parallelized the disjoint-write visibility pass. Behind `Visibility.ParallelThreads` (default 0 = serial = B1) + `Visibility.ParallelMinPlayers` (default 100); requires `Visibility.FullRescan=1`.

- **AI separated from the parallel pass**: the visibility pass now runs `PlayerRelocationNotifier(*player, /*playerMoved=*/false)` — AI line-of-sight suppressed → no creature-AI mutation, no lua FIRE → safe to parallelize. Creature-notices-moved-player is done in a **serial post-pass** via `AIRelocationNotifier` for players that moved (mutates creature AI/threat + fires lua → must be serial).
- **ProcessRelocationNotifies** (Map.cpp): (1) collect dirty players serially; (2) visibility — if `ParallelThreads>=2 && dirty>=ParallelMinPlayers`, partition the dirty list across N `std::thread`s each running the visibility lambda, join; else serial; (3) serial AI pass.
- Config statics `Map::s_visibilityParallelThreads/Min` + `SetVisibilityParallel`, wired in World.cpp.

**Validation:**
- vis_probe with ParallelThreads=4, ParallelMinPlayers=2 → 2 players' visibility computed on 2 threads concurrently (touching each other's locked broadcasters, reading each other's positions): **ALL PASS**, object-visibility healthy.
- load_probe.py (new): 12 players co-located on map 0, wandering 60s, parallel engaged → **12/12 OK, 1884 moves, no crash**.
- **PID-stability check**: worldserver PID unchanged across vis_probe + load_probe (SAME_NO_CRASH), 346s continuous uptime → no crash/restart from the parallel path.
- (A spurious load_probe "12 FAILED" on a re-run was a harness char-name collision — orphaned characters from the prior run — confirmed by PID stability; fixed the harness to delete prior chars.)

**Left default OFF.** First-impl notes (future optimization, not correctness): per-tick `std::thread` spawn (a persistent pool would avoid spawn overhead); the serial AI pass re-traverses cells for moved players (could be combined); the dirty-collection loop scans all players each tick. Remaining before default-on: larger-scale soak + real in-game validation (esp. creature aggro, which the harness only covers by an object-visibility proxy). **Phase B (parallel visibility) core is complete and validated at small/medium scale.**
