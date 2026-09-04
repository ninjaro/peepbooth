#include "monitor/console_watch.hpp"

#include "monitor/platform/process_watch.hpp"
#include "monitor/watchdog.hpp"

#include <charconv>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <ostream>

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

    double cpu_percent(
        const platform::process_sample& current,
        const std::optional<platform::process_sample>& previous
    ) noexcept {
        if (!previous || current.sampled_at_ns <= previous->sampled_at_ns) {
            return 0;
        }
        const double elapsed
            = static_cast<double>(
                  current.sampled_at_ns - previous->sampled_at_ns
              )
            / 1e9;
        const auto ticks = current.cpu_ticks - previous->cpu_ticks;
        return static_cast<double>(ticks) * 100.0
            / (static_cast<double>(platform::clock_ticks_per_second())
               * elapsed);
    }

    void write_snapshot(
        std::ostream& output, const console_options& options,
        const watchdog_snapshot& state, const platform::process_sample& sample,
        const double cpu, const memory_trend_summary& trend
    ) {
        const auto now = platform::monotonic_time_ns();
        const auto health = classify_health(
            state, now,
            static_cast<std::uint64_t>(options.stale_ms) * 1'000'000ULL, cpu
        );
        output << (state.process_name.empty() ? "process" : state.process_name)
               << '[' << options.pid << "] state=" << health_name(health)
               << " cpu=" << std::fixed << std::setprecision(1) << cpu << '%'
               << " rss=" << std::setprecision(1)
               << mib(sample.memory.rss_bytes)
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
                      now >= heartbeat.timestamp_ns
                          ? now - heartbeat.timestamp_ns
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

} // namespace

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

    std::optional<watchdog_session> session;
    for (int attempt = 0; attempt < 50 && process->alive(); ++attempt) {
        session = watchdog_session::attach(process->identity());
        if (session || process->wait_for_exit(100)) {
            break;
        }
    }
    if (!session) {
        error << "monitor: PID " << options.pid
              << " has no compatible watchdog state\n";
        return 1;
    }

    memory_trend memory;
    std::optional<platform::process_sample> previous;
    while (!stop_requested.load(std::memory_order_relaxed)
           && process->alive()) {
        const auto sample = platform::sample_process(options.pid);
        if (!sample
            || sample->identity.start_ticks
                != process->identity().start_ticks) {
            break;
        }
        const double cpu = cpu_percent(*sample, previous);
        memory.add(sample->sampled_at_ns, sample->memory.rss_bytes);
        session->renew_lease(platform::monotonic_time_ns());
        write_snapshot(
            output, options, session->snapshot(), *sample, cpu, memory.summary()
        );
        previous = sample;
        if (options.once || process->wait_for_exit(options.interval_ms)) {
            break;
        }
    }
    const auto final_state = session->snapshot();
    session->release();
    if (!process->alive()) {
        output
            << "process exited; attached-process exit code/signal unavailable"
            << " lifecycle="
            << static_cast<std::uint32_t>(final_state.process_lifecycle)
            << '\n';
    }
    return 0;
}

} // namespace monitor
