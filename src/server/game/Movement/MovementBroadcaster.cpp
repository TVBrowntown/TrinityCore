/*
 * Ported from vmangos (GPLv2+). See MovementBroadcaster.h.
 */

#include "MovementBroadcaster.h"
#include "PlayerBroadcaster.h"
#include "GameTime.h"
#include "Log.h"
#include "Timer.h"

static std::unique_ptr<MovementBroadcaster> s_instance;

MovementBroadcaster* MovementBroadcaster::instance()
{
    return s_instance.get();
}

void MovementBroadcaster::Initialize(std::size_t threads, std::chrono::milliseconds frequency)
{
    s_instance.reset(new MovementBroadcaster(threads, frequency));
}

void MovementBroadcaster::Shutdown()
{
    s_instance.reset();
}

MovementBroadcaster::MovementBroadcaster(std::size_t threads, std::chrono::milliseconds frequency)
    : m_num_threads(threads), m_stop(false), m_sleep_timer(frequency)
{
    if (threads)
        TC_LOG_INFO("server.loading", "[NETWORK] Movement broadcaster running every {}ms with {} threads",
            uint32(frequency.count()), uint32(threads));
    else
        TC_LOG_INFO("server.loading", "[NETWORK] Movement broadcaster disabled, movement relays on map threads");

    // vectors of mutexes can't be resized; build in place before threads start
    std::vector<std::shared_timed_mutex> locks(m_num_threads);
    m_thread_locks = std::move(locks);
    m_thread_players.resize(m_num_threads);

    for (std::size_t i = 0; i < m_num_threads; ++i)
        m_threads.emplace_back(&MovementBroadcaster::Work, this, i);
}

MovementBroadcaster::~MovementBroadcaster()
{
    Stop();
}

void MovementBroadcaster::RegisterPlayer(std::shared_ptr<PlayerBroadcaster> const& player)
{
    if (!m_num_threads || !player)
        return;

    std::size_t index = player->GetGUID().GetRawValue() % m_num_threads;
    std::lock_guard<std::shared_timed_mutex> guard(m_thread_locks[index]);
    m_thread_players[index].insert(player);
}

void MovementBroadcaster::RemovePlayer(std::shared_ptr<PlayerBroadcaster> const& player)
{
    if (!m_num_threads || !player)
        return;

    std::size_t index = player->GetGUID().GetRawValue() % m_num_threads;
    std::lock_guard<std::shared_timed_mutex> guard(m_thread_locks[index]);
    m_thread_players[index].erase(player);
}

void MovementBroadcaster::BroadcastPackets(std::size_t index, uint32& num_packets)
{
    PlayersBCastSet my_players;
    {
        std::shared_lock<std::shared_timed_mutex> guard(m_thread_locks[index]);
        my_players = m_thread_players[index];
    }

    for (std::shared_ptr<PlayerBroadcaster> const& player : my_players)
        player->ProcessQueue(num_packets);
}

void MovementBroadcaster::Work(std::size_t thread_id)
{
    while (!m_stop)
    {
        uint32 num_packets = 0;
        uint32 begin_time = getMSTime();
        BroadcastPackets(thread_id, num_packets);
        uint32 elapsed = GetMSTimeDiffToNow(begin_time);

        if (elapsed > 100)
            TC_LOG_WARN("misc", "MovementBroadcaster thread {}: {}ms to process queue [{} packets]",
                uint32(thread_id), elapsed, num_packets);

        std::this_thread::sleep_for(m_sleep_timer);
    }
}

void MovementBroadcaster::Stop()
{
    m_stop = true;

    for (std::thread& thread : m_threads)
        if (thread.joinable())
            thread.join();

    m_threads.clear();
}
