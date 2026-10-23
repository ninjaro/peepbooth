#include "monitor/observed_process.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace monitor {
namespace {
    constexpr std::size_t rss_slot = heartbeat_channel_count;
    constexpr std::size_t uncertainty_slot = heartbeat_channel_count + 1;
}

incident_tracker::incident_tracker(
    const platform::process_identity identity, const std::size_t capacity
)
    : identity_(identity)
    , capacity_(capacity) { }

void incident_tracker::retain(const diagnostic_incident& incident) {
    if (closed_.size() == capacity_) {
        closed_.pop_front();
        if (overwritten_ != std::numeric_limits<std::uint64_t>::max())
            ++overwritten_;
    }
    closed_.push_back(incident);
}

void incident_tracker::advance(
    const std::size_t slot, const incident_kind kind,
    const observation_record& record,
    const std::optional<channel> progress_channel
) {
    auto& incident = active_[slot];
    if (!incident) {
        incident = diagnostic_incident { kind,
                                         progress_channel,
                                         incident_state::open,
                                         incident_ending::none,
                                         record,
                                         record,
                                         {},
                                         1 };
    } else {
        incident->last = record;
        if (incident->matching_observations
            != std::numeric_limits<std::uint64_t>::max())
            ++incident->matching_observations;
    }
}

void incident_tracker::close(
    const std::size_t slot, const incident_state state,
    const incident_ending ending, const observation_record& record
) {
    auto& incident = active_[slot];
    if (!incident)
        return;
    incident->state = state;
    incident->ending = ending;
    incident->closing = record;
    retain(*incident);
    incident.reset();
}

void incident_tracker::interrupt_progress(
    const incident_ending ending, const observation_record& record
) {
    for (std::size_t slot = 0; slot < heartbeat_channel_count; ++slot)
        close(
            slot,
            ending == incident_ending::producer_stopped
                ? incident_state::interrupted
                : incident_state::ended_unknown,
            ending, record
        );
}

bool incident_tracker::add(const observation_record& record) {
    if (capacity_ == 0 || lifetime_ended_
        || record.identity.pid != identity_.pid
        || record.identity.start_ticks != identity_.start_ticks
        || (previous_ && record.observed_at_ns < previous_->observed_at_ns))
        return false;
    if (previous_ && record.observed_at_ns == previous_->observed_at_ns
        && record.status == previous_->status)
        return false;

    if (!record.status) {
        interrupt_progress(incident_ending::observer_detached, record);
        close(
            rss_slot, incident_state::ended_unknown,
            incident_ending::observer_detached, record
        );
        close(
            uncertainty_slot, incident_state::ended_unknown,
            incident_ending::observer_detached, record
        );
        memory_since_.reset();
    } else if (
        *record.status == observation_status::exited
        || *record.status == observation_status::identity_changed
    ) {
        const bool exited = *record.status == observation_status::exited;
        const auto ending = exited ? incident_ending::process_exit
                                   : incident_ending::identity_changed;
        interrupt_progress(ending, record);
        close(rss_slot, incident_state::ended_unknown, ending, record);
        if (!exited)
            advance(
                uncertainty_slot, incident_kind::unavailable_evidence, record
            );
        close(uncertainty_slot, incident_state::ended_unknown, ending, record);
        if (exited) {
            retain(
                { incident_kind::process_exit,
                  {},
                  incident_state::observed,
                  ending,
                  record,
                  record,
                  record,
                  1 }
            );
        }
        lifetime_ended_ = true;
    } else if (
        *record.status != observation_status::sampled || !record.memory
        || !record.sampled_at_ns
    ) {
        interrupt_progress(incident_ending::observation_gap, record);
        close(
            rss_slot, incident_state::ended_unknown,
            incident_ending::observation_gap, record
        );
        advance(uncertainty_slot, incident_kind::unavailable_evidence, record);
        memory_since_.reset();
    } else {
        const auto& progress = record.progress;
        const bool reattached = previous_ && previous_->progress && progress
            && previous_->progress->observation_started_ns
                != progress->observation_started_ns;
        if (reattached) {
            interrupt_progress(incident_ending::reattached, record);
            close(
                rss_slot, incident_state::ended_unknown,
                incident_ending::reattached, record
            );
            close(
                uncertainty_slot, incident_state::ended_unknown,
                incident_ending::reattached, record
            );
            memory_since_.reset();
        }
        const bool coherent
            = progress
            && std::ranges::all_of(
                  progress->channels, [&](const auto& heartbeat) {
                      return heartbeat.coherent
                          && (!heartbeat.active
                              || (heartbeat.expected_since_ns != 0
                                  && progress->observation_started_ns != 0));
                  }
            );
        if (!coherent) {
            interrupt_progress(incident_ending::observation_gap, record);
            if (progress)
                advance(
                    uncertainty_slot, incident_kind::unavailable_evidence,
                    record
                );
            else
                close(
                    uncertainty_slot, incident_state::ended_unknown,
                    incident_ending::observation_gap, record
                );
        } else {
            close(
                uncertainty_slot, incident_state::resolved,
                incident_ending::condition_cleared, record
            );
            const bool stopping
                = progress->process_lifecycle == lifecycle::stopping
                || progress->process_lifecycle == lifecycle::clean_exit;
            if (stopping) {
                interrupt_progress(incident_ending::producer_stopped, record);
            } else {
                for (std::size_t slot = 0; slot < heartbeat_channel_count;
                     ++slot) {
                    const auto& heartbeat = progress->channels[slot];
                    const auto condition = assess_progress(
                        heartbeat, progress->observation_started_ns,
                        record.observed_at_ns, progress->stale_after_ns
                    );
                    if (active_[slot]) {
                        if (!heartbeat.active)
                            close(
                                slot, incident_state::interrupted,
                                incident_ending::expectation_retired, record
                            );
                        else if (
                            active_[slot]
                                ->last.progress->channels[slot]
                                .expected_since_ns
                            != heartbeat.expected_since_ns
                        )
                            close(
                                slot, incident_state::interrupted,
                                incident_ending::expectation_replaced, record
                            );
                        else if (
                            condition == progress_condition::overdue
                            && heartbeat.sequence
                                > active_[slot]
                                      ->last.progress->channels[slot]
                                      .sequence
                            && heartbeat.timestamp_ns
                                > active_[slot]
                                      ->last.progress->channels[slot]
                                      .timestamp_ns
                        )
                            close(
                                slot, incident_state::resolved,
                                incident_ending::progress_observed, record
                            );
                    }
                    if (condition == progress_condition::overdue)
                        advance(
                            slot, incident_kind::overdue_progress, record,
                            heartbeat.id
                        );
                    else
                        close(
                            slot, incident_state::resolved,
                            incident_ending::condition_cleared, record
                        );
                }
            }
        }
        if (!memory_since_)
            memory_since_ = record.sampled_at_ns;
        const bool supported_window = record.trend
            && *record.sampled_at_ns >= record.trend->duration_ns
            && *record.sampled_at_ns - record.trend->duration_ns
                >= *memory_since_;
        if (!supported_window)
            close(
                rss_slot, incident_state::ended_unknown,
                incident_ending::observation_gap, record
            );
        else if (record.trend->sustained_growth)
            advance(rss_slot, incident_kind::sampled_rss_growth, record);
        else
            close(
                rss_slot, incident_state::resolved,
                incident_ending::condition_cleared, record
            );
    }
    previous_ = record;
    return true;
}

const std::array<
    std::optional<diagnostic_incident>, heartbeat_channel_count + 2>&
incident_tracker::active() const noexcept {
    return active_;
}

const std::deque<diagnostic_incident>&
incident_tracker::closed() const noexcept {
    return closed_;
}

std::uint64_t incident_tracker::overwritten() const noexcept {
    return overwritten_;
}

observation_history::observation_history(
    const platform::process_identity identity, const std::size_t capacity
)
    : identity_(identity)
    , capacity_(capacity)
    , incidents_(identity, capacity) { }

bool observation_history::add(const process_observation& observation) {
    if (capacity_ == 0 || observation.identity.pid != identity_.pid
        || observation.identity.start_ticks != identity_.start_ticks)
        return false;
    observation_record record;
    record.identity = identity_;
    record.observed_at_ns = observation.observed_at_ns;
    record.status = observation.status;
    if (observation.stale_after_ns) {
        record.progress
            = progress_evidence { observation.watchdog.process_lifecycle,
                                  observation.watchdog.observation_started_ns,
                                  *observation.stale_after_ns,
                                  observation.watchdog.heartbeats };
    }
    if (observation.status == observation_status::sampled && observation.sample
        && observation.sample->identity.pid == identity_.pid
        && observation.sample->identity.start_ticks == identity_.start_ticks) {
        record.memory = observation.sample->memory;
        record.cpu_percent = observation.cpu_percent;
        record.responsiveness = observation.responsiveness;
        record.sampled_at_ns = observation.sample->sampled_at_ns;
        record.trend = observation.memory;
    }
    return append(record);
}

bool observation_history::note_gap(const std::uint64_t observed_at_ns) {
    if (capacity_ == 0)
        return false;
    observation_record gap;
    gap.identity = identity_;
    gap.observed_at_ns = observed_at_ns;
    return append(gap);
}

bool observation_history::append(observation_record record) {
    if (capacity_ == 0
        || (!records_.empty()
            && record.observed_at_ns < records_.back().observed_at_ns))
        return false;
    if (records_.size() == capacity_) {
        records_.pop_front();
        if (overwritten_ != std::numeric_limits<std::uint64_t>::max())
            ++overwritten_;
    }
    records_.push_back(record);
    incidents_.add(record);
    return true;
}

const std::deque<observation_record>&
observation_history::records() const noexcept {
    return records_;
}

std::uint64_t observation_history::overwritten() const noexcept {
    return overwritten_;
}

const incident_tracker& observation_history::incidents() const noexcept {
    return incidents_;
}

observed_process::observed_process(
    platform::process_handle process, const std::uint64_t stale_after_ns
)
    : process_(std::move(process))
    , stale_after_ns_(stale_after_ns)
    , clock_ticks_per_second_(platform::clock_ticks_per_second()) {
    latest_.identity = process_.identity();
}

observed_process::observed_process(observed_process&& other) noexcept
    : process_(std::move(other.process_))
    , session_(std::exchange(other.session_, std::nullopt))
    , stale_after_ns_(other.stale_after_ns_)
    , clock_ticks_per_second_(other.clock_ticks_per_second_)
    , previous_(std::exchange(other.previous_, std::nullopt))
    , memory_(std::move(other.memory_))
    , latest_(std::exchange(other.latest_, {})) { }

observed_process&
observed_process::operator=(observed_process&& other) noexcept {
    if (this != &other) {
        session_ = std::exchange(other.session_, std::nullopt);
        process_ = std::move(other.process_);
        stale_after_ns_ = other.stale_after_ns_;
        clock_ticks_per_second_ = other.clock_ticks_per_second_;
        previous_ = std::exchange(other.previous_, std::nullopt);
        memory_ = std::move(other.memory_);
        latest_ = std::exchange(other.latest_, {});
    }
    return *this;
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
    const bool is_alive = alive();
    if (is_alive)
        session_->renew_lease(platform::monotonic_time_ns());
    auto snapshot = session_->snapshot();
    const auto now = platform::monotonic_time_ns();
    if (!is_alive) {
        accept_sample(std::nullopt, std::move(snapshot), now);
        latest_.status = observation_status::exited;
        return latest_;
    }
    return accept_sample(sample, std::move(snapshot), now);
}

const process_observation& observed_process::accept_sample(
    std::optional<platform::process_sample> sample, watchdog_snapshot snapshot,
    const std::uint64_t observed_at_ns
) {
    latest_ = {};
    latest_.identity = process_.identity();
    latest_.observed_at_ns = observed_at_ns;
    latest_.watchdog = std::move(snapshot);
    latest_.stale_after_ns = stale_after_ns_;
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
    previous_ = sample;
    return latest_;
}

} // namespace monitor
