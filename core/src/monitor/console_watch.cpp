#include "monitor/console_watch.hpp"
#include "console_watch_internal.hpp"

#include "monitor/observation_registry.hpp"
#include "monitor/process_discovery.hpp"
#include "monitor/watchdog.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace monitor {
namespace console_detail {

    template <typename Integer>
    bool parse_integer(const std::string_view text, Integer& output) noexcept {
        const auto result
            = std::from_chars(text.data(), text.data() + text.size(), output);
        return result.ec == std::errc {}
        && result.ptr == text.data() + text.size();
    }

} // namespace console_detail

using namespace console_detail;

parsed_command_line
parse_command_line(const std::span<const std::string_view> arguments) noexcept {
    parsed_command_line parsed;
    if (arguments.size() == 1 && arguments.front() == "--list") {
        parsed.action = command_line_action::list;
        return parsed;
    }
    parsed.action = command_line_action::run;
    bool selected = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--help" || argument == "-h") {
            parsed.action = command_line_action::help;
            return parsed;
        }
        if (argument == "--once") {
            parsed.options.once = true;
            continue;
        }
        if (argument == "--all" && !selected) {
            parsed.options.all = true;
            selected = true;
            continue;
        }
        if (++index >= arguments.size()) {
            parsed.action = command_line_action::error;
            return parsed;
        }
        const std::string_view value = arguments[index];
        if (argument == "--pid" && !selected
            && parse_integer(value, parsed.options.pid)
            && parsed.options.pid != 0) {
            selected = true;
            continue;
        }
        if (argument == "--name" && !selected && !value.empty()) {
            parsed.options.name = value;
            selected = true;
            continue;
        }
        if (argument == "--interval-ms"
            && parse_integer(value, parsed.options.interval_ms)
            && parsed.options.interval_ms >= 50
            && parsed.options.interval_ms <= 60'000) {
            continue;
        }
        if (argument == "--history"
            && parse_integer(value, parsed.options.history_capacity)
            && parsed.options.history_capacity != 0) {
            continue;
        }
        if (argument == "--record" && parsed.options.recording_path.empty()
            && !value.empty()) {
            parsed.options.recording_path = value;
            continue;
        }
        if (argument == "--record-bytes"
            && parse_integer(value, parsed.options.recording.bytes)
            && parsed.options.recording.bytes != 0)
            continue;
        if (argument == "--record-checkpoints"
            && parse_integer(value, parsed.options.recording.checkpoints)
            && parsed.options.recording.checkpoints != 0)
            continue;
        if (argument == "--record-frame-bytes"
            && parse_integer(value, parsed.options.recording.frame_bytes)
            && parsed.options.recording.frame_bytes != 0)
            continue;
        if (argument == "--stale-ms"
            && parse_integer(value, parsed.options.stale_ms)
            && parsed.options.stale_ms >= 100) {
            continue;
        }
        parsed.action = command_line_action::error;
        return parsed;
    }
    const bool record = !parsed.options.recording_path.empty();
    const auto limits = parsed.options.recording;
    const bool any_limit = limits.bytes != 0 || limits.checkpoints != 0
        || limits.frame_bytes != 0;
    const bool all_limits = limits.bytes != 0 && limits.checkpoints != 0
        && limits.frame_bytes != 0;
    if ((record
         && (parsed.options.pid == 0 || parsed.options.history_capacity == 0
             || !all_limits))
        || (!record && any_limit))
        parsed.action = command_line_action::error;
    if (!selected
        || (parsed.options.history_capacity != 0 && parsed.options.pid == 0)) {
        parsed.action = command_line_action::error;
    }
    return parsed;
}

void write_console_usage(std::ostream& output) {
    output << "usage: monitor (--pid PID | --name NAME | --all) "
              "[--interval-ms N] [--stale-ms N] [--once]\n"
              "       monitor --pid PID --history N (last N observations and "
              "closed incidents)\n"
              "       monitor --pid PID --history N --record PATH "
              "--record-bytes N "
              "--record-checkpoints N --record-frame-bytes N\n"
              "       monitor --list\n";
}

int run_console_list(std::ostream& output, std::ostream& error) {
    const auto inventory = discover_processes();
    if (inventory.available) {
        output << "PID\tSTART_TICKS\tNAME\n";
        for (const auto& process : inventory.processes) {
            auto name = process.name;
            for (auto& character : name) {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20U || byte == 0x7fU)
                    character = '?';
            }
            output << process.identity.pid << '\t'
                   << process.identity.start_ticks << '\t' << std::quoted(name)
                   << '\n';
        }
    }
    for (const auto& diagnostic : inventory.diagnostics)
        error << "monitor: " << diagnostic << '\n';
    return inventory.available && inventory.diagnostics.empty() ? 0 : 1;
}

int run_console_watch_group(
    const console_options& options, const process_discovery& inventory,
    const std::atomic_bool& stop_requested, std::ostream& output,
    std::ostream& error
) {
    if (stop_requested.load(std::memory_order_relaxed))
        return 0;
    if (options.pid != 0 || options.all == !options.name.empty()
        || options.history_capacity != 0 || !options.recording_path.empty()) {
        error << "monitor: group observation requires exactly one of --all or "
                 "--name; --history requires a single --pid\n";
        return 1;
    }
    if (!inventory.available || !inventory.diagnostics.empty()) {
        error << "monitor: process discovery is incomplete\n";
        for (const auto& diagnostic : inventory.diagnostics)
            error << "monitor: " << diagnostic << '\n';
        return 1;
    }
    std::vector<platform::process_identity> selected;
    if (options.all) {
        for (const auto& endpoint : inventory.processes)
            selected.push_back(endpoint.identity);
    } else {
        const auto named = select_process_name(inventory, options.name);
        if (!named.identity) {
            error << "monitor: " << named.error << '\n';
            return 1;
        }
        selected.push_back(*named.identity);
    }
    if (selected.empty()) {
        error << "monitor: no compatible processes discovered\n";
        return 1;
    }
    observation_registry registry(
        static_cast<std::uint64_t>(options.stale_ms)
        * std::uint64_t { 1'000'000 }
    );
    for (const auto identity : selected) {
        if (stop_requested.load(std::memory_order_relaxed))
            return 0;
        if (!registry.attach(identity)) {
            error << "monitor: PID " << identity.pid
                  << " start_ticks=" << identity.start_ticks
                  << " could not attach to the selected process incarnation\n";
            return 1;
        }
    }
    while (!stop_requested.load(std::memory_order_relaxed)) {
        for (const auto& observation : registry.poll()) {
            if (observation.status == observation_status::exited) {
                output << "PID " << observation.identity.pid
                       << " start_ticks=" << observation.identity.start_ticks
                       << " exited; attached-process exit code/signal "
                          "unavailable lifecycle="
                       << static_cast<std::uint32_t>(
                              observation.watchdog.process_lifecycle
                          )
                       << '\n';
                registry.detach(observation.identity);
                continue;
            }
            if (observation.status != observation_status::sampled) {
                error << "monitor: PID " << observation.identity.pid
                      << " start_ticks=" << observation.identity.start_ticks
                      << (observation.status
                                  == observation_status::identity_changed
                              ? " process identity changed\n"
                              : " resource sample unavailable\n");
                return 1;
            }
            write_console_observation(output, observation);
        }
        if (options.once || registry.size() == 0)
            return 0;
        static_cast<void>(registry.wait_for_exit(options.interval_ms));
    }
    return 0;
}

int run_console_watch(
    const console_options& options, const std::atomic_bool& stop_requested,
    std::ostream& output, std::ostream& error
) {
    if (options.all || !options.name.empty())
        return run_console_watch_group(
            options, discover_processes(), stop_requested, output, error
        );
    auto process = platform::process_handle::attach(options.pid);
    if (!process) {
        error << "monitor: PID " << options.pid
              << " does not identify a running process\n";
        return 1;
    }

    observed_process observer(
        std::move(*process),
        static_cast<std::uint64_t>(options.stale_ms)
            * std::uint64_t { 1'000'000 }
    );
    bool attached = false;
    for (int attempt = 0; attempt < 50
         && !stop_requested.load(std::memory_order_relaxed) && observer.alive();
         ++attempt) {
        attached = observer.try_attach();
        if (attached || observer.wait_for_exit(100)) {
            break;
        }
    }
    if (stop_requested.load(std::memory_order_relaxed)) {
        return 0;
    }
    if (!attached) {
        error << "monitor: PID " << options.pid
              << " has no compatible watchdog state\n";
        return 1;
    }

    observation_history history(
        observer.latest().identity, options.history_capacity
    );
    std::unique_ptr<session_recorder> recorder;
    if (!options.recording_path.empty()) {
        try {
            const auto watcher = platform::read_process_identity(
                platform::current_process_id()
            );
            if (!watcher)
                throw std::runtime_error("watcher identity unavailable");
            recording_session session;
            session.target = observer.latest().identity;
            session.watcher = *watcher;
            session.started_ns = platform::monotonic_time_ns();
            session.started_utc_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                )
                    .count()
            );
            session.interval_ms = options.interval_ms;
            session.stale_after_ns
                = static_cast<std::uint64_t>(options.stale_ms)
                * std::uint64_t { 1'000'000 };
            session.history_capacity = options.history_capacity;
#if defined(__linux__) && !defined(__ANDROID__)
            struct utsname environment {};
            if (::uname(&environment) == 0) {
                session.system = environment.sysname;
                session.release = environment.release;
                session.machine = environment.machine;
            }
            std::ifstream boot("/proc/sys/kernel/random/boot_id");
            std::string boot_id;
            if (std::getline(boot, boot_id))
                session.boot_id = std::move(boot_id);
#endif
            recorder = std::make_unique<session_recorder>(
                std::string(options.recording_path), session, options.recording
            );
        } catch (const std::exception& exception) {
            error << "monitor: cannot start recording: " << exception.what()
                  << '\n';
            return 1;
        }
    }
    bool recording_reported = false;
    const auto checkpoint = [&] {
        if (recorder && recorder->state() == recording_state::active
            && !recorder->append(history) && !recording_reported) {
            error << "monitor: " << recorder->error() << '\n';
            recording_reported = true;
        }
    };
    const auto finish = [&](const int code, const recording_outcome outcome) {
        if (options.history_capacity != 0) {
            history.note_gap(platform::monotonic_time_ns());
            checkpoint();
            write_history(output, history);
        }
        if (recorder) {
            if (recorder->state() == recording_state::active
                && !recorder->finish(
                    outcome, code, platform::monotonic_time_ns()
                )
                && !recording_reported)
                error << "monitor: " << recorder->error() << '\n';
            if (recorder->state() != recording_state::finalized)
                return 1;
        }
        return code;
    };
    bool missing_samples = false;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        const auto& observation = observer.poll();
        history.add(observation);
        checkpoint();
        if (observation.status == observation_status::exited) {
            break;
        }
        if (observation.status != observation_status::sampled) {
            error << "monitor: PID " << options.pid
                  << " observation=" << status_name(observation.status) << '\n';
            if (observation.status != observation_status::sample_unavailable)
                return finish(
                    1,
                    observation.status == observation_status::identity_changed
                        ? recording_outcome::identity_changed
                        : recording_outcome::observation_error
                );
            // Resource fields can disappear before the lifetime handle reports
            // exit. Keep observing without fabricating evidence or hiding the
            // missing sample in the eventual session result.
            missing_samples = true;
        } else {
            write_console_observation(output, observation);
        }
        if (options.once || observer.wait_for_exit(options.interval_ms)) {
            break;
        }
    }
    // Use the same lifetime observation for sampling and the terminal claim.
    // A later exit must not produce an outcome without corresponding evidence.
    const bool exited = !observer.alive();
    if (exited) {
        const auto& final = observer.poll();
        history.add(final);
        checkpoint();
        output
            << "process exited; attached-process exit code/signal unavailable"
            << " lifecycle="
            << static_cast<std::uint32_t>(final.watchdog.process_lifecycle)
            << '\n';
    }
    const auto outcome = exited ? recording_outcome::process_exited
        : stop_requested.load(std::memory_order_relaxed)
        ? recording_outcome::cancelled
        : recording_outcome::once;
    return finish(missing_samples ? 1 : 0, outcome);
}

} // namespace monitor
