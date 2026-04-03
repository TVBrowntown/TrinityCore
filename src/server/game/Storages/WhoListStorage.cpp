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

#include "WhoListStorage.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "GuildMgr.h"
#include "WorldSession.h"
//npcbot
#include "botdatamgr.h"
#include "Creature.h"
#include "Map.h"
//end npcbot

WhoListStorageMgr* WhoListStorageMgr::instance()
{
    static WhoListStorageMgr instance;
    return &instance;
}

void WhoListStorageMgr::Update()
{
    // clear current list
    _whoListStorage.clear();
    _whoListStorage.reserve(sWorld->GetPlayerCount()+1);

    HashMapHolder<Player>::MapType const& m = ObjectAccessor::GetPlayers();
    for (HashMapHolder<Player>::MapType::const_iterator itr = m.begin(); itr != m.end(); ++itr)
    {
        if (!itr->second->FindMap() || itr->second->GetSession()->PlayerLoading())
            continue;

        std::string playerName = itr->second->GetName();
        std::wstring widePlayerName;
        if (!Utf8toWStr(playerName, widePlayerName))
            continue;

        wstrToLower(widePlayerName);

        std::string guildName = sGuildMgr->GetGuildNameById(itr->second->GetGuildId());
        std::wstring wideGuildName;
        if (!Utf8toWStr(guildName, wideGuildName))
            continue;

        wstrToLower(wideGuildName);

        _whoListStorage.emplace_back(itr->second->GetGUID(), itr->second->GetTeam(), itr->second->GetSession()->GetSecurity(), itr->second->GetLevel(),
            itr->second->GetClass(), itr->second->GetRace(), itr->second->GetZoneId(), itr->second->GetNativeGender(), itr->second->IsVisible(),
            widePlayerName, wideGuildName, playerName, guildName);
    }

    //npcbot: add wandering bots to who list
    if (BotDataMgr::AllBotsLoaded())
    {
        NpcBotRegistry const& botList = BotDataMgr::GetExistingNPCBots();
        for (Creature const* bot : botList)
        {
            if (!bot || !bot->IsAlive() || !bot->FindMap() || !bot->IsFreeBot())
                continue;

            std::string botName = bot->GetName();
            std::wstring wideBotName;
            if (!Utf8toWStr(botName, wideBotName))
                continue;
            wstrToLower(wideBotName);

            NpcBotExtras const* extras = BotDataMgr::SelectNpcBotExtras(bot->GetEntry());
            if (!extras)
                continue;

            uint8 botClass = extras->bclass;
            uint8 botRace = extras->race;
            uint32 botTeam = (botRace == 2 || botRace == 5 || botRace == 6 || botRace == 8) ? HORDE : ALLIANCE;
            uint8 botGender = 0;
            if (NpcBotAppearanceData const* appearance = BotDataMgr::SelectNpcBotAppearance(bot->GetEntry()))
                botGender = appearance->gender;

            std::wstring wideGuildEmpty;
            _whoListStorage.emplace_back(bot->GetGUID(), botTeam, SEC_PLAYER, bot->GetLevel(),
                botClass, botRace, bot->GetZoneId(), botGender, true,
                wideBotName, wideGuildEmpty, botName, "");
        }
    }
    //end npcbot
}
