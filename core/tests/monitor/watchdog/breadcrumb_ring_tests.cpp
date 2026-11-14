#include "../../../src/monitor/breadcrumb_ring.hpp"
#include "monitor/client.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/watchdog.hpp"
#include "watchdog_checks.hpp"

#include <array>
#include <barrier>
#include <limits>
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
                    == static_cast<std::uint16_t>(
                        monitor::event::stream_stopped
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

} // namespace monitor_test
