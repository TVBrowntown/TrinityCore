/*
 * Locality-aware adaptive visibility-distance reduction.
 *
 * Tracks live player counts per zone and shrinks open-world sight range only
 * in the zones that are actually crowded — so a packed starting zone tightens
 * to a 50yd floor while a quiet zone on the same busy realm keeps full range.
 * This supersedes the global session-count distance reduction (which shrank
 * every continent regardless of where the players were).
 *
 * Counters are lock-free atomics: they are read on the hot visibility path
 * from BOTH the map-update threads and the main session thread (chat's
 * SendMessageToSet -> GetVisibilityRange), so a plain map would race a rehash.
 * Balance is guaranteed by Player::UpdateDensityCount (one matched inc/dec per
 * player), independent of the stock per-map _zonePlayerCountMap.
 */

#ifndef TRINITY_ZONE_DENSITY_H
#define TRINITY_ZONE_DENSITY_H

#include "Define.h"

// sentinel meaning "no reduction" — larger than any configured visibility range
#define ZONE_VISIBILITY_NO_CAP 99999.0f

namespace ZoneDensity
{
    void PlayerEnteredZone(uint32 zoneId);
    void PlayerLeftZone(uint32 zoneId);
    uint32 GetPlayerCount(uint32 zoneId);

    // visibility-distance cap for a zone given its live population; returns
    // ZONE_VISIBILITY_NO_CAP when uncrowded. Callers std::min this against the
    // configured range so it only ever lowers it.
    float GetVisibilityCap(uint32 zoneId);
}

#endif
