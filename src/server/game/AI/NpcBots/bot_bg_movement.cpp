// BG-specific bot_ai member function implementations
// Separated from bot_ai.cpp for maintainability

#include "Battleground.h"
#include "BattlegroundAB.h"
#include "BattlegroundAV.h"
#include "BattlegroundEY.h"
#include "BattlegroundWS.h"
#include "bot_ai.h"
#include "bot_bg_ai.h"
#include "bot_Events.h"
#include "bot_InstanceEvents.h"
#include "bot_GridNotifiers.h"
#include "botconfig.h"
#include "botdatamgr.h"
#include "botlog.h"
#include "botmgr.h"
#include "botgearscore.h"
#include "botgossip.h"
#include "botspell.h"
#include "bottext.h"
#include "botwanderful.h"
#include "bpet_ai.h"
#include "Bag.h"
#include "BattlegroundMgr.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "CharacterDatabase.h"
#include "Chat.h"
#include "CommonHelpers.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"

bool bot_ai::IsEnemyHealer(Unit const* unit) const
{
    if (!unit)
        return false;
    if (unit->IsNPCBot() && unit->ToCreature()->GetBotAI())
        return unit->ToCreature()->GetBotAI()->HasRole(BOT_ROLE_HEAL);
    if (unit->IsPlayer())
    {
        // Heuristic: check if player is currently casting a heal
        for (uint8 i = 0; i < CURRENT_MAX_SPELL; ++i)
            if (Spell const* sp = unit->GetCurrentSpell(CurrentSpellTypes(i)))
                if (sp->GetSpellInfo()->IsPositive() && sp->GetSpellInfo()->HasEffect(SPELL_EFFECT_HEAL))
                    return true;
    }
    return false;
}


Unit* bot_ai::FindBGPeelTarget() const
{
    if (!me->GetMap()->IsBattlegroundOrArena() || !IAmFree() || !IsWanderer())
        return nullptr;

    Battleground* bg = GetBG();
    if (!bg)
        return nullptr;

    BotBGPersonality peelP = BotBGAIMgr::ComputePersonality(me->GetEntry());
    if (peelP.groupTendency < 0.3f)
        return nullptr;
    if (!BotBGAIMgr::IntelligenceCheck(peelP.intelligence))
        return nullptr;
    if (GetHealthPCT(me) < 40)
        return nullptr;

    TeamId myTeamId = bg->GetBotTeamId(me->GetGUID());
    uint32 myTeam = myTeamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;

    Unit* bestPeelTarget = nullptr;
    float bestPriority = 0.0f;

    for (auto const& [guid, botData] : bg->GetBots())
    {
        if (botData.Team != myTeam)
            continue;
        Creature const* ally = ObjectAccessor::GetCreature(*me, guid);
        if (!ally || ally == me || !ally->IsAlive() || !ally->GetBotAI())
            continue;
        if (ally->GetExactDist2d(me) > 30.0f)
            continue;

        bool isFC = IsFlagCarrier(ally, bg->GetTypeID());
        bool isHealer = ally->GetBotAI()->HasRole(BOT_ROLE_HEAL);

        if (!isFC && !isHealer)
            continue;

        auto const& attackers = ally->getAttackers();
        if (attackers.empty())
            continue;

        for (Unit* attacker : attackers)
        {
            if (!CanBotAttack(attacker))
                continue;
            if (HasBreakableCC(attacker))
                continue;

            float priority = 0.0f;
            if (isFC) priority = 10.0f;
            else if (isHealer) priority = 7.0f;

            priority += (30.0f - std::min(30.0f, ally->GetExactDist2d(attacker))) / 30.0f;
            priority *= peelP.groupTendency;

            if (priority > bestPriority)
            {
                bestPriority = priority;
                bestPeelTarget = attacker;
            }
        }
    }

    return (bestPeelTarget && bestPeelTarget->IsAlive()) ? bestPeelTarget : nullptr;
}

bool bot_ai::TryBGKite(Unit* attacker, uint32 diff)
{
    if (!me->GetMap()->IsBattlegroundOrArena() || !IsRanged() || !attacker || !attacker->IsAlive())
        return false;
    if (CCed(me, true) || IsCasting() || CCed(attacker) || IsFlagCarrier(me))
        return false;

    float dist = me->GetExactDist2d(attacker);
    if (dist > 8.0f || dist < 1.0f)
        return false;

    if (_bgKiteTimer > diff)
    {
        _bgKiteTimer -= diff;
        return false;
    }

    BotBGPersonality kiteP = BotBGAIMgr::ComputePersonality(me->GetEntry());
    if (!BotBGAIMgr::IntelligenceCheck(kiteP.intelligence))
        return false;

    // Low caution bots stand ground more often
    if (kiteP.caution < 0.3f && frand(0.0f, 1.0f) < 0.5f)
        return false;

    // Direction: away from attacker with slight lateral offset
    float angle = attacker->GetAbsoluteAngle(me);
    float lateralOffset = (me->GetEntry() % 2 ? 1.0f : -1.0f) * frand(0.2f, 0.6f);
    angle += lateralOffset;

    float kiteDist = frand(8.0f, 14.0f);
    if (kiteP.caution > 0.6f)
        kiteDist += 4.0f; // cautious bots kite farther

    Position kitePos;
    kitePos.m_positionX = me->GetPositionX() + kiteDist * std::cos(angle);
    kitePos.m_positionY = me->GetPositionY() + kiteDist * std::sin(angle);
    kitePos.m_positionZ = me->GetPositionZ();

    // Validate ground position
    float ground = kitePos.m_positionZ;
    me->UpdateGroundPositionZ(kitePos.m_positionX, kitePos.m_positionY, ground);
    if (ground > INVALID_HEIGHT)
        kitePos.m_positionZ = ground;

    if (me->GetExactDist2d(kitePos) < 3.0f)
        return false;

    BotMovement(BOT_MOVE_POINT, &kitePos);
    me->SetInFront(attacker);

    _bgKiteTimer = urand(1500, 3000);
    return true;
}

Position bot_ai::GetDefenseSpreadPosition(Position const& nodePos) const
{
    if (!me->GetMap()->IsBattlegroundOrArena())
        return nodePos;

    Battleground* bg = GetBG();
    if (!bg)
        return nodePos;

    TeamId myTeamId = bg->GetBotTeamId(me->GetGUID());
    uint32 myTeam = myTeamId == TEAM_ALLIANCE ? ALLIANCE : HORDE;

    uint8 mySlot = 0;
    uint8 totalDefenders = 0;

    for (auto const& [guid, botData] : bg->GetBots())
    {
        if (botData.Team != myTeam) continue;
        Creature const* ally = ObjectAccessor::GetCreature(*me, guid);
        if (!ally || !ally->IsAlive()) continue;
        if (ally->GetExactDist2d(nodePos) > 25.0f) continue;

        if (ally->GetGUID() < me->GetGUID())
            ++mySlot;
        ++totalDefenders;
    }

    if (totalDefenders <= 1)
        return nodePos;

    float spreadRadius = std::min(15.0f, 5.0f + totalDefenders * 2.0f);
    if (IsRanged())
        spreadRadius += 5.0f;

    float angle = (float(M_PI) * 2.0f / totalDefenders) * mySlot;

    Position spreadPos;
    spreadPos.m_positionX = nodePos.GetPositionX() + spreadRadius * std::cos(angle);
    spreadPos.m_positionY = nodePos.GetPositionY() + spreadRadius * std::sin(angle);
    spreadPos.m_positionZ = nodePos.GetPositionZ();

    // Validate ground height
    float ground = spreadPos.m_positionZ;
    me->UpdateGroundPositionZ(spreadPos.m_positionX, spreadPos.m_positionY, ground);
    if (ground > INVALID_HEIGHT)
        spreadPos.m_positionZ = ground;

    return spreadPos;
}

void bot_ai::TriggerBGSpeedBoost()
{
    if (!me->GetMap()->IsBattlegroundOrArena())
        return;
    if (me->IsMounted() || me->HasAuraType(SPELL_AURA_MOD_INCREASE_SPEED))
        return;

    switch (_botclass)
    {
        case BOT_CLASS_ROGUE:
        {
            // Sprint (base: 2983)
            uint32 sprint = GetSpell(2983);
            if (sprint && IsSpellReady(2983, lastdiff, false))
                doCast(me, sprint);
            break;
        }
        case BOT_CLASS_DRUID:
        {
            // Dash (base: 1850) — requires cat form
            uint32 dash = GetSpell(1850);
            if (dash && IsSpellReady(1850, lastdiff, false))
                doCast(me, dash);
            break;
        }
        case BOT_CLASS_SHAMAN:
        {
            // Ghost Wolf (2645) — requires out of combat
            if (!me->IsInCombat())
            {
                uint32 ghostWolf = GetSpell(2645);
                if (ghostWolf && IsSpellReady(2645, lastdiff, false))
                    doCast(me, ghostWolf);
            }
            break;
        }
        default:
            break;
    }
}

void bot_ai::CheckBGObjectiveProximity()
{
    Battleground* bg = GetBG();
    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS) return;

    switch (bg->GetTypeID())
    {
        case BATTLEGROUND_WS:
        {
            // WSG objective pull gradient: strength increases linearly from own flag (0%) to enemy flag (100%)
            // Flag-to-flag distance is ~626 yards. Pull = 1.0 - (distToFlag / 626)
            static constexpr float WSG_FLAG_TO_FLAG_DIST = 626.0f;

            // Flag PICKUP
            if (!IsFlagCarrier(me))
            {
                uint32 flagObjId = (bg->GetBotTeamId(me->GetGUID()) == TEAM_ALLIANCE)
                    ? BG_WS_OBJECT_H_FLAG : BG_WS_OBJECT_A_FLAG;
                if (GameObject* go = bg->GetBGObject(flagObjId, false))
                {
                    if (go->GetGoState() == GO_STATE_READY && go->isSpawned())
                    {
                        float flagDist = me->GetExactDist2d(go);
                        float pull = std::clamp(1.0f - (flagDist / WSG_FLAG_TO_FLAG_DIST), 0.0f, 1.0f);

                        // Gradient redirect: blend objective toward flag based on pull strength
                        // At pull=0 (far): keep current objective. At pull=1 (at flag): objective IS the flag
                        if (pull > 0.1f && _bgHasObjective)
                        {
                            float blendX = _bgObjectivePos.m_positionX * (1.0f - pull) + go->GetPositionX() * pull;
                            float blendY = _bgObjectivePos.m_positionY * (1.0f - pull) + go->GetPositionY() * pull;
                            float blendZ = _bgObjectivePos.m_positionZ * (1.0f - pull) + go->GetPositionZ() * pull;
                            _bgObjectivePos.Relocate(blendX, blendY, blendZ);
                        }
                        else if (pull > 0.1f)
                        {
                            _bgObjectivePos.Relocate(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ());
                            _bgHasObjective = true;
                        }

                        // Within 10 yards: grab it (smart decision based on situation)
                        if (flagDist <= 10.0f)
                        {
                            bool beingAttacked = !me->getAttackers().empty();
                            bool shouldGrab = true;
                            if (beingAttacked)
                            {
                                uint8 nearbyAllies = 0;
                                uint32 myTeam = bg->GetBotTeamId(me->GetGUID()) == TEAM_ALLIANCE ? ALLIANCE : HORDE;
                                for (auto const& [guid, botData] : bg->GetBots())
                                {
                                    if (botData.Team != myTeam) continue;
                                    Creature const* ally = ObjectAccessor::GetCreature(*me, guid);
                                    if (ally && ally != me && ally->IsAlive() && ally->GetExactDist2d(me) < 30.0f)
                                        ++nearbyAllies;
                                }
                                if (GetHealthPCT(me) < 30 && nearbyAllies == 0)
                                    shouldGrab = false;
                            }
                            if (shouldGrab)
                            {
                                if (me->IsMounted()) DismountBot();
                                bg->EventBotClickedOnFlag(me, go);
                                // Only record if we actually picked up the flag
                                if (IsFlagCarrier(me, bg->GetTypeID()))
                                {
                                    BotBGAIMgr::RecordObjectiveCap(me->GetMapId(), me->GetPositionX(), me->GetPositionY());
                                    ++_bgObjectiveCapsCount;
                                    BotBGAIMgr::RecordTimingOutcome(me->GetMapId(),
                                        (_bgCurrentStrategy < BG_STRATEGY_MAX ? _bgCurrentStrategy : 0),
                                        bg->GetStartTime(), true);
                                    TriggerBGSpeedBoost();
                                    _bgHasObjective = false;
                                }
                            }
                        }
                    }
                }
            }
            // Flag DELIVERY: FC always goes straight to cap point, no gradient blending
            if (IsFlagCarrier(me, bg->GetTypeID()))
            {
                float homeX, homeY, homeZ;
                uint32 areaTrigger;
                if (bg->GetBotTeamId(me->GetGUID()) == TEAM_ALLIANCE)
                    { homeX = 1540.42f; homeY = 1481.33f; homeZ = 351.83f; areaTrigger = 3646; }
                else
                    { homeX = 916.02f; homeY = 1434.41f; homeZ = 345.41f; areaTrigger = 3647; }

                float capDist = me->GetExactDist2d(homeX, homeY);

                // FC objective is ALWAYS the cap point — no blending, no gradient
                _bgObjectivePos.Relocate(homeX, homeY, homeZ);
                _bgHasObjective = true;

                // Within 10 yards: cap
                if (capDist <= 10.0f)
                {
                    bool beingAttacked = !me->getAttackers().empty();
                    bool shouldCap = true;
                    if (beingAttacked && GetHealthPCT(me) < 20)
                    {
                        uint8 nearbyAllies = 0;
                        uint32 myTeam = bg->GetBotTeamId(me->GetGUID()) == TEAM_ALLIANCE ? ALLIANCE : HORDE;
                        for (auto const& [guid, botData] : bg->GetBots())
                        {
                            if (botData.Team != myTeam) continue;
                            Creature const* ally = ObjectAccessor::GetCreature(*me, guid);
                            if (ally && ally != me && ally->IsAlive() && ally->GetExactDist2d(me) < 30.0f)
                                ++nearbyAllies;
                        }
                        if (nearbyAllies == 0)
                            shouldCap = false;
                    }
                    if (shouldCap)
                    {
                        bg->HandleBotAreaTrigger(me, areaTrigger);
                        // Only log/record if the flag was actually captured (bot no longer FC)
                        if (!IsFlagCarrier(me, bg->GetTypeID()))
                        {
                            TC_LOG_INFO("server.worldserver", "[BG] WSG: {} ({}) captured the flag!",
                                me->GetName(), bg->GetBotTeamId(me->GetGUID()) == TEAM_ALLIANCE ? "Alliance" : "Horde");
                            BotBGAIMgr::RecordObjectiveCap(me->GetMapId(), me->GetPositionX(), me->GetPositionY());
                            ++_bgObjectiveCapsCount;
                            BotBGAIMgr::RecordTimingOutcome(me->GetMapId(),
                                (_bgCurrentStrategy < BG_STRATEGY_MAX ? _bgCurrentStrategy : 0),
                                bg->GetStartTime(), true);
                            _bgHasObjective = false;
                        }
                    }
                }
            }
            break;
        }
        case BATTLEGROUND_AB:
        {
            // Banner cap: scan for nearest banner within 10 yards
            uint8 node = BG_AB_NODE_STABLES;
            GameObject* obj = bg->GetBGObject(node * 8 + BG_AB_OBJECT_BANNER_NEUTRAL);
            while (node < BG_AB_DYNAMIC_NODES_COUNT && (!obj || !me->IsWithinDistInMap(obj, 10.0f)))
            {
                ++node;
                if (node < BG_AB_DYNAMIC_NODES_COUNT)
                    obj = bg->GetBGObject(node * 8 + BG_AB_OBJECT_BANNER_NEUTRAL);
            }
            if (node < BG_AB_DYNAMIC_NODES_COUNT && obj)
            {
                TeamId teamId = bg->GetBotTeamId(me->GetGUID());
                BattlegroundAB const* bgab = dynamic_cast<BattlegroundAB const*>(bg);
                if (bgab && !bgab->IsNodeOccupied(node, teamId) && !bgab->IsNodeContested(node, teamId))
                {
                    // Count cappers to limit to 2
                    uint8 cappers = 0;
                    for (Unit const* member : BotMgr::GetAllGroupMembers(me))
                    {
                        if (member->GetGUID() == me->GetGUID()) continue;
                        if (Spell const* curSpell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                            if (curSpell->m_spellInfo->Id == OPEN_FLAG_BG)
                                ++cappers;
                    }
                    if (cappers < 2)
                    {
                        if (me->IsMounted()) DismountBot();
                        me->CastSpell(obj, OPEN_FLAG_BG);
                        BotBGAIMgr::RecordObjectiveCap(me->GetMapId(), me->GetPositionX(), me->GetPositionY());
                    }
                }
            }
            break;
        }
        case BATTLEGROUND_EY:
        {
            // EY: pick up Netherstorm flag when near it (proximity-based)
            // Point capture is automatic (BG detects player/bot standing in capture zone)
            if (!IsFlagCarrier(me))
            {
                GameObject* obj = bg->GetBGObject(BG_EY_OBJECT_FLAG_NETHERSTORM);
                if (obj && obj->IsInWorld() && obj->isSpawned() && obj->GetGoState() == GO_STATE_READY &&
                    me->GetExactDist2d(obj) < 10.0f)
                {
                    bool already_used = std::ranges::any_of(BotMgr::GetAllGroupMembers(me), [=, this](Unit const* member) {
                        if (member == me) return false;
                        Spell const* curSpell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                        return curSpell && curSpell->m_spellInfo->Id == OPEN_FLAG_BG && curSpell->m_targets.GetGOTargetGUID() == obj->GetGUID();
                    });
                    if (!already_used)
                    {
                        if (me->IsMounted()) DismountBot();
                        me->CastSpell(obj, OPEN_FLAG_BG);
                        BotBGAIMgr::RecordObjectiveCap(me->GetMapId(), me->GetPositionX(), me->GetPositionY());
                        ++_bgObjectiveCapsCount;
                    }
                }
            }
            // EY flag delivery: handled by area triggers when FC walks into owned point
            // The BG handles this automatically via CheckSomeoneJoinedPoint
            break;
        }
        default:
            break;
    }
}

void bot_ai::OnBotEnterBattleground()
{
    Battleground* bg = ASSERT_NOTNULL(GetBG());

    if (bg->GetStatus() != STATUS_IN_PROGRESS && IsWanderer())
    {
        // Static spawn zones: bots spawn inside their own flag room
        // Hardcoded per BG type and team — no WanderNode dependency
        Position spawnCenter;
        bool hasSpawn = false;
        TeamId myTeamId = bg->GetBotTeamId(me->GetGUID());

        // Use the BG's built-in team start position (the prep/spawn room)
        Position const* startPos = bg->GetTeamStartPosition(myTeamId);
        if (startPos)
        {
            spawnCenter = *startPos;
            hasSpawn = true;
        }

        SetBotCommandState(BOT_COMMAND_STAY);
        if (hasSpawn)
        {
            // Random scatter within the flag room so bots don't stack
            float angle = frand(0.0f, float(M_PI) * 2.0f);
            float dist = frand(2.0f, 8.0f);
            Position scatterPos;
            scatterPos.Relocate(
                spawnCenter.GetPositionX() + dist * std::cos(angle),
                spawnCenter.GetPositionY() + dist * std::sin(angle),
                spawnCenter.GetPositionZ());
            float ground = scatterPos.GetPositionZ();
            me->UpdateGroundPositionZ(scatterPos.GetPositionX(), scatterPos.GetPositionY(), ground);
            if (ground > INVALID_HEIGHT)
                scatterPos.m_positionZ = ground;
            BotMovement(BOT_MOVE_POINT, &scatterPos);
        }
    }

    SelectBGStrategy();
    _bgMatchSnapshots.clear();
    _bgEnemyReadTimer = 0;
    _bgSnapshotTimer = 0;
    _bgDetectedEnemyBehavior = 0;
    _bgWaypointRecordTimer = 0;
    _bgReactionDelay = 0;
    _bgNeedsReassessment = false;
    _bgAssignedRole = 0;
    _bgLastScore = 0;
    _bgLastFlagState = 0;
    _bgKiteTimer = 0;
    _bgStrafeTimer = 0;
    _bgInterruptDelayTimer = 0;
    _bgInterruptTargetGuid.Clear();
    _bgStrategyRevisionTimer = 60000;
    _bgStratKills = 0;
    _bgStratDeaths = 0;
    _bgPlanNodeIdx = 0xFF;
    _bgPlanVersion = 0;
    _bgAtRally = false;
    _bgHasObjective = false;
    _bgObjectivePos = {};
    _bgQEpisode.clear();
    _bgMatchKills = 0;
    _bgMatchDeaths = 0;
    _bgObjectiveCapsCount = 0;

    // Mesh seeding: if no learned waypoints exist for this BG map, seed with a random walk
    // This gives the potential field system something to score on the very first game
    if (bg && IsWanderer() && !BotBGAIMgr::HasLearnedWaypoints(bg->GetBgMap()->GetId()))
    {
        uint32 bgMapId = bg->GetBgMap()->GetId();
        float seedX = me->GetPositionX();
        float seedY = me->GetPositionY();
        float seedZ = me->GetPositionZ();

        // Walk toward map center in random steps, recording each position
        // WSG center: ~1228, 1462. AB center: ~1185, 1015. EY center: ~2174, 1569.
        float centerX, centerY;
        switch (bg->GetTypeID())
        {
            case BATTLEGROUND_WS: centerX = 1228.0f; centerY = 1462.0f; break;
            case BATTLEGROUND_AB: centerX = 1185.0f; centerY = 1015.0f; break;
            case BATTLEGROUND_EY: centerX = 2174.0f; centerY = 1569.0f; break;
            default: centerX = seedX; centerY = seedY; break;
        }

        for (uint8 step = 0; step < 20; ++step)
        {
            // Direction: toward center with random lateral deviation
            float dx = centerX - seedX;
            float dy = centerY - seedY;
            float distToCenter = std::sqrt(dx * dx + dy * dy);
            if (distToCenter < 15.0f) break; // close enough to center

            float baseAngle = std::atan2(dy, dx);
            float deviation = frand(-0.6f, 0.6f); // ~35 degrees random deviation
            float stepDist = frand(15.0f, 30.0f);

            seedX += stepDist * std::cos(baseAngle + deviation);
            seedY += stepDist * std::sin(baseAngle + deviation);

            // Record the position as a learned waypoint
            float ground = seedZ;
            me->UpdateGroundPositionZ(seedX, seedY, ground);
            if (ground > INVALID_HEIGHT)
            {
                seedZ = ground;
                BotBGAIMgr::RecordWaypointVisit(bgMapId, seedX, seedY, seedZ);
            }
        }
    }
}

// End of BG movement functions
