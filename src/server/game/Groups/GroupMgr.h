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

#ifndef _GROUPMGR_H
#define _GROUPMGR_H

#include "Group.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>

class TC_GAME_API GroupMgr
{
private:
    GroupMgr();
    ~GroupMgr();

public:
    static GroupMgr* instance();

    typedef std::map<ObjectGuid::LowType, Group*> GroupContainer;
    typedef std::vector<Group*>      GroupDbContainer;

    Group* GetGroupByGUID(ObjectGuid::LowType guid) const;

    uint32 GenerateNewGroupDbStoreId();
    void   RegisterGroupDbStoreId(uint32 storageId, Group* group);
    void   FreeGroupDbStoreId(Group* group);
    void   SetNextGroupDbStoreId(uint32 storageId) { std::lock_guard<std::mutex> lock(_dbStoreLock); NextGroupDbStoreId = storageId; };
    Group* GetGroupByDbStoreId(uint32 storageId) const;
    void   SetGroupDbStoreSize(uint32 newSize) { std::lock_guard<std::mutex> lock(_dbStoreLock); GroupDbStore.resize(newSize); }

    void Update(uint32 diff);

    void   LoadGroups();
    ObjectGuid::LowType GenerateGroupId();
    void   AddGroup(Group* group);
    void   RemoveGroup(Group* group);

protected:
    // @megaserver A4: group registry made memory-safe under parallel session
    // handlers. NextGroupId is atomic; the coupled NextGroupDbStoreId + GroupDbStore
    // share _dbStoreLock; GroupStore is guarded by a shared_mutex. (Uncontended until C.)
    std::atomic<ObjectGuid::LowType> NextGroupId;
    uint32           NextGroupDbStoreId;
    GroupContainer   GroupStore;
    GroupDbContainer GroupDbStore;
    mutable std::mutex        _dbStoreLock;
    mutable std::shared_mutex _groupStoreLock;
};

#define sGroupMgr GroupMgr::instance()

#endif
