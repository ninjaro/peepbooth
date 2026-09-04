#include "monitor/console_watch.hpp"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

std::atomic_bool stop_requested { false };

void handle_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }

    const auto parsed = monitor::parse_command_line(arguments);
    if (parsed.action == monitor::command_line_action::help) {
        monitor::write_console_usage(std::cout);
        return 0;
    }
    if (parsed.action == monitor::command_line_action::error) {
        monitor::write_console_usage(std::cerr);
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    return monitor::run_console_watch(
        parsed.options, stop_requested, std::cout, std::cerr
    );
}
