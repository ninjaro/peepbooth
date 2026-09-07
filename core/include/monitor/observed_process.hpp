#ifndef CPPR_MONITOR_OBSERVED_PROCESS_HPP
#define CPPR_MONITOR_OBSERVED_PROCESS_HPP

#include "monitor/watchdog.hpp"

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
    observed_process(observed_process&&) = default;
    observed_process& operator=(observed_process&&) = default;

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

#endif // CPPR_MONITOR_OBSERVED_PROCESS_HPP
