/*
 * Movement broadcaster thread pool, ported from vmangos (GPLv2+).
 * N worker threads, each owning a guid-sharded set of PlayerBroadcasters,
 * flush all queued movement packets every Frequency milliseconds.
 *
 * Config (worldserver.conf, read once at startup):
 *   Network.PacketBroadcast.Threads   = 0 disables (movement relays on the
 *                                       map threads, stock behavior)
 *   Network.PacketBroadcast.Frequency = flush interval in ms (default 50)
 */

#ifndef TRINITY_MOVEMENT_BROADCASTER_H
#define TRINITY_MOVEMENT_BROADCASTER_H

#include "Define.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <shared_mutex>
#include <thread>
#include <vector>

class PlayerBroadcaster;

class TC_GAME_API MovementBroadcaster final
{
    typedef std::set<std::shared_ptr<PlayerBroadcaster>> PlayersBCastSet;

    std::size_t m_num_threads;
    std::atomic_bool m_stop;
    std::vector<std::thread> m_threads;
    std::chrono::milliseconds m_sleep_timer;

    std::vector<PlayersBCastSet> m_thread_players;
    std::vector<std::shared_timed_mutex> m_thread_locks;

    void Work(std::size_t thread_id);
    void BroadcastPackets(std::size_t index, uint32& num_packets);

    MovementBroadcaster(std::size_t threads, std::chrono::milliseconds frequency);

public:
    ~MovementBroadcaster();

    // created from World::SetInitialWorldSettings (after config load),
    // stopped from World shutdown
    static void Initialize(std::size_t threads, std::chrono::milliseconds frequency);
    static void Shutdown();
    static MovementBroadcaster* instance();   // nullptr when disabled/uninitialized

    bool IsEnabled() const { return m_num_threads != 0; }

    void RegisterPlayer(std::shared_ptr<PlayerBroadcaster> const& player);
    void RemovePlayer(std::shared_ptr<PlayerBroadcaster> const& player);

    void Stop();
};

#endif
