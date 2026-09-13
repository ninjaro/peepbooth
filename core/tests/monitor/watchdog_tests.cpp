#include "monitor/client.hpp"
#include "monitor/console_watch.hpp"
#include "monitor/observed_process.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/watchdog.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace monitor {
struct observed_process_test_access {
    static const process_observation& accept(
        observed_process& owner, std::optional<platform::process_sample> sample,
        watchdog_snapshot snapshot, const std::uint64_t now
    ) {
        owner.clock_ticks_per_second_ = 100;
        return owner.accept_sample(std::move(sample), std::move(snapshot), now);
    }
};
} // namespace monitor

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

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

void test_command_line_parser() {
    constexpr std::array valid {
        std::string_view { "--pid" },         std::string_view { "42" },
        std::string_view { "--interval-ms" }, std::string_view { "50" },
        std::string_view { "--stale-ms" },    std::string_view { "100" },
        std::string_view { "--once" },
    };
    const auto parsed = monitor::parse_command_line(valid);
    check(
        parsed.action == monitor::command_line_action::run,
        "valid command line runs"
    );
    check(parsed.options.pid == 42, "command line PID");
    check(parsed.options.interval_ms == 50, "command line interval");
    check(parsed.options.stale_ms == 100, "command line stale threshold");
    check(parsed.options.once, "command line once mode");

    constexpr std::array help { std::string_view { "--help" } };
    check(
        monitor::parse_command_line(help).action
            == monitor::command_line_action::help,
        "help has a distinct command-line result"
    );

    constexpr std::array invalid {
        std::string_view { "--interval-ms" },
        std::string_view { "49" },
    };
    check(
        monitor::parse_command_line(invalid).action
            == monitor::command_line_action::error,
        "invalid command line is rejected"
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

void test_client_and_ring() {
#if defined(__linux__) && !defined(__ANDROID__)
    if constexpr (!monitor::instrumentation_enabled) {
        monitor::client disabled_client;
        check(
            !disabled_client.start("watchdog-tests"),
            "instrumentation is inert in unsupported builds"
        );
        return;
    }
    monitor::client client;
    check(client.start("watchdog-tests"), "create client mapping");
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    check(identity.has_value(), "read current process identity");
    if (!identity) {
        return;
    }
    auto raw = monitor::platform::open_process_mapping(identity->pid);
    check(raw.has_value(), "open client mapping");
    client.breadcrumb(monitor::event::configuration_loaded, 1, 1);
    check(
        raw
            && raw->get()->breadcrumb_ticket.load(std::memory_order_acquire)
                == 0,
        "disconnected breadcrumb path is inert"
    );
    auto watcher = monitor::watchdog_session::attach(*identity);
    check(watcher.has_value(), "attach compatible watcher");
    if (!watcher) {
        return;
    }
    client.set_channel_active(monitor::channel::core, true);
    const auto initial = watcher->snapshot();
    client.heartbeat(monitor::channel::core);
    client.heartbeat(monitor::channel::core);
    const auto rate_limited = watcher->snapshot();
    check(
        rate_limited.heartbeats[2].sequence == initial.heartbeats[2].sequence,
        "heartbeat publication is rate limited"
    );
    for (std::uint64_t value = 0; value < 140; ++value) {
        client.breadcrumb(monitor::event::stream_stopped, 7, value);
    }
    const auto snapshot = watcher->snapshot();
    check(
        snapshot.breadcrumbs.size() == monitor::breadcrumb_capacity,
        "ring is bounded"
    );
    if (snapshot.breadcrumbs.size() == monitor::breadcrumb_capacity) {
        check(
            snapshot.breadcrumbs.front().arg0 == 12, "ring drops oldest entries"
        );
        check(
            snapshot.breadcrumbs.back().arg0 == 139, "ring retains newest entry"
        );
    }
    check(snapshot.heartbeats[2].active, "core heartbeat is active");
    watcher->release();
    client.stop();
#endif
}

void test_observation_owner() {
#if defined(__linux__) && !defined(__ANDROID__)
    if constexpr (!monitor::instrumentation_enabled) {
        return;
    }
    monitor::client producer;
    check(producer.start("observation-test"), "start observation producer");
    auto process = monitor::platform::process_handle::attach(
        monitor::platform::current_process_id()
    );
    check(process.has_value(), "attach observation process handle");
    if (!process) {
        return;
    }
    const auto identity = process->identity();
    auto raw = monitor::platform::open_process_mapping(identity.pid);
    check(raw.has_value(), "inspect observation watcher lease");
    if (!raw) {
        return;
    }
    {
        monitor::observed_process owner(std::move(*process));
        check(
            owner.poll().status == monitor::observation_status::not_attached,
            "poll before watchdog attachment is explicit"
        );
        check(owner.try_attach(), "attach observation watchdog");
        monitor::watchdog_snapshot snapshot;
        snapshot.process_name = "deterministic";
        snapshot.heartbeats[1]
            = { monitor::channel::gui, true, 2'000'000'000, 1 };
        monitor::platform::process_sample sample {
            .identity = identity,
            .memory = { .rss_bytes = 1024 },
            .cpu_ticks = 10,
            .sampled_at_ns = 1'000'000'000,
        };
        using access = monitor::observed_process_test_access;
        const auto first
            = access::accept(owner, sample, snapshot, 1'000'000'000);
        check(
            first.sample.has_value() && !first.cpu_percent,
            "first observation has a raw sample but no CPU delta"
        );
        sample.sampled_at_ns = 2'000'000'000;
        sample.cpu_ticks = 110;
        sample.memory.rss_bytes = 2048;
        const auto second
            = access::accept(owner, sample, snapshot, 2'500'000'000);
        check(
            second.cpu_percent == 100.0,
            "CPU is derived from sample time and ticks"
        );
        check(
            second.memory.rss_delta_bytes == 1024,
            "trend belongs to the process owner"
        );
        std::ostringstream text;
        monitor::write_console_observation(text, second);
        check(
            text.str().find("gui=0.500s") != std::string::npos,
            "formatter uses observation time rather than current clock"
        );
        std::ostringstream repeated;
        monitor::write_console_observation(repeated, second);
        check(
            text.str() == repeated.str(),
            "saved observation formats deterministically"
        );

        const auto unavailable
            = access::accept(owner, std::nullopt, snapshot, 3'000'000'000);
        check(
            unavailable.status
                    == monitor::observation_status::sample_unavailable
                && !unavailable.sample && !unavailable.cpu_percent
                && !unavailable.responsiveness,
            "missing resource sample cannot reuse a previous healthy sample"
        );
        sample.sampled_at_ns = 4'000'000'000;
        check(
            !access::accept(owner, sample, snapshot, 4'000'000'000).cpu_percent,
            "sampling recovery establishes a new CPU baseline"
        );
        ++sample.identity.start_ticks;
        const auto mismatch
            = access::accept(owner, sample, snapshot, 5'000'000'000);
        check(
            mismatch.status == monitor::observation_status::identity_changed
                && !mismatch.sample && !mismatch.responsiveness,
            "PID incarnation mismatch is rejected instead of adopted"
        );
        sample.identity = identity;
        ++sample.identity.pid;
        check(
            access::accept(owner, sample, snapshot, 6'000'000'000).status
                == monitor::observation_status::identity_changed,
            "wrong PID is rejected even with matching start ticks"
        );

        monitor::observed_process moved(std::move(owner));
        check(
            raw->get()->watcher_present.load() != 0,
            "moving an observation owner preserves the lease"
        );
        const auto& live = moved.poll();
        check(
            live.status == monitor::observation_status::sampled && live.sample
                && live.watchdog.process_name == "observation-test",
            "moved owner polls the real producer"
        );
    }
    check(
        raw->get()->watcher_present.load() == 0,
        "observation owner destruction releases the lease"
    );
    std::atomic_bool stop_requested { false };
    std::ostringstream output;
    std::ostringstream error;
    monitor::console_options options;
    options.pid = identity.pid;
    options.once = true;
    check(
        monitor::run_console_watch(options, stop_requested, output, error) == 0
            && error.str().empty()
            && output.str().find("observation-test[") != std::string::npos,
        "once mode attaches, formats one observation and returns successfully"
    );
    check(
        raw->get()->watcher_present.load() == 0,
        "once mode releases its watcher lease"
    );
#endif
}

void test_observed_process_exit() {
#if defined(__linux__) && !defined(__ANDROID__)
    if constexpr (!monitor::instrumentation_enabled) {
        return;
    }
    int ready[2];
    int finish[2];
    if (::pipe(ready) != 0) {
        check(false, "create ready pipe");
        return;
    }
    if (::pipe(finish) != 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        check(false, "create finish pipe");
        return;
    }
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        monitor::client producer;
        const char started = producer.start("observed-child") ? '1' : '0';
        static_cast<void>(::write(ready[1], &started, 1));
        char stop;
        static_cast<void>(::read(finish[0], &stop, 1));
        producer.stop();
        ::_exit(0);
    }
    ::close(ready[1]);
    ::close(finish[0]);
    if (child < 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        check(false, "fork observed process");
        return;
    }
    char started {};
    check(
        ::read(ready[0], &started, 1) == 1 && started == '1',
        "child publishes its endpoint before attachment"
    );
    ::close(ready[0]);
    auto handle = monitor::platform::process_handle::attach(
        static_cast<std::uint32_t>(child)
    );
    check(handle.has_value(), "attach child process handle");
    if (handle) {
        monitor::observed_process owner(std::move(*handle));
        check(owner.try_attach(), "attach child watchdog");
        check(
            owner.poll().status == monitor::observation_status::sampled,
            "child observation starts alive"
        );
        ::close(finish[1]);
        static_cast<void>(::waitpid(child, nullptr, 0));
        const auto& exited = owner.poll();
        check(
            exited.status == monitor::observation_status::exited
                && !exited.sample,
            "process exit does not manufacture a final resource sample"
        );
        check(
            exited.watchdog.process_lifecycle == monitor::lifecycle::clean_exit,
            "retained mapping preserves final producer lifecycle"
        );
    } else {
        ::close(finish[1]);
        static_cast<void>(::waitpid(child, nullptr, 0));
    }
#endif
}

} // namespace

int main() {
    test_observation_owner();
    test_observed_process_exit();
    test_status_parser();
    test_command_line_parser();
    test_memory_trend();
    test_health_classification();
    test_client_and_ring();
    if (failures == 0) {
        std::cout << "monitor watchdog focused tests passed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
