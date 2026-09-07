#include "monitor/console_watch.hpp"

#include "monitor/observed_process.hpp"
#include "monitor/watchdog.hpp"

#include <charconv>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <ostream>
#include <utility>

namespace monitor {
namespace {

    template <typename Integer>
    bool parse_integer(const std::string_view text, Integer& output) noexcept {
        const auto result
            = std::from_chars(text.data(), text.data() + text.size(), output);
        return result.ec == std::errc {}
        && result.ptr == text.data() + text.size();
    }

    double mib(const std::uint64_t bytes) noexcept {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

} // namespace

void write_console_observation(
    std::ostream& output, const process_observation& observation
) {
    if (observation.status != observation_status::sampled || !observation.sample
        || !observation.responsiveness) {
        return;
    }
    const auto& state = observation.watchdog;
    const auto& sample = *observation.sample;
    const auto& trend = observation.memory;
    const auto now = observation.observed_at_ns;
    const double cpu = observation.cpu_percent.value_or(0.0);
    const auto health = *observation.responsiveness;
    output << (state.process_name.empty() ? "process" : state.process_name)
           << '[' << observation.identity.pid
           << "] state=" << health_name(health) << " cpu=" << std::fixed
           << std::setprecision(1) << cpu << '%'
           << " rss=" << std::setprecision(1) << mib(sample.memory.rss_bytes)
           << "MiB hwm=" << mib(sample.memory.high_water_bytes)
           << "MiB swap=" << mib(sample.memory.swap_bytes) << "MiB";
    if (trend.duration_ns != 0) {
        const double signed_delta = trend.rss_delta_bytes >= 0
            ? mib(static_cast<std::uint64_t>(trend.rss_delta_bytes))
            : -mib(static_cast<std::uint64_t>(-trend.rss_delta_bytes));
        output << " rss_trend=" << std::showpos << signed_delta
               << std::noshowpos << "MiB sustained_growth="
               << (trend.sustained_growth ? "yes" : "no");
    }
    output << '\n';

    output << "  heartbeat:";
    for (const auto& heartbeat : state.heartbeats) {
        if (!heartbeat.active) {
            continue;
        }
        const double age = heartbeat.timestamp_ns == 0
            ? -1.0
            : static_cast<double>(
                  now >= heartbeat.timestamp_ns ? now - heartbeat.timestamp_ns
                                                : 0
              ) / 1e9;
        output << ' ' << channel_name(heartbeat.id) << '=';
        if (age < 0) {
            output << "pending";
        } else {
            output << std::setprecision(3) << age << 's';
        }
    }
    output << '\n';

    const std::size_t start
        = state.breadcrumbs.size() > 8 ? state.breadcrumbs.size() - 8 : 0;
    if (start < state.breadcrumbs.size()) {
        output << "  recent:";
        for (std::size_t index = start; index < state.breadcrumbs.size();
             ++index) {
            const auto& item = state.breadcrumbs[index];
            output << ' ' << event_name(item.event_id)
                   << "(source=" << item.source_id << ",arg0=" << item.arg0
                   << ",arg1=" << item.arg1 << ')';
        }
        output << '\n';
    }
    output.flush();
}

parsed_command_line
parse_command_line(const std::span<const std::string_view> arguments) noexcept {
    parsed_command_line parsed;
    parsed.action = command_line_action::run;
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
        if (++index >= arguments.size()) {
            parsed.action = command_line_action::error;
            return parsed;
        }
        const std::string_view value = arguments[index];
        if (argument == "--pid" && parse_integer(value, parsed.options.pid)) {
            continue;
        }
        if (argument == "--interval-ms"
            && parse_integer(value, parsed.options.interval_ms)
            && parsed.options.interval_ms >= 50
            && parsed.options.interval_ms <= 60'000) {
            continue;
        }
        if (argument == "--stale-ms"
            && parse_integer(value, parsed.options.stale_ms)
            && parsed.options.stale_ms >= 100) {
            continue;
        }
        parsed.action = command_line_action::error;
        return parsed;
    }
    if (parsed.options.pid == 0) {
        parsed.action = command_line_action::error;
    }
    return parsed;
}

void write_console_usage(std::ostream& output) {
    output << "usage: monitor --pid PID [--interval-ms N] [--stale-ms N] "
              "[--once]\n";
}

int run_console_watch(
    const console_options& options, const std::atomic_bool& stop_requested,
    std::ostream& output, std::ostream& error
) {
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

    while (!stop_requested.load(std::memory_order_relaxed)) {
        const auto& observation = observer.poll();
        if (observation.status == observation_status::exited) {
            break;
        }
        if (observation.status != observation_status::sampled) {
            error << "monitor: PID " << options.pid
                  << (observation.status == observation_status::identity_changed
                          ? " process identity changed\n"
                          : " resource sample unavailable\n");
            return 1;
        }
        write_console_observation(output, observation);
        if (options.once || observer.wait_for_exit(options.interval_ms)) {
            break;
        }
    }
    if (!observer.alive()) {
        const auto& final_state = observer.poll().watchdog;
        output
            << "process exited; attached-process exit code/signal unavailable"
            << " lifecycle="
            << static_cast<std::uint32_t>(final_state.process_lifecycle)
            << '\n';
    }
    return 0;
}

} // namespace monitor
