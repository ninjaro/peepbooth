#include "monitor/client.hpp"
#include "monitor/console_watch.hpp"
#include "monitor/observation_registry.hpp"
#include "monitor/observed_process.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "watchdog_checks.hpp"

#include <sstream>
#include <string>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
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

namespace monitor_test {

void test_observation_history() {
    const monitor::platform::process_identity identity { 42, 99 };
    monitor::observation_history history(identity, 3);
    monitor::process_observation observation;
    observation.identity = identity;
    observation.observed_at_ns = 10;
    observation.status = monitor::observation_status::sampled;
    observation.sample = monitor::platform::process_sample {
        identity, { 4096, 8192, 0 }, 0, 10
    };
    observation.responsiveness = monitor::health::idle;
    check(history.add(observation), "retain actual process observation");
    check(
        history.records().back().memory->rss_bytes == 4096
            && !history.records().back().cpu_percent,
        "unknown first CPU delta is not fabricated as zero"
    );
    check(history.note_gap(11), "retain explicit watcher disconnection");
    check(
        !history.records().back().status && !history.records().back().memory
            && !history.records().back().responsiveness,
        "a gap contains no sampled memory or health"
    );
    observation.observed_at_ns = 12;
    observation.status = monitor::observation_status::sample_unavailable;
    check(history.add(observation), "retain unavailable sample status");
    check(
        !history.records().back().memory
            && !history.records().back().responsiveness,
        "unavailable data cannot copy stale healthy evidence"
    );
    for (std::uint64_t time = 13; time < 1013; ++time) {
        observation.observed_at_ns = time;
        observation.status = monitor::observation_status::sampled;
        observation.cpu_percent = 0.0;
        history.add(observation);
    }
    check(
        history.records().size() == 3 && history.overwritten() == 1000
            && history.records().back().cpu_percent == 0.0,
        "long retention is bounded; measured zero CPU stays distinct from "
        "unknown"
    );
    observation.identity.start_ticks = 100;
    check(!history.add(observation), "PID reuse cannot redirect history");
    check(!history.note_gap(1), "backwards time is rejected");
    observation.identity = identity;
    observation.observed_at_ns = 1013;
    observation.status = monitor::observation_status::exited;
    history.add(observation);
    check(
        history.records().back().status == monitor::observation_status::exited
            && !history.records().back().responsiveness
            && !history.records().back().cpu_percent,
        "exit retains identity and time, not a fabricated final sample"
    );
    monitor::observation_history disabled(identity, 0);
    check(
        !disabled.add(observation) && !disabled.note_gap(1014)
            && disabled.records().empty() && disabled.overwritten() == 0,
        "disabled history retains nothing"
    );
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
        std::ostringstream first_text;
        monitor::write_console_observation(first_text, first);
        check(
            first_text.str().find("cpu=unavailable") != std::string::npos,
            "console cannot present an unavailable CPU delta as zero"
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

        const auto& previous_view = owner.latest();
        monitor::observed_process moved(std::move(owner));
        check(
            previous_view.status == monitor::observation_status::not_attached
                && !previous_view.responsiveness,
            "moved-from observer cannot retain live health or sampling claims"
        );
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
    options.history_capacity = 3;
    check(
        monitor::run_console_watch(options, stop_requested, output, error) == 0
            && error.str().empty()
            && output.str().find("observation-test[") != std::string::npos,
        "once mode attaches, formats one observation and returns successfully"
    );
    check(
        output.str().find("history retained=2 overwritten=0")
                != std::string::npos
            && output.str().find("status=gap") != std::string::npos,
        "single-PID console retains its sample and explicit detach gap"
    );
    check(
        output.str().find("incidents closed=0 active=0 overwritten=0")
            != std::string::npos,
        "a real idle process and ordinary detach create no incident"
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

} // namespace monitor_test
