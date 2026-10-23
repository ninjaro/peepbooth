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
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

monitor::process_observation incident_sample(const std::uint64_t now) {
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

std::size_t active_incidents(const monitor::observation_history& history) {
    return static_cast<std::size_t>(std::ranges::count_if(
        history.incidents().active(),
        [](const auto& item) { return item.has_value(); }
    ));
}

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

void test_recording() {
#if defined(__linux__) && !defined(__ANDROID__)
    const char* requested = std::getenv("MONITOR_RECORDING_FIXTURES");
    auto pattern
        = (std::filesystem::temp_directory_path() / "monitor-recording-XXXXXX")
              .string();
    const char* created = requested ? requested : ::mkdtemp(pattern.data());
    check(created != nullptr, "create isolated recording test directory");
    if (!created)
        return;
    const std::filesystem::path directory(created);
    std::filesystem::create_directories(directory);
    const monitor::recording_session session { .target = { 42, 99 },
                                               .watcher = { 43, 100 },
                                               .started_ns = 1,
                                               .started_utc_ns = 1,
                                               .interval_ms = 50,
                                               .stale_after_ns = 100,
                                               .history_capacity = 2,
                                               .boot_id = "test\"boot\n",
                                               .system = "test",
                                               .release = {},
                                               .machine = "host" };
    const monitor::recording_limits limits { 1'048'576, 100, 65'536 };
    monitor::observation_history history(session.target, 2);
    auto sample = incident_sample(10);
    history.add(sample);
    const auto path
        = [&](const char* name) { return (directory / name).string(); };
    const auto contents = [&](const std::string& filename) {
        std::ifstream input(filename);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto throws = [&](const std::string& filename,
                            const monitor::recording_limits chosen) {
        try {
            monitor::session_recorder rejected(filename, session, chosen);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    {
        monitor::session_recorder writer(
            path("complete.ndjson"), session, limits
        );
        check(
            writer.append(history),
            "record sampled observation with unknown CPU"
        );
        sample.observed_at_ns = 200;
        sample.sample->sampled_at_ns = 200;
        sample.watchdog.heartbeats[0].active = true;
        sample.watchdog.heartbeats[0].expected_since_ns = 1;
        sample.watchdog.heartbeats[0].timestamp_ns = 1;
        sample.stale_after_ns = 100;
        history.add(sample);
        check(writer.append(history), "record genuine overdue incident");
        sample.observed_at_ns = 201;
        sample.sample->sampled_at_ns = 201;
        history.add(sample);
        check(
            writer.append(history), "record incident update without duplicate"
        );
        history.note_gap(202);
        check(writer.append(history), "record gap and ending evidence");
        sample.observed_at_ns = 203;
        sample.status = monitor::observation_status::exited;
        history.add(sample);
        check(writer.append(history), "record early process exit");
        history.note_gap(204);
        check(writer.append(history), "record detach after exit");
        check(
            writer.finish(monitor::recording_outcome::process_exited, 0, 205),
            "finalize recording"
        );
        check(
            writer.state() == monitor::recording_state::finalized
                && writer.checkpoints() == 6
                && writer.bytes()
                    == std::filesystem::file_size(path("complete.ndjson")),
            "recording totals describe exact bytes"
        );
        check(
            !std::filesystem::exists(path("complete.ndjson.partial")),
            "publication removes partial name"
        );
    }
    {
        monitor::observation_history rollover(session.target, 2);
        monitor::session_recorder writer(
            path("rollover.ndjson"), session, limits
        );
        for (std::uint64_t index = 0; index < 8; ++index) {
            auto value = incident_sample(10 + index * 2);
            value.status = monitor::observation_status::sample_unavailable;
            rollover.add(value);
            check(
                writer.append(rollover),
                "record opening uncertainty episode before rollover"
            );
            value.status = monitor::observation_status::sampled;
            ++value.observed_at_ns;
            value.sample->sampled_at_ns = value.observed_at_ns;
            rollover.add(value);
            check(
                writer.append(rollover),
                "record resolution and closed-incident rollover"
            );
        }
        rollover.note_gap(26);
        check(
            writer.append(rollover)
                && writer.finish(monitor::recording_outcome::once, 0, 27),
            "finalize both rollover counters"
        );
    }
    {
        monitor::observation_history active(session.target, 2);
        auto value = incident_sample(200);
        value.stale_after_ns = 100;
        value.watchdog.heartbeats[0].active = true;
        value.watchdog.heartbeats[0].expected_since_ns = 1;
        value.watchdog.heartbeats[0].timestamp_ns = 1;
        active.add(value);
        monitor::session_recorder writer(
            path("active-interrupted.ndjson"), session, limits
        );
        check(
            writer.append(active),
            "preserve active evidence before interruption without inventing an "
            "ending"
        );
    }
    const auto original = contents(path("complete.ndjson"));
    check(
        throws(path("complete.ndjson"), limits)
            && original == contents(path("complete.ndjson")),
        "never overwrite existing recording"
    );
    {
        monitor::session_recorder writer(
            path("interrupted.ndjson"), session, limits
        );
        check(
            writer.append(history),
            "commit checkpoint before simulated interruption"
        );
    }
    check(
        !std::filesystem::exists(path("interrupted.ndjson"))
            && contents(path("interrupted.ndjson.partial"))
                    .find(R"("type":"end")")
                == std::string::npos,
        "destruction leaves honest partial without terminal"
    );
    check(
        throws(path("interrupted.ndjson"), limits),
        "never overwrite interrupted evidence"
    );
    ::symlink(
        path("complete.ndjson").c_str(), path("symlink.ndjson.partial").c_str()
    );
    check(
        throws(path("symlink.ndjson"), limits),
        "partial symlink cannot redirect writes"
    );
    ::symlink(
        path("complete.ndjson").c_str(), path("destination-symlink").c_str()
    );
    check(
        throws(path("destination-symlink"), limits),
        "destination symlink rejected"
    );
    check(
        throws(path("no-parent/file"), limits),
        "missing directory is explicit error"
    );
    check(
        throws(path("zero"), { 0, 0, 0 }), "recording has no implicit limits"
    );
    check(
        throws(path("reserved.partial"), limits),
        "final path cannot masquerade as partial"
    );
    auto invalid_session = session;
    invalid_session.target.pid = 0;
    bool rejected_session = false;
    try {
        monitor::session_recorder invalid(
            path("invalid-session"), invalid_session, limits
        );
    } catch (const std::invalid_argument&) {
        rejected_session = true;
    }
    check(
        rejected_session,
        "reject unknown session identity before creating output"
    );
    check(
        throws(path("tiny"), { 10, 10, 10 }),
        "reject insufficient header/footer capacity"
    );
    {
        monitor::session_recorder writer(
            path("count-limit.ndjson"), session, { 65'536, 1, 65'536 }
        );
        check(
            writer.append(history) && !writer.append(history),
            "checkpoint limit stops additional capture"
        );
        check(
            writer.state() == monitor::recording_state::limited
                && !writer.finish(monitor::recording_outcome::once, 0, 300),
            "limit remains incomplete, never normal completion"
        );
        check(
            contents(path("count-limit.ndjson"))
                    .find(R"("outcome":"limit_reached")")
                != std::string::npos,
            "limit has durable terminal explanation"
        );
    }
    {
        monitor::session_recorder writer(
            path("byte-limit.ndjson"), session, { 4096, 100, 65'536 }
        );
        while (writer.append(history)) { }
        check(
            writer.state() == monitor::recording_state::limited
                && writer.bytes() <= 4096,
            "byte budget includes reserved terminal evidence"
        );
    }
    {
        monitor::session_recorder writer(
            path("frame-limit.ndjson"), session, { 65'536, 100, 1024 }
        );
        check(
            !writer.append(history)
                && writer.state() == monitor::recording_state::limited,
            "oversized incident checkpoint ends honestly within frame budget"
        );
    }
    {
        monitor::session_recorder writer(
            path("foreign.ndjson"), session, limits
        );
        monitor::observation_history foreign({ 99, 99 }, 2);
        auto value = incident_sample(300);
        value.identity.pid = 99;
        foreign.add(value);
        check(
            !writer.append(foreign)
                && writer.state() == monitor::recording_state::failed,
            "foreign identity cannot contaminate session"
        );
    }
    {
        monitor::session_recorder writer(
            path("nonfinite.ndjson"), session, limits
        );
        monitor::observation_history invalid(session.target, 2);
        auto value = incident_sample(300);
        value.cpu_percent = std::numeric_limits<double>::infinity();
        invalid.add(value);
        check(
            !writer.append(invalid)
                && writer.state() == monitor::recording_state::failed,
            "invalid metric cannot become zero or invalid JSON"
        );
    }
    {
        monitor::session_recorder writer(
            path("publish-conflict.ndjson"), session, limits
        );
        writer.append(history);
        history.note_gap(206);
        writer.append(history);
        {
            std::ofstream collision(path("publish-conflict.ndjson"));
            collision << "authored";
        }
        check(
            !writer.finish(monitor::recording_outcome::once, 0, 207)
                && contents(path("publish-conflict.ndjson")) == "authored",
            "publication race cannot replace another file"
        );
        check(
            std::filesystem::exists(path("publish-conflict.ndjson.partial")),
            "failed publication retains evidence"
        );
    }
    // A real short write/EFBIG in a child; no process-wide test policy
    // mutation.
    const auto child = ::fork();
    if (child == 0) {
        monitor::session_recorder writer(
            path("io-error.ndjson"), session, limits
        );
        const struct rlimit limit { writer.bytes() + 32, writer.bytes() + 32 };
        static_cast<void>(std::signal(SIGXFSZ, SIG_IGN));
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0)
            ::_exit(3);
        const bool failed = !writer.append(history)
            && writer.state() == monitor::recording_state::failed;
        ::_exit(failed ? 0 : 4);
    }
    int status = 0;
    check(
        child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status)
            && WEXITSTATUS(status) == 0,
        "filesystem write failure is reported, never finalized"
    );
    check(
        !std::filesystem::exists(path("io-error.ndjson"))
            && std::filesystem::exists(path("io-error.ndjson.partial")),
        "short write preserves interrupted prefix"
    );
    if (!requested)
        std::filesystem::remove_all(directory);
#endif
}

void test_command_line_parser() {
    const auto parses
        = [](const std::initializer_list<std::string_view> values) {
              return monitor::parse_command_line(
                         std::span(values.begin(), values.size())
              )
                  .action;
          };
    using action = monitor::command_line_action;
    check(
        parses(
            { "--pid", "42", "--history", "2", "--record", "capture",
              "--record-bytes", "10000", "--record-checkpoints", "10",
              "--record-frame-bytes", "4096" }
        ) == action::run,
        "recording requires explicit single-target limits"
    );
    check(
        parses({ "--pid", "42", "--record", "capture" }) == action::error,
        "recording cannot invent retention limits"
    );
    check(
        parses({ "--pid", "42", "--record-bytes", "10000" }) == action::error,
        "limits without recording are rejected"
    );
    check(
        parses(
            { "--all", "--history", "2", "--record", "capture",
              "--record-bytes", "10000", "--record-checkpoints", "10",
              "--record-frame-bytes", "4096" }
        ) == action::error,
        "cohort recording is not silently enabled"
    );

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
    const std::array history { std::string_view { "--pid" },
                               std::string_view { "42" },
                               std::string_view { "--history" },
                               std::string_view { "3" } };
    check(
        monitor::parse_command_line(history).options.history_capacity == 3,
        "history capacity is explicit"
    );

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

    const std::array named { std::string_view { "--name" },
                             std::string_view { "producer" },
                             std::string_view { "--once" } };
    const auto by_name = monitor::parse_command_line(named);
    check(
        by_name.action == monitor::command_line_action::run
            && by_name.options.name == "producer" && by_name.options.once
            && !by_name.options.all && by_name.options.pid == 0,
        "exact friendly-name observation is an explicit selector"
    );
    const std::array all { std::string_view { "--all" },
                           std::string_view { "--once" } };
    const auto cohort = monitor::parse_command_line(all);
    check(
        cohort.action == monitor::command_line_action::run && cohort.options.all
            && cohort.options.once,
        "all-process observation is explicit and accepts once mode"
    );
    const std::vector<std::vector<std::string_view>> bad_selections {
        {},
        { "--pid", "0" },
        { "--name" },
        { "--name", "" },
        { "--all", "--name", "p" },
        { "--name", "p", "--all" },
        { "--all", "--pid", "42" },
        { "--pid", "42", "--pid", "43" },
        { "--all", "--all" },
        { "--name", "p", "--name", "p" },
        { "--pid", "42", "--history", "0" },
        { "--pid", "42", "--history", "-1" },
        { "--all", "--history", "3" },
        { "--name", "p", "--history", "3" }
    };
    for (const auto& arguments : bad_selections)
        check(
            monitor::parse_command_line(arguments).action
                == monitor::command_line_action::error,
            "missing, duplicate and conflicting process selectors are rejected"
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
            inventory.diagnostics, [&](const auto& message) {
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
            inventory.processes, [&](const auto& process) {
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
    if (child_identity) {
        registry.clear();
        check(
            registry.wait_for_exit(0),
            "empty registry has no pending lifetime wait"
        );
        monitor::process_discovery cohort;
        cohort.available = true;
        cohort.processes = { { *identity, "discovery\nname" },
                             { *child_identity, "discovery\nname" } };
        monitor::console_options options;
        options.all = true;
        options.once = true;
        std::atomic_bool stop { false };
        std::ostringstream out, err;
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 0
                && err.str().empty(),
            "all/once uses real independent owners for the supplied live cohort"
        );
        const auto parent_label = "[" + std::to_string(identity->pid) + "]";
        const auto child_label
            = "[" + std::to_string(child_identity->pid) + "]";
        check(
            out.str().find(parent_label) != std::string::npos
                && out.str().find(child_label) != std::string::npos,
            "group output contains both actual process observations"
        );
        check(
            state.watcher_present.load() == 0 && retained_child
                && retained_child->get()->watcher_present.load() == 0,
            "once completion releases every cohort lease without unlinking "
            "endpoints"
        );
        options.all = false;
        options.name = "discovery\nname";
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 1
                && err.str().find("ambiguous") != std::string::npos
                && out.str().empty(),
            "CLI name ambiguity fails before observation"
        );
        cohort.processes.pop_back();
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 0
                && err.str().empty()
                && out.str().find(parent_label) != std::string::npos
                && out.str().find(child_label) == std::string::npos,
            "unique-name once selection observes exactly its real process"
        );
        options.name = {};
        options.all = true;
        auto stale_child = *child_identity;
        ++stale_child.start_ticks;
        cohort.processes.push_back({ stale_child, "discovery\nname" });
        auto independent = monitor::watchdog_session::attach(*child_identity);
        if (independent && retained_child) {
            const auto independent_lease
                = retained_child->get()->watcher_lease_until_ns.load();
            out.str("");
            err.str("");
            check(
                monitor::run_console_watch_group(
                    options, cohort, stop, out, err
                ) == 1
                    && err.str().find("selected process incarnation")
                        != std::string::npos,
                "a stale cohort member produces an explicit attach failure"
            );
            check(
                state.watcher_present.load() == 0
                    && retained_child->get()->watcher_present.load() == 1
                    && retained_child->get()->watcher_lease_until_ns.load()
                        == independent_lease,
                "partial attach failure releases A without clearing an "
                "unrelated B lease"
            );
        } else
            check(
                false,
                "independent lease is available for partial-attach regression"
            );
        independent.reset();
        cohort.diagnostics.emplace_back("owned fixture inspection unavailable");
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 1
                && err.str().find("owned fixture inspection unavailable")
                    != std::string::npos,
            "incomplete cohort discovery fails with its diagnostic"
        );
        cohort.diagnostics.clear();
        cohort.processes.clear();
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 1
                && err.str().find("no compatible processes")
                    != std::string::npos,
            "an empty observation cohort is reported explicitly"
        );
        stop.store(true);
        out.str("");
        err.str("");
        check(
            monitor::run_console_watch_group(options, cohort, stop, out, err)
                    == 0
                && err.str().empty(),
            "cancellation before group startup does not acquire leases"
        );
        check(
            registry.attach(*child_identity),
            "restore child owner for retained-exit regression"
        );
        check(
            !registry.wait_for_exit(0),
            "live cohort supports a nonblocking lifetime wait"
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

} // namespace

int main() {
    test_recording();
    test_incident_progress();
    test_incident_gaps();
    test_incident_independent_channels();
    test_incident_checkpoint_between_polls();
    test_incident_memory_and_lifetime();
    test_incident_replacement_and_bounds();
    test_observation_history();
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
    test_expected_progress_classification();
    test_yodau_progress_semantics();
    test_progress_owner();
    test_concurrent_progress_publication();
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
