#include "../../../src/monitor/breadcrumb_ring.hpp"
#include "monitor/client.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/watchdog.hpp"
#include "watchdog_checks.hpp"

#include <array>
#include <barrier>
#include <thread>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace monitor_test {

void test_progress_owner() {
    monitor::client producer;
    if constexpr (!monitor::instrumentation_enabled) {
        auto disabled = producer.expect_progress(monitor::channel::core);
        auto process_disabled
            = monitor::expect_progress(monitor::channel::core);
        check(
            !disabled.valid() && !process_disabled.valid(),
            "unsupported progress ownership remains inert"
        );
        disabled.heartbeat();
        disabled.reset();
        return;
    }
    check(producer.start("expected-progress"), "start expectation producer");
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    if (!identity) {
        check(false, "read expectation producer identity");
        return;
    }
    auto raw = monitor::platform::open_process_mapping(identity->pid);
    auto watcher = monitor::watchdog_session::attach(*identity);
    check(
        raw.has_value() && watcher.has_value(), "attach expectation producer"
    );
    if (!raw || !watcher)
        return;
    producer.heartbeat(monitor::channel::core);
    const auto passive = watcher->snapshot();
    check(
        !passive.heartbeats[2].active && passive.heartbeats[2].sequence == 1
            && monitor::classify_health(
                   passive, passive.heartbeats[2].timestamp_ns + 101, 100, 0
               ) == monitor::health::idle,
        "an unowned pulse does not create an expectation of future work"
    );
    auto core = producer.expect_progress(monitor::channel::core);
    check(core.valid(), "claim exclusive channel ownership");
    auto duplicate = producer.expect_progress(monitor::channel::core);
    check(!duplicate.valid(), "a duplicate claim is explicitly rejected");
    check(
        !producer.expect_progress(static_cast<monitor::channel>(0)).valid(),
        "invalid channels cannot create ownership"
    );
    const auto starting = watcher->snapshot().heartbeats[2];
    check(
        starting.active && starting.timestamp_ns == 0 && starting.sequence == 0
            && starting.expected_since_ns != 0,
        "new ownership cannot inherit old progress"
    );
    producer.set_channel_active(monitor::channel::core, false);
    producer.set_channel_active(monitor::channel::core, true);
    producer.heartbeat(monitor::channel::core);
    duplicate.heartbeat();
    check(
        watcher->snapshot().heartbeats[2].active
            && watcher->snapshot().heartbeats[2].sequence == 0,
        "unrelated/manual publication and retirement cannot alter a scoped "
        "owner"
    );
    core.heartbeat();
    check(
        watcher->snapshot().heartbeats[2].sequence == 1,
        "only the owner can publish its progress"
    );
    auto main = producer.expect_progress(monitor::channel::main);
    check(main.valid(), "other channels remain independently owned");
    std::swap(main, core);
    core.reset();
    core.heartbeat();
    check(
        !core.valid() && main.valid()
            && !watcher->snapshot().heartbeats[0].active
            && watcher->snapshot().heartbeats[2].active,
        "moves transfer exclusive ownership and reset retires only its channel"
    );
    main.reset();
    producer.heartbeat(monitor::channel::core);
    check(
        !watcher->snapshot().heartbeats[2].active,
        "late passive pulses cannot revive retired work"
    );
    {
        auto scoped = producer.expect_progress(monitor::channel::core);
        check(scoped.valid(), "a retired channel can be claimed again");
        check(
            watcher->snapshot().heartbeats[2].sequence == 0,
            "replacement has fresh evidence"
        );
    }
    check(
        !watcher->snapshot().heartbeats[2].active,
        "destruction cancels the expectation"
    );
    auto surviving = producer.expect_progress(monitor::channel::worker);
    watcher->release();
    surviving.heartbeat();
    check(
        surviving.valid() && raw->get()->heartbeats[3].sequence.load() == 0,
        "detaching stops publication without canceling real host work"
    );
    watcher = monitor::watchdog_session::attach(*identity);
    check(watcher.has_value(), "reattach an independently live expectation");
    if (!watcher)
        return;
    const auto attached = watcher->snapshot();
    check(
        attached.observation_started_ns != 0,
        "reattachment starts a new evidence interval"
    );
    raw->get()->watcher_lease_until_ns.store(0);
    const auto resumed = attached.observation_started_ns + 1000;
    watcher->renew_lease(resumed);
    check(
        watcher->snapshot().observation_started_ns == resumed,
        "lease expiry begins a fresh observation interval"
    );
    watcher->renew_lease(resumed + 1);
    check(
        watcher->snapshot().observation_started_ns == resumed,
        "ordinary polling cannot extend missing-progress grace"
    );
    raw->get()->heartbeats[3].revision.fetch_add(1);
    check(
        !watcher->snapshot().heartbeats[3].coherent,
        "bounded snapshot retries report an unfinished publication honestly"
    );
    raw->get()->heartbeats[3].revision.fetch_add(1);
    producer.stop();
    for (const auto& pulse : watcher->snapshot().heartbeats)
        check(
            !pulse.active,
            "producer stop retires all expectations in retained evidence"
        );
    watcher->renew_lease(resumed + 2);
    check(
        raw->get()->watcher_present.load() == 0,
        "terminal producer evidence does not renew an observer lease"
    );
    check(!surviving.valid(), "stop invalidates live owner handles");
    check(producer.start("expectation-restart"), "restart the actual producer");
    auto replacement = producer.expect_progress(monitor::channel::worker);
    surviving.heartbeat();
    surviving.reset();
    check(
        replacement.valid(),
        "a previous incarnation cannot affect a replacement owner"
    );
    watcher->release();
    producer.stop();
    monitor::client::progress_owner orphan;
    {
        monitor::client temporary;
        check(
            temporary.start("owner-outlives-client"),
            "start short-lived producer"
        );
        orphan = temporary.expect_progress(monitor::channel::core);
        check(orphan.valid(), "claim before client destruction");
    }
    check(!orphan.valid(), "owner handle does not retain a destroyed producer");
    orphan.heartbeat();
    orphan.reset();
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
    client.heartbeat(monitor::channel::core);
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

void test_concurrent_progress_publication() {
    if constexpr (!monitor::instrumentation_enabled)
        return;
    monitor::client producer;
    check(
        producer.start("progress-contention"),
        "start concurrent progress producer"
    );
    const auto identity = monitor::platform::read_process_identity(
        monitor::platform::current_process_id()
    );
    if (!identity) {
        check(false, "read concurrent progress identity");
        return;
    }
    auto watcher = monitor::watchdog_session::attach(*identity);
    auto owned = producer.expect_progress(monitor::channel::core);
    check(
        watcher.has_value() && owned.valid(), "attach concurrent progress owner"
    );
    if (!watcher || !owned.valid())
        return;
    std::barrier ready(4);
    std::atomic<unsigned> finished { 0 };
    std::array<std::jthread, 3> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::jthread([&, index] {
            ready.arrive_and_wait();
            for (int turn = 0; turn < 1000; ++turn) {
                if (index == 0) {
                    producer.set_channel_active(monitor::channel::worker, true);
                    producer.heartbeat(monitor::channel::worker);
                    producer.set_channel_active(
                        monitor::channel::worker, false
                    );
                } else {
                    owned.heartbeat();
                    producer.heartbeat(monitor::channel::worker);
                }
            }
            finished.fetch_add(1);
        });
    }
    bool coherent = true;
    ready.arrive_and_wait();
    while (finished.load() != workers.size()) {
        for (const auto& pulse : watcher->snapshot().heartbeats)
            if (pulse.coherent && pulse.active && pulse.sequence != 0)
                coherent
                    = coherent && pulse.timestamp_ns >= pulse.expected_since_ns;
    }
    for (auto& worker : workers)
        worker.join();
    check(
        coherent,
        "coherent progress snapshots cannot mix expectation generations"
    );
    std::barrier cancel(3);
    std::jthread late_publisher([&] {
        cancel.arrive_and_wait();
        for (int turn = 0; turn < 1000; ++turn)
            owned.heartbeat();
    });
    std::jthread repeated_cancel([&] {
        cancel.arrive_and_wait();
        owned.reset();
    });
    cancel.arrive_and_wait();
    owned.reset();
    const auto retired_sequence = watcher->snapshot().heartbeats[2].sequence;
    late_publisher.join();
    repeated_cancel.join();
    check(
        !watcher->snapshot().heartbeats[2].active
            && watcher->snapshot().heartbeats[2].sequence == retired_sequence,
        "concurrent reset rejects every late publication after retirement"
    );
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

} // namespace monitor_test
