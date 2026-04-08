# NPCBot Battleground AI System

## Overview

NPCBots in battlegrounds use a layered AI system that combines real-time potential field navigation, team coordination plans, self-learning from match outcomes, and reinforcement learning (Q-learning) to produce organic, human-like behavior. The system operates across Warsong Gulch (WSG), Arathi Basin (AB), and Eye of the Storm (EY).

Bots can queue and play battlegrounds autonomously without any players present. They learn from every match, improving their strategies, navigation paths, and tactical decisions over time.

BG bots do NOT use the WanderNode graph for navigation. They use potential field steering with learned waypoint mesh data, with MMAP pathfinding as the terrain-level movement system. WanderNodes are only used by world bots outside of battlegrounds.

---

## Architecture Layers

```
Layer 5: Q-Learning (weight optimization with continuous offsets)
Layer 4: Team Coordination (shared plans, rally points)
Layer 3: Potential Field Navigation (vector-based steering)
Layer 2: Combat Intelligence (targeting, CC, kiting, peeling)
Layer 1: Learned Data Systems (waypoint mesh, heatmap, strategies)
Layer 0: Core Movement (MMAP pathfinding)
```

Each layer builds on the ones below it. A bot without any learned data still functions (layers 0-4 with mesh seeding), just less optimally. As data accumulates across matches, layers 1 and 5 improve automatically.

---

## Layer 0: Core Movement

### MMAP Pathfinding
The server's built-in navigation mesh (MMAP) handles terrain-aware movement. When a bot needs to walk from point A to point B, MMAP generates a path that avoids walls, navigates around obstacles, and follows valid walkable surfaces.

### Spawn Positioning
Bots spawn in the BG's team start position (prep room) using `Battleground::GetTeamStartPosition()`. Random scatter (2-8 yards) prevents stacking. No WanderNode dependency.

### Movement Types
- **MOVE_POINT**: Standard walking/running along an MMAP-generated path
- **MOVE_JUMP**: Parabolic arc trajectory for dropping off ledges or jumping gaps
- **MOVE_CHASE**: Follow a moving target (used in combat)

### Safety Systems
- **Air-glide fix**: Every tick, if a bot is more than 3 yards above ground and not jumping/falling, it stops movement and lets the PF system re-route on the next tick
- **Drop detection**: If the target position is 4+ yards below the bot, uses JUMP movement instead of walking
- **Stuck-on-ledge detection**: If a bot hasn't moved for 3+ evade cycles, checks for ground below in the objective direction and jumps down if found
- **Stuck detection**: If a bot hasn't moved for 15+ evade cycles, clears objective and lets the PF system pick a new direction. No hearthstone teleport in BGs.
- **Wall avoidance**: Before every movement command, validates LOS to the target. If blocked, rotates direction left/right in ~23-degree increments (up to ~140 degrees) to find a path around walls. If rotation fails, falls back to MMAP PathGenerator for geometry-aware pathfinding. If all fail, skips the tick.

### Mesh Seeding
On the very first game on a map (no learned data), each bot seeds the waypoint mesh with a random walk from spawn toward the map center. Each bot takes 20 steps with random lateral deviation (15-30 yards per step, +/-35 degrees), recording valid ground positions. With 16-24 bots, this produces 320-480 seed waypoints — enough for the potential field to have meaningful data from game one.

---

## Layer 1: Learned Data Systems

All learned data persists to the MySQL database and loads on server startup. Temporal decay (0.995x per flush) prevents old data from dominating. Waypoint mesh is also decayed and pruned (cells below visitCount=2 are removed).

### Waypoint Mesh
A 3-yard grid of positions recorded every 3 seconds while bots move in BGs. Each cell stores:
- `visitCount`: how many times any bot or player has walked through this cell
- `z`: averaged ground height

After many matches, this creates a dense mesh of positions that bots and players have actually traversed. The potential field's learned waypoint fallback uses this data when raw PF positions fail LOS.

**Player path learning**: Bots also record the positions of nearby players (within 60 yards) who are alive and moving. This means bots learn from player routes — shortcuts, creative paths around walls, and optimal traversal patterns all get absorbed into the mesh automatically. A few games with human players dramatically improves bot pathing in complex areas.

**Database table**: `characters_npcbot_bg_waypoints`

### Heatmap
Same 3-yard grid but tracking combat events:
- `kills`: kills that occurred at this position
- `deaths`: deaths that occurred at this position
- `objCaps`: objective captures (flag grabs, node caps)
- `objDefends`: defensive kills near friendly objectives

The heatmap feeds into danger zone avoidance (bots avoid high-death areas when outnumbered) and adaptive patrol (defenders patrol known engagement hotspots).

**Database table**: `characters_npcbot_bg_heatmap`

### Strategy Learning (Bayesian)
Six strategies with win/loss tracking using Beta(2,2) prior for faster convergence:
- RUSH_FLAG, DEFEND_BASE, SPLIT_ATTACK, GROUP_PUSH, ROAM_KILLS, TURTLE_DEFENSE

**Database table**: `characters_npcbot_bg_strategy`

### Counter-Strategy Learning
Records outcomes when using strategy X against enemy team using strategy Y.

**Database table**: `characters_npcbot_bg_counter_strategy`

### Class Matchup Learning
Tracks win/loss rates for each class-vs-class combination per map.

**Database table**: `characters_npcbot_bg_matchups`

### Group Success, Timing, Win Condition Learning
Additional data layers tracking group size effectiveness, action timing, and game state win probabilities.

**Database tables**: `characters_npcbot_bg_group_success`, `characters_npcbot_bg_timing`, `characters_npcbot_bg_win_conditions`

---

## Layer 2: Combat Intelligence

### Target Selection Pipeline
1. **Peeling**: DPS bots protect allied flag carriers and healers by switching to their attackers
2. **Focus Fire**: Smart bots set a shared team kill target (8-second expiry)
3. **Flag Carrier Priority**: Enemy flag carriers are high-priority targets
4. **Healer Priority**: Enemy healers are prioritized based on aggression personality
5. **CC Awareness**: Bots avoid attacking targets with breakable CC (polymorph, sap, hex, etc.)
6. **Objective Focus Gradient**: As bots approach their objective, they increasingly ignore enemies. Near the flag, healthy bots run past attackers.
7. **Engagement Evaluation**: Combines class matchup history, HP ratio, and numerical advantage

### Combat Positioning
- **Kiting**: Ranged bots move away from melee enemies within 8 yards
- **Strafing**: Melee bots make periodic lateral movements during combat
- **Speed Boost**: Class-specific speed abilities triggered after flag pickup

---

## Layer 3: Potential Field Navigation

Instead of picking destination waypoints, bots compute a **direction vector** from four forces summed together.

### Force Components

**Objective Attraction**: Direction toward the bot's current objective (flag, node, cap point). Always the primary force.

**Ally Repulsion**: Nearby allied bots (within 15 yards) exert inverse-square repulsion. Flag carriers get wider repulsion (22.5 yards), healers get tighter (9 yards). Naturally produces formations.

**Presence Avoidance**: A real-time 15-yard grid tracks where allied bots currently are. Bots steer away from high-presence cells, preventing corridor clustering.

**Enemy Vector**: Uses shared enemy sighting data (5-second TTL). FCs flee from enemy concentrations. Aggressive bots seek enemies.

### Weight Composition
Weights are determined by Q-learning (Layer 5) with continuous offsets, then modulated by personality. Near objectives (< 15 yards), spreading forces reduce to allow clustering for captures.

### Movement Execution
Each Evade() tick when the bot is stationary:
1. Compute potential field direction from Q-learned weight preset
2. Pick a position 15-25 yards ahead in that direction
3. Validate ground height and LOS
4. If LOS fails: rotate direction left/right to find path around walls (6 rotation steps, both directions)
5. If rotation fails: try learned waypoints aligned with PF direction
6. If still blocked: fallback to MMAP PathGenerator for geometry-aware pathfinding
7. Execute movement (MOVE_POINT or MOVE_JUMP for drops)

---

## Layer 4: Team Coordination

### Shared Team Plan
Per-BG-instance, per-team data structure updated every 5 seconds by smart bots.

**WSG**: flagAttackers, flagDefenders, fcEscorts counts
**AB/EY**: Per-node assignments with intent (IGNORE, DEFEND, ATTACK, URGENT) and desired bot count

### Objective Proximity System
All objective interactions (flag pickup, flag delivery, node capping) fire based on proximity to game objects, checked every tick regardless of movement state:
- **WSG flag pickup**: Within 10 yards of enemy flag object
- **WSG flag delivery**: FC within 10 yards of own flag room
- **AB banner cap**: Within 10 yards of nearest uncapped banner
- **EY flag pickup**: Within 10 yards of Netherstorm flag
- Smart fight-or-grab decisions based on HP, nearby allies, and attackers

### Objective Gradient Pull
Bots feel increasing pull toward objectives the closer they get (flag-to-flag gradient from 0% to 100%). Near the flag, healthy bots run past attackers to grab it.

### Enemy Position Memory
Bots share last-known enemy positions across the team with 5-second TTL. Used by the potential field's enemy vector.

### Adaptive Patrol
Defenders patrol heatmap engagement hotspots around their objective instead of standing still, cycling through positions every 10 seconds.

---

## Layer 5: Q-Learning

### Purpose
Learns which potential field weight presets work best for each game state, role, and personality type. Replaces hardcoded weights with data-driven optimization and evolves novel weight combinations through continuous offsets.

### State Space
900 states: `(bgType[3] x momentum[5] x scoreBracket[5] x timeBracket[4] x bgContext[3])`
- timeBracket includes OPENING (first 90s — learnable, not hardcoded)
- bgContext: WSG flag status / AB nodes held / EY points held

### Action Space
7 discrete weight presets: Balanced, Aggressive, Defensive, GroupPush, SoloFlank, FCRun, HealerProtect

### Roles & Personality
4 role-specific Q-tables x 2 personality buckets (aggressive/cautious) = 8 independent Q-tables. Aggressive attackers learn different optimal presets than cautious attackers.

### Continuous Weight Offsets
Each Q-entry stores 4 learned weight offsets that modify the base preset. On wins, offsets nudge toward strengthening the current configuration. On losses, they weaken. Clamped to +/-0.3. Over time, the system discovers novel weight combinations beyond the original 7 presets.

### Exploration (UCB1)
Upper Confidence Bound replaces epsilon-greedy. Each action scores `Q(s,a) + 1.5 * sqrt(ln(totalVisits) / visits(s,a))`. Naturally explores undervisited actions while exploiting known-good ones. 5% random floor preserved. Low-intelligence bots get extra randomness (+20%).

### Reward Signal
Per-match per-bot: `win(+1/-0.5) + matchKills*0.1 - matchDeaths*0.15 + objectiveCaps*0.3 + scoreDiff*0.01`, clamped [-2, 3].

### Update Algorithm
Weighted Monte Carlo with recency bias: last step gets 100% credit, first step gets 50%. Much better credit assignment than pure gamma discounting for long BG episodes.

**Database tables**: `characters_npcbot_bg_qtable`, `characters_npcbot_bg_qmeta`

---

---

## Advanced Coordination Systems

### Intention Broadcasting
Before committing to a decision, each bot broadcasts its intention (ATTACK_FLAG, DEFEND_FLAG, ESCORT_FC, ATTACK_NODE, DEFEND_NODE, ROAM) to a shared team intention map. Other bots read this map to avoid redundancy — if two teammates already intend to attack a node, a bot will pick a different target. Intentions expire after 10 seconds. Used by both WSG role assignment and AB/EY node assignment.

### Cooldown Communication
Bots broadcast when they use major cooldowns (Heroism/Bloodlust). When active team cooldowns exist: potential field objective weight boosted 1.2x (push harder), engagement threshold lowered 0.85x (take more fights). Expired entries auto-pruned.

### Mid-Match Strategy Revision
Every 60 seconds, each bot evaluates its recent performance (kills vs deaths). If deaths > 2× kills with zero objective captures, triggers `_bgNeedsReassessment` which forces a strategy re-evaluation on the next decision. No more waiting until death/respawn to realize a strategy is failing.

### UCB1 Exploration
Replaced epsilon-greedy Q-action selection with Upper Confidence Bound. Each action's score = `Q(s,a) + 1.5 * sqrt(ln(totalVisits) / visits(s,a))`. Naturally explores undervisited actions while exploiting well-known good ones. 5% random floor preserved. Low-intelligence bots get extra randomness.

### State Interpolation
When a Q-state has fewer than 10 visits, blends its Q-value with momentum±1 and scoreBracket±1 neighbors (weight 0.3 each). Produces smoother estimates for rare states. Boundary-safe: validates neighbor keys stay within the same BG type's state range.

### Position-Aware Combat
`ComputeTerritoryFactor` returns 0.0 at enemy base, 0.5 at midfield, 1.0 at own base. Engagement threshold scales: bots fight more aggressively at home (lower threshold) and cautiously in enemy territory (higher threshold). Flag carriers always treated as if in enemy territory (maximum caution).

---

## Personality System

Each bot has 5 personality traits deterministically computed from its creature entry ID:

- **Aggression** (0-1): Attack vs defend preference, healer targeting, Q-personality bucket
- **Caution** (0-1): Danger zone avoidance, engagement threshold, kiting frequency
- **ObjectiveFocus** (0-1): Objectives vs kills priority
- **GroupTendency** (0-1): Plan compliance, repulsion weight, crowd-following
- **Intelligence** (0-1): Decision quality gate (15% floor, 95% cap), Q-exploration rate

---

## Data Flow Summary

```
BG Entry
    ├── Bot spawns at team start position (prep room)
    ├── All BG state variables reset
    ├── If no learned waypoints: seed mesh with random walk
    │
During Match (every tick):
    ├── Team plan evaluated every 5s by smart bots
    ├── Intentions broadcast to team (ATTACK/DEFEND/ESCORT)
    ├── Q-learning selects weight preset via UCB1 + applies continuous offsets
    ├── Potential field computes movement direction (4 vectors)
    ├── Presence grid updated with current bot positions
    ├── Waypoint mesh records bot AND player positions every 3s
    ├── Heatmap records kills, deaths, objectives
    ├── Enemy sightings shared across team (5s TTL)
    ├── Mid-match strategy revision every 60s (performance check)
    ├── Objective proximity checks every tick (flag/node interaction)
    ├── Wall avoidance: LOS + rotation + MMAP fallback before movement
    ├── Cooldown broadcasts shared across team
    │
BG End
    ├── Q-values updated with reward per bot (weighted Monte Carlo)
    ├── Strategy/counter-strategy/win condition outcomes recorded
    ├── All data flushed to DB (with temporal decay + waypoint pruning)
    ├── Runtime state cleared
    │
Server Restart
    └── All 10 DB tables loaded (waypoints, heatmap, strategies, counter-strategies,
        group success, timing, matchups, win conditions, Q-table, Q-meta)
```

---

## Database Tables

| Table | Purpose | Persistence |
|-------|---------|-------------|
| `characters_npcbot_bg_waypoints` | Learned navigation paths | Permanent, decaying + pruned |
| `characters_npcbot_bg_heatmap` | Kill/death/objective spatial data | Permanent, decaying |
| `characters_npcbot_bg_strategy` | Strategy win/loss counts | Permanent, decaying |
| `characters_npcbot_bg_counter_strategy` | Strategy-vs-strategy outcomes | Permanent, decaying |
| `characters_npcbot_bg_group_success` | Group size effectiveness | Permanent, decaying |
| `characters_npcbot_bg_timing` | Action timing effectiveness | Permanent, decaying |
| `characters_npcbot_bg_matchups` | Class-vs-class win rates | Permanent, decaying |
| `characters_npcbot_bg_win_conditions` | Game state win probability | Permanent, decaying |
| `characters_npcbot_bg_qtable` | Q-learning values + weight offsets | Permanent |
| `characters_npcbot_bg_qmeta` | Q-learning metadata (games played) | Permanent |

## File Reference

| File | Purpose |
|------|---------|
| `bot_bg_ai.h` | All BG AI data structures, enums, BotBGAIMgr class declaration |
| `bot_bg_ai.cpp` | Learning systems, potential field, Q-learning, presence grid |
| `bot_ai.h` | Per-bot BG member variables and function declarations |
| `bot_ai.cpp` | Movement pipeline (Evade), combat targeting, objective proximity, team plans |
| `bot_GridNotifiers.h` | CC awareness check in NearestHostileUnitCheck |
| `Battleground.cpp` | BG end recording, Q-value updates, shared state cleanup |
| `botdatamgr.cpp` | Autonomous BG queue management |
| `botconfig.h/cpp` | Configuration (autonomous BG toggle, interval) |
