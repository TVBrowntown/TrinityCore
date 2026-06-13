/*
 * Ported from vmangos (GPLv2+), extended with relevance throttling.
 * See PlayerBroadcaster.h.
 */

#include "PlayerBroadcaster.h"
#include "WorldSocket.h"
#include "Opcodes.h"

bool PlayerBroadcaster::s_throttleEnabled = true;
std::size_t PlayerBroadcaster::s_minListeners = 50;
float PlayerBroadcaster::s_fullRangeSq = 20.0f * 20.0f;
float PlayerBroadcaster::s_coarseRangeSq = 45.0f * 45.0f;

namespace
{
    // Only pure position keepalives are thinnable. State changes (start, stop,
    // jump, fall, facing) are events every observer must get, or remote players
    // visibly desync ("stuck running"). Conservative on purpose.
    inline bool IsThrottleableMovement(uint16 opcode)
    {
        return opcode == MSG_MOVE_HEARTBEAT;
    }
}

void PlayerBroadcaster::ConfigureThrottle(bool enabled, std::size_t minListeners, float fullRange, float coarseRange)
{
    s_throttleEnabled = enabled;
    s_minListeners = minListeners;
    s_fullRangeSq = fullRange * fullRange;
    s_coarseRangeSq = coarseRange * coarseRange;
}

PlayerBroadcaster::PlayerBroadcaster(std::shared_ptr<WorldSocket> socket, ObjectGuid const& self)
    : m_socket(std::move(socket)), m_self(self)
{
    m_queue.reserve(64);
}

PlayerBroadcaster::~PlayerBroadcaster()
{
    m_socket = nullptr;
}

void PlayerBroadcaster::SendPacket(WorldPacket const& packet)
{
    if (m_socket)
        m_socket->SendPacket(packet);
}

void PlayerBroadcaster::SetPosition(float x, float y, float z)
{
    m_posX.store(x, std::memory_order_relaxed);
    m_posY.store(y, std::memory_order_relaxed);
    m_posZ.store(z, std::memory_order_relaxed);
}

void PlayerBroadcaster::AddListener(ObjectGuid listenerGuid, std::shared_ptr<PlayerBroadcaster> const& listener)
{
    if (!listener || listenerGuid == m_self)
        return;

    std::lock_guard<std::mutex> guard(m_listeners_lock);
    m_listeners[listenerGuid].bc = listener;
}

void PlayerBroadcaster::RemoveListener(ObjectGuid listenerGuid)
{
    std::lock_guard<std::mutex> guard(m_listeners_lock);
    m_listeners.erase(listenerGuid);
}

void PlayerBroadcaster::SetListenerPinned(ObjectGuid listenerGuid, bool pinned)
{
    std::lock_guard<std::mutex> guard(m_pinned_lock);
    if (pinned)
        m_pinnedBy.insert(listenerGuid);
    else
        m_pinnedBy.erase(listenerGuid);
}

void PlayerBroadcaster::QueuePacket(WorldPacket packet, bool self, ObjectGuid except)
{
    BroadcastData data;
    data.packet = std::move(packet);
    data.sendToSelf = self;
    data.except = except;

    std::lock_guard<std::mutex> guard(m_queue_lock);

    // overflow: everything queued here is an idempotent MSG_MOVE_* state
    // update, so collapsing onto the newest entry loses nothing important
    if (m_queue.size() >= MAX_QUEUE_SIZE)
    {
        m_queue.back() = std::move(data);
        return;
    }
    m_queue.emplace_back(std::move(data));
}

void PlayerBroadcaster::ProcessQueue(uint32& num_packets)
{
    {
        std::lock_guard<std::mutex> q_check(m_queue_lock);
        if (m_queue.empty())
            return;
    }

    std::unique_lock<std::mutex> q_g(m_queue_lock);
    std::vector<BroadcastData> queue = std::move(m_queue);
    m_queue.clear();
    q_g.unlock();

    // small snapshot of who pinned us (targeters) — taken under its own lock so
    // a concurrent SetSelection never blocks on the listener fan-out below
    std::set<ObjectGuid> pinned;
    {
        std::lock_guard<std::mutex> p_g(m_pinned_lock);
        pinned = m_pinnedBy;
    }

    std::lock_guard<std::mutex> v_g(m_listeners_lock);

    // self-echo (rare path; we normally pass self=false)
    for (BroadcastData& data : queue)
        if (data.sendToSelf && data.except != m_self)
            SendPacket(data.packet);

    // throttling only engages under crowd load — normal play sends everything
    bool const throttle = s_throttleEnabled && m_listeners.size() >= s_minListeners;
    float const mx = m_posX.load(std::memory_order_relaxed);
    float const my = m_posY.load(std::memory_order_relaxed);
    float const mz = m_posZ.load(std::memory_order_relaxed);

    for (auto& [guid, info] : m_listeners)
    {
        // tier: 0 = full (every packet), 1 = mid (every 2nd hb), 2 = coarse (every 4th)
        uint8 tier = 0;
        if (throttle && !pinned.count(guid))
        {
            float const dx = mx - info.bc->m_posX.load(std::memory_order_relaxed);
            float const dy = my - info.bc->m_posY.load(std::memory_order_relaxed);
            float const dz = mz - info.bc->m_posZ.load(std::memory_order_relaxed);
            float const distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > s_coarseRangeSq)
                tier = 2;
            else if (distSq > s_fullRangeSq)
                tier = 1;
        }

        for (BroadcastData& data : queue)
        {
            if (guid == data.except)
                continue;

            if (tier && IsThrottleableMovement(data.packet.GetOpcode()))
            {
                uint8 const period = (tier == 1) ? 2 : 4;
                if (++info.skip % period != 0)
                    continue;   // thin this heartbeat for a distant observer
            }

            info.bc->SendPacket(data.packet);
            ++num_packets;
        }
    }

    m_lastUpdatePackets = num_packets;
}

void PlayerBroadcaster::FreeAtLogout()
{
    m_socket = nullptr;
    std::scoped_lock guard(m_queue_lock, m_listeners_lock, m_pinned_lock);
    m_queue.clear();
    m_listeners.clear();
    m_pinnedBy.clear();
}
