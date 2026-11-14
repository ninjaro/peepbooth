#include "monitor/console_watch.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/watchdog.hpp"
#include "watchdog_checks.hpp"

#include <string>
#include <string_view>

namespace monitor_test {

void test_status_parser() {
    const auto parsed = monitor::platform::parse_proc_status(
        "Name:\ttest\nVmHWM:\t4096 kB\nVmRSS:\t2048 kB\nVmSwap:\t16 kB\n"
    );
    check(parsed.has_value(), "parse /proc status");
    if (parsed) {
        check(parsed->rss_bytes == 2048ULL * 1024ULL, "RSS in bytes");
        check(parsed->high_water_bytes == 4096ULL * 1024ULL, "HWM in bytes");
        check(parsed->swap_bytes == 16ULL * 1024ULL, "swap in bytes");
    }
    check(
        !monitor::platform::parse_proc_status("Name:\ttest\n"),
        "RSS is required"
    );
}

void test_memory_trend() {
    constexpr std::uint64_t second = 1'000'000'000ULL;
    monitor::memory_trend trend(8, 10 * second);
    trend.add(0, 1000);
    trend.add(2 * second, 1100);
    trend.add(4 * second, 1200);
    trend.add(6 * second, 1300);
    const auto growing = trend.summary();
    check(growing.sustained_growth, "monotonic growth is detected");
    check(growing.rss_delta_bytes == 300, "trend delta");

    monitor::memory_trend spike;
    spike.add(0, 1000);
    spike.add(2 * second, 5000);
    spike.add(4 * second, 1000);
    spike.add(6 * second, 1000);
    check(!spike.summary().sustained_growth, "one-off spike is not sustained");

    monitor::memory_trend startup_step;
    startup_step.add(0, 1000);
    startup_step.add(second, 3000);
    startup_step.add(2 * second, 5000);
    startup_step.add(3 * second, 5000);
    startup_step.add(4 * second, 5000);
    startup_step.add(5 * second, 5000);
    startup_step.add(6 * second, 5000);
    check(
        !startup_step.summary().sustained_growth,
        "one allocation followed by a plateau is not sustained"
    );
}

void test_health_classification() {
    monitor::watchdog_snapshot state;
    check(
        monitor::classify_health(state, 1000, 100, 0)
            == monitor::health::awaiting_heartbeat,
        "inactive channels await a heartbeat"
    );
    state.heartbeats[1] = { monitor::channel::gui, true, 100, 1 };
    state.heartbeats[2] = { monitor::channel::core, true, 950, 1 };
    check(
        monitor::classify_health(state, 1000, 100, 0)
            == monitor::health::gui_stalled,
        "current core distinguishes a GUI stall"
    );
    state.heartbeats[2].timestamp_ns = 100;
    check(
        monitor::classify_health(state, 1000, 100, 2)
            == monitor::health::globally_stalled_idle,
        "stale channels and idle CPU classify an idle stall"
    );
    check(
        monitor::classify_health(state, 1000, 100, 75)
            == monitor::health::globally_stalled_busy,
        "stale channels and busy CPU classify a busy stall"
    );

    state.heartbeats[1].timestamp_ns = 950;
    check(
        monitor::classify_health(state, 1000, 100, 2)
            == monitor::health::degraded,
        "fresh GUI must not hide a stale core channel"
    );
    state.heartbeats[1].active = false;
    state.heartbeats[0] = { monitor::channel::main, true, 950, 1 };
    state.heartbeats[3] = { monitor::channel::worker, true, 950, 1 };
    check(
        monitor::classify_health(state, 1000, 100, 75)
            == monitor::health::degraded,
        "fresh main and worker must not hide stale core evidence at high CPU"
    );
    state.heartbeats[2].timestamp_ns = 0;
    check(
        monitor::classify_health(state, 1000, 100, 0)
            == monitor::health::degraded,
        "pending active core is missing evidence, not healthy"
    );
    state.heartbeats[2].active = false;
    check(
        monitor::classify_health(state, 1000, 100, 0)
            == monitor::health::healthy,
        "inactive channels do not degrade health"
    );
    state.heartbeats[2] = { monitor::channel::core, true, 900, 2 };
    check(
        monitor::classify_health(state, 1000, 100, 0)
            == monitor::health::healthy,
        "recovery at the inclusive freshness boundary is healthy"
    );
    check(
        monitor::classify_health(state, 1001, 100, 0)
            == monitor::health::degraded,
        "one tick past the threshold degrades a partial channel set"
    );
    check(
        std::string_view(monitor::health_name(monitor::health::degraded))
            == "DEGRADED",
        "console exposes the partial-channel classification"
    );
}

void test_expected_progress_classification() {
    monitor::watchdog_snapshot state;
    state.process_lifecycle = monitor::lifecycle::running;
    check(
        monitor::classify_health(state, 1000, 100, 0) == monitor::health::idle,
        "no expectation is healthy inactivity, not a hang"
    );
    state.heartbeats[2] = { monitor::channel::core, true, 0, 0, 1000 };
    check(
        monitor::classify_health(state, 1100, 100, 0)
            == monitor::health::awaiting_heartbeat,
        "activation starts the configured evidence interval without progress"
    );
    check(
        monitor::classify_health(state, 1101, 100, 0)
            == monitor::health::globally_stalled_idle,
        "missing first owner progress times out after the same interval"
    );
    state.heartbeats[1] = { monitor::channel::gui, true, 1101, 1 };
    check(
        monitor::classify_health(state, 1101, 100, 0)
            == monitor::health::degraded,
        "another channel cannot satisfy the missing owner obligation"
    );
    state.heartbeats[2].timestamp_ns = 1099;
    state.heartbeats[2].sequence = 1;
    state.observation_started_ns = 1200;
    state.heartbeats[1].active = false;
    check(
        monitor::classify_health(state, 1300, 100, 0)
            == monitor::health::awaiting_heartbeat,
        "reattachment does not borrow pre-attachment evidence or idle gaps"
    );
    check(
        monitor::classify_health(state, 1301, 100, 90)
            == monitor::health::globally_stalled_busy,
        "reattachment cannot hide a continuing lack of progress indefinitely"
    );
    state.heartbeats[2].timestamp_ns = 1201;
    check(
        monitor::classify_health(state, 1301, 100, 0)
            == monitor::health::healthy,
        "actual owner progress recovers at the inclusive age boundary"
    );
    state.heartbeats[0].coherent = false;
    check(
        monitor::classify_health(state, 1301, 100, 0)
            == monitor::health::evidence_unavailable,
        "an incomplete publication is unknown, never fabricated health"
    );
    for (const auto ending :
         { monitor::lifecycle::stopping, monitor::lifecycle::clean_exit }) {
        state.process_lifecycle = ending;
        check(
            monitor::classify_health(state, 9000, 100, 90)
                == monitor::health::idle,
            "ending producer lifecycle has no live progress expectations"
        );
    }
}

void test_yodau_progress_semantics() {
    // These are the states emitted by Yodau's callback/supervisor owners, not
    // invented per-frame pulses. Use the current operator default only to place
    // observations on its inclusive boundary; this introduces no new timeout.
    const auto stale
        = static_cast<std::uint64_t>(monitor::console_options {}.stale_ms)
        * 1'000'000ULL;
    const auto start = stale;
    const auto late = start + stale + 1;
    monitor::watchdog_snapshot state;
    state.process_lifecycle = monitor::lifecycle::running;
    state.observation_started_ns = start;
    state.heartbeats[2] = { monitor::channel::core, false, start, 1, 0 };
    check(
        monitor::classify_health(state, late, stale, 90)
            == monitor::health::idle,
        "passive activity and busy CPU create no Yodau obligation"
    );
    state.heartbeats[2] = { monitor::channel::core, true, 0, 0, start };
    check(
        monitor::classify_health(state, start + stale, stale, 0)
            == monitor::health::awaiting_heartbeat,
        "an executing callback is pending, not hung at the inclusive boundary"
    );
    check(
        monitor::classify_health(state, late, stale, 90)
            == monitor::health::globally_stalled_busy,
        "CPU activity is not a callback completion checkpoint"
    );
    state.heartbeats[0] = { monitor::channel::main, true, late, 1, start };
    check(
        monitor::classify_health(state, late, stale, 0)
            == monitor::health::degraded,
        "completed supervisor polls cannot conceal an overdue callback"
    );
    state.heartbeats[0].timestamp_ns += stale;
    ++state.heartbeats[0].sequence;
    check(
        monitor::classify_health(state, late + stale, stale, 90)
            == monitor::health::degraded,
        "continued main progress extends only main evidence"
    );
    state.heartbeats[2].active = false;
    state.heartbeats[2].expected_since_ns = 0;
    check(
        monitor::classify_health(state, late + stale, stale, 0)
            == monitor::health::healthy,
        "callback retirement leaves only the responsive supervisor supervised"
    );
    state.heartbeats[0] = { monitor::channel::main, true, 0, 0, start };
    check(
        monitor::classify_health(state, late, stale, 0)
            == monitor::health::globally_stalled_idle,
        "a completed frame cannot conceal blocked headless startup or teardown"
    );
    state.heartbeats[0].active = false;
    state.heartbeats[0].expected_since_ns = 0;
    state.heartbeats[1] = { monitor::channel::gui, true, late, 1, start };
    state.heartbeats[2] = { monitor::channel::core, true, 0, 0, start };
    check(
        monitor::classify_health(state, late, stale, 0)
            == monitor::health::degraded,
        "the GUI host's timer cannot conceal an overdue frame callback"
    );
    state.heartbeats[1].active = false;
    state.heartbeats[1].expected_since_ns = 0;
    state.heartbeats[0] = { monitor::channel::main, true, late, 1, start };
    state.heartbeats[2] = { monitor::channel::core, true, 0, 0, late };
    check(
        monitor::classify_health(state, late, stale, 0)
            == monitor::health::awaiting_heartbeat,
        "new work does not inherit the preceding callback's completion"
    );
    state.heartbeats[2].active = false;
    state.heartbeats[2].expected_since_ns = 0;
    state.heartbeats[0].active = false;
    state.heartbeats[0].expected_since_ns = 0;
    check(
        monitor::classify_health(state, late + stale, stale, 0)
            == monitor::health::idle,
        "joined EOF or cancellation leaves no promise of another frame"
    );
    state.heartbeats[0] = { monitor::channel::main, true, late, 1, start };
    state.observation_started_ns = late + stale;
    const auto attached = state.observation_started_ns;
    check(
        monitor::classify_health(state, attached + stale, stale, 0)
            == monitor::health::awaiting_heartbeat,
        "reattachment cannot treat an old supervisor poll as current progress"
    );
    for (int poll = 0; poll < 4; ++poll) {
        check(
            monitor::classify_health(state, attached + stale + 1, stale, 0)
                == monitor::health::globally_stalled_idle,
            "watcher polling cannot postpone the missing-progress boundary"
        );
    }
    check(
        state.observation_started_ns == attached
            && state.heartbeats[0].timestamp_ns == late
            && state.heartbeats[0].expected_since_ns == start,
        "classification does not manufacture producer or attachment progress"
    );
}

} // namespace monitor_test
