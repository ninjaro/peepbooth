#pragma once

#include "monitor/console_watch.hpp"
#include "monitor/observed_process.hpp"

#include <algorithm>

namespace monitor_test {
inline monitor::process_observation incident_sample(const std::uint64_t now) {
    monitor::process_observation result;
    result.identity = { 42, 99 };
    result.observed_at_ns = now;
    result.status = monitor::observation_status::sampled;
    result.sample = monitor::platform::process_sample {
        result.identity, { 4096, 8192, 0 }, 0, now
    };
    result.stale_after_ns
        = static_cast<std::uint64_t>(monitor::console_options {}.stale_ms)
        * 1'000'000ULL;
    result.watchdog.process_lifecycle = monitor::lifecycle::running;
    result.watchdog.observation_started_ns = 1;
    for (std::size_t index = 0; index < monitor::heartbeat_channel_count;
         ++index)
        result.watchdog.heartbeats[index].id
            = static_cast<monitor::channel>(index + 1U);
    result.responsiveness = monitor::health::idle;
    return result;
}

inline std::size_t
active_incidents(const monitor::observation_history& history) {
    return static_cast<std::size_t>(std::ranges::count_if(
        history.incidents().active(),
        [](const auto& item) { return item.has_value(); }
    ));
}
} // namespace monitor_test
