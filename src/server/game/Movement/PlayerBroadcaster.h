/*
 * Player movement-packet broadcaster, ported from vmangos (GPLv2+), extended
 * with interest-management / relevance throttling.
 *
 * Relays a player's MSG_MOVE_* packets to everyone who can see them, from
 * dedicated broadcaster threads instead of the map update thread.
 *
 * Interest management: in a crowd (listener count >= a threshold) the worker
 * prioritises who gets full vs. coarse movement. Near observers and anyone who
 * has this player TARGETED get every packet; distant observers get heartbeats
 * thinned by distance. Only heartbeats are ever thinned — start/stop/jump/
 * facing (state changes) always go through, so movement never desyncs.
 *
 * Thread-safety: this object never dereferences Player/Map state. It holds the
 * mover's WorldSocket and the broadcasters of listeners (shared_ptr), plus an
 * atomic cached position used purely as a throttling heuristic. Distances are
 * computed from cached positions, never from live game state.
 */

#ifndef TRINITY_PLAYER_BROADCASTER_H
#define TRINITY_PLAYER_BROADCASTER_H

#include "ObjectGuid.h"
#include "WorldPacket.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

class WorldSocket;

class TC_GAME_API PlayerBroadcaster final
{
    struct BroadcastData
    {
        WorldPacket packet;
        bool sendToSelf;
        ObjectGuid except;
    };

    struct ListenerInfo
    {
        std::shared_ptr<PlayerBroadcaster> bc;
        uint8 skip = 0;   // heartbeat skip counter for throttling
    };

    static constexpr std::size_t MAX_QUEUE_SIZE = 500;

    std::shared_ptr<WorldSocket> m_socket;
    ObjectGuid m_self;

    std::map<ObjectGuid, ListenerInfo> m_listeners;
    std::set<ObjectGuid> m_pinnedBy;            // listeners who targeted us (force full)
    std::vector<BroadcastData> m_queue;
    std::mutex m_listeners_lock;                // guards m_listeners
    std::mutex m_pinned_lock;                   // guards m_pinnedBy (separate so
                                                // SetSelection never waits on a fan-out)
    std::mutex m_queue_lock;

    // cached owner position (throttling heuristic only; relaxed atomics)
    std::atomic<float> m_posX{ 0.0f };
    std::atomic<float> m_posY{ 0.0f };
    std::atomic<float> m_posZ{ 0.0f };

    uint32 m_lastUpdatePackets = 0;

    void ProcessQueue(uint32& num_packets);
    void SendPacket(WorldPacket const& packet);

    // throttle config (set once from worldserver.conf at startup)
    static bool s_throttleEnabled;
    static std::size_t s_minListeners;
    static float s_fullRangeSq;
    static float s_coarseRangeSq;

public:
    PlayerBroadcaster(std::shared_ptr<WorldSocket> socket, ObjectGuid const& self);
    ~PlayerBroadcaster();

    ObjectGuid GetGUID() const { return m_self; }

    static void ConfigureThrottle(bool enabled, std::size_t minListeners, float fullRange, float coarseRange);

    // mover side: queue one of our own movement packets for relay
    void QueuePacket(WorldPacket packet, bool self, ObjectGuid except);
    // cheap position cache update (map thread, when the owner moves)
    void SetPosition(float x, float y, float z);

    // listener side: `listener` starts/stops receiving our movement
    void AddListener(ObjectGuid listenerGuid, std::shared_ptr<PlayerBroadcaster> const& listener);
    void RemoveListener(ObjectGuid listenerGuid);

    // combat relevance: `listener` targeted us (or untargeted us) — pin them to
    // full fidelity regardless of distance
    void SetListenerPinned(ObjectGuid listenerGuid, bool pinned);

    // logout: drop the socket and all state
    void FreeAtLogout();

    friend class MovementBroadcaster;
};

#endif
