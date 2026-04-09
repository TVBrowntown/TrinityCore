#ifndef BOT_BG_AI_H
#define BOT_BG_AI_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Battleground;
class Creature;

// Grid cell key for 3-yard grid
struct BGGridCell
{
    uint32 mapId;
    int16 gridX;
    int16 gridY;

    bool operator==(BGGridCell const& o) const
    {
        return mapId == o.mapId && gridX == o.gridX && gridY == o.gridY;
    }
};

struct BGGridCellHash
{
    size_t operator()(BGGridCell const& c) const noexcept
    {
        size_t h = std::hash<uint32>{}(c.mapId);
        h ^= std::hash<int16>{}(c.gridX) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int16>{}(c.gridY) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct BGLearnedWaypoint
{
    float z;
    uint32 visitCount;
    uint32 wallHits{0}; // times bots hit LOS failures here — penalizes wall-adjacent paths
};

struct BGHeatmapData
{
    uint32 kills;
    uint32 deaths;
    uint32 objCaps;
    uint32 objDefends;
};

struct BGStrategyData
{
    uint32 successCount;
    uint32 failCount;
};

// Per-bot personality traits (computed from entry ID, never stored)
struct BotBGPersonality
{
    float aggression;      // 0.0-1.0: prefers attacking vs defending
    float caution;         // 0.0-1.0: retreats early vs fights to death
    float objectiveFocus;  // 0.0-1.0: prioritizes objectives vs chasing kills
    float groupTendency;   // 0.0-1.0: sticks with allies vs roams solo
    float intelligence;    // 0.0-1.0: quality of decisions + reaction speed
};

enum BGStrategyId : uint32
{
    BG_STRATEGY_RUSH_FLAG       = 0,
    BG_STRATEGY_DEFEND_BASE     = 1,
    BG_STRATEGY_SPLIT_ATTACK    = 2,
    BG_STRATEGY_GROUP_PUSH      = 3,
    BG_STRATEGY_ROAM_KILLS      = 4,
    BG_STRATEGY_TURTLE_DEFENSE  = 5,
    BG_STRATEGY_MAX
};

// BG decision action types
enum BGDecisionAction : uint8
{
    BG_ACTION_ATTACK_NODE = 0,
    BG_ACTION_DEFEND_NODE,
    BG_ACTION_GRAB_FLAG,
    BG_ACTION_INTERCEPT_FC,
    BG_ACTION_NINJA_CAP,
    BG_ACTION_FIGHT_MID,
    BG_ACTION_GRAB_BUFF,
    BG_ACTION_RUSH_BOSS,
    BG_ACTION_ASSAULT_TOWER,
    BG_ACTION_KILL_CAPTAIN,
    BG_ACTION_CAPTURE_MINE,
    BG_ACTION_PATROL,
    BG_ACTION_MAX
};

enum BGTimeBracket : uint8 { BG_TIME_OPENING = 0, BG_TIME_EARLY = 1, BG_TIME_MID = 2, BG_TIME_LATE = 3 };
enum BGScoreBracket : uint8 { BG_SCORE_LOSING_BAD = 0, BG_SCORE_LOSING = 1, BG_SCORE_TIED = 2, BG_SCORE_WINNING = 3, BG_SCORE_WINNING_BIG = 4 };
enum BGEnemyBehavior : uint8 { BG_ENEMY_UNKNOWN = 0, BG_ENEMY_AGGRESSIVE = 1, BG_ENEMY_DEFENSIVE = 2, BG_ENEMY_SPLIT = 3, BG_ENEMY_ZERG = 4 };
enum BGMomentum : uint8 { BG_MOMENTUM_WIPED = 0, BG_MOMENTUM_OUTNUMBERED = 1, BG_MOMENTUM_EVEN = 2, BG_MOMENTUM_ADVANTAGE = 3, BG_MOMENTUM_DOMINATING = 4 };

// Q-Learning enums and structs
enum BGQRole : uint8 { BG_QROLE_ATTACKER = 0, BG_QROLE_DEFENDER = 1, BG_QROLE_FC = 2, BG_QROLE_HEALER = 3, BG_QROLE_MAX = 4 };
enum BGQAction : uint8 { BG_QACTION_BALANCED = 0, BG_QACTION_AGGRESSIVE = 1, BG_QACTION_DEFENSIVE = 2, BG_QACTION_GROUP_PUSH = 3, BG_QACTION_SOLO_FLANK = 4, BG_QACTION_FC_RUN = 5, BG_QACTION_HEALER_PROT = 6, BG_QACTION_MAX = 7 };

struct BGQWeightPreset { float wObj; float wRepulsion; float wPresence; float wEnemy; };
struct BGQEntry {
    float qValue{0.0f};
    uint32 visitCount{0};
    float wObjOff{0.0f};     // learned offset to objective weight
    float wRepOff{0.0f};     // learned offset to repulsion weight
    float wPresOff{0.0f};    // learned offset to presence weight
    float wEnemyOff{0.0f};   // learned offset to enemy weight
};
struct BGQEpisodeStep { uint16 stateKey; uint8 action; uint8 role; uint8 personalityBucket; };
struct BGQMatchStats { uint16 kills{}; uint16 deaths{}; uint32 objCaps{}; };

// Potential field weight constants
static constexpr float PF_WEIGHT_OBJECTIVE = 1.0f;
static constexpr float PF_WEIGHT_REPULSION = 0.4f;
static constexpr float PF_WEIGHT_PRESENCE  = 0.2f;
static constexpr float PF_WEIGHT_ENEMY_FC  = 0.3f;
static constexpr float PF_WEIGHT_ENEMY_AGGRO = -0.2f;
static constexpr float PF_WEIGHT_ENEMY_DEFAULT = 0.1f;
static constexpr float PF_PRESENCE_CELL_SIZE = 15.0f;
static constexpr float PF_REPULSION_RADIUS = 15.0f;

struct BotNavigationContext {
    float objAttrX{}, objAttrY{};
    float repulsionX{}, repulsionY{};
    float presenceX{}, presenceY{};
    float enemyX{}, enemyY{};
    float wallAvoidX{}, wallAvoidY{};
    float finalDirX{}, finalDirY{};
    float finalMagnitude{};
};

struct BGPresenceCell {
    int16 cellX, cellY;
    bool operator==(BGPresenceCell const& o) const { return cellX == o.cellX && cellY == o.cellY; }
};
struct BGPresenceCellHash {
    size_t operator()(BGPresenceCell const& c) const noexcept {
        size_t h = std::hash<int16>{}(c.cellX);
        h ^= std::hash<int16>{}(c.cellY) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
struct BGPresenceGrid {
    std::unordered_map<BGPresenceCell, float, BGPresenceCellHash> cells;
    uint32 lastUpdateTime{};
};

struct BGCounterStrategyData { uint32 wins; uint32 losses; };
struct BGGroupSuccessData { uint32 successes; uint32 failures; };
struct BGTimingData { uint32 successes; uint32 failures; };
struct BGMatchupData { uint32 wins; uint32 losses; };
struct BGWinConditionData { uint32 wins; uint32 losses; };

// Match state snapshot for win condition tracking
struct BGMatchSnapshot { uint8 timeBracket; uint8 nodesHeld; uint8 scoreBracket; };

struct BGEngagementContext
{
    uint8 myClass;
    uint8 enemyClass;
    uint8 myHpPct;
    uint8 enemyHpPct;
    int8 supportDiff; // allySupportCount - enemySupportCount
};

struct BGFocusTarget
{
    ObjectGuid targetGuid;
    uint32 expiryTime;    // getMSTime() when this expires
    uint8 assignerClass;
};

// Team coordination plan structs
constexpr uint8 BG_COORD_MAX_NODES = 7;

enum BGNodeIntent : uint8
{
    BG_NODE_IGNORE  = 0,
    BG_NODE_DEFEND  = 1,
    BG_NODE_ATTACK  = 2,
    BG_NODE_URGENT  = 3,
};

struct BGNodeAssignment
{
    uint8 intent{};
    uint8 desiredCount{};
    uint8 currentCount{};
    float posX{}, posY{};
};

struct BGRallyPoint
{
    float posX{}, posY{}, posZ{};
    uint32 expiryTime{};
    uint8 minGroupSize{};
    uint8 arrivedCount{};
    uint8 targetNodeIdx{0xFF};
    bool active{};
};

struct BGTeamPlan
{
    BGNodeAssignment nodes[BG_COORD_MAX_NODES];
    uint8 activeNodeCount{};
    BGRallyPoint rally;
    uint8 flagAttackers{};
    uint8 flagDefenders{};
    uint8 fcEscorts{};
    uint32 lastEvalTime{};
    uint32 planVersion{};
};

struct BGEnemySighting { float posX, posY; uint32 lastSeenTime; uint8 enemyClass; };

enum BGIntentType : uint8 { INTENT_ATTACK_FLAG=0, INTENT_DEFEND_FLAG=1, INTENT_ESCORT_FC=2,
    INTENT_ATTACK_NODE=3, INTENT_DEFEND_NODE=4, INTENT_ROAM=5, INTENT_CHASE_FC=6, INTENT_MAX=7 };
struct BGBotIntention { uint8 intentType; uint8 targetNodeIdx; uint32 timestamp; };
struct BGTeamCooldown { uint32 expiryTime; uint8 cooldownType; };
struct BGPatrolPoint { float x, y; float engagementScore; };

// --- Phase 1: Combat Intelligence Systems ---

// Interrupt claim: one bot claims the next interrupt on a target, others hold
struct BGInterruptClaim {
    ObjectGuid claimerGuid;     // who claimed it
    ObjectGuid targetGuid;      // who is being interrupted
    uint32 claimTime{0};        // when claimed
    uint32 expiryTime{0};       // auto-expire after 4s
};

// DR categories for 3.3.5a WotLK
enum BGDRCategory : uint8 {
    DR_STUN = 0,
    DR_FEAR,
    DR_ROOT,
    DR_SILENCE,
    DR_INCAPACITATE,    // polymorph, hex, sap, gouge, repentance
    DR_DISORIENT,       // blind, scatter shot
    DR_HORROR,          // death coil, psychic horror
    DR_CYCLONE,
    DR_CHARGE,          // charge, intercept stun
    DR_NONE = 0xFF
};

// DR state per target per category
struct BGDREntry {
    uint8 stacks{0};            // 0=full, 1=half, 2=quarter, 3=immune
    uint32 lastApplicationTime{0}; // resets after 18s
};

// Burst readiness per bot
struct BGBurstReadiness {
    ObjectGuid botGuid;
    uint32 readyTime{0};        // when announced ready
    uint32 expiryTime{0};       // 8s expiry
};

// Utility-based action evaluation — bots choose between immediate needs and long-term goals
enum BGUtilityAction : uint8 {
    // WSG actions
    BG_UTIL_GRAB_ENEMY_FLAG = 0,
    BG_UTIL_DELIVER_FLAG,
    BG_UTIL_CHASE_ENEMY_FC,
    BG_UTIL_ESCORT_FRIENDLY_FC,
    BG_UTIL_DEFEND_OWN_FLAG,
    BG_UTIL_FIGHT_MIDFIELD,
    BG_UTIL_RETURN_DROPPED_FLAG,    // WSG: our flag dropped on ground, return it
    BG_UTIL_PROTECT_FC,             // WSG: peel attackers off our FC
    // AB/EY node actions (targetNode specifies which)
    BG_UTIL_ATTACK_NODE,
    BG_UTIL_DEFEND_NODE,
    BG_UTIL_REINFORCE_NODE,
    BG_UTIL_RESPOND_NODE_ATTACK,    // AB/EY: enemies near our node but not yet contested
    // EY flag
    BG_UTIL_GRAB_NEUTRAL_FLAG,
    BG_UTIL_DELIVER_NEUTRAL_FLAG,
    BG_UTIL_CHASE_NEUTRAL_FC,      // EY: enemy grabbed Netherstorm flag
    BG_UTIL_MAX
};

struct BGUtilityResult {
    BGUtilityAction action{BG_UTIL_FIGHT_MIDFIELD};
    uint8 targetNode{0xFF};
    float score{0.0f};
    Position targetPos{};
    uint8 role{1};          // 1=attack, 2=defend
    uint8 intentType{INTENT_ROAM};
};

class BotBGAIMgr
{
public:
    // Lifecycle
    static void LoadFromDB();
    static void SaveToDB();
    static void FlushPendingData();

    // Personality (pure function, deterministic from entry ID)
    static BotBGPersonality ComputePersonality(uint32 entryId);

    // Intelligence checks
    static bool IntelligenceCheck(float intelligence);
    static uint32 ComputeReactionDelay(float intelligence);

    // Utility-based action evaluation: scores all possible actions, returns the best one
    static BGUtilityResult EvaluateUtilityActions(
        Creature const* me, Battleground const* bg, BotBGPersonality const& personality,
        uint8 momentum, bool isFC, bool isHealer);

    // Waypoint mesh
    static void RecordWaypointVisit(uint32 mapId, float x, float y, float z);
    static void RecordWallHit(uint32 mapId, float x, float y);
    // Returns positions with visit counts for weighted selection
    struct LearnedWPResult { Position pos; uint32 visitCount; uint32 wallHits; };
    static std::vector<LearnedWPResult> GetLearnedWaypointsNear(uint32 mapId, float x, float y, float radius);
    static bool HasLearnedWaypoints(uint32 mapId);

    // Heatmap
    static void RecordKill(uint32 mapId, float x, float y);
    static void RecordDeath(uint32 mapId, float x, float y);
    static void RecordObjectiveCap(uint32 mapId, float x, float y);
    static void RecordObjectiveDefend(uint32 mapId, float x, float y);
    static std::optional<BGHeatmapData> GetHeatmapData(uint32 mapId, float x, float y);
    static std::optional<BGHeatmapData> GetHeatmapDataRadius(uint32 mapId, float x, float y, float radius);

    // Strategy
    static void RecordStrategyOutcome(uint32 mapId, uint32 strategyId, bool success);
    static float GetStrategyWeight(uint32 mapId, uint32 strategyId);
    static uint32 SelectStrategy(uint32 mapId, float intelligence);

    // Layer 1: Counter-strategy
    static void RecordCounterStrategyOutcome(uint32 mapId, uint32 myStrat, uint32 enemyStrat, bool won);
    static float GetCounterStrategyWeight(uint32 mapId, uint32 myStrat, uint32 enemyStrat);
    static uint32 SelectCounterStrategy(uint32 mapId, uint32 enemyDominantStrat, float intelligence);

    // Layer 2: Enemy reading (runtime only, no DB)
    static BGEnemyBehavior ClassifyEnemyBehavior(Creature const* me, Battleground const* bg, TeamId myTeamId);

    // Shared enemy behavior per BG instance (computed once by smart bots, read by all)
    static void UpdateSharedEnemyBehavior(uint32 bgInstanceId, Creature const* observer, Battleground const* bg, TeamId teamId);
    static uint8 GetSharedEnemyBehavior(uint32 bgInstanceId);
    static void ClearSharedEnemyBehavior(uint32 bgInstanceId);

    // Layer 3: Group success
    static void RecordGroupSuccess(uint32 mapId, float x, float y, uint8 groupSize, bool success);
    static float GetGroupSuccessRate(uint32 mapId, float x, float y, uint8 groupSize);

    // Layer 4: Timing
    static void RecordTimingOutcome(uint32 mapId, uint32 action, uint32 elapsedMs, bool success);
    static float GetTimingWeight(uint32 mapId, uint32 action, uint32 elapsedMs);
    static BGTimeBracket GetTimeBracket(uint32 elapsedMs);

    // Layer 5: Class matchups
    static void RecordClassMatchup(uint32 mapId, uint8 myClass, uint8 enemyClass, bool won);
    static float GetClassMatchupWeight(uint32 mapId, uint8 myClass, uint8 enemyClass);

    // Layer 6: Win conditions
    static void RecordWinConditionSnapshot(uint32 mapId, uint8 timeBracket, uint8 nodesHeld, uint8 scoreBracket, bool won);
    static float GetWinConditionProbability(uint32 mapId, uint8 timeBracket, uint8 nodesHeld, uint8 scoreBracket);
    static BGScoreBracket GetScoreBracket(uint32 myScore, uint32 enemyScore);

    // Combat engagement evaluation
    static float EvaluateEngagement(uint32 mapId, BGEngagementContext const& ctx);
    static float ComputeTerritoryFactor(uint32 mapId, TeamId teamId, float x, float y);

    // Focus fire management
    static void SetFocusTarget(uint32 bgInstanceId, TeamId teamId, ObjectGuid target, uint8 assignerClass);
    static ObjectGuid GetFocusTarget(uint32 bgInstanceId, TeamId teamId);
    static void ClearFocusTarget(uint32 bgInstanceId, TeamId teamId);
    static void CleanupExpiredFocusTargets();

    // Team coordination plans
    static std::optional<BGTeamPlan> GetTeamPlan(uint32 bgInstanceId, TeamId teamId);
    static void MaybeUpdateTeamPlan(uint32 bgInstanceId, TeamId teamId,
        Creature const* evaluator, Battleground const* bg);
    static void IncrementRallyArrived(uint32 bgInstanceId, TeamId teamId);
    static void ClearTeamPlan(uint32 bgInstanceId);

    // Key helpers
    static uint32 MakeCounterKey(uint32 mapId, uint32 myStrat, uint32 enemyStrat);
    static uint64 MakeGroupKey(uint32 mapId, float x, float y, uint8 groupSize);
    static uint32 MakeTimingKey(uint32 mapId, uint32 action, uint8 timeBracket);
    static uint32 MakeMatchupKey(uint32 mapId, uint8 myClass, uint8 enemyClass);
    static uint64 MakeWinCondKey(uint32 mapId, uint8 timeBracket, uint8 nodesHeld, uint8 scoreBracket);

    // Grid helpers
    static int16 SnapToGrid(float coord);
    static BGGridCell MakeCell(uint32 mapId, float x, float y);

private:
    static inline std::shared_mutex _lock;

    // Shared enemy behavior per BG instance (runtime only)
    static inline std::unordered_map<uint32, uint8> _sharedEnemyBehavior; // key: bg instanceId -> behavior

    // Focus fire targets per BG instance per team (runtime only)
    static inline std::unordered_map<uint64, BGFocusTarget> _focusTargets; // key: (instanceId << 1) | teamId

    // Team coordination plans per BG instance per team (runtime only)
    static inline std::unordered_map<uint64, BGTeamPlan> _teamPlans; // key: (instanceId << 1) | teamId

    // Enemy sightings per BG instance per team (runtime only, 10s TTL)
    static inline std::unordered_map<uint64, std::unordered_map<ObjectGuid, BGEnemySighting>> _enemySightings;

    // Real-time presence grid per BG instance per team (runtime only, decaying)
    static inline std::unordered_map<uint64, BGPresenceGrid> _presenceGrids;

    // Intention broadcasting per BG instance per team (runtime only)
    static inline std::unordered_map<uint64, std::unordered_map<ObjectGuid, BGBotIntention>> _teamIntentions;

    // Team cooldown communication per BG instance per team (runtime only)
    static inline std::unordered_map<uint64, std::vector<BGTeamCooldown>> _teamCooldowns;

    // Combat intelligence (Phase 1, runtime only)
    // Interrupt claims: key = (instanceId << 1) | teamId, value = per-target claims
    static inline std::unordered_map<uint64, std::unordered_map<ObjectGuid, BGInterruptClaim>> _interruptClaims;
    // DR tracking: key = instanceId, value = per-target per-category DR state
    static inline std::unordered_map<uint32, std::unordered_map<uint64, BGDREntry>> _drTracking; // inner key = targetGuid | (category << 56)
    // Burst readiness: key = (instanceId << 1) | teamId
    static inline std::unordered_map<uint64, std::vector<BGBurstReadiness>> _burstReadiness;
    // Offensive CDs on cooldown: key = (instanceId << 1) | teamId
    static inline std::unordered_map<uint64, std::vector<uint32>> _offensiveCDExpiry; // list of expiry times

    // Q-Learning table: key = (role << 24) | (stateKey << 8) | action
    static inline std::unordered_map<uint32, BGQEntry> _qTable;
    static inline uint32 _qGamesPlayed{0};
    static const BGQWeightPreset _qPresets[BG_QACTION_MAX];

    // Persistent data (loaded from/saved to DB)
    static inline std::unordered_set<uint32> _waypointMapIds; // fast mapId lookup for HasLearnedWaypoints
    static inline std::unordered_map<BGGridCell, BGLearnedWaypoint, BGGridCellHash> _waypointMesh;
    static inline std::unordered_map<BGGridCell, BGHeatmapData, BGGridCellHash> _heatmap;
    static inline std::unordered_map<uint64, BGStrategyData> _strategies; // key = (mapId << 32) | strategyId

    // Pending data (accumulated during match, flushed to DB)
    static inline std::unordered_map<BGGridCell, BGLearnedWaypoint, BGGridCellHash> _pendingWaypoints;
    static inline std::unordered_map<BGGridCell, BGHeatmapData, BGGridCellHash> _pendingHeatmap;
    static inline std::unordered_map<uint64, BGStrategyData> _pendingStrategies;

    static uint64 MakeStrategyKey(uint32 mapId, uint32 strategyId);

    // Layer 1: Counter-strategy (key: mapId<<16 | myStrat<<8 | enemyStrat)
    static inline std::unordered_map<uint32, BGCounterStrategyData> _counterStrategies;
    static inline std::unordered_map<uint32, BGCounterStrategyData> _pendingCounterStrategies;

    // Layer 3: Group size success (key: packed mapId + grid + groupSize)
    static inline std::unordered_map<uint64, BGGroupSuccessData> _groupSuccess;
    static inline std::unordered_map<uint64, BGGroupSuccessData> _pendingGroupSuccess;

    // Layer 4: Timing (key: mapId<<16 | action<<8 | timeBracket)
    static inline std::unordered_map<uint32, BGTimingData> _timing;
    static inline std::unordered_map<uint32, BGTimingData> _pendingTiming;

    // Layer 5: Class matchups (key: mapId<<16 | myClass<<8 | enemyClass)
    static inline std::unordered_map<uint32, BGMatchupData> _matchups;
    static inline std::unordered_map<uint32, BGMatchupData> _pendingMatchups;

    // Layer 6: Win conditions (key: uint64(mapId)<<24 | timeBracket<<16 | nodesHeld<<8 | scoreBracket)
    static inline std::unordered_map<uint64, BGWinConditionData> _winConditions;
    static inline std::unordered_map<uint64, BGWinConditionData> _pendingWinConditions;

public:
    struct ScoredWaypoint {
        Position pos;
        uint32 visitCount;
        float score;
    };

    // Route through learned waypoints instead of straight-line movement
    static std::optional<Position> GetNextRouteWaypoint(
        uint32 mapId, float fromX, float fromY, float toX, float toY,
        float corridorWidth = 25.0f);

    // Score a waypoint using visit count + heatmap kill/death data
    static float ScoreWaypointPosition(uint32 mapId, float x, float y, uint32 visitCount);

    // Scored route waypoint with personality influence
    static std::optional<Position> GetNextScoredRouteWaypoint(
        uint32 mapId, float fromX, float fromY, float toX, float toY,
        float aggression = 0.5f, float caution = 0.5f, float groupTendency = 0.5f,
        float corridorWidth = 25.0f, uint8 momentum = BG_MOMENTUM_EVEN,
        uint32 bgInstanceId = 0, TeamId teamId = TEAM_NEUTRAL, bool isFC = false);

    // Patrol hotspots from heatmap
    static std::vector<BGPatrolPoint> GetPatrolHotspots(
        uint32 mapId, float centerX, float centerY, float radius, uint8 maxPoints = 4);

    // Enemy sighting management
    static void RecordEnemySighting(uint32 bgInstanceId, TeamId team, ObjectGuid enemy, float x, float y, uint8 cls);
    static uint8 GetEnemyConcentration(uint32 bgInstanceId, TeamId team, float x, float y, float radius, uint32 maxAgeMs = 5000);
    static void ClearEnemySightings(uint32 bgInstanceId);

    // Potential field navigation
    static void UpdatePresenceGrid(uint32 bgInstanceId, TeamId teamId, Battleground const* bg, Creature const* observer);
    static float GetPresenceValue(uint32 bgInstanceId, TeamId teamId, float x, float y);
    static void GetPresenceAvoidanceVector(uint32 bgInstanceId, TeamId teamId, float x, float y, float& outX, float& outY);
    static void ClearPresenceGrid(uint32 bgInstanceId);

    // Intention broadcasting
    static void BroadcastIntention(uint32 bgInstanceId, TeamId teamId, ObjectGuid botGuid, uint8 intentType, uint8 targetNode = 0xFF);
    static uint8 CountIntentions(uint32 bgInstanceId, TeamId teamId, uint8 intentType, uint8 targetNode = 0xFF);
    static void ClearIntentions(uint32 bgInstanceId);

    // Cooldown communication
    static void BroadcastCooldown(uint32 bgInstanceId, TeamId teamId, uint32 durationMs, uint8 type = 0);
    static bool HasActiveTeamCooldown(uint32 bgInstanceId, TeamId teamId);
    static void ClearCooldowns(uint32 bgInstanceId);

    // --- Combat Intelligence (Phase 1) ---

    // Interrupt coordination: claim system prevents double-kicks
    static bool ClaimInterrupt(uint32 bgInstanceId, TeamId teamId, ObjectGuid claimerGuid, ObjectGuid targetGuid);
    static bool HasInterruptClaim(uint32 bgInstanceId, TeamId teamId, ObjectGuid targetGuid, ObjectGuid excludeBot = ObjectGuid::Empty);
    static void ClearInterruptClaims(uint32 bgInstanceId);

    // DR tracking: per-target per-category diminishing returns
    static void RecordCCApplication(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category);
    static uint8 GetDRStacks(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category);
    static float GetDRMultiplier(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category);
    static bool IsTargetDRImmune(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category);
    static void ClearDRTracking(uint32 bgInstanceId);

    // Burst coordination: team burst windows
    static void AnnounceBurstReady(uint32 bgInstanceId, TeamId teamId, ObjectGuid botGuid);
    static uint8 CountBurstReady(uint32 bgInstanceId, TeamId teamId);
    static void ClearBurstReadiness(uint32 bgInstanceId);

    // Cooldown-aware aggression: track team offensive CD availability
    static void RecordOffensiveCDUsed(uint32 bgInstanceId, TeamId teamId, uint32 cooldownDurationMs);
    static float GetTeamBurstAvailability(uint32 bgInstanceId, TeamId teamId); // 0-1, 1 = all CDs ready
    static void ClearOffensiveCDs(uint32 bgInstanceId);

    // Compute intelligence-based interrupt reaction delay (milliseconds)
    static uint32 ComputeInterruptDelay(float intelligence);

    // BG heal triage: score a potential heal target by role importance + health deficit
    // Returns priority score (higher = heal first). Intelligence gates triage quality.
    static float ComputeHealPriority(Unit const* healer, Unit const* target, Battleground const* bg,
        TeamId healerTeamId, float intelligence);

    // Q-Learning
    static void LoadQTableFromDB();
    static void FlushQTableToDB();
    static uint16 ComputeQStateKey(uint32 mapId, uint8 momentum, uint8 scoreBracket, uint8 timeBracket, uint8 bgContext = 0);
    static uint8 DetermineQRole(bool isFC, bool isHealer, uint8 assignedRole);
    static uint8 SelectQAction(uint8 role, uint16 stateKey, float intelligence, uint8 personalityBucket = 0);
    static BGQWeightPreset const& GetQWeightPreset(uint8 action);
    static BGQWeightPreset GetQWeightPresetWithOffsets(uint8 role, uint16 stateKey, uint8 action, uint8 personalityBucket);
    static void UpdateQValues(std::vector<BGQEpisodeStep> const& episode, float reward);
    static uint32 GetQGamesPlayed() { return _qGamesPlayed; }
    static void IncrementQGamesPlayed() { ++_qGamesPlayed; }
    static void ComputeAllyRepulsion(Creature const* me, Battleground const* bg, TeamId teamId, float& outX, float& outY, bool isFC = false, bool isHealer = false);
    static void ComputeEnemyVector(uint32 bgInstanceId, TeamId teamId, float x, float y, float aggression, bool isFC, float& outX, float& outY);
    static BotNavigationContext ComputePotentialField(Creature const* me, Battleground const* bg, Position const& objectivePos, BotBGPersonality const& personality, uint8 momentum, bool isFC, bool isHealer = false, BGQWeightPreset const* qPreset = nullptr);
};

#endif // BOT_BG_AI_H
