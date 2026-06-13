/*
 * Dynamic visibility scaling, ported from AzerothCore (SunwellCore /
 * pussywizard), GPLv2+.
 */

#include "DynamicVisibility.h"

uint8 DynamicVisibilityMgr::visibilitySettingsIndex = 0;

void DynamicVisibilityMgr::Update(uint32 sessionCount)
{
    // hysteresis: step up at the bracket boundary, step back down only once
    // the population falls 100 below it, so the index doesn't flap
    if (sessionCount >= (visibilitySettingsIndex + 1) * ((uint32)VISIBILITY_SETTINGS_PLAYER_INTERVAL) && visibilitySettingsIndex < VISIBILITY_SETTINGS_MAX_INTERVAL_NUM - 1)
        ++visibilitySettingsIndex;
    else if (visibilitySettingsIndex && sessionCount < visibilitySettingsIndex * ((uint32)VISIBILITY_SETTINGS_PLAYER_INTERVAL) - 100)
        --visibilitySettingsIndex;
}
