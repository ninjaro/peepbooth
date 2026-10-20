#include "monitor/process_discovery.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

namespace monitor {
namespace {
#if defined(__linux__) && !defined(__ANDROID__)
    std::optional<std::uint32_t> endpoint_pid(std::string_view text) noexcept {
        if (text.empty() || text.front() == '0')
            return std::nullopt;
        std::uint32_t pid {};
        const auto parsed
            = std::from_chars(text.data(), text.data() + text.size(), pid);
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()
            || pid == 0)
            return std::nullopt;
        return pid;
    }
#endif
} // namespace

process_discovery discover_processes() {
    process_discovery result;
#if defined(__linux__) && !defined(__ANDROID__)
    const auto prefix = "monitor-watchdog-"
        + std::to_string(platform::current_user_id()) + "-";
    std::error_code error;
    std::filesystem::directory_iterator entry("/dev/shm", error);
    const std::filesystem::directory_iterator end;
    if (error) {
        result.diagnostics.push_back(
            "cannot enumerate /dev/shm: " + error.message()
        );
        return result;
    }
    result.available = true;
    while (entry != end) {
        const auto name = entry->path().filename().string();
        if (name.starts_with(prefix)) {
            const auto pid
                = endpoint_pid(std::string_view(name).substr(prefix.size()));
            if (!pid) {
                result.diagnostics.push_back(
                    name + ": malformed endpoint name"
                );
            } else {
                std::string reason;
                if (auto process
                    = platform::inspect_process_mapping(*pid, &reason))
                    result.processes.push_back(std::move(*process));
                else {
                    auto diagnostic = name;
                    diagnostic.append(": ").append(reason);
                    result.diagnostics.push_back(std::move(diagnostic));
                }
            }
        }
        entry.increment(error);
        if (error) {
            result.available = false;
            result.diagnostics.push_back(
                "cannot finish /dev/shm enumeration: " + error.message()
            );
            break;
        }
    }
    std::ranges::sort(result.processes, {}, [](const auto& process) {
        return process.identity.pid;
    });
    std::ranges::sort(result.diagnostics);
#else
    result.diagnostics.emplace_back(
        "local endpoint discovery requires desktop Linux"
    );
#endif
    return result;
}

process_name_selection select_process_name(
    const process_discovery& inventory, const std::string_view name
) {
    if (!inventory.available || !inventory.diagnostics.empty())
        return { std::nullopt, "process discovery is incomplete" };
    if (name.empty())
        return { std::nullopt, "process name must not be empty" };
    process_name_selection result;
    for (const auto& process : inventory.processes) {
        if (process.name != name)
            continue;
        if (result.identity)
            return { std::nullopt,
                     "process name is ambiguous; select an exact identity" };
        result.identity = process.identity;
    }
    if (!result.identity)
        result.error = "process name was not found";
    return result;
}

} // namespace monitor
