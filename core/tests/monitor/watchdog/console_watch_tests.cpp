#include "monitor/console_watch.hpp"
#include "watchdog_checks.hpp"
#include "watchdog_fixture.hpp"

#include <array>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace monitor_test {

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

} // namespace monitor_test
