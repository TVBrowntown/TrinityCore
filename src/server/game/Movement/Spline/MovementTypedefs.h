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

#ifndef TRINITYSERVER_TYPEDEFS_H
#define TRINITYSERVER_TYPEDEFS_H

#include "Common.h"
#include <atomic>

namespace G3D
{
    class Vector3;
    class Vector4;
}

namespace Movement
{
    using G3D::Vector3;
    using G3D::Vector4;

    inline uint32 SecToMS(float sec)
    {
        return static_cast<uint32>(sec * 1000.f);
    }

    inline float MSToSec(uint32 ms)
    {
        return ms / 1000.f;
    }

    float computeFallTime(float path_length, bool isSafeFall);
    float computeFallElevation(float t_passed, bool isSafeFall, float start_velocity = 0.0f);

    // @megaserver D: m_counter is atomic so spline-id generation is safe when
    // creatures pathfind/move on parallel combat-update threads. The CAS loop
    // reproduces the original wrap-at-limit semantics exactly (TSan-confirmed race
    // on Movement::splineIdGen via counter::Increase()).
    template<class T, T limit>
    class counter
    {
    public:
        counter() { init(); }

        void Increase() { NewId(); }

        T NewId()
        {
            T cur = m_counter.load(std::memory_order_relaxed);
            T next;
            do
            {
                next = (cur == limit) ? T(0) : T(cur + 1);
            } while (!m_counter.compare_exchange_weak(cur, next, std::memory_order_relaxed));
            return next;
        }

        T getCurrent() const { return m_counter.load(std::memory_order_relaxed); }

    private:
        void init() { m_counter.store(0, std::memory_order_relaxed); }
        std::atomic<T> m_counter;
    };

    typedef counter<uint32, 0xFFFFFFFF> UInt32Counter;

    TC_GAME_API extern float gravity;
    TC_GAME_API extern UInt32Counter splineIdGen;
}

#endif // TRINITYSERVER_TYPEDEFS_H
