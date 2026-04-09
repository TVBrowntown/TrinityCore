#include "bot_bg_ai.h"
#include "bot_ai.h"
#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Random.h"
#include "Timer.h"
#include <algorithm>
#include <cmath>

BotBGPersonality BotBGAIMgr::ComputePersonality(uint32 entryId)
{
    auto rawTrait = [entryId](uint32 seed) -> float {
        uint32 h = entryId * 2654435761u + seed * 2246822519u;
        h ^= (h >> 16);
        h *= 0x45d9f3b;
        h ^= (h >> 16);
        return float(h % 101) / 100.0f;
    };
    // Average two raw values for triangular distribution (clusters around 0.5)
    auto trait = [&rawTrait](uint32 seed) -> float {
        return (rawTrait(seed) + rawTrait(seed + 0x1000)) * 0.5f;
    };
    return BotBGPersonality{
        .aggression     = trait(0xA001),
        .caution        = trait(0xB002),
        .objectiveFocus = trait(0xC003),
        .groupTendency  = trait(0xD004),
        .intelligence   = trait(0xE005)
    };
}

bool BotBGAIMgr::IntelligenceCheck(float intelligence)
{
    float effectiveChance = std::max(0.15f, intelligence * 0.95f); // floor at 15%
    return frand(0.0f, 1.0f) < effectiveChance;
}

uint32 BotBGAIMgr::ComputeReactionDelay(float intelligence)
{
    uint32 base = uint32((1.0f - intelligence) * 3000.0f + 500.0f);
    int32 jitter = int32(base) / 4;
    return uint32(std::max(int32(200), int32(base) + irand(-jitter, jitter)));
}

int16 BotBGAIMgr::SnapToGrid(float coord)
{
    return int16(std::floor(coord / 3.0f));
}

BGGridCell BotBGAIMgr::MakeCell(uint32 mapId, float x, float y)
{
    return BGGridCell{ mapId, SnapToGrid(x), SnapToGrid(y) };
}

uint64 BotBGAIMgr::MakeStrategyKey(uint32 mapId, uint32 strategyId)
{
    return (uint64(mapId) << 32) | uint64(strategyId);
}

// --- Waypoint mesh ---

void BotBGAIMgr::RecordWaypointVisit(uint32 mapId, float x, float y, float z)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::unique_lock lock(_lock);
    auto& wp = _pendingWaypoints[cell];
    // Running average for Z
    if (wp.visitCount == 0)
        wp.z = z;
    else
        wp.z = (wp.z * wp.visitCount + z) / (wp.visitCount + 1);
    wp.visitCount++;
}

void BotBGAIMgr::RecordWallHit(uint32 mapId, float x, float y)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::unique_lock lock(_lock);
    _pendingWaypoints[cell].wallHits++;
    // Also update live mesh for immediate avoidance
    _waypointMesh[cell].wallHits++;
}

std::vector<BotBGAIMgr::LearnedWPResult> BotBGAIMgr::GetLearnedWaypointsNear(uint32 mapId, float x, float y, float radius)
{
    std::vector<LearnedWPResult> result;
    int16 gridRadius = int16(std::ceil(radius / 3.0f));
    int16 centerX = SnapToGrid(x);
    int16 centerY = SnapToGrid(y);
    float radiusSq = radius * radius;

    std::shared_lock lock(_lock);
    for (int16 gx = centerX - gridRadius; gx <= centerX + gridRadius; ++gx)
    {
        for (int16 gy = centerY - gridRadius; gy <= centerY + gridRadius; ++gy)
        {
            BGGridCell cell{ mapId, gx, gy };
            auto it = _waypointMesh.find(cell);
            if (it != _waypointMesh.end())
            {
                float wx = float(gx) * 3.0f + 1.5f;
                float wy = float(gy) * 3.0f + 1.5f;
                float dx = wx - x, dy = wy - y;
                if (dx * dx + dy * dy <= radiusSq)
                    result.push_back(LearnedWPResult{ Position(wx, wy, it->second.z), it->second.visitCount, it->second.wallHits });
            }
        }
    }
    return result;
}

bool BotBGAIMgr::HasLearnedWaypoints(uint32 mapId)
{
    std::shared_lock lock(_lock);
    if (_waypointMapIds.contains(mapId))
        return true;
    // Also check pending waypoints (from current match, not yet flushed)
    for (auto const& [cell, _] : _pendingWaypoints)
        if (cell.mapId == mapId) return true;
    return false;
}

// --- Heatmap ---

void BotBGAIMgr::RecordKill(uint32 mapId, float x, float y)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::unique_lock lock(_lock);
    _pendingHeatmap[cell].kills++;
}

void BotBGAIMgr::RecordDeath(uint32 mapId, float x, float y)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::unique_lock lock(_lock);
    _pendingHeatmap[cell].deaths++;
}

void BotBGAIMgr::RecordObjectiveCap(uint32 mapId, float x, float y)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::unique_lock lock(_lock);
    _pendingHeatmap[cell].objCaps++;
}

void BotBGAIMgr::RecordObjectiveDefend(uint32 mapId, float x, float y)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::unique_lock lock(_lock);
    _pendingHeatmap[cell].objDefends++;
}

std::optional<BGHeatmapData> BotBGAIMgr::GetHeatmapData(uint32 mapId, float x, float y)
{
    BGGridCell cell = MakeCell(mapId, x, y);
    std::shared_lock lock(_lock);
    auto it = _heatmap.find(cell);
    if (it != _heatmap.end())
        return it->second;
    return std::nullopt;
}

std::optional<BGHeatmapData> BotBGAIMgr::GetHeatmapDataRadius(uint32 mapId, float x, float y, float radius)
{
    int16 gridRadius = int16(std::ceil(radius / 3.0f));
    int16 centerX = SnapToGrid(x);
    int16 centerY = SnapToGrid(y);
    float radiusSq = radius * radius;

    BGHeatmapData result{0, 0, 0, 0};
    bool found = false;

    std::shared_lock lock(_lock);
    for (int16 gx = centerX - gridRadius; gx <= centerX + gridRadius; ++gx)
    {
        for (int16 gy = centerY - gridRadius; gy <= centerY + gridRadius; ++gy)
        {
            float wx = float(gx) * 3.0f + 1.5f;
            float wy = float(gy) * 3.0f + 1.5f;
            float dx = wx - x, dy = wy - y;
            if (dx * dx + dy * dy > radiusSq)
                continue;

            BGGridCell cell{ mapId, gx, gy };
            auto it = _heatmap.find(cell);
            if (it != _heatmap.end())
            {
                result.kills += it->second.kills;
                result.deaths += it->second.deaths;
                result.objCaps += it->second.objCaps;
                result.objDefends += it->second.objDefends;
                found = true;
            }
        }
    }
    return found ? std::optional(result) : std::nullopt;
}

// --- Strategy ---

void BotBGAIMgr::RecordStrategyOutcome(uint32 mapId, uint32 strategyId, bool success)
{
    uint64 key = MakeStrategyKey(mapId, strategyId);
    std::unique_lock lock(_lock);
    auto& data = _pendingStrategies[key];
    if (success) data.successCount++; else data.failCount++;
}

float BotBGAIMgr::GetStrategyWeight(uint32 mapId, uint32 strategyId)
{
    uint64 key = MakeStrategyKey(mapId, strategyId);
    std::shared_lock lock(_lock);
    auto it = _strategies.find(key);
    // Beta prior with alpha=2, beta=2 (4 pseudo-observations at 50%)
    uint32 successes = 2, failures = 2;
    if (it != _strategies.end())
    {
        successes += it->second.successCount;
        failures += it->second.failCount;
    }
    return float(successes) / float(successes + failures);
}

uint32 BotBGAIMgr::SelectStrategy(uint32 mapId, float intelligence)
{
    if (!IntelligenceCheck(intelligence))
        return urand(0, BG_STRATEGY_MAX - 1); // dumb: random

    // Smart: weighted selection based on learned outcomes
    float weights[BG_STRATEGY_MAX];
    float totalWeight = 0.0f;
    for (uint32 i = 0; i < BG_STRATEGY_MAX; ++i)
    {
        weights[i] = GetStrategyWeight(mapId, i);
        totalWeight += weights[i];
    }
    if (totalWeight <= 0.0f)
        return urand(0, BG_STRATEGY_MAX - 1);

    float roll = frand(0.0f, totalWeight);
    float cumulative = 0.0f;
    for (uint32 i = 0; i < BG_STRATEGY_MAX; ++i)
    {
        cumulative += weights[i];
        if (roll <= cumulative)
            return i;
    }
    return 0;
}

// --- Phase 1: Combat Intelligence Systems ---

// Helper: make team key from instance + team
static uint64 MakeCombatKey(uint32 bgInstanceId, TeamId teamId)
{
    return (uint64(bgInstanceId) << 1) | uint64(teamId);
}

// Helper: make DR key from target GUID counter + category
// Uses low 32 bits of GUID (unique per creature) combined with category
static uint64 MakeDRKey(ObjectGuid targetGuid, BGDRCategory category)
{
    return (uint64(targetGuid.GetCounter()) << 8) | uint64(category);
}

// --- 1.1 Interrupt Claim System ---

bool BotBGAIMgr::ClaimInterrupt(uint32 bgInstanceId, TeamId teamId, ObjectGuid claimerGuid, ObjectGuid targetGuid)
{
    uint64 key = MakeCombatKey(bgInstanceId, teamId);
    uint32 now = getMSTime();
    std::unique_lock lock(_lock);

    auto& claims = _interruptClaims[key];

    // Prune expired claims
    for (auto it = claims.begin(); it != claims.end();)
    {
        if (now > it->second.expiryTime)
            it = claims.erase(it);
        else
            ++it;
    }

    // Check if someone else already claimed this target
    auto it = claims.find(targetGuid);
    if (it != claims.end() && it->second.claimerGuid != claimerGuid)
        return false; // someone else has it

    // Claim it
    claims[targetGuid] = BGInterruptClaim{ claimerGuid, targetGuid, now, now + 4000 };
    return true;
}

bool BotBGAIMgr::HasInterruptClaim(uint32 bgInstanceId, TeamId teamId, ObjectGuid targetGuid, ObjectGuid excludeBot)
{
    uint64 key = MakeCombatKey(bgInstanceId, teamId);
    uint32 now = getMSTime();
    std::shared_lock lock(_lock);

    auto mapIt = _interruptClaims.find(key);
    if (mapIt == _interruptClaims.end()) return false;

    auto it = mapIt->second.find(targetGuid);
    if (it == mapIt->second.end()) return false;
    if (now > it->second.expiryTime) return false; // expired
    if (!excludeBot.IsEmpty() && it->second.claimerGuid == excludeBot) return false; // our own claim
    return true;
}

void BotBGAIMgr::ClearInterruptClaims(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    // Clear both teams
    _interruptClaims.erase(MakeCombatKey(bgInstanceId, TEAM_ALLIANCE));
    _interruptClaims.erase(MakeCombatKey(bgInstanceId, TEAM_HORDE));
}

// --- 1.2 Diminishing Returns Tracker ---

void BotBGAIMgr::RecordCCApplication(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category)
{
    if (category == DR_NONE) return;
    uint64 drKey = MakeDRKey(targetGuid, category);
    uint32 now = getMSTime();
    std::unique_lock lock(_lock);

    auto& entry = _drTracking[bgInstanceId][drKey];
    // DR resets after 18 seconds of no applications
    if (now - entry.lastApplicationTime > 18000)
        entry.stacks = 0;

    entry.lastApplicationTime = now;
    if (entry.stacks < 3)
        entry.stacks++;
}

uint8 BotBGAIMgr::GetDRStacks(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category)
{
    if (category == DR_NONE) return 0;
    uint64 drKey = MakeDRKey(targetGuid, category);
    uint32 now = getMSTime();
    std::shared_lock lock(_lock);

    auto instIt = _drTracking.find(bgInstanceId);
    if (instIt == _drTracking.end()) return 0;
    auto it = instIt->second.find(drKey);
    if (it == instIt->second.end()) return 0;
    // DR resets after 18s
    if (now - it->second.lastApplicationTime > 18000) return 0;
    return it->second.stacks;
}

float BotBGAIMgr::GetDRMultiplier(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category)
{
    uint8 stacks = GetDRStacks(bgInstanceId, targetGuid, category);
    switch (stacks)
    {
        case 0: return 1.0f;    // full duration
        case 1: return 0.5f;    // half
        case 2: return 0.25f;   // quarter
        default: return 0.0f;   // immune
    }
}

bool BotBGAIMgr::IsTargetDRImmune(uint32 bgInstanceId, ObjectGuid targetGuid, BGDRCategory category)
{
    return GetDRStacks(bgInstanceId, targetGuid, category) >= 3;
}

void BotBGAIMgr::ClearDRTracking(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _drTracking.erase(bgInstanceId);
}

// --- 1.3 Burst Coordination ---

void BotBGAIMgr::AnnounceBurstReady(uint32 bgInstanceId, TeamId teamId, ObjectGuid botGuid)
{
    uint64 key = MakeCombatKey(bgInstanceId, teamId);
    uint32 now = getMSTime();
    std::unique_lock lock(_lock);

    auto& readyList = _burstReadiness[key];

    // Prune expired entries
    std::erase_if(readyList, [now](BGBurstReadiness const& r) { return now > r.expiryTime; });

    // Update or add
    for (auto& r : readyList)
    {
        if (r.botGuid == botGuid)
        {
            r.readyTime = now;
            r.expiryTime = now + 8000;
            return;
        }
    }
    readyList.push_back(BGBurstReadiness{ botGuid, now, now + 8000 });
}

uint8 BotBGAIMgr::CountBurstReady(uint32 bgInstanceId, TeamId teamId)
{
    uint64 key = MakeCombatKey(bgInstanceId, teamId);
    uint32 now = getMSTime();
    std::shared_lock lock(_lock);

    auto it = _burstReadiness.find(key);
    if (it == _burstReadiness.end()) return 0;

    uint8 count = 0;
    for (auto const& r : it->second)
        if (now <= r.expiryTime) ++count;
    return count;
}

void BotBGAIMgr::ClearBurstReadiness(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _burstReadiness.erase(MakeCombatKey(bgInstanceId, TEAM_ALLIANCE));
    _burstReadiness.erase(MakeCombatKey(bgInstanceId, TEAM_HORDE));
}

// --- 1.4 Cooldown-Aware Aggression ---

void BotBGAIMgr::RecordOffensiveCDUsed(uint32 bgInstanceId, TeamId teamId, uint32 cooldownDurationMs)
{
    uint64 key = MakeCombatKey(bgInstanceId, teamId);
    uint32 now = getMSTime();
    std::unique_lock lock(_lock);

    auto& cdList = _offensiveCDExpiry[key];
    // Prune expired
    std::erase_if(cdList, [now](uint32 expiry) { return now > expiry; });
    cdList.push_back(now + cooldownDurationMs);
}

float BotBGAIMgr::GetTeamBurstAvailability(uint32 bgInstanceId, TeamId teamId)
{
    uint64 key = MakeCombatKey(bgInstanceId, teamId);
    uint32 now = getMSTime();
    std::shared_lock lock(_lock);

    auto it = _offensiveCDExpiry.find(key);
    if (it == _offensiveCDExpiry.end()) return 1.0f; // no CDs tracked = assume all ready

    uint8 total = uint8(it->second.size());
    if (total == 0) return 1.0f;

    uint8 onCD = 0;
    for (uint32 expiry : it->second)
        if (now < expiry) ++onCD;

    return 1.0f - float(onCD) / float(total);
}

void BotBGAIMgr::ClearOffensiveCDs(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _offensiveCDExpiry.erase(MakeCombatKey(bgInstanceId, TEAM_ALLIANCE));
    _offensiveCDExpiry.erase(MakeCombatKey(bgInstanceId, TEAM_HORDE));
}

// --- Intelligence-Gated Interrupt Delay ---

uint32 BotBGAIMgr::ComputeInterruptDelay(float intelligence)
{
    // Smart bots react fast, dumb bots react slow (or not at all)
    // Returns milliseconds to wait before attempting interrupt
    if (intelligence < 0.3f) return 99999; // effectively never (gated elsewhere too)
    if (intelligence < 0.5f) return urand(1500, 2500);
    if (intelligence < 0.7f) return urand(800, 1200);
    if (intelligence < 0.85f) return urand(300, 800);
    return urand(150, 400); // top tier: near-instant
}

// --- BG Heal Triage ---

float BotBGAIMgr::ComputeHealPriority(Unit const* healer, Unit const* target, Battleground const* bg,
    TeamId healerTeamId, float intelligence)
{
    if (!healer || !target || !bg || !target->IsAlive())
        return 0.0f;

    float hpPct = float(target->GetHealth()) / float(std::max(target->GetMaxHealth(), 1u)) * 100.0f;

    // Dumb healers: priority = pure health deficit (lowest HP wins)
    if (intelligence < 0.3f)
        return 100.0f - hpPct;

    // Role weight: FC > healer > DPS
    float roleWeight = 1.0f;
    bool targetIsFC = false;
    bool targetIsHealer = false;

    if (target->IsNPCBot())
    {
        bot_ai const* targetAI = target->ToCreature()->GetBotAI();
        if (targetAI)
        {
            targetIsHealer = targetAI->HasRole(BOT_ROLE_HEAL);
            // Check if this bot is carrying a flag
            if (bg->GetTypeID() == BATTLEGROUND_WS)
            {
                ObjectGuid fcGuid = bg->GetFlagPickerGUID(bg->GetOtherTeamId(healerTeamId));
                if (fcGuid == target->GetGUID()) targetIsFC = true;
            }
            else if (bg->GetTypeID() == BATTLEGROUND_EY)
            {
                BattlegroundEY const* ey = dynamic_cast<BattlegroundEY const*>(bg);
                if (ey && ey->GetFlagPickerGUID() == target->GetGUID()) targetIsFC = true;
            }
        }
    }

    if (targetIsFC) roleWeight = 3.0f;
    else if (targetIsHealer) roleWeight = 2.0f;

    // Health deficit score (0 = full, 100 = dead)
    float deficit = 100.0f - hpPct;

    // Incoming damage factor: target in combat with multiple attackers = higher priority
    float incomingFactor = 1.0f;
    if (intelligence >= 0.5f && target->IsInCombat())
    {
        uint8 attackerCount = 0;
        for (auto const& [_, ref] : target->GetThreatManager().GetThreatenedByMeList())
            (void)ref, ++attackerCount;
        // Simple proxy: count attackers targeting this unit
        if (target->GetVictim())
        {
            for (auto const& attacker : target->getAttackers())
                if (attacker && attacker->IsAlive()) ++attackerCount;
        }
        incomingFactor = 1.0f + float(attackerCount) * 0.15f;
    }

    // Distance penalty: prefer closer targets (minor factor)
    float dist = healer->GetExactDist2d(target);
    float distPenalty = dist > 30.0f ? 0.8f : 1.0f;

    // Moderate intelligence: role + deficit
    if (intelligence < 0.5f)
        return deficit * roleWeight * distPenalty;

    // Smart: role + deficit + incoming damage
    return deficit * roleWeight * incomingFactor * distPenalty;
}

// --- Utility-Based Action Evaluation ---

BGUtilityResult BotBGAIMgr::EvaluateUtilityActions(
    Creature const* me, Battleground const* bg, BotBGPersonality const& p,
    uint8 momentum, bool isFC, bool isHealer)
{
    if (!me || !bg) return {};

    TeamId myTeamId = bg->GetBotTeamId(me->GetGUID());
    uint32 myTeamVal = myTeamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    TeamId enemyTeamId = myTeamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    uint32 bgInstId = bg->GetInstanceID();
    float myX = me->GetPositionX(), myY = me->GetPositionY();

    // Per-bot deterministic scatter (consistent per bot, prevents stacking on exact positions)
    uint32 scatter = me->GetEntry() * 2654435761u;
    float scatterAngle = float(scatter % 628) / 100.0f;
    float scatterDist = 5.0f + float(scatter % 1000) / 100.0f;

    // Momentum multipliers for attack vs defend preference
    float attackBias = 0.0f, defendBias = 0.0f;
    if (momentum >= BG_MOMENTUM_DOMINATING) attackBias = 0.15f;
    else if (momentum >= BG_MOMENTUM_ADVANTAGE) attackBias = 0.08f;
    else if (momentum <= BG_MOMENTUM_WIPED) { defendBias = 0.15f; attackBias = -0.1f; }
    else if (momentum <= BG_MOMENTUM_OUTNUMBERED) { defendBias = 0.08f; attackBias = -0.05f; }

    bool isOpeningRush = bg->GetStartTime() < 210000;

    std::vector<BGUtilityResult> candidates;

    auto addCandidate = [&](BGUtilityAction action, float score, Position pos, uint8 role,
                            uint8 intent, uint8 node = 0xFF) {
        if (score <= 0.0f) return;
        score = std::min(score, 1.0f);
        // Apply scatter to prevent stacking on exact position
        pos.m_positionX += scatterDist * std::cos(scatterAngle);
        pos.m_positionY += scatterDist * std::sin(scatterAngle);
        candidates.push_back(BGUtilityResult{ action, node, score, pos, role, intent });
    };

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_WS:
        {
            // WSG flag positions
            static constexpr float FLAG_A_X = 1540.42f, FLAG_A_Y = 1481.33f, FLAG_A_Z = 351.83f;
            static constexpr float FLAG_H_X = 916.02f, FLAG_H_Y = 1434.41f, FLAG_H_Z = 345.41f;
            static constexpr float MID_X = 1228.0f, MID_Y = 1458.0f, MID_Z = 340.0f;

            float myFlagX = myTeamId == TEAM_ALLIANCE ? FLAG_A_X : FLAG_H_X;
            float myFlagY = myTeamId == TEAM_ALLIANCE ? FLAG_A_Y : FLAG_H_Y;
            float myFlagZ = myTeamId == TEAM_ALLIANCE ? FLAG_A_Z : FLAG_H_Z;
            float enemyFlagX = myTeamId == TEAM_ALLIANCE ? FLAG_H_X : FLAG_A_X;
            float enemyFlagY = myTeamId == TEAM_ALLIANCE ? FLAG_H_Y : FLAG_A_Y;
            float enemyFlagZ = myTeamId == TEAM_ALLIANCE ? FLAG_H_Z : FLAG_A_Z;

            ObjectGuid enemyFCGuid = bg->GetFlagPickerGUID(myTeamId); // enemy carrying OUR flag
            ObjectGuid friendlyFCGuid = bg->GetFlagPickerGUID(enemyTeamId); // our bot carrying THEIR flag
            bool enemyHasOurFlag = !enemyFCGuid.IsEmpty();
            bool weHaveTheirFlag = !friendlyFCGuid.IsEmpty();

            // --- DELIVER FLAG (FC only — always top priority) ---
            if (isFC)
            {
                addCandidate(BG_UTIL_DELIVER_FLAG, 1.0f,
                    Position(myFlagX, myFlagY, myFlagZ), 1, INTENT_ATTACK_FLAG);
                break; // FC does nothing else
            }

            // --- GRAB ENEMY FLAG ---
            if (!weHaveTheirFlag) // flag is at base or on ground
            {
                float dist = std::sqrt((enemyFlagX-myX)*(enemyFlagX-myX) + (enemyFlagY-myY)*(enemyFlagY-myY));
                float distFactor = std::max(0.0f, 1.0f - dist / 800.0f);
                float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ATTACK_FLAG) * 0.15f;
                float score = (0.75f + attackBias + p.aggression * 0.15f + p.objectiveFocus * 0.1f)
                    * distFactor - stacking;
                if (isOpeningRush) score += 0.1f;
                addCandidate(BG_UTIL_GRAB_ENEMY_FLAG, score,
                    Position(enemyFlagX, enemyFlagY, enemyFlagZ), 1, INTENT_ATTACK_FLAG);
            }

            // --- CHASE ENEMY FC (immediate — enemy has our flag) ---
            if (enemyHasOurFlag)
            {
                Position fcPos(enemyFlagX, enemyFlagY, enemyFlagZ); // fallback
                Unit* enemyFC = ObjectAccessor::GetUnit(*me, enemyFCGuid);
                if (enemyFC && enemyFC->IsAlive())
                    fcPos.Relocate(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ());

                float dist = me->GetExactDist2d(fcPos);
                float distFactor = std::max(0.0f, 1.0f - dist / 800.0f);
                float stacking = CountIntentions(bgInstId, myTeamId, INTENT_CHASE_FC) * 0.20f;
                float score = (0.85f + p.aggression * 0.1f) * distFactor - stacking;
                addCandidate(BG_UTIL_CHASE_ENEMY_FC, score, fcPos, 1, INTENT_CHASE_FC);
            }

            // --- ESCORT FRIENDLY FC ---
            if (weHaveTheirFlag)
            {
                Unit* friendlyFC = ObjectAccessor::GetUnit(*me, friendlyFCGuid);
                Position fcPos(myFlagX, myFlagY, myFlagZ);
                if (friendlyFC && friendlyFC->IsAlive())
                    fcPos.Relocate(friendlyFC->GetPositionX(), friendlyFC->GetPositionY(), friendlyFC->GetPositionZ());

                float dist = me->GetExactDist2d(fcPos);
                float distFactor = std::max(0.0f, 1.0f - dist / 400.0f);
                float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ESCORT_FC) * 0.2f;
                float score = (0.55f + p.groupTendency * 0.15f + p.caution * 0.1f) * distFactor - stacking;
                // More escorts needed if enemy has our flag too (both flags out)
                if (enemyHasOurFlag) score += 0.1f;
                addCandidate(BG_UTIL_ESCORT_FRIENDLY_FC, score, fcPos, 1, INTENT_ESCORT_FC);
            }

            // --- RETURN DROPPED FLAG (immediate — our flag is on the ground) ---
            {
                BattlegroundWS* ws = const_cast<BattlegroundWS*>(dynamic_cast<BattlegroundWS const*>(bg));
                if (ws)
                {
                    uint32 myTeam = myTeamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
                    uint8 myFlagState = ws->GetFlagState(myTeam);
                    if (myFlagState == BG_WS_FLAG_STATE_ON_GROUND)
                    {
                        // Our flag is dropped — returning it is very urgent
                        ObjectGuid droppedGuid = ws->GetDroppedFlagGUID(myTeam);
                        GameObject const* droppedFlag = !droppedGuid.IsEmpty()
                            ? ObjectAccessor::GetGameObject(*me, droppedGuid) : nullptr;
                        Position flagPos(myFlagX, myFlagY, myFlagZ);
                        if (droppedFlag)
                            flagPos.Relocate(droppedFlag->GetPositionX(), droppedFlag->GetPositionY(), droppedFlag->GetPositionZ());

                        float dist = me->GetExactDist2d(flagPos);
                        float distFactor = std::max(0.0f, 1.0f - dist / 600.0f);
                        float score = 0.92f * distFactor; // very high — almost as urgent as FC delivering
                        addCandidate(BG_UTIL_RETURN_DROPPED_FLAG, score, flagPos, 1, INTENT_DEFEND_FLAG);
                    }
                }
            }

            // --- PROTECT FC (immediate — our FC is under attack) ---
            if (weHaveTheirFlag)
            {
                Unit* friendlyFC2 = ObjectAccessor::GetUnit(*me, friendlyFCGuid);
                if (friendlyFC2 && friendlyFC2->IsAlive() && friendlyFC2->IsInCombat())
                {
                    float dist = me->GetExactDist2d(friendlyFC2);
                    float distFactor = std::max(0.0f, 1.0f - dist / 300.0f);
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ESCORT_FC) * 0.2f;
                    float score = (0.80f + p.groupTendency * 0.1f) * distFactor - stacking;
                    addCandidate(BG_UTIL_PROTECT_FC, score,
                        Position(friendlyFC2->GetPositionX(), friendlyFC2->GetPositionY(), friendlyFC2->GetPositionZ()),
                        1, INTENT_ESCORT_FC);
                }
            }

            // --- DEFEND OWN FLAG (only if flag is actually at base) ---
            if (!enemyHasOurFlag)
            {
                float stacking = CountIntentions(bgInstId, myTeamId, INTENT_DEFEND_FLAG) * 0.15f;
                float score = 0.45f + defendBias + p.caution * 0.2f + p.objectiveFocus * 0.1f - stacking;
                // If we have their flag, defending ours is more important (need both for cap)
                if (weHaveTheirFlag) score += 0.25f;
                addCandidate(BG_UTIL_DEFEND_OWN_FLAG, score,
                    Position(myFlagX, myFlagY, myFlagZ), 2, INTENT_DEFEND_FLAG);
            }

            // --- FIGHT MIDFIELD (always available, low priority) ---
            {
                float score = 0.25f + p.aggression * 0.2f - p.objectiveFocus * 0.15f;
                if (isOpeningRush) score += 0.15f;
                addCandidate(BG_UTIL_FIGHT_MIDFIELD, score,
                    Position(MID_X, MID_Y, MID_Z), 1, INTENT_ROAM);
            }
            break;
        }
        case BATTLEGROUND_AB:
        {
            BattlegroundAB const* ab = dynamic_cast<BattlegroundAB const*>(bg);
            if (!ab) break;

            // Count how many nodes we hold
            uint8 nodesHeld = 0;
            for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
                if (ab->IsNodeOccupied(n, myTeamId)) ++nodesHeld;

            // Strategic need: we need at least 3 nodes to win
            float strategicNeed = (nodesHeld < 3) ? 0.2f : 0.0f;

            for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
            {
                float nodeX = BG_AB_NodePositions[n].GetPositionX();
                float nodeY = BG_AB_NodePositions[n].GetPositionY();
                float nodeZ = BG_AB_NodePositions[n].GetPositionZ();
                float dist = std::sqrt((nodeX-myX)*(nodeX-myX) + (nodeY-myY)*(nodeY-myY));
                float distFactor = std::max(0.0f, 1.0f - dist / 600.0f);

                bool ours = ab->IsNodeOccupied(n, myTeamId);
                bool theirs = ab->IsNodeOccupied(n, enemyTeamId);
                bool contested = ab->IsNodeContested(n, myTeamId) || ab->IsNodeContested(n, enemyTeamId);

                if (!ours)
                {
                    // ATTACK NODE
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ATTACK_NODE, n) * 0.12f;
                    float base = theirs ? 0.6f : 0.7f; // neutral > enemy (easier to cap)
                    float score = (base + attackBias + strategicNeed + p.aggression * 0.12f)
                        * distFactor - stacking;
                    if (isOpeningRush) score += 0.1f;
                    addCandidate(BG_UTIL_ATTACK_NODE, score,
                        Position(nodeX, nodeY, nodeZ), 1, INTENT_ATTACK_NODE, n);
                }

                if (ours)
                {
                    // DEFEND NODE
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_DEFEND_NODE, n) * 0.15f;
                    float base = 0.4f;
                    float score = (base + defendBias + p.caution * 0.15f + p.groupTendency * 0.1f) * distFactor - stacking;
                    // If barely holding 3, defense is more important
                    if (nodesHeld <= 3) score += 0.12f;
                    addCandidate(BG_UTIL_DEFEND_NODE, score,
                        Position(nodeX, nodeY, nodeZ), 2, INTENT_DEFEND_NODE, n);
                }

                if (contested)
                {
                    // REINFORCE contested node (immediate need)
                    uint8 alliesNear = CountIntentions(bgInstId, myTeamId, INTENT_ATTACK_NODE, n)
                        + CountIntentions(bgInstId, myTeamId, INTENT_DEFEND_NODE, n);
                    if (alliesNear < 4) // don't over-stack
                    {
                        float score = (0.8f + p.aggression * 0.1f + p.groupTendency * 0.1f) * distFactor;
                        addCandidate(BG_UTIL_REINFORCE_NODE, score,
                            Position(nodeX, nodeY, nodeZ), 1, INTENT_ATTACK_NODE, n);
                    }
                }

                // RESPOND TO NODE ATTACK: our node not yet contested but enemies nearby
                if (ours && !contested)
                {
                    // Count enemy bots near this node
                    uint8 enemiesNear = 0;
                    uint32 enemyTeamVal = myTeamVal == ALLIANCE ? HORDE : ALLIANCE;
                    for (auto const& [guid, botData] : bg->GetBots())
                    {
                        if (botData.Team != enemyTeamVal) continue;
                        Creature const* enemy = ObjectAccessor::GetCreature(*me, guid);
                        if (!enemy || !enemy->IsAlive()) continue;
                        float eDist = std::sqrt(
                            (enemy->GetPositionX()-nodeX)*(enemy->GetPositionX()-nodeX) +
                            (enemy->GetPositionY()-nodeY)*(enemy->GetPositionY()-nodeY));
                        if (eDist < 40.0f) ++enemiesNear;
                    }
                    if (enemiesNear > 0)
                    {
                        float score = (0.72f + float(enemiesNear) * 0.05f) * distFactor;
                        addCandidate(BG_UTIL_RESPOND_NODE_ATTACK, score,
                            Position(nodeX, nodeY, nodeZ), 2, INTENT_DEFEND_NODE, n);
                    }
                }
            }

            // FIGHT MIDFIELD (roaming)
            {
                float score = 0.2f + p.aggression * 0.15f - p.objectiveFocus * 0.1f;
                addCandidate(BG_UTIL_FIGHT_MIDFIELD, score,
                    Position(1185.0f, 1184.0f, -56.0f), 1, INTENT_ROAM); // AB center
            }
            break;
        }
        case BATTLEGROUND_EY:
        {
            BattlegroundEY const* ey = dynamic_cast<BattlegroundEY const*>(bg);
            if (!ey) break;

            uint8 pointsHeld = 0;
            for (uint8 pt = 0; pt < EY_POINTS_MAX; ++pt)
                if (ey->GetPointOwner(pt) == myTeamId) ++pointsHeld;

            float strategicNeed = (pointsHeld < 2) ? 0.2f : 0.0f;

            // EY Flag handling
            ObjectGuid eyFCGuid = ey->GetFlagPickerGUID();
            bool flagPickedUp = !eyFCGuid.IsEmpty();

            if (isFC)
            {
                // Deliver flag to nearest owned point
                float bestDist = 9999.0f;
                uint8 bestPoint = 0xFF;
                for (uint8 pt = 0; pt < EY_POINTS_MAX; ++pt)
                {
                    if (ey->GetPointOwner(pt) != myTeamId) continue;
                    float dist = me->GetExactDist2d(BG_EY_TriggerPositions[pt]);
                    if (dist < bestDist) { bestDist = dist; bestPoint = pt; }
                }
                if (bestPoint < EY_POINTS_MAX)
                {
                    addCandidate(BG_UTIL_DELIVER_NEUTRAL_FLAG, 1.0f,
                        Position(BG_EY_TriggerPositions[bestPoint].GetPositionX(),
                            BG_EY_TriggerPositions[bestPoint].GetPositionY(),
                            BG_EY_TriggerPositions[bestPoint].GetPositionZ()),
                        1, INTENT_ATTACK_FLAG, bestPoint);
                }
                // No owned points: FC should help capture one instead of breaking
                // Fall through to point evaluation below
                if (bestPoint < EY_POINTS_MAX) break;
            }

            // Grab Netherstorm flag if not picked up
            if (!flagPickedUp)
            {
                static constexpr float EY_FLAG_X = 2174.78f, EY_FLAG_Y = 1569.0f, EY_FLAG_Z = 1160.0f;
                float dist = std::sqrt((EY_FLAG_X-myX)*(EY_FLAG_X-myX) + (EY_FLAG_Y-myY)*(EY_FLAG_Y-myY));
                float distFactor = std::max(0.0f, 1.0f - dist / 500.0f);
                float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ATTACK_FLAG) * 0.15f;
                float score = (0.65f + p.objectiveFocus * 0.15f) * distFactor - stacking;
                if (pointsHeld > 0) score += 0.1f; // flag is worth more if we have points to cap at
                if (pointsHeld == 0) score -= 0.25f; // useless without owned points to deliver to
                addCandidate(BG_UTIL_GRAB_NEUTRAL_FLAG, score,
                    Position(EY_FLAG_X, EY_FLAG_Y, EY_FLAG_Z), 1, INTENT_ATTACK_FLAG);
            }

            // Escort friendly FC
            if (flagPickedUp)
            {
                Unit* fc = ObjectAccessor::GetUnit(*me, eyFCGuid);
                if (fc && fc->IsAlive() && bg->GetBotTeamId(fc->GetGUID()) == myTeamId)
                {
                    float dist = me->GetExactDist2d(fc);
                    float distFactor = std::max(0.0f, 1.0f - dist / 400.0f);
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ESCORT_FC) * 0.2f;
                    float score = (0.5f + p.groupTendency * 0.15f) * distFactor - stacking;
                    addCandidate(BG_UTIL_ESCORT_FRIENDLY_FC, score,
                        Position(fc->GetPositionX(), fc->GetPositionY(), fc->GetPositionZ()),
                        1, INTENT_ESCORT_FC);
                }
            }

            // Chase enemy FC (enemy grabbed the Netherstorm flag)
            if (flagPickedUp)
            {
                Unit* enemyFC = ObjectAccessor::GetUnit(*me, eyFCGuid);
                if (enemyFC && enemyFC->IsAlive() && bg->GetBotTeamId(enemyFC->GetGUID()) != myTeamId)
                {
                    float dist = me->GetExactDist2d(enemyFC);
                    float distFactor = std::max(0.0f, 1.0f - dist / 600.0f);
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_CHASE_FC) * 0.20f;
                    float score = (0.78f + p.aggression * 0.1f) * distFactor - stacking;
                    addCandidate(BG_UTIL_CHASE_NEUTRAL_FC, score,
                        Position(enemyFC->GetPositionX(), enemyFC->GetPositionY(), enemyFC->GetPositionZ()),
                        1, INTENT_CHASE_FC);
                }
            }

            // Point control
            uint32 eyEnemyTeamVal = myTeamVal == ALLIANCE ? HORDE : ALLIANCE;
            for (uint8 pt = 0; pt < EY_POINTS_MAX; ++pt)
            {
                float ptX = BG_EY_TriggerPositions[pt].GetPositionX();
                float ptY = BG_EY_TriggerPositions[pt].GetPositionY();
                float ptZ = BG_EY_TriggerPositions[pt].GetPositionZ();
                float dist = std::sqrt((ptX-myX)*(ptX-myX) + (ptY-myY)*(ptY-myY));
                float distFactor = std::max(0.0f, 1.0f - dist / 600.0f);

                TeamId owner = ey->GetPointOwner(pt);
                bool ours = (owner == myTeamId);

                if (!ours)
                {
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_ATTACK_NODE, pt) * 0.12f;
                    float base = (owner == TEAM_NEUTRAL) ? 0.65f : 0.55f;
                    float score = (base + attackBias + strategicNeed + p.aggression * 0.12f)
                        * distFactor - stacking;
                    addCandidate(BG_UTIL_ATTACK_NODE, score,
                        Position(ptX, ptY, ptZ), 1, INTENT_ATTACK_NODE, pt);
                }
                else
                {
                    float stacking = CountIntentions(bgInstId, myTeamId, INTENT_DEFEND_NODE, pt) * 0.15f;
                    float score = (0.4f + defendBias + p.caution * 0.15f + p.groupTendency * 0.1f) * distFactor - stacking;
                    if (pointsHeld <= 2) score += 0.12f;
                    addCandidate(BG_UTIL_DEFEND_NODE, score,
                        Position(ptX, ptY, ptZ), 2, INTENT_DEFEND_NODE, pt);
                }

                // Respond to enemies near our point (not yet contesting)
                if (ours)
                {
                    uint8 enemiesNear = 0;
                    for (auto const& [guid, botData] : bg->GetBots())
                    {
                        if (botData.Team != eyEnemyTeamVal) continue;
                        Creature const* enemy = ObjectAccessor::GetCreature(*me, guid);
                        if (!enemy || !enemy->IsAlive()) continue;
                        if (std::sqrt((enemy->GetPositionX()-ptX)*(enemy->GetPositionX()-ptX) +
                            (enemy->GetPositionY()-ptY)*(enemy->GetPositionY()-ptY)) < 40.0f)
                            ++enemiesNear;
                    }
                    if (enemiesNear > 0)
                    {
                        float score = (0.72f + float(enemiesNear) * 0.05f) * distFactor;
                        addCandidate(BG_UTIL_RESPOND_NODE_ATTACK, score,
                            Position(ptX, ptY, ptZ), 2, INTENT_DEFEND_NODE, pt);
                    }
                }

                // Reinforce: EY doesn't have formal contested state, but if enemy
                // recently took a point we owned, treat it as contested
                if (!ours && owner != TEAM_NEUTRAL)
                {
                    // Enemy owns it — check if allies are fighting there
                    uint8 alliesNear = CountIntentions(bgInstId, myTeamId, INTENT_ATTACK_NODE, pt);
                    if (alliesNear > 0 && alliesNear < 4)
                    {
                        float score = (0.75f + p.aggression * 0.1f + p.groupTendency * 0.1f) * distFactor;
                        addCandidate(BG_UTIL_REINFORCE_NODE, score,
                            Position(ptX, ptY, ptZ), 1, INTENT_ATTACK_NODE, pt);
                    }
                }
            }

            // Fight midfield
            {
                float score = 0.2f + p.aggression * 0.15f - p.objectiveFocus * 0.1f;
                addCandidate(BG_UTIL_FIGHT_MIDFIELD, score,
                    Position(2174.0f, 1569.0f, 1160.0f), 1, INTENT_ROAM);
            }
            break;
        }
        default:
            break;
    }

    if (candidates.empty())
        return {};

    // Pick highest-scoring action
    BGUtilityResult best = candidates[0];
    for (auto const& c : candidates)
        if (c.score > best.score) best = c;

    return best;
}

// --- DB Load/Save ---

void BotBGAIMgr::LoadFromDB()
{
    uint32 count = 0;

    // Load waypoints
    // Auto-migration: add wall_hits column if missing
    bool hasWallHits = false;
    {
        QueryResult colCheck = CharacterDatabase.Query(
            "SHOW COLUMNS FROM characters_npcbot_bg_waypoints LIKE 'wall_hits'");
        if (colCheck && colCheck->GetRowCount() > 0)
            hasWallHits = true;
        else
            CharacterDatabase.DirectExecute("ALTER TABLE characters_npcbot_bg_waypoints ADD COLUMN wall_hits INT UNSIGNED NOT NULL DEFAULT 0");
        hasWallHits = true; // either existed or just added
    }

    if (QueryResult result = CharacterDatabase.Query(
        hasWallHits
            ? "SELECT map_id, grid_x, grid_y, z, visit_count, wall_hits FROM characters_npcbot_bg_waypoints"
            : "SELECT map_id, grid_x, grid_y, z, visit_count FROM characters_npcbot_bg_waypoints"))
    {
        do {
            Field* f = result->Fetch();
            BGGridCell cell{ f[0].GetUInt16(), f[1].GetInt16(), f[2].GetInt16() };
            uint32 wallHits = hasWallHits ? f[5].GetUInt32() : 0;
            _waypointMesh[cell] = BGLearnedWaypoint{ f[3].GetFloat(), f[4].GetUInt32(), wallHits };
            _waypointMapIds.insert(cell.mapId);
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG learned waypoints", count);

    // Load heatmap
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, cell_x, cell_y, kills, deaths, obj_caps, obj_defends FROM characters_npcbot_bg_heatmap"))
    {
        do {
            Field* f = result->Fetch();
            BGGridCell cell{ f[0].GetUInt16(), f[1].GetInt16(), f[2].GetInt16() };
            _heatmap[cell] = BGHeatmapData{ f[3].GetUInt32(), f[4].GetUInt32(), f[5].GetUInt32(), f[6].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG heatmap cells", count);

    // Load strategies
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, strategy_id, success_count, fail_count FROM characters_npcbot_bg_strategy"))
    {
        do {
            Field* f = result->Fetch();
            uint64 key = MakeStrategyKey(f[0].GetUInt16(), f[1].GetUInt8());
            _strategies[key] = BGStrategyData{ f[2].GetUInt32(), f[3].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG strategy records", count);

    // Load counter-strategies
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, my_strategy, enemy_strategy, wins, losses FROM characters_npcbot_bg_counter_strategy"))
    {
        do {
            Field* f = result->Fetch();
            uint32 key = MakeCounterKey(f[0].GetUInt16(), f[1].GetUInt8(), f[2].GetUInt8());
            _counterStrategies[key] = BGCounterStrategyData{ f[3].GetUInt32(), f[4].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG counter-strategy records", count);

    // Load group success
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, cell_x, cell_y, group_size, successes, failures FROM characters_npcbot_bg_group_success"))
    {
        do {
            Field* f = result->Fetch();
            uint32 mapId = f[0].GetUInt16();
            int16 gx = f[1].GetInt16(), gy = f[2].GetInt16();
            uint8 gs = f[3].GetUInt8();
            uint64 key = (uint64(mapId) << 48) | (uint64(uint16(gx)) << 32) | (uint64(uint16(gy)) << 16) | gs;
            _groupSuccess[key] = BGGroupSuccessData{ f[4].GetUInt32(), f[5].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG group success records", count);

    // Load timing
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, action, time_bracket, successes, failures FROM characters_npcbot_bg_timing"))
    {
        do {
            Field* f = result->Fetch();
            uint32 key = MakeTimingKey(f[0].GetUInt16(), f[1].GetUInt8(), f[2].GetUInt8());
            _timing[key] = BGTimingData{ f[3].GetUInt32(), f[4].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG timing records", count);

    // Load class matchups
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, my_class, enemy_class, wins, losses FROM characters_npcbot_bg_matchups"))
    {
        do {
            Field* f = result->Fetch();
            uint32 key = MakeMatchupKey(f[0].GetUInt16(), f[1].GetUInt8(), f[2].GetUInt8());
            _matchups[key] = BGMatchupData{ f[3].GetUInt32(), f[4].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG class matchup records", count);

    // Load win conditions
    count = 0;
    if (QueryResult result = CharacterDatabase.Query("SELECT map_id, time_bracket, nodes_held, score_bracket, wins, losses FROM characters_npcbot_bg_win_conditions"))
    {
        do {
            Field* f = result->Fetch();
            uint64 key = MakeWinCondKey(f[0].GetUInt16(), f[1].GetUInt8(), f[2].GetUInt8(), f[3].GetUInt8());
            _winConditions[key] = BGWinConditionData{ f[4].GetUInt32(), f[5].GetUInt32() };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG win condition records", count);

    // Q-Learning table
    LoadQTableFromDB();
}

void BotBGAIMgr::FlushPendingData()
{
    std::unique_lock lock(_lock);

    // Temporal decay: multiply all existing counters by 0.995 (half-life ~139 flushes)
    // This ensures recent data has more influence than old data
    constexpr float DECAY_FACTOR = 0.995f;
    for (auto& [cell, hd] : _heatmap)
    {
        hd.kills = uint32(hd.kills * DECAY_FACTOR);
        hd.deaths = uint32(hd.deaths * DECAY_FACTOR);
        hd.objCaps = uint32(hd.objCaps * DECAY_FACTOR);
        hd.objDefends = uint32(hd.objDefends * DECAY_FACTOR);
    }
    for (auto& [key, sd] : _strategies)
    {
        sd.successCount = uint32(sd.successCount * DECAY_FACTOR);
        sd.failCount = uint32(sd.failCount * DECAY_FACTOR);
    }

    // Flush waypoints
    for (auto const& [cell, wp] : _pendingWaypoints)
    {
        _waypointMapIds.insert(cell.mapId);
        auto& stored = _waypointMesh[cell];
        if (stored.visitCount == 0)
        {
            stored = wp;
        }
        else
        {
            stored.z = (stored.z * stored.visitCount + wp.z * wp.visitCount) / (stored.visitCount + wp.visitCount);
            stored.visitCount += wp.visitCount;
            stored.wallHits += wp.wallHits;
        }
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_waypoints (map_id, grid_x, grid_y, z, visit_count, wall_hits) VALUES ({}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE z = {}, visit_count = visit_count + {}, wall_hits = wall_hits + {}",
            cell.mapId, int32(cell.gridX), int32(cell.gridY), stored.z, wp.visitCount, wp.wallHits,
            stored.z, wp.visitCount, wp.wallHits);
    }
    _pendingWaypoints.clear();

    // Waypoint mesh decay + pruning: prevent unbounded growth
    uint32 prunedWaypoints = 0;
    for (auto it = _waypointMesh.begin(); it != _waypointMesh.end();)
    {
        it->second.visitCount = uint32(it->second.visitCount * DECAY_FACTOR);
        it->second.wallHits = uint32(it->second.wallHits * DECAY_FACTOR);
        if (it->second.visitCount < 2)
        {
            CharacterDatabase.PExecute(
                "DELETE FROM characters_npcbot_bg_waypoints WHERE map_id = {} AND grid_x = {} AND grid_y = {}",
                it->first.mapId, int32(it->first.gridX), int32(it->first.gridY));
            it = _waypointMesh.erase(it);
            ++prunedWaypoints;
        }
        else
            ++it;
    }
    if (prunedWaypoints > 0)
        TC_LOG_INFO("server.loading", ">> Pruned {} stale waypoint mesh cells", prunedWaypoints);

    // Flush heatmap
    for (auto const& [cell, hd] : _pendingHeatmap)
    {
        auto& stored = _heatmap[cell];
        stored.kills += hd.kills;
        stored.deaths += hd.deaths;
        stored.objCaps += hd.objCaps;
        stored.objDefends += hd.objDefends;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_heatmap (map_id, cell_x, cell_y, kills, deaths, obj_caps, obj_defends) VALUES ({}, {}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE kills = kills + {}, deaths = deaths + {}, obj_caps = obj_caps + {}, obj_defends = obj_defends + {}",
            cell.mapId, int32(cell.gridX), int32(cell.gridY), hd.kills, hd.deaths, hd.objCaps, hd.objDefends,
            hd.kills, hd.deaths, hd.objCaps, hd.objDefends);
    }
    _pendingHeatmap.clear();

    // Flush strategies
    for (auto const& [key, sd] : _pendingStrategies)
    {
        uint32 mapId = uint32(key >> 32);
        uint32 stratId = uint32(key & 0xFFFFFFFF);
        auto& stored = _strategies[key];
        stored.successCount += sd.successCount;
        stored.failCount += sd.failCount;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_strategy (map_id, strategy_id, success_count, fail_count) VALUES ({}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE success_count = success_count + {}, fail_count = fail_count + {}",
            mapId, stratId, sd.successCount, sd.failCount,
            sd.successCount, sd.failCount);
    }
    _pendingStrategies.clear();

    // Decay new layer data
    for (auto& [key, d] : _counterStrategies)
    {
        d.wins = uint32(d.wins * DECAY_FACTOR);
        d.losses = uint32(d.losses * DECAY_FACTOR);
    }
    for (auto& [key, d] : _groupSuccess)
    {
        d.successes = uint32(d.successes * DECAY_FACTOR);
        d.failures = uint32(d.failures * DECAY_FACTOR);
    }
    for (auto& [key, d] : _timing)
    {
        d.successes = uint32(d.successes * DECAY_FACTOR);
        d.failures = uint32(d.failures * DECAY_FACTOR);
    }
    for (auto& [key, d] : _matchups)
    {
        d.wins = uint32(d.wins * DECAY_FACTOR);
        d.losses = uint32(d.losses * DECAY_FACTOR);
    }
    for (auto& [key, d] : _winConditions)
    {
        d.wins = uint32(d.wins * DECAY_FACTOR);
        d.losses = uint32(d.losses * DECAY_FACTOR);
    }

    // Flush counter-strategies
    for (auto const& [key, d] : _pendingCounterStrategies)
    {
        uint32 mapId = (key >> 16) & 0xFFFF;
        uint32 myStrat = (key >> 8) & 0xFF;
        uint32 enemyStrat = key & 0xFF;
        auto& stored = _counterStrategies[key];
        stored.wins += d.wins;
        stored.losses += d.losses;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_counter_strategy (map_id, my_strategy, enemy_strategy, wins, losses) VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE wins = wins + {}, losses = losses + {}",
            mapId, myStrat, enemyStrat, d.wins, d.losses,
            d.wins, d.losses);
    }
    _pendingCounterStrategies.clear();

    // Flush group success
    for (auto const& [key, d] : _pendingGroupSuccess)
    {
        uint32 mapId = uint32(key >> 48);
        int16 gx = int16(uint16((key >> 32) & 0xFFFF));
        int16 gy = int16(uint16((key >> 16) & 0xFFFF));
        uint8 gs = uint8(key & 0xFF);
        auto& stored = _groupSuccess[key];
        stored.successes += d.successes;
        stored.failures += d.failures;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_group_success (map_id, cell_x, cell_y, group_size, successes, failures) VALUES ({}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE successes = successes + {}, failures = failures + {}",
            mapId, int32(gx), int32(gy), uint32(gs), d.successes, d.failures,
            d.successes, d.failures);
    }
    _pendingGroupSuccess.clear();

    // Flush timing
    for (auto const& [key, d] : _pendingTiming)
    {
        uint32 mapId = (key >> 16) & 0xFFFF;
        uint32 action = (key >> 8) & 0xFF;
        uint32 tb = key & 0xFF;
        auto& stored = _timing[key];
        stored.successes += d.successes;
        stored.failures += d.failures;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_timing (map_id, action, time_bracket, successes, failures) VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE successes = successes + {}, failures = failures + {}",
            mapId, action, tb, d.successes, d.failures,
            d.successes, d.failures);
    }
    _pendingTiming.clear();

    // Flush class matchups
    for (auto const& [key, d] : _pendingMatchups)
    {
        uint32 mapId = (key >> 16) & 0xFFFF;
        uint32 myClass = (key >> 8) & 0xFF;
        uint32 enemyClass = key & 0xFF;
        auto& stored = _matchups[key];
        stored.wins += d.wins;
        stored.losses += d.losses;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_matchups (map_id, my_class, enemy_class, wins, losses) VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE wins = wins + {}, losses = losses + {}",
            mapId, myClass, enemyClass, d.wins, d.losses,
            d.wins, d.losses);
    }
    _pendingMatchups.clear();

    // Flush win conditions
    for (auto const& [key, d] : _pendingWinConditions)
    {
        uint32 mapId = uint32(key >> 24);
        uint32 tb = uint32((key >> 16) & 0xFF);
        uint32 nodesHeld = uint32((key >> 8) & 0xFF);
        uint32 scoreBracket = uint32(key & 0xFF);
        auto& stored = _winConditions[key];
        stored.wins += d.wins;
        stored.losses += d.losses;
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_win_conditions (map_id, time_bracket, nodes_held, score_bracket, wins, losses) VALUES ({}, {}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE wins = wins + {}, losses = losses + {}",
            mapId, tb, nodesHeld, scoreBracket, d.wins, d.losses,
            d.wins, d.losses);
    }
    _pendingWinConditions.clear();
}

void BotBGAIMgr::SaveToDB()
{
    FlushPendingData();
    FlushQTableToDB();
}

// --- Key helpers ---

uint32 BotBGAIMgr::MakeCounterKey(uint32 mapId, uint32 myStrat, uint32 enemyStrat)
{
    return (mapId << 16) | ((myStrat & 0xFF) << 8) | (enemyStrat & 0xFF);
}

uint64 BotBGAIMgr::MakeGroupKey(uint32 mapId, float x, float y, uint8 groupSize)
{
    int16 gx = SnapToGrid(x), gy = SnapToGrid(y);
    return (uint64(mapId) << 48) | (uint64(uint16(gx)) << 32) | (uint64(uint16(gy)) << 16) | groupSize;
}

uint32 BotBGAIMgr::MakeTimingKey(uint32 mapId, uint32 action, uint8 timeBracket)
{
    return (mapId << 16) | ((action & 0xFF) << 8) | timeBracket;
}

uint32 BotBGAIMgr::MakeMatchupKey(uint32 mapId, uint8 myClass, uint8 enemyClass)
{
    return (mapId << 16) | (uint32(myClass) << 8) | enemyClass;
}

uint64 BotBGAIMgr::MakeWinCondKey(uint32 mapId, uint8 timeBracket, uint8 nodesHeld, uint8 scoreBracket)
{
    return (uint64(mapId) << 24) | (uint64(timeBracket) << 16) | (uint64(nodesHeld) << 8) | scoreBracket;
}

BGTimeBracket BotBGAIMgr::GetTimeBracket(uint32 elapsedMs)
{
    // Opening: first 90s after gates open (BG start < 210000 = 120s prep + 90s opening)
    if (elapsedMs < 210000) return BG_TIME_OPENING;
    uint32 minutes = elapsedMs / 60000;
    if (minutes < 5) return BG_TIME_EARLY;
    if (minutes < 12) return BG_TIME_MID;
    return BG_TIME_LATE;
}

BGScoreBracket BotBGAIMgr::GetScoreBracket(uint32 myScore, uint32 enemyScore)
{
    if (myScore + 500 < enemyScore) return BG_SCORE_LOSING_BAD;
    if (myScore < enemyScore) return BG_SCORE_LOSING;
    if (myScore == enemyScore) return BG_SCORE_TIED;
    if (myScore > enemyScore + 500) return BG_SCORE_WINNING_BIG;
    return BG_SCORE_WINNING;
}

// --- Layer 1: Counter-strategy ---

void BotBGAIMgr::RecordCounterStrategyOutcome(uint32 mapId, uint32 myStrat, uint32 enemyStrat, bool won)
{
    uint32 key = MakeCounterKey(mapId, myStrat, enemyStrat);
    std::unique_lock lock(_lock);
    auto& data = _pendingCounterStrategies[key];
    if (won) ++data.wins; else ++data.losses;
}

float BotBGAIMgr::GetCounterStrategyWeight(uint32 mapId, uint32 myStrat, uint32 enemyStrat)
{
    uint32 key = MakeCounterKey(mapId, myStrat, enemyStrat);
    std::shared_lock lock(_lock);
    auto it = _counterStrategies.find(key);
    uint32 w = 2, l = 2; // Beta prior
    if (it != _counterStrategies.end()) { w += it->second.wins; l += it->second.losses; }
    return float(w) / float(w + l);
}

uint32 BotBGAIMgr::SelectCounterStrategy(uint32 mapId, uint32 enemyDominantStrat, float intelligence)
{
    if (!IntelligenceCheck(intelligence))
        return urand(0, BG_STRATEGY_MAX - 1);

    float weights[BG_STRATEGY_MAX];
    float total = 0.0f;
    for (uint32 i = 0; i < BG_STRATEGY_MAX; ++i)
    {
        weights[i] = GetCounterStrategyWeight(mapId, i, enemyDominantStrat);
        total += weights[i];
    }
    if (total <= 0.0f) return urand(0, BG_STRATEGY_MAX - 1);

    float roll = frand(0.0f, total);
    float cum = 0.0f;
    for (uint32 i = 0; i < BG_STRATEGY_MAX; ++i)
    {
        cum += weights[i];
        if (roll <= cum) return i;
    }
    return 0;
}

// --- Layer 2: Enemy behavior classification (runtime only) ---

BGEnemyBehavior BotBGAIMgr::ClassifyEnemyBehavior(Creature const* me, Battleground const* bg, TeamId myTeamId)
{
    uint32 enemyTeam = (myTeamId == TEAM_ALLIANCE) ? HORDE : ALLIANCE;
    std::vector<Position> enemyPositions;

    for (auto const& [guid, botData] : bg->GetBots())
    {
        if (botData.Team != enemyTeam) continue;
        Creature const* enemy = ObjectAccessor::GetCreature(*me, guid);
        if (enemy && enemy->IsAlive())
            enemyPositions.push_back(enemy->GetPosition());
    }

    if (enemyPositions.size() < 3) return BG_ENEMY_UNKNOWN;

    // Check for zerg: >60% within 30yd of each other
    uint32 clustered = 0;
    for (size_t i = 0; i < enemyPositions.size(); ++i)
    {
        uint32 nearby = 0;
        for (size_t j = 0; j < enemyPositions.size(); ++j)
        {
            if (i != j)
            {
                float dx = enemyPositions[i].GetPositionX() - enemyPositions[j].GetPositionX();
                float dy = enemyPositions[i].GetPositionY() - enemyPositions[j].GetPositionY();
                if (dx*dx + dy*dy < 900.0f) ++nearby; // 30yd
            }
        }
        if (nearby >= enemyPositions.size() / 2) ++clustered;
    }
    if (clustered > enemyPositions.size() * 6 / 10) return BG_ENEMY_ZERG;

    // Check spread: how many distinct clusters (>50yd apart)?
    uint32 clusters = 0;
    std::vector<bool> visited(enemyPositions.size(), false);
    for (size_t i = 0; i < enemyPositions.size(); ++i)
    {
        if (visited[i]) continue;
        visited[i] = true;
        ++clusters;
        for (size_t j = i + 1; j < enemyPositions.size(); ++j)
        {
            if (!visited[j])
            {
                float dx = enemyPositions[i].GetPositionX() - enemyPositions[j].GetPositionX();
                float dy = enemyPositions[i].GetPositionY() - enemyPositions[j].GetPositionY();
                if (dx*dx + dy*dy < 2500.0f) visited[j] = true; // 50yd cluster
            }
        }
    }
    if (clusters >= 3) return BG_ENEMY_SPLIT;

    // Check aggressive vs defensive using distance to us
    float mapCenterX = 0, mapCenterY = 0;
    for (auto const& pos : enemyPositions)
    {
        mapCenterX += pos.GetPositionX();
        mapCenterY += pos.GetPositionY();
    }
    mapCenterX /= enemyPositions.size();
    mapCenterY /= enemyPositions.size();

    float myCenterX = me->GetPositionX();
    float myCenterY = me->GetPositionY();
    float distToMe = std::sqrt((mapCenterX - myCenterX) * (mapCenterX - myCenterX) + (mapCenterY - myCenterY) * (mapCenterY - myCenterY));
    if (distToMe < 100.0f) return BG_ENEMY_AGGRESSIVE;

    return BG_ENEMY_DEFENSIVE;
}

void BotBGAIMgr::UpdateSharedEnemyBehavior(uint32 bgInstanceId, Creature const* observer, Battleground const* bg, TeamId teamId)
{
    std::unique_lock lock(_lock);
    _sharedEnemyBehavior[bgInstanceId] = uint8(ClassifyEnemyBehavior(observer, bg, teamId));
}

uint8 BotBGAIMgr::GetSharedEnemyBehavior(uint32 bgInstanceId)
{
    std::shared_lock lock(_lock);
    auto it = _sharedEnemyBehavior.find(bgInstanceId);
    return it != _sharedEnemyBehavior.end() ? it->second : 0;
}

void BotBGAIMgr::ClearSharedEnemyBehavior(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _sharedEnemyBehavior.erase(bgInstanceId);
}

// --- Layer 3: Group success ---

void BotBGAIMgr::RecordGroupSuccess(uint32 mapId, float x, float y, uint8 groupSize, bool success)
{
    uint64 key = MakeGroupKey(mapId, x, y, groupSize);
    std::unique_lock lock(_lock);
    auto& data = _pendingGroupSuccess[key];
    if (success) ++data.successes; else ++data.failures;
}

float BotBGAIMgr::GetGroupSuccessRate(uint32 mapId, float x, float y, uint8 groupSize)
{
    uint64 key = MakeGroupKey(mapId, x, y, groupSize);
    std::shared_lock lock(_lock);
    auto it = _groupSuccess.find(key);
    uint32 s = 2, f = 2; // Beta prior
    if (it != _groupSuccess.end()) { s += it->second.successes; f += it->second.failures; }
    return float(s) / float(s + f);
}
// --- Layer 4: Timing ---

void BotBGAIMgr::RecordTimingOutcome(uint32 mapId, uint32 action, uint32 elapsedMs, bool success)
{
    uint8 tb = uint8(GetTimeBracket(elapsedMs));
    uint32 key = MakeTimingKey(mapId, action, tb);
    std::unique_lock lock(_lock);
    auto& data = _pendingTiming[key];
    if (success) ++data.successes; else ++data.failures;
}

float BotBGAIMgr::GetTimingWeight(uint32 mapId, uint32 action, uint32 elapsedMs)
{
    uint8 tb = uint8(GetTimeBracket(elapsedMs));
    uint32 key = MakeTimingKey(mapId, action, tb);
    std::shared_lock lock(_lock);
    auto it = _timing.find(key);
    uint32 s = 2, f = 2; // Beta prior
    if (it != _timing.end()) { s += it->second.successes; f += it->second.failures; }
    return float(s) / float(s + f);
}

// --- Layer 5: Class matchups ---

void BotBGAIMgr::RecordClassMatchup(uint32 mapId, uint8 myClass, uint8 enemyClass, bool won)
{
    uint32 key = MakeMatchupKey(mapId, myClass, enemyClass);
    std::unique_lock lock(_lock);
    auto& data = _pendingMatchups[key];
    if (won) ++data.wins; else ++data.losses;
}

float BotBGAIMgr::GetClassMatchupWeight(uint32 mapId, uint8 myClass, uint8 enemyClass)
{
    uint32 key = MakeMatchupKey(mapId, myClass, enemyClass);
    std::shared_lock lock(_lock);
    auto it = _matchups.find(key);
    uint32 w = 2, l = 2; // Beta prior
    if (it != _matchups.end()) { w += it->second.wins; l += it->second.losses; }
    return float(w) / float(w + l);
}

// --- Layer 6: Win conditions ---

void BotBGAIMgr::RecordWinConditionSnapshot(uint32 mapId, uint8 timeBracket, uint8 nodesHeld, uint8 scoreBracket, bool won)
{
    uint64 key = MakeWinCondKey(mapId, timeBracket, nodesHeld, scoreBracket);
    std::unique_lock lock(_lock);
    auto& data = _pendingWinConditions[key];
    if (won) ++data.wins; else ++data.losses;
}

float BotBGAIMgr::GetWinConditionProbability(uint32 mapId, uint8 timeBracket, uint8 nodesHeld, uint8 scoreBracket)
{
    uint64 key = MakeWinCondKey(mapId, timeBracket, nodesHeld, scoreBracket);
    std::shared_lock lock(_lock);
    auto it = _winConditions.find(key);
    uint32 w = 2, l = 2; // Beta prior
    if (it != _winConditions.end()) { w += it->second.wins; l += it->second.losses; }
    return float(w) / float(w + l);
}

// ============================================================================
// Enhanced waypoint mesh: route-through, scored, personality-influenced routing
// ============================================================================

std::optional<Position> BotBGAIMgr::GetNextRouteWaypoint(
    uint32 mapId, float fromX, float fromY, float toX, float toY, float corridorWidth)
{
    // Direction vector from -> to
    float dx = toX - fromX;
    float dy = toY - fromY;
    float totalDist = std::sqrt(dx * dx + dy * dy);
    if (totalDist < 15.0f)
        return std::nullopt; // too close, just go straight

    // Normalize direction
    float dirX = dx / totalDist;
    float dirY = dy / totalDist;

    // Perpendicular vector
    float perpX = -dirY;
    float perpY = dirX;

    // Query corridor: midpoint with radius = half the distance
    float midX = (fromX + toX) * 0.5f;
    float midY = (fromY + toY) * 0.5f;
    float queryRadius = totalDist * 0.5f + corridorWidth;

    auto waypoints = GetLearnedWaypointsNear(mapId, midX, midY, queryRadius);
    if (waypoints.empty())
        return std::nullopt;

    // Filter and score waypoints in the corridor
    ScoredWaypoint best;
    best.score = -1.0f;

    for (auto const& wp : waypoints)
    {
        if (wp.visitCount < 5)
            continue; // minimum traffic threshold

        float wpX = wp.pos.GetPositionX();
        float wpY = wp.pos.GetPositionY();

        // Vector from 'from' to waypoint
        float vx = wpX - fromX;
        float vy = wpY - fromY;

        // Project onto from->to direction (how far along the path)
        float projection = (vx * dirX + vy * dirY) / totalDist;
        if (projection < 0.15f || projection > 0.85f)
            continue; // too close to start or end

        // Perpendicular distance (how far off the path)
        float perpDist = std::abs(vx * perpX + vy * perpY);
        if (perpDist > corridorWidth)
            continue; // outside corridor

        // Distance from current position
        float distFromMe = std::sqrt(vx * vx + vy * vy);
        if (distFromMe < 10.0f)
            continue; // too close to where we are

        float score = float(wp.visitCount);
        if (score > best.score)
        {
            best.pos = wp.pos;
            best.visitCount = wp.visitCount;
            best.score = score;
        }
    }

    if (best.score < 0.0f)
        return std::nullopt;

    return best.pos;
}

float BotBGAIMgr::ScoreWaypointPosition(uint32 mapId, float x, float y, uint32 visitCount)
{
    // Base score from log(visitCount) — compressed range lets personality modifiers matter
    // Heatmap kill/death scoring is handled by the caller with personality weighting
    // to avoid double-counting combat data
    float score = std::log(float(std::max(visitCount, 1u)) + 1.0f);

    // Objective relevance bonus (not double-counted — only checked here)
    auto hd = GetHeatmapData(mapId, x, y);
    if (hd && (hd->objCaps + hd->objDefends > 0))
        score *= 1.2f;

    // Wall penalty: reduce score for cells where bots frequently hit walls
    BGGridCell cell = MakeCell(mapId, x, y);
    std::shared_lock lock(_lock);
    auto it = _waypointMesh.find(cell);
    if (it != _waypointMesh.end() && it->second.wallHits > 0)
        score *= 1.0f / (1.0f + float(it->second.wallHits) * 0.5f);

    return score;
}

std::optional<Position> BotBGAIMgr::GetNextScoredRouteWaypoint(
    uint32 mapId, float fromX, float fromY, float toX, float toY,
    float aggression, float caution, float groupTendency, float corridorWidth,
    uint8 momentum, uint32 bgInstanceId, TeamId teamId, bool isFC)
{
    float dx = toX - fromX;
    float dy = toY - fromY;
    float totalDist = std::sqrt(dx * dx + dy * dy);
    if (totalDist < 15.0f)
        return std::nullopt;

    float dirX = dx / totalDist;
    float dirY = dy / totalDist;
    float perpX = -dirY;
    float perpY = dirX;

    float midX = (fromX + toX) * 0.5f;
    float midY = (fromY + toY) * 0.5f;
    // Cap query radius to avoid scanning huge grid areas for long distances
    float queryRadius = std::min(totalDist * 0.5f + corridorWidth, 80.0f);

    auto waypoints = GetLearnedWaypointsNear(mapId, midX, midY, queryRadius);
    if (waypoints.empty())
        return std::nullopt;

    // Find max visit count for normalization (for groupTendency scaling)
    uint32 maxVisits = 1;
    for (auto const& wp : waypoints)
        if (wp.visitCount > maxVisits) maxVisits = wp.visitCount;

    std::vector<ScoredWaypoint> candidates;

    for (auto const& wp : waypoints)
    {
        if (wp.visitCount < 5)
            continue;

        float wpX = wp.pos.GetPositionX();
        float wpY = wp.pos.GetPositionY();

        float vx = wpX - fromX;
        float vy = wpY - fromY;

        // Projection ratio: how far along the from→to path
        float projection = (vx * dirX + vy * dirY) / totalDist;
        if (projection < 0.10f || projection > 0.90f)
            continue;

        // Perpendicular distance: how far off the direct path
        float perpDist = std::abs(vx * perpX + vy * perpY);
        if (perpDist > corridorWidth)
            continue;

        float distFromMe = std::sqrt(vx * vx + vy * vy);
        if (distFromMe < 8.0f)
            continue;

        // Forward progress guarantee: waypoint must be closer to destination than we are
        float wpDistToDest = std::sqrt((wpX - toX) * (wpX - toX) + (wpY - toY) * (wpY - toY));
        if (wpDistToDest >= totalDist * 0.95f)
            continue; // doesn't make meaningful forward progress

        // Base score from log(visitCount)
        float score = std::log(float(std::max(wp.visitCount, 1u)) + 1.0f);

        // Single heatmap lookup for all scoring (objective bonus + personality K/D)
        auto hd = GetHeatmapData(mapId, wpX, wpY);
        if (hd)
        {
            // Objective relevance bonus
            if (hd->objCaps + hd->objDefends > 0)
                score *= 1.2f;

            float totalEvents = float(hd->kills + hd->deaths + 1);
            float killRatio = float(hd->kills) / totalEvents;
            float deathRatio = float(hd->deaths) / totalEvents;

            // Base K/D influence (everyone benefits from good routes)
            float kdRatio = float(hd->kills + 1) / float(hd->deaths + 1);
            score *= std::sqrt(kdRatio); // sqrt for moderate influence

            // Personality-weighted K/D on top
            score *= (1.0f + aggression * killRatio);
            score *= (1.0f - caution * 0.6f * deathRatio);

            // Danger zone avoidance: strong penalty when outnumbered in high-death areas
            if (momentum <= BG_MOMENTUM_OUTNUMBERED && deathRatio > 0.4f)
            {
                float penalty = (momentum == BG_MOMENTUM_WIPED) ? 0.2f : 0.4f;
                penalty += (1.0f - penalty) * (1.0f - caution);
                score *= penalty;
            }
            else if (momentum >= BG_MOMENTUM_DOMINATING && deathRatio > 0.3f)
                score *= 1.1f; // dominating: slight bonus for contested areas
        }

        // Enemy sighting avoidance: avoid areas with recently seen enemies
        if (bgInstanceId > 0 && teamId != TEAM_NEUTRAL)
        {
            uint8 nearbyEnemies = GetEnemyConcentration(bgInstanceId, teamId, wpX, wpY, 25.0f);
            if (nearbyEnemies > 0)
            {
                if (isFC)
                    score *= std::max(0.1f, 1.0f - nearbyEnemies * 0.3f);
                else if (momentum <= BG_MOMENTUM_OUTNUMBERED)
                    score *= std::max(0.3f, 1.0f - nearbyEnemies * 0.2f);
                else if (momentum >= BG_MOMENTUM_ADVANTAGE && aggression > 0.5f)
                    score *= 1.0f + nearbyEnemies * 0.1f;
            }
        }

        // Group tendency: crowd following vs road less traveled
        float visitNormalized = float(wp.visitCount) / float(maxVisits);
        float crowdFactor = visitNormalized * groupTendency + (1.0f - visitNormalized) * (1.0f - groupTendency);
        score *= (0.3f + crowdFactor * 1.4f); // range 0.3 to 1.7 — stronger effect

        candidates.push_back(ScoredWaypoint{ wp.pos, wp.visitCount, score });
    }

    // Shortcut discovery: find high-traffic waypoints outside corridor when options are sparse
    if (candidates.size() < 3)
    {
        float widerRadius = std::min(totalDist * 0.5f + 60.0f, 100.0f);
        auto widerWaypoints = GetLearnedWaypointsNear(mapId, midX, midY, widerRadius);
        for (auto const& wp : widerWaypoints)
        {
            if (wp.visitCount < 20) continue;

            float wpX = wp.pos.GetPositionX();
            float wpY = wp.pos.GetPositionY();
            float vx = wpX - fromX, vy = wpY - fromY;
            float distFromMe = std::sqrt(vx * vx + vy * vy);
            if (distFromMe < 10.0f) continue;

            float wpDistToDest = std::sqrt((wpX - toX) * (wpX - toX) + (wpY - toY) * (wpY - toY));
            if (wpDistToDest >= totalDist * 0.65f) continue; // must cut at least 35%

            // Dedup
            bool dup = false;
            for (auto const& c : candidates)
                if (std::abs(c.pos.GetPositionX() - wpX) < 3.0f && std::abs(c.pos.GetPositionY() - wpY) < 3.0f)
                { dup = true; break; }
            if (dup) continue;

            float distSavings = 1.0f - (wpDistToDest / totalDist);
            float score = std::log(float(wp.visitCount) + 1.0f) * (1.0f + distSavings);

            auto hd = GetHeatmapData(mapId, wpX, wpY);
            if (hd)
            {
                float kdRatio = float(hd->kills + 1) / float(hd->deaths + 1);
                score *= std::sqrt(kdRatio);
            }

            candidates.push_back(ScoredWaypoint{ wp.pos, wp.visitCount, score });
        }
    }

    if (candidates.empty())
        return std::nullopt;

    // Sort descending by score, pick weighted random from top 5 for more diversity
    std::sort(candidates.begin(), candidates.end(),
        [](ScoredWaypoint const& a, ScoredWaypoint const& b) { return a.score > b.score; });

    size_t topN = std::min(size_t(5), candidates.size());
    float totalScore = 0.0f;
    for (size_t i = 0; i < topN; ++i)
        totalScore += candidates[i].score;

    if (totalScore <= 0.0f)
        return candidates[0].pos;

    float roll = frand(0.0f, totalScore);
    float cum = 0.0f;
    for (size_t i = 0; i < topN; ++i)
    {
        cum += candidates[i].score;
        if (roll <= cum)
            return candidates[i].pos;
    }

    return candidates[0].pos;
}

// Combat engagement evaluation: combines class matchup history with runtime context
float BotBGAIMgr::EvaluateEngagement(uint32 mapId, BGEngagementContext const& ctx)
{
    float baseRate = GetClassMatchupWeight(mapId, ctx.myClass, ctx.enemyClass);

    // Adjust for HP advantage
    float hpFactor = float(ctx.myHpPct) / float(std::max(ctx.enemyHpPct, uint8(1)));
    baseRate *= std::clamp(hpFactor, 0.5f, 1.5f);

    // Adjust for numerical advantage (capped at ±0.3 contribution)
    baseRate += std::clamp(ctx.supportDiff * 0.1f, -0.3f, 0.3f);

    return std::clamp(baseRate, 0.0f, 1.0f);
}

float BotBGAIMgr::ComputeTerritoryFactor(uint32 mapId, TeamId teamId, float x, float y)
{
    // Returns 0.0 at enemy base, 0.5 at midfield, 1.0 at own base
    float allyBaseX, allyBaseY, enemyBaseX, enemyBaseY;
    switch (mapId)
    {
        case 489: // WSG
            if (teamId == TEAM_ALLIANCE)
                { allyBaseX = 1540.f; allyBaseY = 1481.f; enemyBaseX = 916.f; enemyBaseY = 1434.f; }
            else
                { allyBaseX = 916.f; allyBaseY = 1434.f; enemyBaseX = 1540.f; enemyBaseY = 1481.f; }
            break;
        case 529: // AB
            if (teamId == TEAM_ALLIANCE)
                { allyBaseX = 1285.f; allyBaseY = 1281.f; enemyBaseX = 713.f; enemyBaseY = 1281.f; }
            else
                { allyBaseX = 713.f; allyBaseY = 1281.f; enemyBaseX = 1285.f; enemyBaseY = 1281.f; }
            break;
        case 566: // EY
            if (teamId == TEAM_ALLIANCE)
                { allyBaseX = 2523.f; allyBaseY = 1596.f; enemyBaseX = 1807.f; enemyBaseY = 1539.f; }
            else
                { allyBaseX = 1807.f; allyBaseY = 1539.f; enemyBaseX = 2523.f; enemyBaseY = 1596.f; }
            break;
        default:
            return 0.5f;
    }
    float distToAlly = std::sqrt((x - allyBaseX) * (x - allyBaseX) + (y - allyBaseY) * (y - allyBaseY));
    float distToEnemy = std::sqrt((x - enemyBaseX) * (x - enemyBaseX) + (y - enemyBaseY) * (y - enemyBaseY));
    float total = distToAlly + distToEnemy;
    if (total < 1.0f) return 0.5f;
    return std::clamp(distToEnemy / total, 0.0f, 1.0f); // 1.0 at own base, 0.0 at enemy base
}

// Focus fire management
void BotBGAIMgr::SetFocusTarget(uint32 bgInstanceId, TeamId teamId, ObjectGuid target, uint8 assignerClass)
{
    std::unique_lock lock(_lock);
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    _focusTargets[key] = BGFocusTarget{ target, getMSTime() + 8000, assignerClass };
}

ObjectGuid BotBGAIMgr::GetFocusTarget(uint32 bgInstanceId, TeamId teamId)
{
    std::shared_lock lock(_lock);
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    auto it = _focusTargets.find(key);
    if (it == _focusTargets.end())
        return ObjectGuid::Empty;
    if (getMSTime() > it->second.expiryTime)
        return ObjectGuid::Empty;
    return it->second.targetGuid;
}

void BotBGAIMgr::ClearFocusTarget(uint32 bgInstanceId, TeamId teamId)
{
    std::unique_lock lock(_lock);
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    _focusTargets.erase(key);
}

void BotBGAIMgr::CleanupExpiredFocusTargets()
{
    std::unique_lock lock(_lock);
    uint32 now = getMSTime();
    for (auto it = _focusTargets.begin(); it != _focusTargets.end();)
    {
        if (now > it->second.expiryTime)
            it = _focusTargets.erase(it);
        else
            ++it;
    }

    // Also clean expired enemy sightings (15s TTL)
    for (auto& [key, sightings] : _enemySightings)
    {
        for (auto sit = sightings.begin(); sit != sightings.end();)
        {
            if (now - sit->second.lastSeenTime > 8000)
                sit = sightings.erase(sit);
            else
                ++sit;
        }
    }
}

// =====================================================
// Team coordination plan
// =====================================================

static uint64 MakeTeamPlanKey(uint32 bgInstanceId, TeamId teamId)
{
    return (uint64(bgInstanceId) << 1) | uint64(teamId);
}

static uint8 CountAliveTeamBots(Battleground const* bg, TeamId teamId, Creature const* observer)
{
    uint32 teamVal = teamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    uint8 count = 0;
    for (auto const& [guid, botData] : bg->GetBots())
    {
        if (botData.Team != teamVal) continue;
        Creature const* bot = ObjectAccessor::GetCreature(*observer, guid);
        if (bot && bot->IsAlive()) ++count;
    }
    return count;
}

// Compute momentum for plan evaluation (mirrors bot_ai.cpp logic)
static uint8 ComputeMomentum(Battleground const* bg, TeamId teamId, Creature const* observer)
{
    uint32 myTeam = teamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    uint8 aliveAllies = 0, aliveEnemies = 0;
    for (auto const& [guid, botData] : bg->GetBots())
    {
        Creature const* bot = ObjectAccessor::GetCreature(*observer, guid);
        if (!bot || !bot->IsAlive()) continue;
        if (botData.Team == myTeam) ++aliveAllies;
        else ++aliveEnemies;
    }
    if (aliveEnemies == 0) return 4; // dominating
    float ratio = float(aliveAllies) / float(aliveEnemies);
    if (ratio >= 3.0f) return 4;      // dominating
    if (ratio >= 2.0f) return 3;      // advantage
    if (ratio >= 0.5f) return 2;      // even
    if (ratio >= 0.33f) return 1;     // outnumbered
    return 0;                          // wiped
}

static void EvaluateWSGPlan(BGTeamPlan& plan, Battleground const* bg, TeamId teamId,
    uint8 teamSize, uint8 momentum, bool openingRush)
{
    plan.activeNodeCount = 0; // WSG doesn't use node assignments

    BattlegroundWS const* wsg = dynamic_cast<BattlegroundWS const*>(bg);
    if (!wsg) return;

    TeamId enemyTeamId = teamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    uint32 myScore = bg->GetTeamScore(teamId);
    uint32 enemyScore = bg->GetTeamScore(enemyTeamId);
    bool myFCAlive = !wsg->GetFlagPickerGUID(enemyTeamId).IsEmpty(); // we carry THEIR flag
    bool enemyFCAlive = !wsg->GetFlagPickerGUID(teamId).IsEmpty(); // they carry OUR flag
    bool ourFlagOnBase = !enemyFCAlive; // our flag is home if enemy doesn't have it

    if (openingRush)
    {
        // Opening: all bots push forward, Q-learning chooses HOW they move
        // (GroupPush preset = midfield fight, Aggressive = rush flag, etc.)
        plan.flagAttackers = teamSize;
        plan.flagDefenders = 0;
        plan.fcEscorts = 0;
        plan.rally = {};
        return;
    }

    // Base attack ratio from momentum
    float attackRatio;
    switch (momentum)
    {
        case 4: attackRatio = 0.90f; break; // dominating
        case 3: attackRatio = 0.80f; break; // advantage
        case 2: attackRatio = 0.70f; break; // even
        case 1: attackRatio = 0.50f; break; // outnumbered
        default: attackRatio = 0.25f; break; // wiped
    }

    // Score adjustments
    if (myScore == 2) attackRatio = std::max(attackRatio, 0.85f); // one from winning
    if (enemyScore >= 2) attackRatio = std::min(attackRatio, 0.60f); // protect
    if (myScore == 0 && enemyScore >= 1) attackRatio = std::max(attackRatio, 0.80f); // must catch up

    uint8 totalAttackers = uint8(teamSize * attackRatio);

    // FC escorts
    plan.fcEscorts = 0;
    if (myFCAlive && totalAttackers > 0)
    {
        plan.fcEscorts = std::min(uint8(2), totalAttackers);
        totalAttackers -= plan.fcEscorts;
    }

    plan.flagAttackers = totalAttackers;
    plan.flagDefenders = ourFlagOnBase ? std::max(uint8(1), uint8(teamSize - totalAttackers - plan.fcEscorts)) : 0;

    // Rally: group up before pushing enemy flag room
    plan.rally = {};
    if (plan.flagAttackers >= 3 && !myFCAlive && momentum >= 2)
    {
        // WSG midfield rally point (approximate center of map 489)
        plan.rally.active = true;
        plan.rally.posX = 1536.0f;
        plan.rally.posY = 1466.0f;
        plan.rally.posZ = 352.0f;
        plan.rally.minGroupSize = std::min(uint8(3), plan.flagAttackers);
        plan.rally.expiryTime = getMSTime() + 15000;
        plan.rally.targetNodeIdx = 0xFF; // WSG doesn't use node index
    }
}

static void EvaluateABPlan(BGTeamPlan& plan, Battleground const* bg, TeamId teamId,
    uint8 teamSize, uint8 momentum, bool openingRush, Creature const* observer)
{
    BattlegroundAB const* ab = dynamic_cast<BattlegroundAB const*>(bg);
    if (!ab) return;

    plan.activeNodeCount = BG_AB_DYNAMIC_NODES_COUNT; // 5 nodes
    plan.flagAttackers = 0;
    plan.flagDefenders = 0;
    plan.fcEscorts = 0;

    TeamId enemyTeamId = teamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    uint32 myTeamVal = teamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    uint32 myScore = bg->GetTeamScore(teamId);
    uint32 enemyScore = bg->GetTeamScore(enemyTeamId);

    uint8 nodesOwned = 0;
    for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
    {
        if (ab->IsNodeOccupied(n, teamId)) ++nodesOwned;

        plan.nodes[n].posX = BG_AB_NodePositions[n].GetPositionX();
        plan.nodes[n].posY = BG_AB_NodePositions[n].GetPositionY();
        plan.nodes[n].currentCount = 0;
    }

    // How many nodes do we need?
    uint8 targetNodes = 3;
    BGScoreBracket sb = BotBGAIMgr::GetScoreBracket(myScore, enemyScore);
    if (sb == BG_SCORE_LOSING_BAD) targetNodes = 4;
    if (sb == BG_SCORE_WINNING_BIG) targetNodes = 3;

    if (openingRush)
    {
        // Rush 3 closest nodes: sort by distance and pick the 3 nearest
        struct NodeDist { uint8 idx; float dist; };
        std::array<NodeDist, BG_AB_DYNAMIC_NODES_COUNT> nodeDists;
        for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
            nodeDists[n] = { n, observer->GetExactDist2d(BG_AB_NodePositions[n]) };
        std::sort(nodeDists.begin(), nodeDists.end(), [](NodeDist const& a, NodeDist const& b) {
            return a.dist < b.dist;
        });

        uint8 rushCount = std::min(uint8(3), uint8(BG_AB_DYNAMIC_NODES_COUNT));
        uint8 perNode = std::max(uint8(1), uint8(teamSize / rushCount));
        for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
        {
            plan.nodes[n].intent = BG_NODE_IGNORE;
            plan.nodes[n].desiredCount = 0;
        }
        for (uint8 i = 0; i < rushCount; ++i)
        {
            plan.nodes[nodeDists[i].idx].intent = BG_NODE_ATTACK;
            plan.nodes[nodeDists[i].idx].desiredCount = perNode;
        }
        plan.rally = {};
        return;
    }

    // Wiped: regroup at closest owned node
    if (momentum == 0)
    {
        for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
        {
            if (ab->IsNodeOccupied(n, teamId))
            {
                plan.nodes[n].intent = BG_NODE_DEFEND;
                plan.nodes[n].desiredCount = teamSize;
            }
            else
            {
                plan.nodes[n].intent = BG_NODE_IGNORE;
                plan.nodes[n].desiredCount = 0;
            }
        }
        plan.rally = {};
        return;
    }

    // Assign intent per node
    uint8 urgentNodeIdx = 0xFF;
    float bestUrgentScore = 0.0f;

    for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
    {
        if (ab->IsNodeOccupied(n, teamId))
        {
            plan.nodes[n].intent = BG_NODE_DEFEND;
            plan.nodes[n].desiredCount = (nodesOwned >= targetNodes) ? 2 : 1;
        }
        else if (ab->IsNodeContested(n, teamId))
        {
            plan.nodes[n].intent = BG_NODE_DEFEND;
            plan.nodes[n].desiredCount = 2;
        }
        else if (nodesOwned < targetNodes)
        {
            plan.nodes[n].intent = BG_NODE_ATTACK;
            plan.nodes[n].desiredCount = 3;

            // Score for URGENT selection: prefer closer unowned nodes
            float dist = observer->GetExactDist2d(BG_AB_NodePositions[n]);
            float score = 500.0f - std::min(dist, 500.0f);
            // Neutral nodes are easier to cap than enemy-occupied
            if (!ab->IsNodeOccupied(n, enemyTeamId)) score += 200.0f;
            if (score > bestUrgentScore)
            {
                bestUrgentScore = score;
                urgentNodeIdx = n;
            }
        }
        else
        {
            plan.nodes[n].intent = BG_NODE_IGNORE;
            plan.nodes[n].desiredCount = 0;
        }
    }

    // Mark best attack target as URGENT if we need more nodes
    if (urgentNodeIdx < BG_AB_DYNAMIC_NODES_COUNT && nodesOwned < targetNodes)
    {
        plan.nodes[urgentNodeIdx].intent = BG_NODE_URGENT;
        plan.nodes[urgentNodeIdx].desiredCount = std::min(uint8(teamSize / 2), uint8(5));
    }

    // Budget: scale desiredCounts to not exceed teamSize
    uint8 totalDesired = 0;
    for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
        totalDesired += plan.nodes[n].desiredCount;
    if (totalDesired > teamSize && totalDesired > 0)
    {
        float scale = float(teamSize) / float(totalDesired);
        for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
        {
            plan.nodes[n].desiredCount = std::max(
                uint8(plan.nodes[n].intent > BG_NODE_IGNORE ? 1 : 0),
                uint8(plan.nodes[n].desiredCount * scale));
        }
        // Post-scaling clamp: if minimums pushed total over budget, trim lowest-priority nodes
        totalDesired = 0;
        for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
            totalDesired += plan.nodes[n].desiredCount;
        while (totalDesired > teamSize)
        {
            // Find the lowest-priority node with desiredCount > 1 and reduce it
            uint8 trimNode = 0xFF;
            uint8 lowestIntent = BG_NODE_URGENT + 1;
            for (uint8 n = 0; n < BG_AB_DYNAMIC_NODES_COUNT; ++n)
            {
                if (plan.nodes[n].desiredCount > 1 && plan.nodes[n].intent < lowestIntent)
                {
                    lowestIntent = plan.nodes[n].intent;
                    trimNode = n;
                }
            }
            if (trimNode == 0xFF) break; // can't trim further
            --plan.nodes[trimNode].desiredCount;
            --totalDesired;
        }
    }

    // Rally for coordinated push on URGENT node
    plan.rally = {};
    if (urgentNodeIdx < BG_AB_DYNAMIC_NODES_COUNT &&
        plan.nodes[urgentNodeIdx].desiredCount >= 3 && momentum >= 2)
    {
        // Rally point: midway between observer and urgent target
        plan.rally.active = true;
        plan.rally.posX = (observer->GetPositionX() + plan.nodes[urgentNodeIdx].posX) * 0.5f;
        plan.rally.posY = (observer->GetPositionY() + plan.nodes[urgentNodeIdx].posY) * 0.5f;
        plan.rally.posZ = observer->GetPositionZ();
        plan.rally.minGroupSize = 3;
        plan.rally.expiryTime = getMSTime() + 12000;
        plan.rally.targetNodeIdx = urgentNodeIdx;
    }
}

static void EvaluateEYPlan(BGTeamPlan& plan, Battleground const* bg, TeamId teamId,
    uint8 teamSize, uint8 momentum, bool openingRush, Creature const* observer)
{
    BattlegroundEY const* ey = dynamic_cast<BattlegroundEY const*>(bg);
    if (!ey) return;

    plan.activeNodeCount = EY_POINTS_MAX; // 4 points
    plan.flagAttackers = 0;
    plan.flagDefenders = 0;
    plan.fcEscorts = 0;

    TeamId enemyTeamId = teamId == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;
    uint32 myTeamVal = teamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    uint32 myScore = bg->GetTeamScore(teamId);
    uint32 enemyScore = bg->GetTeamScore(enemyTeamId);

    uint8 pointsOwned = 0;
    for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
    {
        if (ey->GetPointOwner(p) == teamId) ++pointsOwned;
        plan.nodes[p].posX = BG_EY_TriggerPositions[p].GetPositionX();
        plan.nodes[p].posY = BG_EY_TriggerPositions[p].GetPositionY();
        plan.nodes[p].currentCount = 0;
    }

    // Flag carrier escort
    // Flag carrier escort allocation
    ObjectGuid fcGuid = ey->GetFlagPickerGUID();
    if (!fcGuid.IsEmpty())
    {
        // Check if FC is on our team
        if (bg->GetBotTeamId(fcGuid) == teamId)
        {
            plan.fcEscorts = std::min(uint8(2), uint8(teamSize > 3 ? 2 : 1));
        }
    }

    BGScoreBracket sb = BotBGAIMgr::GetScoreBracket(myScore, enemyScore);
    uint8 targetPoints = 3;
    if (sb == BG_SCORE_LOSING_BAD) targetPoints = 3;
    if (sb == BG_SCORE_WINNING_BIG) targetPoints = 2;

    if (openingRush)
    {
        // Rush 3 closest points
        struct PointDist { uint8 idx; float dist; };
        std::array<PointDist, EY_POINTS_MAX> pointDists;
        for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
            pointDists[p] = { p, observer->GetExactDist2d(BG_EY_TriggerPositions[p]) };
        std::sort(pointDists.begin(), pointDists.end(), [](PointDist const& a, PointDist const& b) {
            return a.dist < b.dist;
        });

        uint8 rushCount = std::min(uint8(3), uint8(EY_POINTS_MAX));
        uint8 perPoint = std::max(uint8(1), uint8(teamSize / rushCount));
        for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
        {
            plan.nodes[p].intent = BG_NODE_IGNORE;
            plan.nodes[p].desiredCount = 0;
        }
        for (uint8 i = 0; i < rushCount; ++i)
        {
            plan.nodes[pointDists[i].idx].intent = BG_NODE_ATTACK;
            plan.nodes[pointDists[i].idx].desiredCount = perPoint;
        }
        plan.rally = {};
        return;
    }

    uint8 urgentIdx = 0xFF;
    float bestScore = 0.0f;

    for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
    {
        TeamId owner = ey->GetPointOwner(p);
        if (owner == teamId)
        {
            plan.nodes[p].intent = BG_NODE_DEFEND;
            plan.nodes[p].desiredCount = (pointsOwned >= targetPoints) ? 2 : 1;
        }
        else if (pointsOwned < targetPoints)
        {
            plan.nodes[p].intent = BG_NODE_ATTACK;
            plan.nodes[p].desiredCount = 3;

            float dist = observer->GetExactDist2d(BG_EY_TriggerPositions[p]);
            float score = 500.0f - std::min(dist, 500.0f);
            if (owner == TEAM_NEUTRAL) score += 200.0f;
            if (score > bestScore) { bestScore = score; urgentIdx = p; }
        }
        else
        {
            plan.nodes[p].intent = BG_NODE_IGNORE;
            plan.nodes[p].desiredCount = 0;
        }
    }

    if (urgentIdx < EY_POINTS_MAX && pointsOwned < targetPoints)
    {
        plan.nodes[urgentIdx].intent = BG_NODE_URGENT;
        plan.nodes[urgentIdx].desiredCount = std::min(uint8(teamSize / 2), uint8(5));
    }

    // Budget
    uint8 totalDesired = plan.fcEscorts;
    for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
        totalDesired += plan.nodes[p].desiredCount;
    if (totalDesired > teamSize && totalDesired > 0)
    {
        uint8 nodePool = (teamSize > plan.fcEscorts) ? teamSize - plan.fcEscorts : 0;
        uint8 nodeTotal = totalDesired - plan.fcEscorts;
        if (nodeTotal > 0)
        {
            float scale = float(nodePool) / float(nodeTotal);
            for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
                plan.nodes[p].desiredCount = std::max(uint8(plan.nodes[p].intent > BG_NODE_IGNORE ? 1 : 0),
                    uint8(plan.nodes[p].desiredCount * scale));
        }
        // Post-scaling clamp: trim lowest-priority if still over budget
        totalDesired = plan.fcEscorts;
        for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
            totalDesired += plan.nodes[p].desiredCount;
        while (totalDesired > teamSize)
        {
            uint8 trimPoint = 0xFF;
            uint8 lowestIntent = BG_NODE_URGENT + 1;
            for (uint8 p = 0; p < EY_POINTS_MAX; ++p)
            {
                if (plan.nodes[p].desiredCount > 1 && plan.nodes[p].intent < lowestIntent)
                {
                    lowestIntent = plan.nodes[p].intent;
                    trimPoint = p;
                }
            }
            if (trimPoint == 0xFF) break;
            --plan.nodes[trimPoint].desiredCount;
            --totalDesired;
        }
    }

    // Rally
    plan.rally = {};
    if (urgentIdx < EY_POINTS_MAX && plan.nodes[urgentIdx].desiredCount >= 3 && momentum >= 2)
    {
        plan.rally.active = true;
        plan.rally.posX = (observer->GetPositionX() + plan.nodes[urgentIdx].posX) * 0.5f;
        plan.rally.posY = (observer->GetPositionY() + plan.nodes[urgentIdx].posY) * 0.5f;
        plan.rally.posZ = observer->GetPositionZ();
        plan.rally.minGroupSize = 3;
        plan.rally.expiryTime = getMSTime() + 12000;
        plan.rally.targetNodeIdx = urgentIdx;
    }
}

std::optional<BGTeamPlan> BotBGAIMgr::GetTeamPlan(uint32 bgInstanceId, TeamId teamId)
{
    std::shared_lock lock(_lock);
    uint64 key = MakeTeamPlanKey(bgInstanceId, teamId);
    auto it = _teamPlans.find(key);
    if (it == _teamPlans.end())
        return std::nullopt;
    return it->second; // return copy, safe after lock releases
}

void BotBGAIMgr::MaybeUpdateTeamPlan(uint32 bgInstanceId, TeamId teamId,
    Creature const* evaluator, Battleground const* bg)
{
    if (!bg || !evaluator) return;

    uint64 key = MakeTeamPlanKey(bgInstanceId, teamId);

    // Fast check: is plan still fresh?
    {
        std::shared_lock lock(_lock);
        auto it = _teamPlans.find(key);
        if (it != _teamPlans.end() && getMSTime() - it->second.lastEvalTime < 5000)
            return;
    }

    // Only smart bots update the plan
    BotBGPersonality p = ComputePersonality(evaluator->GetEntry());
    if (!IntelligenceCheck(p.intelligence))
        return;

    std::unique_lock lock(_lock);
    auto& plan = _teamPlans[key];

    // Double-check after acquiring lock
    if (getMSTime() - plan.lastEvalTime < 2000)
        return;

    uint8 teamSize = CountAliveTeamBots(bg, teamId, evaluator);
    if (teamSize == 0) return;

    uint8 momentum = ComputeMomentum(bg, teamId, evaluator);
    bool openingRush = bg->GetStartTime() < 210000;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_WS:
            EvaluateWSGPlan(plan, bg, teamId, teamSize, momentum, openingRush);
            break;
        case BATTLEGROUND_AB:
            EvaluateABPlan(plan, bg, teamId, teamSize, momentum, openingRush, evaluator);
            break;
        case BATTLEGROUND_EY:
            EvaluateEYPlan(plan, bg, teamId, teamSize, momentum, openingRush, evaluator);
            break;
        default:
            // AV etc. not yet supported — mark as evaluated to prevent re-entry
            plan.lastEvalTime = getMSTime();
            return;
    }

    plan.lastEvalTime = getMSTime();
    plan.planVersion++;
}

void BotBGAIMgr::IncrementRallyArrived(uint32 bgInstanceId, TeamId teamId)
{
    std::unique_lock lock(_lock);
    uint64 key = MakeTeamPlanKey(bgInstanceId, teamId);
    auto it = _teamPlans.find(key);
    if (it != _teamPlans.end() && it->second.rally.active)
        ++it->second.rally.arrivedCount;
}

void BotBGAIMgr::ClearTeamPlan(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _teamPlans.erase(MakeTeamPlanKey(bgInstanceId, TEAM_ALLIANCE));
    _teamPlans.erase(MakeTeamPlanKey(bgInstanceId, TEAM_HORDE));
}

// Patrol hotspots: find heatmap cells with highest engagement around a position
std::vector<BGPatrolPoint> BotBGAIMgr::GetPatrolHotspots(
    uint32 mapId, float centerX, float centerY, float radius, uint8 maxPoints)
{
    std::shared_lock lock(_lock);

    int16 cGridX = SnapToGrid(centerX);
    int16 cGridY = SnapToGrid(centerY);
    int16 gridRadius = int16(std::ceil(radius / 3.0f));
    float r2 = radius * radius;

    std::vector<BGPatrolPoint> points;

    for (int16 gx = cGridX - gridRadius; gx <= cGridX + gridRadius; ++gx)
    {
        for (int16 gy = cGridY - gridRadius; gy <= cGridY + gridRadius; ++gy)
        {
            BGGridCell cell{ mapId, gx, gy };
            auto it = _heatmap.find(cell);
            if (it == _heatmap.end()) continue;

            float px = float(gx) * 3.0f + 1.5f;
            float py = float(gy) * 3.0f + 1.5f;
            float dx = px - centerX, dy = py - centerY;
            if (dx * dx + dy * dy > r2) continue;

            float engagement = float(it->second.kills + it->second.deaths);
            if (engagement < 2.0f) continue;

            points.push_back(BGPatrolPoint{ px, py, engagement });
        }
    }

    std::sort(points.begin(), points.end(),
        [](auto const& a, auto const& b) { return a.engagementScore > b.engagementScore; });

    if (points.size() > maxPoints)
        points.resize(maxPoints);

    return points;
}

// Enemy sighting management
void BotBGAIMgr::RecordEnemySighting(uint32 bgInstanceId, TeamId team,
    ObjectGuid enemy, float x, float y, uint8 cls)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(team);
    std::unique_lock lock(_lock);
    _enemySightings[key][enemy] = BGEnemySighting{ x, y, getMSTime(), cls };
}

uint8 BotBGAIMgr::GetEnemyConcentration(uint32 bgInstanceId, TeamId team,
    float x, float y, float radius, uint32 maxAgeMs)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(team);
    std::shared_lock lock(_lock);
    auto it = _enemySightings.find(key);
    if (it == _enemySightings.end()) return 0;

    uint32 now = getMSTime();
    float r2 = radius * radius;
    uint8 count = 0;
    for (auto const& [guid, s] : it->second)
    {
        if (now - s.lastSeenTime > maxAgeMs) continue;
        float dx = s.posX - x, dy = s.posY - y;
        if (dx * dx + dy * dy <= r2)
            ++count;
    }
    return count;
}

void BotBGAIMgr::ClearEnemySightings(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    uint64 key0 = (uint64(bgInstanceId) << 1) | uint64(TEAM_ALLIANCE);
    uint64 key1 = (uint64(bgInstanceId) << 1) | uint64(TEAM_HORDE);
    _enemySightings.erase(key0);
    _enemySightings.erase(key1);
}

// =====================================================
// Potential Field Navigation
// =====================================================

static BGPresenceCell MakePresenceCell(float x, float y)
{
    return BGPresenceCell{ int16(std::floor(x / PF_PRESENCE_CELL_SIZE)), int16(std::floor(y / PF_PRESENCE_CELL_SIZE)) };
}

void BotBGAIMgr::UpdatePresenceGrid(uint32 bgInstanceId, TeamId teamId,
    Battleground const* bg, Creature const* observer)
{
    if (!bg || !observer) return;
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);

    std::unique_lock lock(_lock);
    auto& grid = _presenceGrids[key];

    uint32 now = getMSTime();
    if (grid.lastUpdateTime > 0 && now - grid.lastUpdateTime < 500) return; // throttle: max 2x per second

    // Decay all cells (skip on first call when lastUpdateTime is 0)
    if (grid.lastUpdateTime > 0)
    {
        float deltaSeconds = float(now - grid.lastUpdateTime) / 1000.0f;
        float decayFactor = std::pow(0.98f, deltaSeconds);
        for (auto it = grid.cells.begin(); it != grid.cells.end();)
        {
            it->second *= decayFactor;
            if (it->second < 0.01f)
                it = grid.cells.erase(it);
            else
                ++it;
        }
    }
    grid.lastUpdateTime = now;

    // Increment cells for each alive ally bot
    uint32 myTeam = teamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    for (auto const& [guid, botData] : bg->GetBots())
    {
        if (botData.Team != myTeam) continue;
        Creature const* bot = ObjectAccessor::GetCreature(*observer, guid);
        if (!bot || !bot->IsAlive()) continue;
        BGPresenceCell cell = MakePresenceCell(bot->GetPositionX(), bot->GetPositionY());
        grid.cells[cell] = std::min(1.0f, grid.cells[cell] + 0.15f);
    }
}

float BotBGAIMgr::GetPresenceValue(uint32 bgInstanceId, TeamId teamId, float x, float y)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    std::shared_lock lock(_lock);
    auto it = _presenceGrids.find(key);
    if (it == _presenceGrids.end()) return 0.0f;
    BGPresenceCell cell = MakePresenceCell(x, y);
    auto cit = it->second.cells.find(cell);
    return (cit != it->second.cells.end()) ? cit->second : 0.0f;
}

void BotBGAIMgr::GetPresenceAvoidanceVector(uint32 bgInstanceId, TeamId teamId,
    float x, float y, float& outX, float& outY)
{
    outX = outY = 0.0f;
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    std::shared_lock lock(_lock);
    auto it = _presenceGrids.find(key);
    if (it == _presenceGrids.end()) return;

    BGPresenceCell center = MakePresenceCell(x, y);
    // Scan 3x3 neighborhood, compute avoidance (away from high-presence cells)
    for (int16 dx = -1; dx <= 1; ++dx)
    {
        for (int16 dy = -1; dy <= 1; ++dy)
        {
            if (dx == 0 && dy == 0) continue;
            BGPresenceCell neighbor{ int16(center.cellX + dx), int16(center.cellY + dy) };
            auto cit = it->second.cells.find(neighbor);
            if (cit == it->second.cells.end()) continue;
            float presence = cit->second;
            // Vector FROM high-presence cell (away from it)
            float cellCenterX = float(neighbor.cellX) * PF_PRESENCE_CELL_SIZE + PF_PRESENCE_CELL_SIZE * 0.5f;
            float cellCenterY = float(neighbor.cellY) * PF_PRESENCE_CELL_SIZE + PF_PRESENCE_CELL_SIZE * 0.5f;
            float dirX = x - cellCenterX;
            float dirY = y - cellCenterY;
            float dist = std::sqrt(dirX * dirX + dirY * dirY);
            if (dist > 0.1f)
            {
                outX += (dirX / dist) * presence;
                outY += (dirY / dist) * presence;
            }
        }
    }
}

void BotBGAIMgr::ClearPresenceGrid(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _presenceGrids.erase((uint64(bgInstanceId) << 1) | uint64(TEAM_ALLIANCE));
    _presenceGrids.erase((uint64(bgInstanceId) << 1) | uint64(TEAM_HORDE));
}

// Intention broadcasting
void BotBGAIMgr::BroadcastIntention(uint32 bgInstanceId, TeamId teamId, ObjectGuid botGuid, uint8 intentType, uint8 targetNode)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    std::unique_lock lock(_lock);
    _teamIntentions[key][botGuid] = BGBotIntention{ intentType, targetNode, getMSTime() };
}

uint8 BotBGAIMgr::CountIntentions(uint32 bgInstanceId, TeamId teamId, uint8 intentType, uint8 targetNode)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    std::shared_lock lock(_lock);
    auto it = _teamIntentions.find(key);
    if (it == _teamIntentions.end()) return 0;
    uint8 count = 0;
    uint32 now = getMSTime();
    for (auto const& [guid, intent] : it->second)
    {
        if (now - intent.timestamp > 10000) continue; // stale intention (10s)
        if (intent.intentType == intentType && (targetNode == 0xFF || intent.targetNodeIdx == targetNode))
            ++count;
    }
    return count;
}

void BotBGAIMgr::ClearIntentions(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _teamIntentions.erase((uint64(bgInstanceId) << 1) | uint64(TEAM_ALLIANCE));
    _teamIntentions.erase((uint64(bgInstanceId) << 1) | uint64(TEAM_HORDE));
}

// Cooldown communication
void BotBGAIMgr::BroadcastCooldown(uint32 bgInstanceId, TeamId teamId, uint32 durationMs, uint8 type)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    std::unique_lock lock(_lock);
    _teamCooldowns[key].push_back(BGTeamCooldown{ getMSTime() + durationMs, type });
}

bool BotBGAIMgr::HasActiveTeamCooldown(uint32 bgInstanceId, TeamId teamId)
{
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);
    // Use unique_lock to prune expired entries while checking
    std::unique_lock lock(_lock);
    auto it = _teamCooldowns.find(key);
    if (it == _teamCooldowns.end()) return false;
    uint32 now = getMSTime();
    bool hasActive = false;
    std::erase_if(it->second, [now](BGTeamCooldown const& cd) { return now >= cd.expiryTime; });
    for (auto const& cd : it->second)
        if (now < cd.expiryTime) { hasActive = true; break; }
    if (it->second.empty())
        _teamCooldowns.erase(it);
    return hasActive;
}

void BotBGAIMgr::ClearCooldowns(uint32 bgInstanceId)
{
    std::unique_lock lock(_lock);
    _teamCooldowns.erase((uint64(bgInstanceId) << 1) | uint64(TEAM_ALLIANCE));
    _teamCooldowns.erase((uint64(bgInstanceId) << 1) | uint64(TEAM_HORDE));
}

void BotBGAIMgr::ComputeAllyRepulsion(Creature const* me, Battleground const* bg,
    TeamId teamId, float& outX, float& outY, bool isFC, bool isHealer)
{
    outX = outY = 0.0f;
    if (!me || !bg) return;

    float radius = PF_REPULSION_RADIUS;
    if (isFC) radius *= 1.5f;
    else if (isHealer) radius *= 0.6f;

    uint32 myTeam = teamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;
    for (auto const& [guid, botData] : bg->GetBots())
    {
        if (botData.Team != myTeam) continue;
        Creature const* ally = ObjectAccessor::GetCreature(*me, guid);
        if (!ally || ally == me || !ally->IsAlive()) continue;

        float dist = me->GetExactDist2d(ally);
        if (dist >= radius || dist < 0.5f) continue;

        // Direction: ally → self (push away)
        float dirX = me->GetPositionX() - ally->GetPositionX();
        float dirY = me->GetPositionY() - ally->GetPositionY();
        float magnitude = 1.0f / std::max(dist * dist, 4.0f); // inverse-square, clamped
        float len = std::sqrt(dirX * dirX + dirY * dirY);
        if (len > 0.1f)
        {
            outX += (dirX / len) * magnitude;
            outY += (dirY / len) * magnitude;
        }
    }
}

void BotBGAIMgr::ComputeEnemyVector(uint32 bgInstanceId, TeamId teamId,
    float x, float y, float aggression, bool isFC, float& outX, float& outY)
{
    outX = outY = 0.0f;
    uint64 key = (uint64(bgInstanceId) << 1) | uint64(teamId);

    std::shared_lock lock(_lock);
    auto it = _enemySightings.find(key);
    if (it == _enemySightings.end()) return;

    uint32 now = getMSTime();
    for (auto const& [guid, s] : it->second)
    {
        if (now - s.lastSeenTime > 10000) continue;
        float dx = s.posX - x, dy = s.posY - y;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > 40.0f || dist < 1.0f) continue;

        float dirX = dx / dist;
        float dirY = dy / dist;
        float magnitude = 1.0f / dist;

        // FC flees, aggressive seeks, others mildly avoid
        if (isFC)
        {
            outX -= dirX * magnitude; // flee
            outY -= dirY * magnitude;
        }
        else if (aggression > 0.6f)
        {
            outX += dirX * magnitude; // seek
            outY += dirY * magnitude;
        }
        else
        {
            outX -= dirX * magnitude * 0.5f; // mild avoidance
            outY -= dirY * magnitude * 0.5f;
        }
    }
}

BotNavigationContext BotBGAIMgr::ComputePotentialField(
    Creature const* me, Battleground const* bg, Position const& objectivePos,
    BotBGPersonality const& p, uint8 momentum, bool isFC, bool isHealer,
    BGQWeightPreset const* qPreset)
{
    BotNavigationContext ctx;
    if (!me || !bg) return ctx;

    float myX = me->GetPositionX(), myY = me->GetPositionY();

    // 1. Objective attraction: direction toward objective
    float objDx = objectivePos.m_positionX - myX;
    float objDy = objectivePos.m_positionY - myY;
    float objDist = std::sqrt(objDx * objDx + objDy * objDy);
    if (objDist > 0.1f)
    {
        ctx.objAttrX = objDx / objDist;
        ctx.objAttrY = objDy / objDist;
    }

    // 2. Ally repulsion
    TeamId teamId = bg->GetBotTeamId(me->GetGUID());
    ComputeAllyRepulsion(me, bg, teamId, ctx.repulsionX, ctx.repulsionY, isFC, isHealer);

    // 3. Presence avoidance
    uint32 bgInstanceId = bg->GetInstanceID();
    GetPresenceAvoidanceVector(bgInstanceId, teamId, myX, myY, ctx.presenceX, ctx.presenceY);

    // 4. Enemy vector
    ComputeEnemyVector(bgInstanceId, teamId, myX, myY, p.aggression, isFC, ctx.enemyX, ctx.enemyY);

    // 4b. Wall avoidance: repel from nearby cells with high wall hits
    // Scans a 20-yard radius for wall-hit cells and creates repulsion from them
    {
        auto wallWaypoints = GetLearnedWaypointsNear(me->GetMapId(), myX, myY, 20.0f);
        for (auto const& wp : wallWaypoints)
        {
            if (wp.wallHits < 2) continue; // ignore minor hits
            float dx = myX - wp.pos.GetPositionX();
            float dy = myY - wp.pos.GetPositionY();
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 1.0f) dist = 1.0f;
            // Stronger repulsion for more wall hits, inverse with distance
            float strength = std::min(float(wp.wallHits), 10.0f) / (dist * dist);
            ctx.wallAvoidX += (dx / dist) * strength;
            ctx.wallAvoidY += (dy / dist) * strength;
        }
        // Normalize if significant
        float wallMag = std::sqrt(ctx.wallAvoidX * ctx.wallAvoidX + ctx.wallAvoidY * ctx.wallAvoidY);
        if (wallMag > 0.1f)
        {
            ctx.wallAvoidX /= wallMag;
            ctx.wallAvoidY /= wallMag;
        }
    }

    // 5. Compute weights: Q-learned preset as base, personality modulation on top
    float wObj, wRepulsion, wPresence, wEnemy;
    if (qPreset)
    {
        wObj = qPreset->wObj;
        wRepulsion = qPreset->wRepulsion;
        wPresence = qPreset->wPresence;
        wEnemy = qPreset->wEnemy;
    }
    else
    {
        wObj = PF_WEIGHT_OBJECTIVE;
        wRepulsion = PF_WEIGHT_REPULSION;
        wPresence = PF_WEIGHT_PRESENCE;
        wEnemy = PF_WEIGHT_ENEMY_DEFAULT;
    }
    // Personality modulation on top of Q-learned (or default) base
    if (isFC) wObj = std::max(wObj, 1.5f);
    if (isHealer)
    {
        wRepulsion = std::min(wRepulsion, 0.15f); // stay near allies
        wPresence = std::min(wPresence, 0.05f);   // don't avoid allied clusters
        wObj *= 0.7f;                              // reduced objective pull (don't stand on flag)
        wEnemy = std::max(wEnemy, 0.35f);          // stronger enemy avoidance (stay at range)
    }
    else
    {
        wRepulsion *= (0.75f + (1.0f - p.groupTendency) * 0.5f);
        wPresence *= (0.75f + (1.0f - p.groupTendency) * 0.5f);
    }
    if (!isFC && p.aggression > 0.6f) wEnemy = std::min(wEnemy, PF_WEIGHT_ENEMY_AGGRO);

    // Near objective: reduce spreading forces to allow clustering for caps
    if (objDist < 15.0f)
    {
        wRepulsion = 0.1f;
        wPresence = 0.05f;
    }

    // Wall avoidance weight — scales with how much wall data exists nearby
    float wWall = 0.5f;

    // Cooldown communication: push harder when team has active major cooldowns
    if (HasActiveTeamCooldown(bgInstanceId, teamId))
        wObj *= 1.2f;

    // 6. Weighted sum (5 forces)
    float sumX = wObj * ctx.objAttrX + wRepulsion * ctx.repulsionX + wPresence * ctx.presenceX + wEnemy * ctx.enemyX + wWall * ctx.wallAvoidX;
    float sumY = wObj * ctx.objAttrY + wRepulsion * ctx.repulsionY + wPresence * ctx.presenceY + wEnemy * ctx.enemyY + wWall * ctx.wallAvoidY;

    ctx.finalMagnitude = std::sqrt(sumX * sumX + sumY * sumY);
    if (ctx.finalMagnitude > 0.1f)
    {
        ctx.finalDirX = sumX / ctx.finalMagnitude;
        ctx.finalDirY = sumY / ctx.finalMagnitude;
    }
    else
    {
        // Forces cancel out — fall back to pure objective direction
        ctx.finalDirX = ctx.objAttrX;
        ctx.finalDirY = ctx.objAttrY;
        ctx.finalMagnitude = 0.1f;
    }

    return ctx;
}

// =====================================================
// Q-Learning System
// =====================================================

const BGQWeightPreset BotBGAIMgr::_qPresets[BG_QACTION_MAX] = {
    { 1.0f, 0.40f, 0.20f,  0.10f }, // BALANCED
    { 0.8f, 0.20f, 0.10f, -0.30f }, // AGGRESSIVE
    { 1.2f, 0.50f, 0.30f,  0.30f }, // DEFENSIVE
    { 1.0f, 0.15f, 0.05f,  0.00f }, // GROUP_PUSH
    { 0.7f, 0.60f, 0.40f, -0.10f }, // SOLO_FLANK
    { 1.5f, 0.60f, 0.10f,  0.50f }, // FC_RUN
    { 0.9f, 0.15f, 0.05f,  0.20f }, // HEALER_PROT
};

static uint32 MakeQKey(uint8 role, uint16 stateKey, uint8 action, uint8 personalityBucket = 0)
{
    // role(3 bits) | personalityBucket(1 bit) | stateKey(16 bits) | action(8 bits)
    return (uint32(role) << 25) | (uint32(personalityBucket & 1) << 24) | (uint32(stateKey) << 8) | uint32(action);
}

uint16 BotBGAIMgr::ComputeQStateKey(uint32 mapId, uint8 momentum, uint8 scoreBracket, uint8 timeBracket, uint8 bgContext)
{
    uint8 bgIdx;
    switch (mapId)
    {
        case 489: bgIdx = 0; break; // WSG
        case 529: bgIdx = 1; break; // AB
        case 566: bgIdx = 2; break; // EY
        default:  bgIdx = 0; break;
    }
    // 3 BGs × 5 momentum × 5 score × 4 time × 3 context = 900 states
    return uint16(bgIdx * 300 + momentum * 60 + scoreBracket * 12 + timeBracket * 3 + std::min(bgContext, uint8(2)));
}

uint8 BotBGAIMgr::DetermineQRole(bool isFC, bool isHealer, uint8 assignedRole)
{
    if (isFC) return BG_QROLE_FC;
    if (isHealer) return BG_QROLE_HEALER;
    if (assignedRole == 2) return BG_QROLE_DEFENDER;
    return BG_QROLE_ATTACKER;
}

uint8 BotBGAIMgr::SelectQAction(uint8 role, uint16 stateKey, float intelligence, uint8 personalityBucket)
{
    // Small epsilon floor for pure random exploration
    float epsilon = 0.05f;
    if (intelligence < 0.4f)
        epsilon += 0.2f;
    if (frand(0.0f, 1.0f) < epsilon)
        return uint8(urand(0, BG_QACTION_MAX - 1));

    // UCB1 exploration: Q(s,a) + C * sqrt(ln(totalVisits) / visitCount(s,a))
    // Naturally explores undervisited actions while exploiting well-known good ones
    constexpr float UCB_C = 1.5f;

    std::shared_lock lock(_lock);

    // Sum total visits across all actions for this state
    uint32 totalVisits = 0;
    for (uint8 a = 0; a < BG_QACTION_MAX; ++a)
    {
        auto it = _qTable.find(MakeQKey(role, stateKey, a, personalityBucket));
        if (it != _qTable.end())
            totalVisits += it->second.visitCount;
    }
    float lnTotal = (totalVisits > 0) ? std::log(float(totalVisits)) : 0.0f;

    float bestUCB = -999.0f;
    uint8 bestAction = BG_QACTION_BALANCED;
    for (uint8 a = 0; a < BG_QACTION_MAX; ++a)
    {
        uint32 key = MakeQKey(role, stateKey, a, personalityBucket);
        auto it = _qTable.find(key);

        float q = 0.0f;
        uint32 visits = 0;
        if (it != _qTable.end())
        {
            q = it->second.qValue;
            visits = it->second.visitCount;
        }

        // State interpolation: blend with neighbors when low confidence
        if (visits < 10)
        {
            float totalW = (visits > 0) ? 1.0f : 0.0f;
            float blendedQ = q * totalW;
            // Decompose state key to safely compute neighbors
            // State = bgIdx*300 + momentum*60 + score*12 + time*3 + context
            uint16 bgBase = (stateKey / 300) * 300; // bgIdx portion
            uint8 mom = uint8((stateKey / 60) % 5);
            uint8 score = uint8((stateKey / 12) % 5);
            // Check momentum±1 neighbors (stay within same bgIdx)
            for (int8 mOff = -1; mOff <= 1; mOff += 2)
            {
                int newMom = int(mom) + mOff;
                if (newMom < 0 || newMom > 4) continue; // out of range
                int16 keyDelta = int16(mOff) * 60;
                int32 neighborKey32 = int32(stateKey) + keyDelta;
                if (neighborKey32 < int32(bgBase) || neighborKey32 >= int32(bgBase + 300)) continue; // crossed bgIdx boundary
                uint16 neighborKey = uint16(neighborKey32);
                auto nit = _qTable.find(MakeQKey(role, neighborKey, a, personalityBucket));
                if (nit != _qTable.end() && nit->second.visitCount > 0)
                {
                    blendedQ += nit->second.qValue * 0.3f;
                    totalW += 0.3f;
                }
            }
            // Check scoreBracket±1 neighbors
            for (int8 sOff = -1; sOff <= 1; sOff += 2)
            {
                int newScore = int(score) + sOff;
                if (newScore < 0 || newScore > 4) continue;
                int16 keyDelta = int16(sOff) * 12;
                int32 neighborKey32 = int32(stateKey) + keyDelta;
                if (neighborKey32 < int32(bgBase) || neighborKey32 >= int32(bgBase + 300)) continue;
                uint16 neighborKey = uint16(neighborKey32);
                auto nit = _qTable.find(MakeQKey(role, neighborKey, a, personalityBucket));
                if (nit != _qTable.end() && nit->second.visitCount > 0)
                {
                    blendedQ += nit->second.qValue * 0.3f;
                    totalW += 0.3f;
                }
            }
            if (totalW > 0.0f)
                q = blendedQ / totalW;
        }

        // UCB bonus: high for unvisited actions, low for well-explored ones
        float explorationBonus = (visits > 0) ? UCB_C * std::sqrt(lnTotal / float(visits)) : UCB_C * 3.0f;
        float ucbScore = q + explorationBonus;

        if (ucbScore > bestUCB) { bestUCB = ucbScore; bestAction = a; }
    }
    return bestAction;
}

BGQWeightPreset const& BotBGAIMgr::GetQWeightPreset(uint8 action)
{
    return _qPresets[std::min(action, uint8(BG_QACTION_MAX - 1))];
}

BGQWeightPreset BotBGAIMgr::GetQWeightPresetWithOffsets(uint8 role, uint16 stateKey, uint8 action, uint8 personalityBucket)
{
    BGQWeightPreset preset = _qPresets[std::min(action, uint8(BG_QACTION_MAX - 1))];

    std::shared_lock lock(_lock);
    uint32 key = MakeQKey(role, stateKey, action, personalityBucket);
    auto it = _qTable.find(key);
    if (it != _qTable.end())
    {
        preset.wObj += it->second.wObjOff;
        preset.wRepulsion += it->second.wRepOff;
        preset.wPresence += it->second.wPresOff;
        preset.wEnemy += it->second.wEnemyOff;
    }
    return preset;
}

void BotBGAIMgr::UpdateQValues(std::vector<BGQEpisodeStep> const& episode, float reward)
{
    if (episode.empty()) return;

    constexpr float ALPHA = 0.1f;
    int totalSteps = int(episode.size());

    std::unique_lock lock(_lock);

    // Weighted Monte Carlo with recency bias:
    // Last step gets full weight (1.0), first step gets 50% weight (0.5)
    // Much better credit assignment than gamma^N which approaches 0 for early decisions
    for (int i = totalSteps - 1; i >= 0; --i)
    {
        float recencyWeight = 1.0f - (float(totalSteps - 1 - i) / float(std::max(totalSteps - 1, 1))) * 0.5f;
        uint32 key = MakeQKey(episode[i].role, episode[i].stateKey, episode[i].action, episode[i].personalityBucket);
        auto& entry = _qTable[key];
        entry.qValue += ALPHA * recencyWeight * (reward - entry.qValue);
        entry.visitCount++;

        // Continuous weight optimization: nudge offsets based on outcome
        // On positive reward: increase offsets (strengthen current weight adjustments)
        // On negative reward: decrease offsets (weaken current adjustments)
        constexpr float OFFSET_NUDGE = 0.05f;
        constexpr float OFFSET_CLAMP = 0.3f;
        float nudgeDir = (reward > 0.0f) ? OFFSET_NUDGE : -OFFSET_NUDGE;
        entry.wObjOff = std::clamp(entry.wObjOff + nudgeDir * recencyWeight * 0.1f, -OFFSET_CLAMP, OFFSET_CLAMP);
        entry.wRepOff = std::clamp(entry.wRepOff + nudgeDir * recencyWeight * 0.05f, -OFFSET_CLAMP, OFFSET_CLAMP);
        entry.wPresOff = std::clamp(entry.wPresOff + nudgeDir * recencyWeight * 0.03f, -OFFSET_CLAMP, OFFSET_CLAMP);
        entry.wEnemyOff = std::clamp(entry.wEnemyOff + nudgeDir * recencyWeight * 0.05f, -OFFSET_CLAMP, OFFSET_CLAMP);
    }
}

void BotBGAIMgr::LoadQTableFromDB()
{
    uint32 count = 0;
    if (QueryResult result = CharacterDatabase.Query(
        "SELECT role, state_key, action, q_value, visit_count, w_obj_off, w_rep_off, w_pres_off, w_enemy_off FROM characters_npcbot_bg_qtable"))
    {
        do {
            Field* f = result->Fetch();
            uint8 rolePers = f[0].GetUInt8();
            uint8 role = rolePers >> 1;
            uint8 persBucket = rolePers & 1;
            uint32 key = MakeQKey(role, f[1].GetUInt16(), f[2].GetUInt8(), persBucket);
            _qTable[key] = BGQEntry{
                f[3].GetFloat(), f[4].GetUInt32(),
                f[5].GetFloat(), f[6].GetFloat(), f[7].GetFloat(), f[8].GetFloat()
            };
            ++count;
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} BG Q-table entries", count);

    if (QueryResult result = CharacterDatabase.Query(
        "SELECT value FROM characters_npcbot_bg_qmeta WHERE key_name = 'games_played'"))
    {
        _qGamesPlayed = result->Fetch()[0].GetUInt32();
    }
    float eps = std::max(0.1f, 0.5f - 0.004f * float(_qGamesPlayed));
    TC_LOG_INFO("server.loading", ">> BG Q-learning: {} games played (epsilon = {:.2f})", _qGamesPlayed, eps);
}

void BotBGAIMgr::FlushQTableToDB()
{
    std::shared_lock lock(_lock);
    for (auto const& [key, entry] : _qTable)
    {
        if (entry.visitCount == 0 && std::abs(entry.qValue) < 0.001f) continue;
        uint8 rolePers = uint8(key >> 24); // role(3 bits) | personality(1 bit) packed
        uint16 stateKey = uint16((key >> 8) & 0xFFFF);
        uint8 action = uint8(key & 0xFF);
        CharacterDatabase.PExecute(
            "INSERT INTO characters_npcbot_bg_qtable (role, state_key, action, q_value, visit_count, w_obj_off, w_rep_off, w_pres_off, w_enemy_off) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}) ON DUPLICATE KEY UPDATE q_value = {}, visit_count = {}, w_obj_off = {}, w_rep_off = {}, w_pres_off = {}, w_enemy_off = {}",
            uint32(rolePers), uint32(stateKey), uint32(action), entry.qValue, entry.visitCount,
            entry.wObjOff, entry.wRepOff, entry.wPresOff, entry.wEnemyOff,
            entry.qValue, entry.visitCount, entry.wObjOff, entry.wRepOff, entry.wPresOff, entry.wEnemyOff);
    }
    CharacterDatabase.PExecute(
        "INSERT INTO characters_npcbot_bg_qmeta (key_name, value) VALUES ('games_played', {}) "
        "ON DUPLICATE KEY UPDATE value = {}",
        _qGamesPlayed, _qGamesPlayed);
}
