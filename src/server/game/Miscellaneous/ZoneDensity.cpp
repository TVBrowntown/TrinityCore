/*
 * See ZoneDensity.h.
 */

#include "ZoneDensity.h"

#include <array>
#include <atomic>

namespace
{
    // covers all stock WotLK zone ids (max ~5000) with generous headroom for
    // custom content; ~256KB of atomics, zero-initialized at static init.
    constexpr uint32 ZONE_ARRAY_SIZE = 0x10000;
    std::array<std::atomic<uint32>, ZONE_ARRAY_SIZE> s_zonePlayers{};

    // per-zone population brackets. Tuned so normal play is untouched (a zone
    // rarely holds 200+) and only genuine launch/event crowds tighten toward
    // the 50yd floor. 50yd stays above WoW's ~40yd max combat range so you can
    // always see whatever can hit you.
    inline float BracketDistance(uint32 n)
    {
        if (n < 200)  return ZONE_VISIBILITY_NO_CAP;
        if (n < 400)  return 90.0f;
        if (n < 600)  return 80.0f;
        if (n < 800)  return 70.0f;
        if (n < 1000) return 60.0f;
        if (n < 1400) return 55.0f;
        return 50.0f;
    }
}

void ZoneDensity::PlayerEnteredZone(uint32 zoneId)
{
    if (zoneId && zoneId < ZONE_ARRAY_SIZE)
        s_zonePlayers[zoneId].fetch_add(1, std::memory_order_relaxed);
}

void ZoneDensity::PlayerLeftZone(uint32 zoneId)
{
    if (!zoneId || zoneId >= ZONE_ARRAY_SIZE)
        return;

    // defensive saturating decrement: never wrap below zero even if a stray
    // dec arrives without a matching inc
    uint32 cur = s_zonePlayers[zoneId].load(std::memory_order_relaxed);
    while (cur && !s_zonePlayers[zoneId].compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed))
    { }
}

uint32 ZoneDensity::GetPlayerCount(uint32 zoneId)
{
    return (zoneId && zoneId < ZONE_ARRAY_SIZE)
        ? s_zonePlayers[zoneId].load(std::memory_order_relaxed)
        : 0;
}

float ZoneDensity::GetVisibilityCap(uint32 zoneId)
{
    return BracketDistance(GetPlayerCount(zoneId));
}
