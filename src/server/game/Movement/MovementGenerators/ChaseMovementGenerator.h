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

#ifndef TRINITY_CHASEMOVEMENTGENERATOR_H
#define TRINITY_CHASEMOVEMENTGENERATOR_H

#include "AbstractFollower.h"
#include "MovementDefines.h"
#include "MovementGenerator.h"
#include "Optional.h"
#include "Position.h"
#include "Timer.h"

class PathGenerator;
class Unit;

class ChaseMovementGenerator : public MovementGenerator, public AbstractFollower
{
    public:
        explicit ChaseMovementGenerator(Unit* target, Optional<ChaseRange> range = {}, Optional<ChaseAngle> angle = {});
        ~ChaseMovementGenerator();

        void Initialize(Unit*) override;
        void Reset(Unit*) override;
        bool Update(Unit*, uint32) override;
        void Deactivate(Unit*) override;
        void Finalize(Unit*, bool, bool) override;
        MovementGeneratorType GetMovementGeneratorType() const override { return CHASE_MOTION_TYPE; }

        void UnitSpeedChanged() override { _lastTargetPosition.reset(); }

    private:
        static constexpr uint32 RANGE_CHECK_INTERVAL = 100; // time (ms) until we attempt to recalculate
        static constexpr uint32 RANGE_CHECK_INTERVAL_SMOOTH = 200; // longer interval when movement is smooth
        static constexpr float PATH_RECALC_DISTANCE_THRESHOLD = 2.0f; // yards - only recalc if destination changes significantly
        static constexpr float MIN_CHASE_RELOCATE_DIST_SQ = 1.5f * 1.5f; // don't start a new spline for tiny movements

        // Hysteresis band so brief speed wobbles don't snap predictive mode on/off
        static constexpr float PREDICT_ENABLE_SPEED  = 1.5f; // yd/s — must reach this to start predicting
        static constexpr float PREDICT_DISABLE_SPEED = 0.5f; // yd/s — must fall below this to stop predicting

        Optional<ChaseRange> const _range;
        Optional<ChaseAngle> const _angle;

        std::unique_ptr<PathGenerator> _path;
        Optional<Position> _lastTargetPosition;
        Optional<Position> _lastDestination; // actual destination we're pathing to (works for all chase modes)
        TimeTracker _rangeCheckTimer;
        bool _movingTowards = true;
        bool _mutualChase = true;
        uint32 _smoothMovementCount = 0; // tracks consecutive smooth updates
        bool _predictiveActive = false; // sticky predictive-pursuit state, gated by hysteresis band above

        // Predictive pursuit. 0.5s look-ahead = up to ~3.5y lead at run speed,
        // short enough that a target turn corrects within one re-solve cycle.
        Position PredictTargetPosition(Unit* owner, Unit* target, float maxPredictionTime = 0.5f);
};

#endif
