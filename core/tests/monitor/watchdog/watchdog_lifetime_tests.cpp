#include "monitor/watchdog.hpp"
#include "watchdog_checks.hpp"
#include "watchdog_fixture.hpp"

namespace monitor_test {

void test_incident_memory_and_lifetime() {
    using namespace monitor;
    auto sample = incident_sample(1);
    observation_history history(sample.identity, 3);
    memory_trend trend;
    for (std::uint64_t point = 0; point < 8; ++point) {
        sample.sample->sampled_at_ns = sample.observed_at_ns
            = 1 + point * 1'000'000'000ULL;
        sample.sample->memory.rss_bytes = 4096 * (point + 1);
        trend.add(sample.observed_at_ns, sample.sample->memory.rss_bytes);
        sample.memory = trend.summary();
        history.add(sample);
    }
    check(
        active_incidents(history) == 1
            && history.incidents().active()[heartbeat_channel_count]->kind
                == incident_kind::sampled_rss_growth,
        "only the existing sustained-growth rule opens a resource observation, "
        "not a leak diagnosis"
    );
    auto unavailable = sample;
    unavailable.status = observation_status::sample_unavailable;
    unavailable.observed_at_ns++;
    history.add(unavailable);
    sample.sample->sampled_at_ns = sample.observed_at_ns += 1'000'000'000ULL;
    trend.add(sample.observed_at_ns, sample.sample->memory.rss_bytes + 4096);
    sample.memory = trend.summary();
    history.add(sample);
    check(
        !history.incidents().active()[heartbeat_channel_count],
        "an old RSS window crossing a gap cannot establish a new continuous "
        "episode"
    );
    memory_trend fresh;
    const auto began = sample.observed_at_ns;
    for (std::uint64_t point = 0; point < 8; ++point) {
        sample.sample->sampled_at_ns = sample.observed_at_ns
            = began + point * 1'000'000'000ULL;
        sample.sample->memory.rss_bytes = 8192 * (point + 1);
        fresh.add(sample.observed_at_ns, sample.sample->memory.rss_bytes);
        sample.memory = fresh.summary();
        history.add(sample);
    }
    check(
        history.incidents().active()[heartbeat_channel_count].has_value(),
        "a fully observed fresh growth window is admissible"
    );
    sample.memory.sustained_growth = false;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    check(
        history.incidents().closed().back().kind
                == incident_kind::sampled_rss_growth
            && history.incidents().closed().back().state
                == incident_state::resolved,
        "a cleared trend finding is not a claim that a leak was repaired"
    );
    sample.memory.sustained_growth = true;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    sample.status = observation_status::exited;
    sample.watchdog.process_lifecycle = lifecycle::clean_exit;
    history.add(sample);
    check(
        active_incidents(history) == 0
            && history.incidents().closed().back().kind
                == incident_kind::process_exit
            && history.incidents().closed().back().state
                == incident_state::observed
            && !history.incidents().closed().back().first.memory,
        "exit is a lifetime fact, not a hang, leak or final RSS sample"
    );
    const auto count = history.incidents().closed().size();
    sample.observed_at_ns++;
    history.add(sample);
    history.note_gap(++sample.observed_at_ns);
    check(
        history.incidents().closed().size() == count,
        "repeated exit polls and final detach do not duplicate incidents"
    );
    auto restarted = incident_sample(sample.observed_at_ns + 1);
    ++restarted.identity.start_ticks;
    restarted.sample->identity = restarted.identity;
    check(
        !history.add(restarted), "process restart cannot redirect old evidence"
    );
    observation_history short_lived(restarted.identity, 3);
    auto early_exit = restarted;
    early_exit.status = observation_status::exited;
    short_lived.add(early_exit);
    check(
        short_lived.incidents().closed().size() == 1
            && short_lived.incidents().closed().back().kind
                == incident_kind::process_exit
            && !short_lived.incidents().closed().back().first.memory,
        "exit before the first resource sample has no fabricated metrics"
    );
    observation_history new_process(restarted.identity, 3);
    new_process.add(restarted);
    check(
        active_incidents(new_process) == 0
            && new_process.incidents().closed().empty(),
        "a short-lived or restarted process has no invented CPU/progress fault"
    );
}

void test_incident_replacement_and_bounds() {
    using namespace monitor;
    auto sample = incident_sample(1);
    const auto core = channel_index(channel::core);
    sample.watchdog.heartbeats[core] = { channel::core, true, 0, 0, 1 };
    observation_history history(sample.identity, 2);
    for (int episode = 0; episode < 200; ++episode) {
        sample.sample->sampled_at_ns = sample.observed_at_ns
            += *sample.stale_after_ns + 1;
        history.add(sample);
        sample.watchdog.heartbeats[core].timestamp_ns = ++sample.observed_at_ns;
        ++sample.watchdog.heartbeats[core].sequence;
        sample.sample->sampled_at_ns = sample.observed_at_ns;
        history.add(sample);
    }
    check(
        history.records().size() == 2
            && history.incidents().closed().size() == 2
            && history.incidents().overwritten() == 198
            && active_incidents(history) == 0,
        "repeated episodes retain bounded evidence and explicit overwrite "
        "counts"
    );
    sample.sample->sampled_at_ns = sample.observed_at_ns
        += *sample.stale_after_ns + 1;
    history.add(sample);
    sample.watchdog.heartbeats[core].expected_since_ns
        = ++sample.observed_at_ns;
    sample.watchdog.heartbeats[core].timestamp_ns = 0;
    sample.sample->sampled_at_ns = sample.observed_at_ns;
    history.add(sample);
    check(
        history.incidents().closed().back().ending
                == incident_ending::expectation_replaced
            && active_incidents(history) == 0,
        "replacement interrupts rather than inherits an overdue expectation"
    );
    sample.sample->sampled_at_ns = sample.observed_at_ns
        += *sample.stale_after_ns + 1;
    history.add(sample);
    sample.watchdog.process_lifecycle = lifecycle::stopping;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    check(
        history.incidents().closed().back().ending
                == incident_ending::producer_stopped
            && history.incidents().closed().back().state
                == incident_state::interrupted,
        "producer shutdown does not claim successful operation progress"
    );
    observation_history disabled(sample.identity, 0);
    disabled.add(sample);
    disabled.note_gap(sample.observed_at_ns + 1);
    check(
        disabled.incidents().closed().empty()
            && active_incidents(disabled) == 0,
        "zero capacity disables incidents"
    );
}

} // namespace monitor_test
