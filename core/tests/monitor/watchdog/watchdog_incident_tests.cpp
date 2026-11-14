#include "monitor/watchdog.hpp"
#include "watchdog_checks.hpp"
#include "watchdog_fixture.hpp"

#include <sstream>
#include <string>

namespace monitor_test {

void test_incident_progress() {
    using namespace monitor;
    auto sample = incident_sample(1);
    const auto stale = *sample.stale_after_ns;
    const auto core = channel_index(channel::core);
    observation_history history(sample.identity, 2);
    // CPU inactivity, high RSS and even an unsupported aggregate summary are
    // insufficient: a live, coherent expected-progress channel is required.
    sample.cpu_percent = 0.0;
    sample.sample->memory.rss_bytes = 9ULL * 1024ULL * 1024ULL * 1024ULL;
    sample.sample->memory.high_water_bytes = sample.sample->memory.rss_bytes;
    sample.responsiveness = health::globally_stalled_idle;
    history.add(sample);
    check(
        active_incidents(history) == 0,
        "idle CPU and high RSS create no finding"
    );
    sample.watchdog.heartbeats[core] = { channel::core, true, 0, 0, 1 };
    sample.observed_at_ns = 1 + stale;
    sample.sample->sampled_at_ns = sample.observed_at_ns;
    history.add(sample);
    check(
        active_incidents(history) == 0,
        "inclusive stale boundary creates no incident"
    );
    const auto first = ++sample.observed_at_ns;
    sample.sample->sampled_at_ns = sample.observed_at_ns;
    sample.cpu_percent.reset();
    history.add(sample);
    check(
        history.incidents().active()[core].has_value(),
        "genuinely overdue owned progress opens a finding"
    );
    for (int repeat = 0; repeat < 100; ++repeat) {
        ++sample.observed_at_ns;
        sample.sample->sampled_at_ns = sample.observed_at_ns;
        history.add(sample);
    }
    const auto& finding = *history.incidents().active()[core];
    check(
        finding.kind == incident_kind::overdue_progress
            && finding.first.observed_at_ns == first
            && finding.matching_observations == 101
            && history.records().size() == 2
            && history.incidents().closed().empty(),
        "rollover preserves first evidence and updates one incident"
    );
    history.add(sample);
    check(
        finding.matching_observations == 101,
        "reusing one observation does not count another sample"
    );
    sample.watchdog.heartbeats[core].timestamp_ns = ++sample.observed_at_ns;
    sample.watchdog.heartbeats[core].sequence = 1;
    sample.sample->sampled_at_ns = sample.observed_at_ns;
    history.add(sample);
    const auto& resolved = history.incidents().closed().back();
    check(
        active_incidents(history) == 0
            && resolved.state == incident_state::resolved
            && resolved.ending == incident_ending::condition_cleared
            && resolved.closing && !resolved.first.cpu_percent,
        "current progress closes the condition without inventing CPU activity"
    );
    std::ostringstream text;
    write_console_incident(text, resolved);
    check(
        text.str().find("source=watchdog") != std::string::npos
            && text.str().find("channel=core") != std::string::npos
            && text.str().find("cpu=unavailable") != std::string::npos
            && text.str().find("sampled_at_ns=") != std::string::npos
            && text.str().find("stale_after_ns=") != std::string::npos
            && text.str().find("expected_since_ns=1") != std::string::npos,
        "incident output identifies source, channel and actual timing evidence"
    );

    sample.observed_at_ns += stale + 1;
    sample.sample->sampled_at_ns = sample.observed_at_ns;
    history.add(sample);
    sample.watchdog.heartbeats[core].active = false;
    sample.watchdog.heartbeats[core].expected_since_ns = 0;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    check(
        history.incidents().closed().back().state == incident_state::interrupted
            && history.incidents().closed().back().ending
                == incident_ending::expectation_retired,
        "scope retirement is not a successful-progress receipt"
    );
}

void test_incident_gaps() {
    using namespace monitor;
    auto sample = incident_sample(1);
    const auto core = channel_index(channel::core);
    sample.watchdog.heartbeats[core] = { channel::core, true, 0, 0, 1 };
    sample.sample->sampled_at_ns = sample.observed_at_ns
        += *sample.stale_after_ns + 1;
    observation_history history(sample.identity, 8);
    history.add(sample);
    auto missing = sample;
    missing.observed_at_ns++;
    missing.status = observation_status::sample_unavailable;
    history.add(missing);
    check(
        history.incidents().closed().back().state
                == incident_state::ended_unknown
            && history.incidents().closed().back().ending
                == incident_ending::observation_gap,
        "missing sample interrupts a stall; it never resolves it"
    );
    missing.observed_at_ns++;
    history.add(missing);
    check(
        active_incidents(history) == 1
            && history.incidents().active().back()->matching_observations == 2
            && !history.incidents().active().back()->first.memory,
        "one uncertainty episode records repeated missing samples without "
        "stale metrics"
    );
    sample.sample->sampled_at_ns = sample.observed_at_ns
        = missing.observed_at_ns + 1;
    sample.watchdog.heartbeats[core].timestamp_ns = sample.observed_at_ns;
    history.add(sample);
    check(
        active_incidents(history) == 0
            && history.incidents().closed().back().state
                == incident_state::resolved
            && history.incidents().closed().front().state
                == incident_state::ended_unknown,
        "sampling recovery closes uncertainty, not the interrupted earlier "
        "stall"
    );

    sample.observed_at_ns += *sample.stale_after_ns + 1;
    sample.sample->sampled_at_ns = sample.observed_at_ns;
    history.add(sample);
    history.note_gap(++sample.observed_at_ns);
    check(
        active_incidents(history) == 0
            && history.incidents().closed().back().ending
                == incident_ending::observer_detached,
        "explicit detach ends open evidence as unknown"
    );
    sample.watchdog.observation_started_ns = sample.observed_at_ns + 1;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    check(
        active_incidents(history) == 0,
        "reattached grace is not stale or fabricated progress"
    );
    sample.sample->sampled_at_ns = sample.observed_at_ns
        += *sample.stale_after_ns + 1;
    history.add(sample);
    sample.watchdog.observation_started_ns = sample.observed_at_ns + 1;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    check(
        history.incidents().closed().back().ending
                == incident_ending::reattached
            && history.incidents().closed().back().state
                == incident_state::ended_unknown,
        "expired observer interval cannot resolve a preceding stall"
    );
    sample.watchdog.heartbeats[core].coherent = false;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    check(
        active_incidents(history) == 1
            && history.incidents().active().back()->kind
                == incident_kind::unavailable_evidence,
        "incoherent watchdog evidence is uncertainty, not a new hang"
    );
    std::ostringstream uncertain;
    write_console_incident(uncertain, *history.incidents().active().back());
    check(
        uncertain.str().find("watchdog_coherent=no") != std::string::npos,
        "incoherent uncertainty reports its underlying evidence"
    );
    sample.watchdog.heartbeats[core].coherent = true;
    sample.watchdog.heartbeats[core].expected_since_ns = 0;
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    history.add(sample);
    std::ostringstream missing_metadata;
    write_console_incident(
        missing_metadata, *history.incidents().active().back()
    );
    check(
        missing_metadata.str().find("expectation_metadata=unavailable")
                != std::string::npos
            && !history.incidents().active()[core],
        "missing expectation metadata reports uncertainty instead of a new "
        "stall"
    );
    history.note_gap(++sample.observed_at_ns);
    check(
        history.incidents().closed().back().state
            == incident_state::ended_unknown,
        "ending an unavailable interval has no fabricated resolution"
    );
}

void test_incident_checkpoint_between_polls() {
    using namespace monitor;
    auto sample = incident_sample(1);
    const auto core = channel_index(channel::core);
    const auto stale = *sample.stale_after_ns;
    sample.watchdog.heartbeats[core] = { channel::core, true, 0, 0, 1 };
    sample.sample->sampled_at_ns = sample.observed_at_ns += stale + 1;
    observation_history history(sample.identity, 3);
    history.add(sample);
    sample.watchdog.heartbeats[core].timestamp_ns = sample.observed_at_ns + 1;
    sample.watchdog.heartbeats[core].sequence = 1;
    sample.sample->sampled_at_ns = sample.observed_at_ns += stale + 2;
    history.add(sample);
    check(
        history.incidents().closed().size() == 1
            && history.incidents().closed().back().ending
                == incident_ending::progress_observed
            && history.incidents().active()[core]->matching_observations == 1
            && history.incidents().active()[core]->first.observed_at_ns
                == sample.observed_at_ns,
        "a checkpoint between two overdue samples separates episodes; no "
        "continuous stall is invented"
    );
}

void test_incident_independent_channels() {
    using namespace monitor;
    auto sample = incident_sample(1);
    const auto core = channel_index(channel::core);
    const auto main = channel_index(channel::main);
    sample.watchdog.heartbeats[core] = { channel::core, true, 0, 0, 1 };
    sample.sample->sampled_at_ns = sample.observed_at_ns
        += *sample.stale_after_ns + 1;
    sample.watchdog.heartbeats[main]
        = { channel::main, true, sample.observed_at_ns, 1, 1 };
    sample.responsiveness
        = health::healthy; // Raw channel evidence is authoritative.
    observation_history history(sample.identity, 4);
    history.add(sample);
    check(
        history.incidents().active()[core]
            && !history.incidents().active()[main],
        "another channel's progress cannot conceal an overdue owner"
    );
    sample.sample->sampled_at_ns = ++sample.observed_at_ns;
    sample.watchdog.heartbeats[main].timestamp_ns = sample.observed_at_ns;
    history.add(sample);
    check(
        history.incidents().active()[core]->matching_observations == 2,
        "unrelated activity does not resolve or replace the existing finding"
    );
    sample.status = observation_status::identity_changed;
    sample.observed_at_ns++;
    history.add(sample);
    check(
        active_incidents(history) == 0
            && history.incidents().closed().front().state
                == incident_state::ended_unknown
            && history.incidents().closed().front().ending
                == incident_ending::identity_changed
            && history.incidents().closed().back().kind
                != incident_kind::process_exit,
        "identity loss is unknown termination, never an invented exit or "
        "recovery"
    );
}

} // namespace monitor_test
