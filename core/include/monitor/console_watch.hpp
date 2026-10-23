#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "monitor/platform/process_watch.hpp"

namespace monitor {

struct process_observation;
struct process_discovery;
struct diagnostic_incident;
class observation_history;

struct recording_limits {
    std::uint64_t bytes {};
    std::uint64_t checkpoints {};
    std::size_t frame_bytes {};
};

struct recording_session {
    platform::process_identity target;
    platform::process_identity watcher;
    std::uint64_t started_ns {};
    std::uint64_t started_utc_ns {};
    int interval_ms {};
    std::uint64_t stale_after_ns {};
    std::size_t history_capacity {};
    std::optional<std::string> boot_id;
    std::optional<std::string> system;
    std::optional<std::string> release;
    std::optional<std::string> machine;
};

enum class recording_state { active, finalized, limited, failed };
enum class recording_outcome {
    process_exited,
    once,
    cancelled,
    identity_changed,
    observation_error
};

// Watcher-only, synchronous bounded checkpoints. No destructor finalization:
// unexpected termination leaves PATH.partial, never an invented terminal event.
class session_recorder final {
public:
    session_recorder(
        const std::string& path, const recording_session& session,
        recording_limits limits
    );
    ~session_recorder();
    session_recorder(const session_recorder&) = delete;
    session_recorder& operator=(const session_recorder&) = delete;
    bool append(const observation_history& history);
    bool finish(
        recording_outcome outcome, int observation_exit_status,
        std::uint64_t ended_ns
    );
    [[nodiscard]] recording_state state() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
    [[nodiscard]] std::uint64_t checkpoints() const noexcept;
    [[nodiscard]] std::uint64_t bytes() const noexcept;

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

struct console_options {
    std::uint32_t pid {};
    int interval_ms { 1000 };
    int stale_ms { 3000 };
    bool once {};
    bool all {};
    // Explicit opt-in, single PID only; retain the last N compact observations.
    std::size_t history_capacity {};
    std::string_view recording_path;
    recording_limits recording;
    // Borrowed from argv/caller; must outlive observation.
    std::string_view name;
};

enum class command_line_action {
    run,
    list,
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

int run_console_list(std::ostream& output, std::ostream& error);

// Formats only the supplied observation; performs no clock, process or IPC
// reads.
void write_console_observation(
    std::ostream& output, const process_observation& observation
);

// Descriptive evidence only; no new alert severity or exit-status policy.
void write_console_incident(
    std::ostream& output, const diagnostic_incident& incident
);

// Observes a fixed, explicitly selected cohort from a discovery snapshot.
// Main supplies real discovery; other frontends can reuse a validated
// inventory.
int run_console_watch_group(
    const console_options& options, const process_discovery& inventory,
    const std::atomic_bool& stop_requested, std::ostream& output,
    std::ostream& error
);

int run_console_watch(
    const console_options& options, const std::atomic_bool& stop_requested,
    std::ostream& output, std::ostream& error
);

} // namespace monitor
