#pragma once

#include "monitor/watchdog.hpp"

#include <cstddef>
#include <deque>

namespace monitor {

enum class observation_status {
    not_attached,
    sampled,
    sample_unavailable,
    identity_changed,
    exited,
};

struct process_observation {
    platform::process_identity identity;
    std::uint64_t observed_at_ns {};
    observation_status status { observation_status::not_attached };
    watchdog_snapshot watchdog;
    std::optional<platform::process_sample> sample;
    std::optional<double> cpu_percent;
    memory_trend_summary memory;
    std::optional<health> responsiveness;
    // Actual watcher policy, not a predicted operation duration. Absent when
    // no watchdog snapshot was acquired.
    std::optional<std::uint64_t> stale_after_ns;
};

struct progress_evidence {
    lifecycle process_lifecycle { lifecycle::starting };
    std::uint64_t observation_started_ns {};
    std::uint64_t stale_after_ns {};
    std::array<heartbeat_snapshot, heartbeat_channel_count> channels {};
};

// Compact watcher-side evidence. A gap is an explicit frontend disconnection,
// not a synthesized process sample. No breadcrumbs or producer history here.
struct observation_record {
    platform::process_identity identity;
    std::uint64_t observed_at_ns {};
    std::optional<observation_status> status;
    std::optional<platform::process_memory> memory;
    std::optional<double> cpu_percent;
    std::optional<health> responsiveness;
    std::optional<progress_evidence> progress;
    std::optional<std::uint64_t> sampled_at_ns;
    std::optional<memory_trend_summary> trend;
};

enum class incident_kind {
    overdue_progress,
    sampled_rss_growth,
    unavailable_evidence,
    process_exit,
};
enum class incident_state {
    open,
    resolved,
    interrupted,
    ended_unknown,
    observed
};
enum class incident_ending {
    none,
    condition_cleared,
    progress_observed,
    expectation_retired,
    expectation_replaced,
    observation_gap,
    observer_detached,
    reattached,
    process_exit,
    identity_changed,
    producer_stopped,
};

// First/last qualifying evidence and, when closed, the actual ending evidence.
// No severity, causal diagnosis, operation ID, persisted format or event
// stream.
struct diagnostic_incident {
    incident_kind kind {};
    std::optional<channel> progress_channel;
    incident_state state { incident_state::open };
    incident_ending ending { incident_ending::none };
    observation_record first;
    observation_record last;
    std::optional<observation_record> closing;
    std::uint64_t matching_observations { 1 };
};

class incident_tracker final {
public:
    // One active finding per fixed channel, plus RSS trend and uncertainty.
    // Up to capacity closed incidents; zero disables all tracking.
    incident_tracker(platform::process_identity identity, std::size_t capacity);
    bool add(const observation_record& record);
    [[nodiscard]] const std::array<
        std::optional<diagnostic_incident>, heartbeat_channel_count + 2>&
    active() const noexcept;
    [[nodiscard]] const std::deque<diagnostic_incident>&
    closed() const noexcept;
    [[nodiscard]] std::uint64_t overwritten() const noexcept;

private:
    void advance(
        std::size_t slot, incident_kind kind, const observation_record& record,
        std::optional<channel> progress_channel = {}
    );
    void close(
        std::size_t slot, incident_state state, incident_ending ending,
        const observation_record& record
    );
    void interrupt_progress(
        incident_ending ending, const observation_record& record
    );
    void retain(const diagnostic_incident& incident);
    platform::process_identity identity_;
    std::size_t capacity_;
    std::array<std::optional<diagnostic_incident>, heartbeat_channel_count + 2>
        active_;
    std::deque<diagnostic_incident> closed_;
    std::uint64_t overwritten_ {};
    std::optional<observation_record> previous_;
    std::optional<std::uint64_t> memory_since_;
    bool lifetime_ended_ {};
};

class observation_history final {
public:
    // Capacity is chosen by the frontend; zero disables retention. One exact
    // process incarnation per history. No clock, polling or gap inference.
    observation_history(
        platform::process_identity identity, std::size_t capacity
    );
    bool add(const process_observation& observation);
    bool note_gap(std::uint64_t observed_at_ns);
    [[nodiscard]] const std::deque<observation_record>&
    records() const noexcept;
    [[nodiscard]] std::uint64_t overwritten() const noexcept;
    [[nodiscard]] const incident_tracker& incidents() const noexcept;

private:
    bool append(observation_record record);
    platform::process_identity identity_;
    std::size_t capacity_;
    std::deque<observation_record> records_;
    std::uint64_t overwritten_ {};
    incident_tracker incidents_;
};

// One serially used watcher-side owner per process. No timer, output, history
// log or producer instrumentation lives here. Move transfers the watcher lease.
class observed_process final {
public:
    explicit observed_process(
        platform::process_handle process,
        std::uint64_t stale_after_ns = 3'000'000'000ULL
    );
    observed_process(const observed_process&) = delete;
    observed_process& operator=(const observed_process&) = delete;
    observed_process(observed_process&& other) noexcept;
    observed_process& operator=(observed_process&& other) noexcept;

    // Nonblocking; the frontend owns retry/cancellation policy.
    [[nodiscard]] bool try_attach();
    [[nodiscard]] bool alive() const;
    [[nodiscard]] bool wait_for_exit(int timeout_ms) const;
    // The returned reference lasts until the next poll or owner destruction.
    [[nodiscard]] const process_observation& poll();
    [[nodiscard]] const process_observation& latest() const noexcept;

private:
    friend struct observed_process_test_access;
    const process_observation& accept_sample(
        std::optional<platform::process_sample> sample,
        watchdog_snapshot snapshot, std::uint64_t observed_at_ns
    );

    platform::process_handle process_;
    std::optional<watchdog_session> session_;
    std::uint64_t stale_after_ns_;
    long clock_ticks_per_second_;
    std::optional<platform::process_sample> previous_;
    memory_trend memory_;
    process_observation latest_;
};

} // namespace monitor
