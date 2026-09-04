#ifndef CPPR_MONITOR_WATCHDOG_HPP
#define CPPR_MONITOR_WATCHDOG_HPP

#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/shared_state.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace monitor {

inline constexpr std::uint64_t default_watcher_lease_ns = 3'000'000'000ULL;

enum class health {
    awaiting_heartbeat,
    healthy,
    gui_stalled,
    globally_stalled_idle,
    globally_stalled_busy,
};

struct heartbeat_snapshot {
    channel id { channel::main };
    bool active {};
    std::uint64_t timestamp_ns {};
    std::uint64_t sequence {};
};

struct watchdog_snapshot {
    std::string process_name;
    lifecycle process_lifecycle { lifecycle::starting };
    std::array<heartbeat_snapshot, heartbeat_channel_count> heartbeats {};
    std::vector<breadcrumb> breadcrumbs;
};

class watchdog_session final {
public:
    watchdog_session() = default;
    ~watchdog_session();
    watchdog_session(const watchdog_session&) = delete;
    watchdog_session& operator=(const watchdog_session&) = delete;
    watchdog_session(watchdog_session&& other) noexcept;
    watchdog_session& operator=(watchdog_session&& other) noexcept;

    [[nodiscard]] static std::optional<watchdog_session>
    attach(platform::process_identity identity);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] watchdog_snapshot snapshot() const;
    void renew_lease(
        std::uint64_t now_ns,
        std::uint64_t duration_ns = default_watcher_lease_ns
    ) noexcept;
    void release() noexcept;

private:
    platform::shared_mapping mapping_;
};

struct memory_trend_summary {
    std::uint64_t duration_ns {};
    std::int64_t rss_delta_bytes {};
    double bytes_per_second {};
    bool sustained_growth {};
};

class memory_trend final {
public:
    explicit memory_trend(
        std::size_t capacity = 120,
        std::uint64_t window_ns = 30ULL * 60ULL * 1'000'000'000ULL
    );

    void add(std::uint64_t timestamp_ns, std::uint64_t rss_bytes);
    [[nodiscard]] memory_trend_summary summary() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct point {
        std::uint64_t timestamp_ns {};
        std::uint64_t rss_bytes {};
    };

    std::size_t capacity_;
    std::uint64_t window_ns_;
    std::deque<point> points_;
};

[[nodiscard]] health classify_health(
    const watchdog_snapshot& snapshot, std::uint64_t now_ns,
    std::uint64_t stale_after_ns, double cpu_percent
) noexcept;
[[nodiscard]] const char* health_name(health value) noexcept;
[[nodiscard]] const char* channel_name(channel value) noexcept;
[[nodiscard]] const char* event_name(std::uint16_t value) noexcept;

} // namespace monitor

#endif // CPPR_MONITOR_WATCHDOG_HPP
