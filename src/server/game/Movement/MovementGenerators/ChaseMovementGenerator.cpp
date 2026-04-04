/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ChaseMovementGenerator.h"
#include "Creature.h"
#include "Map.h"
#include "CreatureAI.h"
#include "G3DPosition.hpp"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "PathGenerator.h"
#include "Unit.h"
#include "Util.h"
#include "TSCreature.h"

static bool HasLostTarget(Unit* owner, Unit* target)
{
    return owner->GetVictim() != target;
}

static bool IsMutualChase(Unit* owner, Unit* target)
{
    if (target->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
        return false;

    if (ChaseMovementGenerator* movement = dynamic_cast<ChaseMovementGenerator*>(target->GetMotionMaster()->GetCurrentMovementGenerator()))
        return movement->GetTarget() == owner;

    return false;
}

static bool PositionOkay(Unit* owner, Unit* target, Optional<float> minDistance, Optional<float> maxDistance, Optional<ChaseAngle> angle)
{
    float const distSq = owner->GetExactDistSq(target);
    if (minDistance && distSq < square(*minDistance))
        return false;
    if (maxDistance && distSq > square(*maxDistance))
        return false;
    if (angle && !angle->IsAngleOkay(target->GetRelativeAngle(owner)))
        return false;
    if (!owner->IsWithinLOSInMap(target))
        return false;
    return true;
}

static void DoMovementInform(Unit* owner, Unit* target)
{
    if (owner->GetTypeId() != TYPEID_UNIT)
        return;

    if (CreatureAI* AI = owner->ToCreature()->AI())
        AI->MovementInform(CHASE_MOTION_TYPE, target->GetGUID().GetCounter());

    // @tswow-begin
    if (owner->IsCreature()) {
        FIRE_ID(owner->ToCreature()->GetCreatureTemplate()->events.id,Creature,OnMovementInform,TSCreature(owner->ToCreature()),CHASE_MOTION_TYPE,target->GetGUID().GetCounter());
    }
    // @tswow-end
}

ChaseMovementGenerator::ChaseMovementGenerator(Unit *target, Optional<ChaseRange> range, Optional<ChaseAngle> angle) : AbstractFollower(ASSERT_NOTNULL(target)), _range(range),
    _angle(angle), _rangeCheckTimer(RANGE_CHECK_INTERVAL)
{
    Mode = MOTION_MODE_DEFAULT;
    Priority = MOTION_PRIORITY_NORMAL;
    Flags = MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING;
    BaseUnitState = UNIT_STATE_CHASE;
}
ChaseMovementGenerator::~ChaseMovementGenerator() = default;

Position ChaseMovementGenerator::PredictTargetPosition(Unit* owner, Unit* target, float maxPredictionTime)
{
    Position current = target->GetPosition();

    // Update velocity tracking
    UpdateTargetVelocity(RANGE_CHECK_INTERVAL);

    float ourSpeed = owner->GetSpeed(MOVE_RUN);
    if (ourSpeed < 0.1f)
        return current;

    // Calculate current distance
    G3D::Vector3 ownerPos(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ());
    G3D::Vector3 targetPos(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
    G3D::Vector3 toTarget = targetPos - ownerPos;
    toTarget.z = 0; // Ignore vertical for interception math

    float distanceXY = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
    if (distanceXY < 0.1f)
        return current;

    // Get target's velocity
    G3D::Vector3 const& velocity = GetTargetVelocity();
    float targetSpeed = velocity.length();

    // BEHAVIOR 1: Target is stationary or moving very slowly
    // In this case, just approach directly - no prediction needed
    if (!HasVelocityData() || targetSpeed < 0.5f)
    {
        // Simply return current position - the pathfinding system will handle approach
        return current;
    }

    // BEHAVIOR 2: Target is moving - use predictive interception

    // Normalize direction to target
    G3D::Vector3 dirToTarget = toTarget / distanceXY;

    // Calculate target's velocity in 2D
    G3D::Vector3 velocityXY(velocity.x, velocity.y, 0);

    // Calculate how much of target's movement is toward/away from us
    float approachVelocity = velocityXY.dot(dirToTarget);

    // Calculate closure rate (relative speed at which we're getting closer)
    float closureRate = ourSpeed - approachVelocity;

    // BEHAVIOR 3: Target is escaping faster than we can chase
    // Fall back to direct pursuit of current position
    if (closureRate <= 0.1f)
        return current;

    // BEHAVIOR 4: Predictive interception for moving targets
    // Solve for optimal interception point using quadratic equation

    float a = targetSpeed * targetSpeed - ourSpeed * ourSpeed;
    float b = 2.0f * velocityXY.dot(toTarget);
    float c = -(distanceXY * distanceXY);

    float timeToIntercept;

    // If speeds are very similar, use simplified calculation
    if (std::abs(a) < 0.01f)
    {
        if (std::abs(b) < 0.01f)
            timeToIntercept = distanceXY / ourSpeed;
        else
            timeToIntercept = -c / b;
    }
    else
    {
        // Solve quadratic: a*t^2 + b*t + c = 0
        float discriminant = b * b - 4.0f * a * c;

        if (discriminant < 0)
        {
            // No perfect interception - use closure rate
            timeToIntercept = distanceXY / closureRate;
        }
        else
        {
            float sqrtDisc = std::sqrt(discriminant);
            float t1 = (-b + sqrtDisc) / (2.0f * a);
            float t2 = (-b - sqrtDisc) / (2.0f * a);

            // Choose smallest positive time
            if (t1 > 0 && t2 > 0)
                timeToIntercept = std::min(t1, t2);
            else if (t1 > 0)
                timeToIntercept = t1;
            else if (t2 > 0)
                timeToIntercept = t2;
            else
                timeToIntercept = distanceXY / ourSpeed; // Fallback
        }
    }

    // Clamp prediction time
    timeToIntercept = std::max(0.0f, std::min(timeToIntercept, maxPredictionTime));

    // BEHAVIOR 5: Smart prediction scaling based on distance
    // Close range: less prediction (more reactive)
    // Long range: more prediction (more interception)
    float predictionScale = 1.0f;
    if (distanceXY < 10.0f)
    {
        // Within 10 yards, scale down prediction to be more reactive
        predictionScale = distanceXY / 10.0f;
    }
    else if (timeToIntercept * targetSpeed > distanceXY * 1.5f)
    {
        // If prediction overshoots too far, reduce it
        predictionScale = 0.6f;
    }

    timeToIntercept *= predictionScale;

    // Calculate predicted position
    Position predicted = current;
    predicted.m_positionX += velocity.x * timeToIntercept;
    predicted.m_positionY += velocity.y * timeToIntercept;

    // Validate that the predicted position is reachable — check LOS from target's
    // current position. If the prediction is behind a wall or over a cliff, fall back
    // to the target's actual position.
    float predZ = predicted.m_positionZ;
    target->UpdateAllowedPositionZ(predicted.m_positionX, predicted.m_positionY, predZ);
    predicted.m_positionZ = predZ;

    if (!target->IsWithinLOS(predicted.m_positionX, predicted.m_positionY, predicted.m_positionZ))
        return current;

    return predicted;
}

void ChaseMovementGenerator::Initialize(Unit* /*owner*/)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING | MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    AddFlag(MOVEMENTGENERATOR_FLAG_INITIALIZED | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

    _path = nullptr;
    _lastTargetPosition.reset();
    _lastDestination.reset();
}

void ChaseMovementGenerator::Reset(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);

    Initialize(owner);
}

bool ChaseMovementGenerator::Update(Unit* owner, uint32 diff)
{
    // owner might be dead or gone (can we even get nullptr here?)
    if (!owner || !owner->IsAlive())
        return false;

    // our target might have gone away
    Unit* const target = GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    // the owner might be unable to move (rooted or casting), or we have lost the target, pause movement
    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE) || owner->IsMovementPreventedByCasting() || HasLostTarget(owner, target))
    {
        owner->StopMoving();
        _lastTargetPosition.reset();
        if (Creature* cOwner = owner->ToCreature())
            cOwner->SetCannotReachTarget(false);
        return true;
    }

    bool const mutualChase = IsMutualChase(owner, target);
    float const hitboxSum = owner->GetCombatReach() + target->GetCombatReach();
    float const minRange = _range ? _range->MinRange + hitboxSum : CONTACT_DISTANCE;
    float const minTarget = (_range ? _range->MinTolerance : 0.0f) + hitboxSum;
    float const maxRange = _range ? _range->MaxRange + hitboxSum : owner->GetMeleeRange(target); // melee range already includes hitboxes
    float const maxTarget = _range ? _range->MaxTolerance + hitboxSum : CONTACT_DISTANCE + hitboxSum;
    Optional<ChaseAngle> angle = mutualChase ? Optional<ChaseAngle>() : _angle;

    bool const isMoving = owner->HasUnitState(UNIT_STATE_CHASE_MOVE) && !owner->movespline->Finalized();

    // periodically check if we're already in the expected range...
    _rangeCheckTimer.Update(diff);
    if (_rangeCheckTimer.Passed())
    {
        // Adaptive update interval: use longer interval when movement is smooth and predictable
        G3D::Vector3 const& velocity = GetTargetVelocity();
        float targetSpeed = velocity.length();
        bool smoothMovement = HasVelocityData() && targetSpeed > 0.5f && targetSpeed < 10.0f;

        if (smoothMovement && _smoothMovementCount < 5)
            _smoothMovementCount++;
        else if (!smoothMovement && _smoothMovementCount > 0)
            _smoothMovementCount = 0;

        // Use longer interval when we've had several smooth updates
        uint32 interval = (_smoothMovementCount >= 3) ? RANGE_CHECK_INTERVAL_SMOOTH : RANGE_CHECK_INTERVAL;
        _rangeCheckTimer.Reset(interval);

        if (HasFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED) && PositionOkay(owner, target, _movingTowards ? Optional<float>() : minTarget, _movingTowards ? maxTarget : Optional<float>(), angle))
        {
            RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
            _path = nullptr;
            _lastDestination.reset();
            if (Creature* cOwner = owner->ToCreature())
                cOwner->SetCannotReachTarget(false);
            owner->StopMoving();
            owner->SetInFront(target);
            DoMovementInform(owner, target);
            return true;
        }
    }

    // if we're done moving, we want to clean up
    if (owner->HasUnitState(UNIT_STATE_CHASE_MOVE) && owner->movespline->Finalized())
    {
        RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
        _path = nullptr;
        _lastDestination.reset();
        if (Creature* cOwner = owner->ToCreature())
            cOwner->SetCannotReachTarget(false);
        owner->ClearUnitState(UNIT_STATE_CHASE_MOVE);
        owner->SetInFront(target);
        DoMovementInform(owner, target);
    }

    // Always track target position for velocity calculations
    _lastTargetPosition = target->GetPosition();
    _mutualChase = mutualChase;

    // Don't recalculate if we're already in acceptable position and not moving
    if (!isMoving && PositionOkay(owner, target, minRange, maxRange, angle))
        return true;

    // If currently moving and we're close to the target, let the current spline finish
    // instead of interrupting with a new path (prevents stop-start jitter at close range)
    if (isMoving)
    {
        float distToTargetSq = owner->GetExactDistSq(target);
        // If we're within twice max range, the current path will likely get us close enough
        if (distToTargetSq < square(maxRange * 2.0f))
        {
            // Check if we're roughly heading the right direction by comparing
            // our distance to target vs the path endpoint distance to target
            Movement::PointsArray const& path = _path ? _path->GetPath() : Movement::PointsArray();
            if (!path.empty())
            {
                G3D::Vector3 const& pathEnd = path.back();
                float pathEndDistSq = square(pathEnd.x - target->GetPositionX())
                                    + square(pathEnd.y - target->GetPositionY());
                // If our path endpoint is within max range of target, let it finish
                if (pathEndDistSq < square(maxRange))
                    return true;
            }
        }
    }

    // Calculate where we want to go
    Creature* const cOwner = owner->ToCreature();
    if (cOwner && !target->isInAccessiblePlaceFor(cOwner))
    {
        cOwner->SetCannotReachTarget(true);
        cOwner->StopMoving();
        _path = nullptr;
        _lastDestination.reset();
        return true;
    }

    bool const moveToward = !owner->IsInDist(target, maxRange);

    float x, y, z;
    bool shortenPath;

    if (moveToward && !angle)
    {
        Position predicted = PredictTargetPosition(owner, target);
        predicted.GetPosition(x, y, z);
        shortenPath = true;
    }
    else
    {
        target->GetNearPoint(owner, x, y, z, (moveToward ? maxTarget : minTarget) - hitboxSum, angle ? target->ToAbsoluteAngle(angle->RelativeAngle) : target->GetAbsoluteAngle(owner));
        shortenPath = false;
    }

    // Path stability: skip recalc if destination hasn't changed enough
    // This works for ALL chase modes (predictive, angle-based, nearpoint)
    bool destChangedSignificantly = true;
    if (_lastDestination.has_value() && isMoving)
    {
        float destChangeSq = square(x - _lastDestination->GetPositionX())
                           + square(y - _lastDestination->GetPositionY());
        if (destChangeSq < square(PATH_RECALC_DISTANCE_THRESHOLD))
            return true;

        // FIX 1: If destination changed a lot, invalidate the cached poly path
        // so PathGenerator doesn't reuse 80% of the old route that pointed
        // toward the previous target location
        if (destChangeSq > square(PATH_RECALC_DISTANCE_THRESHOLD * 3.0f) && _path)
            _path->InvalidateOldPath();

        destChangedSignificantly = true;
    }

    if (owner->IsHovering())
        owner->UpdateAllowedPositionZ(x, y, z);

    // Don't start a new spline for tiny movements (prevents micro-jitter at close range)
    if (!isMoving)
    {
        float moveSq = square(owner->GetPositionX() - x) + square(owner->GetPositionY() - y);
        if (moveSq < MIN_CHASE_RELOCATE_DIST_SQ && PositionOkay(owner, target, minRange, maxRange, angle))
            return true;
    }

    // Build path
    if (!_path || moveToward != _movingTowards)
        _path = std::make_unique<PathGenerator>(owner);

    bool success = _path->CalculatePath(x, y, z, owner->CanFly());
    if (!success || (_path->GetPathType() & PATHFIND_NOPATH))
    {
        if (cOwner)
            cOwner->SetCannotReachTarget(true);
        owner->StopMoving();
        return true;
    }

    // FIX 3: If the path is incomplete, check that it actually gets us closer to the target.
    // Incomplete paths end at an intermediate navmesh point that might be in the wrong direction.
    if (_path->GetPathType() & PATHFIND_INCOMPLETE)
    {
        Movement::PointsArray const& points = _path->GetPath();
        if (points.size() >= 2)
        {
            G3D::Vector3 const& pathEnd = points.back();
            float pathEndDistSq = square(pathEnd.x - target->GetPositionX())
                                + square(pathEnd.y - target->GetPositionY());
            float ownerDistSq = square(owner->GetPositionX() - target->GetPositionX())
                              + square(owner->GetPositionY() - target->GetPositionY());
            // If the incomplete path endpoint isn't meaningfully closer than where we are now, reject it
            if (pathEndDistSq >= ownerDistSq * 0.9f)
            {
                if (cOwner)
                    cOwner->SetCannotReachTarget(true);
                owner->StopMoving();
                return true;
            }
        }
    }

    if (shortenPath)
        _path->ShortenPathUntilDist(PositionToVector3(target), maxTarget);

    if (cOwner)
        cOwner->SetCannotReachTarget(false);

    // Save the destination we're actually pathing to
    _lastDestination = Position(x, y, z);
    _movingTowards = moveToward;

    bool walk = false;
    if (cOwner && !cOwner->IsPet())
    {
        switch (cOwner->GetMovementTemplate().GetChase())
        {
            case CreatureChaseMovementType::CanWalk:
                walk = owner->IsWalking();
                break;
            case CreatureChaseMovementType::AlwaysWalk:
                walk = true;
                break;
            default:
                break;
        }
    }

    owner->AddUnitState(UNIT_STATE_CHASE_MOVE);
    AddFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

    Movement::MoveSplineInit init(owner);

    // When both units are in water, the navmesh only has surface-level polygons
    // so paths stay at the water surface. Fix by interpolating Z toward the target's depth
    // while keeping the navmesh X/Y for horizontal obstacle avoidance.
    // Use IsUnderWater for a more stable check — IsInWater can flicker near the surface
    if ((owner->IsInWater() || owner->IsUnderWater()) && (target->IsInWater() || target->IsUnderWater()))
    {
        Movement::PointsArray adjustedPath = _path->GetPath();
        if (adjustedPath.size() >= 2)
        {
            float startZ = owner->GetPositionZ();
            float endZ = target->GetPositionZ();
            float totalDist = 0.0f;

            for (size_t i = 1; i < adjustedPath.size(); ++i)
            {
                float dx = adjustedPath[i].x - adjustedPath[i - 1].x;
                float dy = adjustedPath[i].y - adjustedPath[i - 1].y;
                totalDist += std::sqrt(dx * dx + dy * dy);
            }

            if (totalDist > 0.0f)
            {
                float accumDist = 0.0f;
                adjustedPath[0].z = startZ;
                for (size_t i = 1; i < adjustedPath.size(); ++i)
                {
                    float dx = adjustedPath[i].x - adjustedPath[i - 1].x;
                    float dy = adjustedPath[i].y - adjustedPath[i - 1].y;
                    accumDist += std::sqrt(dx * dx + dy * dy);
                    float t = accumDist / totalDist;
                    float interpZ = startZ + (endZ - startZ) * t;

                    // Clamp above underwater terrain so the path doesn't cut through hills
                    float groundZ = owner->GetMap()->GetHeight(adjustedPath[i].x, adjustedPath[i].y, interpZ + 5.0f, true);
                    if (groundZ > INVALID_HEIGHT)
                        interpZ = std::max(interpZ, groundZ + 1.0f);

                    adjustedPath[i].z = interpZ;
                }
            }
        }
        init.MovebyPath(adjustedPath);
    }
    else
        init.MovebyPath(_path->GetPath());

    init.SetWalk(walk);
    init.SetSmooth();
    init.Launch();

    // and then, finally, we're done for the tick
    return true;
}

void ChaseMovementGenerator::Deactivate(Unit* owner)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    RemoveFlag(MOVEMENTGENERATOR_FLAG_TRANSITORY | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
    owner->ClearUnitState(UNIT_STATE_CHASE_MOVE);
    if (Creature* cOwner = owner->ToCreature())
        cOwner->SetCannotReachTarget(false);
}

void ChaseMovementGenerator::Finalize(Unit* owner, bool active, bool/* movementInform*/)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_FINALIZED);
    if (active)
    {
        owner->ClearUnitState(UNIT_STATE_CHASE_MOVE);
        if (Creature* cOwner = owner->ToCreature())
            cOwner->SetCannotReachTarget(false);
    }
}
