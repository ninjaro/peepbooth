#include "../../src/monitor/breadcrumb_ring.hpp"
#include "monitor/client.hpp"
#include "monitor/console_watch.hpp"
#include "monitor/observation_registry.hpp"
#include "monitor/observed_process.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/process_discovery.hpp"
#include "monitor/watchdog.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
        return owner.accept_sample(sample, std::move(snapshot), now);
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

    constexpr std::array list { std::string_view { "--list" } };
    check(
        monitor::parse_command_line(list).action
            == monitor::command_line_action::list,
        "read-only list selection does not require PID"
    );
    constexpr std::array mixed { std::string_view { "--list" },
                                 std::string_view { "--pid" },
                                 std::string_view { "42" } };
    check(
        monitor::parse_command_line(mixed).action
            == monitor::command_line_action::error,
        "list and observation selections cannot be combined"
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
        check(
            !monitor::start_process("disabled-public-api"),
            "disabled public startup does not construct the process client"
        );
        monitor::heartbeat(monitor::channel::core);
        monitor::record_breadcrumb(monitor::event::stream_stopped);
        check(
            !monitor::platform::open_process_mapping(
                monitor::platform::current_process_id()
            ),
            "disabled public calls create no endpoint"
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
    if (raw) {
        raw->get()->magic = 0x4350505257444f47ULL;
        check(
            !monitor::watchdog_session::attach(*identity),
            "obsolete shared state is rejected"
        );
        raw->get()->magic = monitor::shared_state_magic;
    }
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

void test_client_lifecycle() {
#if defined(__linux__) && !defined(__ANDROID__)
    if constexpr (!monitor::instrumentation_enabled)
        return;
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    check(identity.has_value(), "lifecycle process identity");
    if (!identity)
        return;
    monitor::client producer;
    producer.stop();
    for (int cycle = 0; cycle < 3; ++cycle) {
        check(producer.start("lifecycle"), "start producer incarnation");
        check(producer.start("ignored"), "start is idempotent while running");
        auto watcher = monitor::watchdog_session::attach(*identity);
        check(watcher.has_value(), "attach lifecycle watcher");
        if (!watcher)
            return;
        producer.heartbeat(monitor::channel::main);
        check(
            watcher->snapshot().heartbeats[0].sequence == 1,
            "new mapping publishes its first heartbeat without an old throttle"
        );
        // Workers finish all instrumentation before the lifecycle owner stops.
        // The barrier orders attachment before work without scheduler sleeps.
        std::barrier ready(5);
        std::array<std::jthread, 4> workers;
        for (std::size_t index = 0; index < workers.size(); ++index) {
            workers[index] = std::jthread([&producer, &ready, index] {
                ready.arrive_and_wait();
                producer.heartbeat(monitor::channel::worker);
                producer.breadcrumb(
                    monitor::event::stream_stopped,
                    static_cast<std::uint32_t>(index), index
                );
            });
        }
        ready.arrive_and_wait();
        for (auto& worker : workers)
            worker.join();
        check(
            watcher->snapshot().breadcrumbs.size() == workers.size(),
            "joined workers publish complete bounded records"
        );
        watcher->release();
        auto raw = monitor::platform::open_process_mapping(identity->pid);
        check(raw.has_value(), "watcher detach preserves producer endpoint");
        if (!raw)
            return;
        const auto ticket = raw->get()->breadcrumb_ticket.load();
        producer.breadcrumb(monitor::event::output_ready);
        check(
            raw->get()->breadcrumb_ticket.load() == ticket,
            "detached publication has no retained backlog"
        );
        watcher = monitor::watchdog_session::attach(*identity);
        check(watcher.has_value(), "watcher reattaches to running producer");
        if (!watcher)
            return;
        watcher->renew_lease(0, 0);
        producer.breadcrumb(monitor::event::output_ready);
        check(
            raw->get()->watcher_present.load() == 0
                && raw->get()->breadcrumb_ticket.load() == ticket,
            "expired lease stops publication without a sleep"
        );
        watcher->renew_lease(monitor::platform::monotonic_time_ns());
        producer.breadcrumb(monitor::event::output_ready);
        check(
            raw->get()->breadcrumb_ticket.load() == ticket + 1,
            "renewed observer resumes current publication"
        );
        producer.stop();
        producer.stop();
        check(!producer.available(), "repeated stop is inert");
        check(
            watcher->snapshot().process_lifecycle
                == monitor::lifecycle::clean_exit,
            "observer retains final clean exit after producer unmaps"
        );
        check(
            !monitor::platform::open_process_mapping(identity->pid),
            "only producer teardown removes its endpoint"
        );
        producer.heartbeat(monitor::channel::worker);
        producer.breadcrumb(monitor::event::stream_stopped);
    }
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

void test_process_discovery() {
#if defined(__linux__) && !defined(__ANDROID__)
    auto mapping = monitor::platform::create_process_mapping("discovery\nname");
    check(
        mapping.has_value(),
        "create discovery endpoint independently of client build mode"
    );
    if (!mapping)
        return;
    const auto pid = monitor::platform::current_process_id();
    const auto identity = monitor::platform::read_process_identity(pid);
    check(identity.has_value(), "read discovery process identity");
    if (!identity)
        return;
    auto watcher = monitor::watchdog_session::attach(*identity);
    check(watcher.has_value(), "attach existing observer before discovery");
    if (!watcher)
        return;
    auto& state = *mapping->get();
    const auto lease = state.watcher_lease_until_ns.load();
    std::string reason;
    const auto inspected
        = monitor::platform::inspect_process_mapping(pid, &reason);
    check(
        inspected && reason.empty()
            && inspected->identity.start_ticks == identity->start_ticks
            && inspected->name == "discovery\nname",
        "read-only inspection retains exact identity and bounded name"
    );
    const auto original_ticks = state.process_start_ticks;
    state.process_start_ticks = original_ticks + 1;
    check(
        !monitor::platform::inspect_process_mapping(pid, &reason)
            && reason.find("stale process identity") != std::string::npos,
        "discovery rejects an old PID incarnation"
    );
    state.process_start_ticks = original_ticks;
    const auto magic = state.magic;
    state.magic = 0;
    check(
        !monitor::platform::inspect_process_mapping(pid, &reason),
        "discovery rejects an incompatible layout"
    );
    state.magic = magic;
    state.process_lifecycle.store(
        static_cast<std::uint32_t>(monitor::lifecycle::starting)
    );
    check(
        !monitor::platform::inspect_process_mapping(pid, &reason),
        "discovery does not read an unpublished initialization"
    );
    state.process_lifecycle.store(
        static_cast<std::uint32_t>(monitor::lifecycle::running)
    );
    const int descriptor = ::shm_open(
        monitor::platform::shared_memory_name(pid).c_str(), O_RDWR | O_CLOEXEC,
        0
    );
    check(
        descriptor >= 0,
        "open owned endpoint for read-only permission regression"
    );
    if (descriptor >= 0) {
        check(
            ::fchmod(descriptor, S_IRUSR) == 0, "make owned endpoint read-only"
        );
        check(
            monitor::platform::inspect_process_mapping(pid, &reason)
                .has_value(),
            "discovery works without writable endpoint access"
        );
        check(
            ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0,
            "restore owned endpoint permissions"
        );
        ::close(descriptor);
    }
    const auto name = monitor::platform::shared_memory_name(pid);
    for (const auto& rejected :
         { name + "-malformed",
           name.substr(0, name.rfind('-') + 1) + "0" + std::to_string(pid),
           std::string("/monitor-watchdog-")
               + std::to_string(monitor::platform::current_user_id() + 1U) + "-"
               + std::to_string(pid) }) {
        const int file = ::shm_open(
            rejected.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
            S_IRUSR | S_IWUSR
        );
        check(file >= 0, "create exclusively owned discovery-name fixture");
        if (file < 0)
            continue;
        const auto inventory = monitor::discover_processes();
        const auto filename = rejected.substr(1);
        const bool reported = std::ranges::any_of(
            inventory.diagnostics,
            [&](const auto& message) {
                return message.starts_with(filename + ":");
            }
        );
        const bool foreign = filename.starts_with(
            "monitor-watchdog-"
            + std::to_string(monitor::platform::current_user_id() + 1U) + "-"
        );
        check(
            inventory.available && reported != foreign,
            "malformed current-user names are diagnosed; foreign names are "
            "ignored"
        );
        ::close(file);
        check(
            ::shm_unlink(rejected.c_str()) == 0,
            "only the fixture owner removes its rejected endpoint"
        );
    }
    std::ostringstream text, errors;
    static_cast<void>(monitor::run_console_list(text, errors));
    check(
        text.str().find("PID\tSTART_TICKS\tNAME\n") == 0
            && text.str().find("discovery?name") != std::string::npos,
        "list output identifies incarnations and cannot inject rows through "
        "names"
    );
    check(
        state.watcher_present.load() == 1
            && state.watcher_lease_until_ns.load() == lease,
        "inspection and enumeration neither renew nor clear another observer "
        "lease"
    );
    check(
        monitor::platform::open_process_mapping(pid).has_value(),
        "listing does not unlink the producer endpoint"
    );

    int ready[2], finish[2];
    if (::pipe(ready) != 0) {
        check(false, "create discovery ready pipe");
        return;
    }
    if (::pipe(finish) != 0) {
        ::close(ready[0]);
        ::close(ready[1]);
        check(false, "create discovery finish pipe");
        return;
    }
    const auto child = ::fork();
    if (child == 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        auto producer
            = monitor::platform::create_process_mapping("discovery\nname");
        const char started = producer ? '1' : '0';
        static_cast<void>(::write(ready[1], &started, 1));
        char stop {};
        static_cast<void>(::read(finish[0], &stop, 1));
        // This raw platform producer also runs in Release, where the public
        // client is inert. Publish the same final lifecycle as client::stop().
        if (producer)
            producer->get()->process_lifecycle.store(
                static_cast<std::uint32_t>(monitor::lifecycle::clean_exit),
                std::memory_order_release
            );
        producer.reset();
        ::_exit(0);
    }
    ::close(ready[1]);
    ::close(finish[0]);
    if (child < 0) {
        ::close(ready[0]);
        ::close(finish[1]);
        check(false, "fork second discovery producer");
        return;
    }
    char started {};
    check(
        ::read(ready[0], &started, 1) == 1 && started == '1',
        "second real endpoint is published before enumeration"
    );
    ::close(ready[0]);
    const auto child_identity = monitor::platform::read_process_identity(
        static_cast<std::uint32_t>(child)
    );
    if (child_identity) {
        auto child_watcher = monitor::watchdog_session::attach(*child_identity);
        auto child_mapping
            = monitor::platform::open_process_mapping(child_identity->pid);
        const auto inventory = monitor::discover_processes();
        const auto same_names = std::ranges::count_if(
            inventory.processes,
            [&](const auto& process) {
                return process.name == "discovery\nname"
                    && (process.identity.pid == pid
                        || process.identity.pid == child_identity->pid);
            }
        );
        check(
            same_names == 2,
            "equal names retain two distinct process identities without "
            "arbitrary selection"
        );
        check(
            std::ranges::is_sorted(
                inventory.processes, {},
                [](const auto& process) { return process.identity.pid; }
            ),
            "discovery order is deterministic"
        );
        check(
            child_watcher && child_mapping,
            "attach independent observer to second process"
        );
        if (child_watcher && child_mapping) {
            const auto child_lease
                = child_mapping->get()->watcher_lease_until_ns.load();
            watcher->release();
            static_cast<void>(monitor::discover_processes());
            check(
                child_mapping->get()->watcher_present.load() == 1
                    && child_mapping->get()->watcher_lease_until_ns.load()
                        == child_lease,
                "detaching A and listing cannot disturb B's lease"
            );
        }
    } else {
        check(false, "second producer identity is available");
    }
    monitor::observation_registry registry;
    std::optional<monitor::platform::shared_mapping> retained_child;
    if (child_identity) {
        check(
            registry.attach(*identity) && registry.attach(*child_identity),
            "registry owns two live exact process incarnations"
        );
        retained_child
            = monitor::platform::open_process_mapping(child_identity->pid);
        check(
            registry.size() == 2 && retained_child,
            "registry stores one owner per selection without a process cap"
        );
        auto stale = *identity;
        ++stale.start_ticks;
        check(
            !registry.attach(stale) && registry.size() == 2,
            "a stale incarnation cannot replace the selected process"
        );
        check(
            !registry.detach(stale) && registry.size() == 2,
            "detaching an unmatched incarnation cannot erase the current one"
        );
        const auto lease_a = state.watcher_lease_until_ns.load();
        check(
            registry.attach(*identity) && registry.size() == 2
                && state.watcher_lease_until_ns.load() == lease_a,
            "repeated attachment reuses its owner without renewing a lease"
        );
        auto selections = registry.identities();
        check(
            selections.size() == 2
                && std::ranges::is_sorted(
                    selections, {}, [](const auto& item) { return item.pid; }
                ),
            "registry identities have deterministic order"
        );
        const auto snapshot = registry.poll();
        check(
            snapshot.size() == 2
                && std::ranges::all_of(
                    snapshot,
                    [](const auto& observation) {
                        return observation.status
                            == monitor::observation_status::sampled
                            && observation.sample && !observation.cpu_percent;
                    }
                ),
            "each registry owner establishes its own first CPU baseline"
        );
        static_cast<void>(registry.poll());
        check(
            snapshot.size() == 2 && !snapshot[0].cpu_percent
                && !snapshot[1].cpu_percent,
            "returned value observations survive later polling"
        );
        monitor::observation_registry moved(std::move(registry));
        registry = std::move(moved);
        if (retained_child) {
            const auto lease_b
                = retained_child->get()->watcher_lease_until_ns.load();
            check(
                registry.detach(*identity) && registry.size() == 1
                    && state.watcher_present.load() == 0
                    && retained_child->get()->watcher_present.load() == 1
                    && retained_child->get()->watcher_lease_until_ns.load()
                        == lease_b,
                "moving owners and detaching A preserve B's exact lease"
            );
        }
        monitor::process_discovery names;
        names.available = true;
        names.processes
            = { { *identity, "same" }, { *child_identity, "same" } };
        const auto ambiguous = monitor::select_process_name(names, "same");
        check(
            !ambiguous.identity
                && ambiguous.error.find("ambiguous") != std::string::npos,
            "equal names require explicit identity selection"
        );
        names.processes.pop_back();
        const auto unique = monitor::select_process_name(names, "same");
        check(
            unique.identity && unique.error.empty()
                && unique.identity->pid == identity->pid
                && unique.identity->start_ticks == identity->start_ticks,
            "unique exact names retain the selected incarnation"
        );
        check(
            !monitor::select_process_name(names, "sam").identity
                && !monitor::select_process_name(names, "").identity,
            "name selection neither matches prefixes nor accepts empty names"
        );
        names.diagnostics.emplace_back("an endpoint could not be inspected");
        const auto incomplete = monitor::select_process_name(names, "same");
        check(
            !incomplete.identity
                && incomplete.error.find("incomplete") != std::string::npos,
            "incomplete discovery cannot prove a name unambiguous"
        );
        names.diagnostics.clear();
        names.available = false;
        check(
            !monitor::select_process_name(names, "same").identity,
            "unavailable discovery cannot select a friendly name"
        );
    }
    ::close(finish[1]);
    int status {};
    check(
        ::waitpid(child, &status, 0) == child && WIFEXITED(status)
            && WEXITSTATUS(status) == 0,
        "discovery child shuts down and removes its own endpoint"
    );
    if (child_identity) {
        const auto exited = registry.poll();
        check(
            exited.size() == 1 && exited[0].identity.pid == child_identity->pid
                && exited[0].status == monitor::observation_status::exited
                && exited[0].watchdog.process_lifecycle
                    == monitor::lifecycle::clean_exit,
            "registry retains final lifecycle evidence until explicit removal"
        );
        check(
            !registry.attach(*child_identity) && registry.size() == 1,
            "a repeated selection cannot revive an exited process"
        );
        registry.clear();
        check(
            registry.size() == 0 && retained_child
                && retained_child->get()->watcher_present.load() == 0,
            "clearing the registry releases only its owned remaining lease"
        );
    }
    const auto final = monitor::discover_processes();
    check(
        std::ranges::none_of(
            final.processes,
            [&](const auto& process) {
                return process.identity.pid
                    == static_cast<std::uint32_t>(child);
            }
        ),
        "exited producer is absent from the next inventory"
    );
#else
    const auto inventory = monitor::discover_processes();
    check(
        !inventory.available && !inventory.diagnostics.empty(),
        "unsupported discovery is explicit"
    );
#endif
}

void test_breadcrumb_wrap_order() {
#if defined(__linux__) && !defined(__ANDROID__)
    if constexpr (!monitor::instrumentation_enabled)
        return;
    monitor::client producer;
    check(producer.start("ring-order"), "start ring order producer");
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    if (!identity) {
        check(false, "ring order process identity");
        return;
    }
    auto raw = monitor::platform::open_process_mapping(identity->pid);
    auto watcher = monitor::watchdog_session::attach(*identity);
    if (!raw || !watcher) {
        check(false, "ring order mapping and watcher");
        return;
    }
    auto& state = *raw->get();
    const monitor::breadcrumb current {
        .timestamp_ns = 200,
        .event_id = 23,
        .flags = 17,
        .source_id = 2,
        .arg0 = 42,
        .arg1 = 43,
    };
    const monitor::breadcrumb delayed {
        .timestamp_ns = 100,
        .event_id = 22,
        .flags = 16,
        .source_id = 1,
        .arg0 = 1,
        .arg1 = 2,
    };
    check(
        !monitor::detail::publish_breadcrumb(state, 0, current),
        "zero is not a publication ticket"
    );
    check(
        !monitor::detail::publish_breadcrumb(
            state, (std::numeric_limits<std::uint64_t>::max() >> 1U) + 1U,
            current
        ),
        "ticket exhaustion cannot wrap the commit encoding"
    );
    constexpr std::uint64_t wrapped = 1 + monitor::breadcrumb_capacity;
    state.breadcrumb_ticket.store(wrapped);
    check(
        monitor::detail::publish_breadcrumb(state, wrapped, current),
        "new ticket publishes after wrapping an unclaimed reservation"
    );
    check(
        !monitor::detail::publish_breadcrumb(state, 1, delayed),
        "delayed reservation cannot replace a newer committed ticket"
    );
    const auto snapshot = watcher->snapshot();
    check(
        snapshot.breadcrumbs.size() == 1
            && snapshot.breadcrumbs.front().arg0 == current.arg0
            && snapshot.breadcrumbs.front().arg1 == current.arg1,
        "real observer retains the newer complete payload"
    );
    auto& slot = state.breadcrumbs[0];
    // The first publisher is now suspended after claiming, before payload
    // completion. A wrapped writer must not touch any of its fields.
    slot.commit.store(3);
    slot.ticket.store(1);
    slot.arg0.store(delayed.arg0);
    slot.arg1.store(delayed.arg1);
    check(
        !monitor::detail::publish_breadcrumb(state, wrapped, current),
        "busy slot drops a wrapped publication without waiting"
    );
    check(
        slot.commit.load() == 3 && slot.ticket.load() == 1
            && slot.arg0.load() == delayed.arg0
            && slot.arg1.load() == delayed.arg1,
        "wrapped publisher leaves the in-flight owner's payload untouched"
    );
    check(
        watcher->snapshot().breadcrumbs.empty(),
        "observer omits an in-progress or overwritten record"
    );
    slot.commit.store(2); // original owner completes; its ticket has expired
    state.breadcrumb_ticket.store(wrapped + monitor::breadcrumb_capacity);
    check(
        monitor::detail::publish_breadcrumb(
            state, wrapped + monitor::breadcrumb_capacity, current
        ),
        "slot accepts later work after its owner completes"
    );
    check(
        watcher->snapshot().breadcrumbs.size() == 1,
        "observer recovers after a dropped wrap without reset or retry"
    );
#endif
}

void test_concurrent_breadcrumb_snapshots() {
#if defined(__linux__) && !defined(__ANDROID__)
    if constexpr (!monitor::instrumentation_enabled)
        return;
    monitor::client producer;
    check(producer.start("ring-writers"), "start concurrent ring producer");
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    if (!identity) {
        check(false, "concurrent ring process identity");
        return;
    }
    auto watcher = monitor::watchdog_session::attach(*identity);
    if (!watcher) {
        check(false, "attach concurrent ring watcher");
        return;
    }
    watcher->renew_lease(
        monitor::platform::monotonic_time_ns(), 60'000'000'000ULL
    );
    std::barrier start(5);
    std::atomic<unsigned> completed { 0 };
    std::array<std::jthread, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::jthread([&, index] {
            const auto source = static_cast<std::uint32_t>(index + 1);
            start.arrive_and_wait();
            for (std::uint64_t iteration = 0; iteration < 4096; ++iteration) {
                const auto value
                    = (static_cast<std::uint64_t>(source) << 32U) | iteration;
                producer.breadcrumb(
                    monitor::event::stream_stopped, source, value, ~value, 17
                );
            }
            completed.fetch_add(1);
        });
    }
    bool coherent = true;
    std::size_t records = 0;
    const auto inspect = [&] {
        const auto snapshot = watcher->snapshot();
        coherent = coherent
            && snapshot.breadcrumbs.size() <= monitor::breadcrumb_capacity;
        for (const auto& record : snapshot.breadcrumbs) {
            ++records;
            coherent = coherent && record.arg1 == ~record.arg0
                && record.source_id
                    == static_cast<std::uint32_t>(record.arg0 >> 32U)
                && record.event_id
                    == static_cast<std::uint16_t>(monitor::event::stream_stopped
                    )
                && record.flags == 17;
        }
    };
    start.arrive_and_wait();
    while (completed.load() < workers.size())
        inspect();
    for (auto& worker : workers)
        worker.join();
    inspect();
    check(records > 0, "concurrent observer receives real publications");
    check(coherent, "wrapped concurrent records never mix publisher payloads");
#endif
}

} // namespace

int main() {
    const auto name = monitor::platform::shared_memory_name(42);
    check(
        name.starts_with("/monitor-watchdog-") && name.ends_with("-42"),
        "producer/watchdog endpoint uses the current Monitor identity"
    );
    test_observation_owner();
    test_observed_process_exit();
    test_status_parser();
    test_command_line_parser();
    test_memory_trend();
    test_health_classification();
    test_client_and_ring();
    test_client_lifecycle();
    test_process_discovery();
    test_breadcrumb_wrap_order();
    test_concurrent_breadcrumb_snapshots();
    if (failures == 0) {
        std::cout << "monitor watchdog focused tests passed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
