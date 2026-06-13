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

#include "FollowMovementGenerator.h"
#include "Creature.h"
#include "Map.h"
#include "CreatureAI.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Optional.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "Unit.h"
#include "Util.h"
#include "TSCreature.h"

static void DoMovementInform(Unit* owner, Unit* target)
{
    if (owner->GetTypeId() != TYPEID_UNIT)
        return;

    if (CreatureAI* AI = owner->ToCreature()->AI())
        AI->MovementInform(FOLLOW_MOTION_TYPE, target->GetGUID().GetCounter());

    // @tswow-begin
    if (owner->IsCreature()) {
        FIRE_ID(owner->ToCreature()->GetCreatureTemplate()->events.id,Creature,OnMovementInform,TSCreature(owner->ToCreature()),FOLLOW_MOTION_TYPE, target->GetGUID().GetCounter());
    }
    // @tswow-end
}

FollowMovementGenerator::FollowMovementGenerator(Unit* target, float range, ChaseAngle angle) : AbstractFollower(ASSERT_NOTNULL(target)), _range(range), _angle(angle), _checkTimer(CHECK_INTERVAL)
{
    Mode = MOTION_MODE_DEFAULT;
    Priority = MOTION_PRIORITY_NORMAL;
    Flags = MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING;
    BaseUnitState = UNIT_STATE_FOLLOW;
}
FollowMovementGenerator::~FollowMovementGenerator() = default;

static bool PositionOkay(Unit* owner, Unit* target, float range, Optional<ChaseAngle> angle = {})
{
    if (owner->GetExactDistSq(target) > square(owner->GetCombatReach() + target->GetCombatReach() + range))
        return false;

    return !angle || angle->IsAngleOkay(target->GetRelativeAngle(owner));
}

void FollowMovementGenerator::Initialize(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_INITIALIZATION_PENDING | MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    AddFlag(MOVEMENTGENERATOR_FLAG_INITIALIZED | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

    owner->StopMoving();
    UpdatePetSpeed(owner);
    _path = nullptr;
    _lastTargetPosition.reset();
    _lastDestination.reset();
    _lastTargetWasMoving = false;
    _lastWalkSent = false;
}

void FollowMovementGenerator::Reset(Unit* owner)
{
    RemoveFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);

    Initialize(owner);
}

bool FollowMovementGenerator::Update(Unit* owner, uint32 diff)
{
    // owner might be dead or gone
    if (!owner || !owner->IsAlive())
        return false;

    // our target might have gone away
    Unit* const target = GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    if (owner->HasUnitState(UNIT_STATE_NOT_MOVE) || owner->IsMovementPreventedByCasting())
    {
        _path = nullptr;
        owner->StopMoving();
        _lastTargetPosition.reset();
        _lastDestination.reset();
        return true;
    }

    _checkTimer.Update(diff);
    if (_checkTimer.Passed())
    {
        _checkTimer.Reset(CHECK_INTERVAL);
        if (HasFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED) && PositionOkay(owner, target, _range, _angle))
        {
            RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
            _path = nullptr;
            owner->StopMoving();
            _lastTargetPosition.reset();
            _lastDestination.reset();
            DoMovementInform(owner, target);
            return true;
        }
    }

    // Capture spline-finalize BEFORE the cleanup block clears UNIT_STATE_FOLLOW_MOVE.
    // We need to re-enter the path block this tick if a truncated run-spline just
    // finished, so the walk-finish leg can launch immediately after.
    bool const splineJustFinalized = owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) && owner->movespline->Finalized();

    if (owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) && owner->movespline->Finalized())
    {
        RemoveFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
        _path = nullptr;
        _lastDestination.reset();
        owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
        DoMovementInform(owner, target);
    }

    // Re-evaluate when the target moves OR transitions from moving to stopped
    // OR our own spline just finished (so a two-stage approach can fire its
    // second leg without waiting for the owner to move again).
    bool const targetMoved = !_lastTargetPosition || _lastTargetPosition->GetExactDistSq(target->GetPosition()) > 0.0f;
    bool const targetJustStopped = _lastTargetWasMoving && !target->isMoving();
    if (targetMoved || targetJustStopped || splineJustFinalized)
    {
        _lastTargetPosition = target->GetPosition();
        _lastTargetWasMoving = target->isMoving();
        if (owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE) || !PositionOkay(owner, target, _range + FOLLOW_RANGE_TOLERANCE))
        {
            if (!_path)
                _path = std::make_unique<PathGenerator>(owner);

            float x, y, z;

            // select angle
            float tAngle;
            float const curAngle = target->GetRelativeAngle(owner);
            if (_angle.IsAngleOkay(curAngle))
                tAngle = curAngle;
            else
            {
                float const diffUpper = Position::NormalizeOrientation(curAngle - _angle.UpperBound());
                float const diffLower = Position::NormalizeOrientation(_angle.LowerBound() - curAngle);
                if (diffUpper < diffLower)
                    tAngle = _angle.UpperBound();
                else
                    tAngle = _angle.LowerBound();
            }

            target->GetNearPoint(owner, x, y, z, _range, target->ToAbsoluteAngle(tAngle));

            if (owner->IsHovering())
                owner->UpdateAllowedPositionZ(x, y, z);

            // Two-stage approach: when a pet is approaching its stopped,
            // out-of-combat owner, the FINAL leg should be at walk speed.
            //  - Already within WALK_FINISH_RADIUS  → walk this whole spline.
            //  - Farther away                       → truncate this run-spline
            //    to end WALK_FINISH_RADIUS short of the real destination, so
            //    the next Update tick (after splineJustFinalized) re-evaluates
            //    with the pet now within range and naturally fires the
            //    walk-spline for the remaining leg.
            // Net effect: consistent run-then-walk-then-stop regardless of
            // starting distance, instead of "run the whole way → snap stop".
            bool walkFinish = false;
            if (Pet* oPet = owner->ToPet())
            {
                if (target->GetGUID() == oPet->GetOwnerGUID()
                    && !target->isMoving()
                    && !owner->IsInCombat())
                {
                    float const dx = x - owner->GetPositionX();
                    float const dy = y - owner->GetPositionY();
                    float const dist = std::sqrt(dx * dx + dy * dy);
                    if (dist < WALK_FINISH_RADIUS)
                    {
                        walkFinish = true;
                    }
                    else if (dist > 0.01f)
                    {
                        // Cut the destination short along the direct line
                        // from pet to dest. Pathfinder still routes around
                        // obstacles to reach the truncated point.
                        float const t = (dist - WALK_FINISH_RADIUS) / dist;
                        float const dz = z - owner->GetPositionZ();
                        x = owner->GetPositionX() + dx * t;
                        y = owner->GetPositionY() + dy * t;
                        z = owner->GetPositionZ() + dz * t;
                        owner->UpdateAllowedPositionZ(x, y, z);
                    }
                }
            }
            bool const newWalk = target->IsWalking() || walkFinish;

            // If we already have a follow spline in flight aimed at almost the
            // same spot AND at the same walk/run speed, let it finish instead
            // of bursting a new one every tick (each new spline triggers a
            // stop+restart packet, which is the jittery "move-stop-move-stop"
            // the player sees when backpedalling). A walk/run flip is allowed
            // through so the walk-finish can pre-empt the active run-spline.
            if (owner->HasUnitState(UNIT_STATE_FOLLOW_MOVE)
                && _lastDestination
                && !owner->movespline->Finalized()
                && newWalk == _lastWalkSent)
            {
                float destChangeSq = square(x - _lastDestination->GetPositionX())
                                   + square(y - _lastDestination->GetPositionY());
                if (destChangeSq < square(FOLLOW_RECALC_DISTANCE))
                    return true;
            }

            // pets are allowed to "cheat" on pathfinding when following their master
            bool allowShortcut = false;
            if (Pet* oPet = owner->ToPet())
            {
                if (target->GetGUID() == oPet->GetOwnerGUID())
                    allowShortcut = true;
            }

            bool success = _path->CalculatePath(x, y, z, allowShortcut);
            if (!success || (_path->GetPathType() & PATHFIND_NOPATH))
            {
                owner->StopMoving();
                return true;
            }

            owner->AddUnitState(UNIT_STATE_FOLLOW_MOVE);
            AddFlag(MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);

            Movement::MoveSplineInit init(owner);

            // Interpolate Z toward target depth when both units are in water
            if (owner->IsInWater() && target->IsInWater())
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

            init.SetWalk(newWalk);
            init.SetSmooth(); // CatmullRom for smooth orientation along path
            // Only align facing to the owner when the owner has actually stopped.
            // Otherwise each new follow-spline (fired as the owner keeps moving)
            // would snap the pet forward mid-stride, producing visible head-jerks
            // while the player backpedals or strafes.
            if (!target->isMoving())
                init.SetFacing(target->GetOrientation());
            init.Launch();
            _lastDestination = Position(x, y, z);
            _lastWalkSent = newWalk;
        }
    }
    return true;
}

void FollowMovementGenerator::Deactivate(Unit* owner)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_DEACTIVATED);
    RemoveFlag(MOVEMENTGENERATOR_FLAG_TRANSITORY | MOVEMENTGENERATOR_FLAG_INFORM_ENABLED);
    owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
}

void FollowMovementGenerator::Finalize(Unit* owner, bool active, bool/* movementInform*/)
{
    AddFlag(MOVEMENTGENERATOR_FLAG_FINALIZED);
    if (active)
    {
        owner->ClearUnitState(UNIT_STATE_FOLLOW_MOVE);
        UpdatePetSpeed(owner);
    }
}

void FollowMovementGenerator::UpdatePetSpeed(Unit* owner)
{
    if (Pet* oPet = owner->ToPet())
    {
        if (!GetTarget() || GetTarget()->GetGUID() == owner->GetOwnerGUID())
        {
            oPet->UpdateSpeed(MOVE_RUN);
            oPet->UpdateSpeed(MOVE_WALK);
            oPet->UpdateSpeed(MOVE_SWIM);
        }
    }
}
