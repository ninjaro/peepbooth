#include "monitor/observed_process.hpp"

#include <utility>

namespace monitor {

observed_process::observed_process(
    platform::process_handle process, const std::uint64_t stale_after_ns
)
    : process_(std::move(process))
    , stale_after_ns_(stale_after_ns)
    , clock_ticks_per_second_(platform::clock_ticks_per_second()) {
    latest_.identity = process_.identity();
}

bool observed_process::try_attach() {
    if (session_ && session_->valid()) {
        return true;
    }
    if (!alive()) {
        return false;
    }
    session_ = watchdog_session::attach(process_.identity());
    return session_.has_value();
}

bool observed_process::alive() const { return process_.alive(); }

bool observed_process::wait_for_exit(const int timeout_ms) const {
    return process_.wait_for_exit(timeout_ms);
}

const process_observation& observed_process::latest() const noexcept {
    return latest_;
}

const process_observation& observed_process::poll() {
    if (!session_ || !session_->valid()) {
        latest_ = {};
        latest_.identity = process_.identity();
        latest_.observed_at_ns = platform::monotonic_time_ns();
        return latest_;
    }
    // Capture raw data before fixing the observation timestamp. Presentation
    // uses this same instant even if formatting is delayed.
    auto sample = platform::sample_process(process_.identity().pid);
    auto snapshot = session_->snapshot();
    const bool is_alive = alive();
    const auto now = platform::monotonic_time_ns();
    if (!is_alive) {
        accept_sample(std::nullopt, std::move(snapshot), now);
        latest_.status = observation_status::exited;
        return latest_;
    }
    session_->renew_lease(now);
    return accept_sample(std::move(sample), std::move(snapshot), now);
}

const process_observation& observed_process::accept_sample(
    std::optional<platform::process_sample> sample, watchdog_snapshot snapshot,
    const std::uint64_t observed_at_ns
) {
    latest_ = {};
    latest_.identity = process_.identity();
    latest_.observed_at_ns = observed_at_ns;
    latest_.watchdog = std::move(snapshot);
    latest_.memory = memory_.summary();
    if (!sample) {
        latest_.status = observation_status::sample_unavailable;
        previous_.reset();
        return latest_;
    }
    if (sample->identity.pid != latest_.identity.pid
        || sample->identity.start_ticks != latest_.identity.start_ticks) {
        latest_.status = observation_status::identity_changed;
        previous_.reset();
        return latest_;
    }
    if (previous_ && sample->sampled_at_ns > previous_->sampled_at_ns
        && sample->cpu_ticks >= previous_->cpu_ticks) {
        const double elapsed
            = static_cast<double>(
                  sample->sampled_at_ns - previous_->sampled_at_ns
              )
            / 1e9;
        latest_.cpu_percent
            = static_cast<double>(sample->cpu_ticks - previous_->cpu_ticks)
            * 100.0 / (static_cast<double>(clock_ticks_per_second_) * elapsed);
    }
    memory_.add(sample->sampled_at_ns, sample->memory.rss_bytes);
    latest_.status = observation_status::sampled;
    latest_.sample = sample;
    latest_.memory = memory_.summary();
    latest_.responsiveness = classify_health(
        latest_.watchdog, observed_at_ns, stale_after_ns_,
        latest_.cpu_percent.value_or(0.0)
    );
    previous_ = std::move(sample);
    return latest_;
}

} // namespace monitor
