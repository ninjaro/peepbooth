#ifndef CPPR_MONITOR_CONSOLE_WATCH_HPP
#define CPPR_MONITOR_CONSOLE_WATCH_HPP

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>

namespace monitor {

struct console_options {
    std::uint32_t pid {};
    int interval_ms { 1000 };
    int stale_ms { 3000 };
    bool once {};
};

enum class command_line_action {
    run,
    help,
    error,
};

struct parsed_command_line {
    command_line_action action { command_line_action::error };
    console_options options;
};

[[nodiscard]] parsed_command_line
parse_command_line(std::span<const std::string_view> arguments) noexcept;

void write_console_usage(std::ostream& output);

int run_console_watch(
    const console_options& options, const std::atomic_bool& stop_requested,
    std::ostream& output, std::ostream& error
);

} // namespace monitor

#endif // CPPR_MONITOR_CONSOLE_WATCH_HPP
