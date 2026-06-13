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

#ifndef TRINITY_FOLLOWMOVEMENTGENERATOR_H
#define TRINITY_FOLLOWMOVEMENTGENERATOR_H

#include "AbstractFollower.h"
#include "MovementDefines.h"
#include "MovementGenerator.h"
#include "Optional.h"
#include "Position.h"
#include "Timer.h"

class PathGenerator;
class Unit;

#define FOLLOW_RANGE_TOLERANCE 1.0f

class FollowMovementGenerator : public MovementGenerator, public AbstractFollower
{
    public:
        explicit FollowMovementGenerator(Unit* target, float range, ChaseAngle angle);
        ~FollowMovementGenerator();

        void Initialize(Unit*) override;
        void Reset(Unit*) override;
        bool Update(Unit*, uint32) override;
        void Deactivate(Unit*) override;
        void Finalize(Unit*, bool, bool) override;
        MovementGeneratorType GetMovementGeneratorType() const override { return FOLLOW_MOTION_TYPE; }

        void UnitSpeedChanged() override { _lastTargetPosition.reset(); }

    private:
        static constexpr uint32 CHECK_INTERVAL = 100;
        // While target is moving, skip relaunching the spline unless the new
        // destination has drifted at least this far from the active spline's
        // endpoint. Prevents per-tick spline churn that causes visible stutter
        // when the owner backpedals or strafes continuously.
        static constexpr float FOLLOW_RECALC_DISTANCE = 2.0f;
        // When a pet is making a short final approach to a stopped owner and
        // isn't in combat, walk the spline instead of running it. Produces a
        // visible "settle" rather than the run-overshoot/teleport that happens
        // when a short run-spline ends right under the owner's feet.
        static constexpr float WALK_FINISH_RADIUS = 4.0f;

        void UpdatePetSpeed(Unit* owner);

        float const _range;
        ChaseAngle const _angle;

        TimeTracker _checkTimer;
        std::unique_ptr<PathGenerator> _path;
        Optional<Position> _lastTargetPosition;
        Optional<Position> _lastDestination; // endpoint the currently active follow spline is heading toward
        bool _lastTargetWasMoving = false;   // so we can detect a target stop and re-evaluate (e.g. trigger walk-finish)
        bool _lastWalkSent = false;          // walk-flag of the currently active spline; flips bypass stability gate
};

#endif
