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

#ifndef _MMAP_MANAGER_H
#define _MMAP_MANAGER_H

#include "Define.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include <string>
#include <unordered_map>
#include <vector>

//  move map related classes
namespace MMAP
{
    // @megaserver D: a dtNavMeshQuery holds a mutable node pool and is NOT thread-safe,
    // but the underlying dtNavMesh is read-only and shareable. So instead of one query
    // per instance, keep a small POOL of queries per instance, one per "query slot".
    // Each parallel combat-update worker uses its own slot (SetNavMeshQuerySlot), so
    // concurrent pathfinding never shares a query. Slot 0 is the main/serial thread.
    enum { NAV_QUERY_SLOTS = 33 };  // slot 0 (main) + up to 32 parallel workers

    typedef std::unordered_map<uint32, dtTileRef> MMapTileSet;
    typedef std::unordered_map<uint32, std::vector<dtNavMeshQuery*>> NavMeshQuerySet;

    // set the calling thread's navmesh-query slot (0 = main); a parallel worker sets index+1
    TC_COMMON_API void SetNavMeshQuerySlot(int slot);

    // dummy struct to hold map's mmap data
    struct TC_COMMON_API MMapData
    {
        MMapData(dtNavMesh* mesh) : navMesh(mesh) { }
        ~MMapData()
        {
            for (NavMeshQuerySet::iterator i = navMeshQueries.begin(); i != navMeshQueries.end(); ++i)
                for (dtNavMeshQuery* q : i->second)
                    if (q)
                        dtFreeNavMeshQuery(q);

            if (navMesh)
                dtFreeNavMesh(navMesh);
        }

        // instanceId -> per-slot query pool (size NAV_QUERY_SLOTS; slots created lazily,
        // each used by one thread at a time so no per-query synchronization is needed)
        NavMeshQuerySet navMeshQueries;

        dtNavMesh* navMesh;
        MMapTileSet loadedTileRefs;        // maps [map grid coords] to [dtTile]
    };

    typedef std::unordered_map<uint32, MMapData*> MMapDataSet;

    // singleton class
    // holds all all access to mmap loading unloading and meshes
    class TC_COMMON_API MMapManager
    {
        public:
            MMapManager() : loadedTiles(0), thread_safe_environment(true) {}
            ~MMapManager();

            void InitializeThreadUnsafe(const std::vector<uint32>& mapIds);
            bool loadMap(std::string const& basePath, uint32 mapId, int32 x, int32 y);
            bool loadMapInstance(std::string const& basePath, uint32 mapId, uint32 instanceId);
            bool unloadMap(uint32 mapId, int32 x, int32 y);
            bool unloadMap(uint32 mapId);
            bool unloadMapInstance(uint32 mapId, uint32 instanceId);

            // the returned [dtNavMeshQuery const*] is NOT threadsafe
            dtNavMeshQuery const* GetNavMeshQuery(uint32 mapId, uint32 instanceId);
            dtNavMesh const* GetNavMesh(uint32 mapId);

            uint32 getLoadedTilesCount() const { return loadedTiles; }
            uint32 getLoadedMapsCount() const { return uint32(loadedMMaps.size()); }
        private:
            bool loadMapData(std::string const& basePath, uint32 mapId);
            uint32 packTileID(int32 x, int32 y);

            MMapDataSet::const_iterator GetMMapData(uint32 mapId) const;
            MMapDataSet loadedMMaps;
            uint32 loadedTiles;
            bool thread_safe_environment;
    };
}

#endif
