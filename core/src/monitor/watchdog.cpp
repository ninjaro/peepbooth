#include "monitor/watchdog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace monitor {
namespace {

    bool valid_state(
        const shared_state& state, const platform::process_identity identity
    ) noexcept {
        return state.magic == shared_state_magic
            && state.version == shared_state_version
            && state.header_size == offsetof(shared_state, heartbeats)
            && state.total_size == sizeof(shared_state)
            && state.pid == identity.pid
            && state.uid == platform::current_user_id()
            && state.process_start_ticks == identity.start_ticks;
    }

    heartbeat_snapshot
    read_heartbeat(const heartbeat_slot& slot, const channel id) noexcept {
        heartbeat_snapshot result;
        result.id = id;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = slot.sequence.load(std::memory_order_acquire);
            result.active = slot.active.load(std::memory_order_acquire) != 0;
            result.timestamp_ns
                = slot.timestamp_ns.load(std::memory_order_acquire);
            const auto after = slot.sequence.load(std::memory_order_acquire);
            result.sequence = after;
            if (before == after) {
                break;
            }
        }
        return result;
    }

    std::optional<breadcrumb> read_breadcrumb(
        const breadcrumb_slot& slot, const std::uint64_t ticket
    ) noexcept {
        const std::uint64_t expected = ticket << 1U;
        const auto before = slot.commit.load(std::memory_order_seq_cst);
        if (before != expected
            || slot.ticket.load(std::memory_order_relaxed) != ticket) {
            return std::nullopt;
        }
        const auto packed
            = slot.event_and_flags.load(std::memory_order_relaxed);
        breadcrumb result {
            .timestamp_ns = slot.timestamp_ns.load(std::memory_order_relaxed),
            .event_id = static_cast<std::uint16_t>(packed & 0xffffU),
            .flags = static_cast<std::uint16_t>(packed >> 16U),
            .source_id = slot.source_id.load(std::memory_order_relaxed),
            .arg0 = slot.arg0.load(std::memory_order_relaxed),
            .arg1 = slot.arg1.load(std::memory_order_relaxed),
        };
        return slot.commit.load(std::memory_order_seq_cst) == expected
            ? std::optional(result)
            : std::nullopt;
    }

} // namespace

watchdog_session::~watchdog_session() { release(); }

watchdog_session::watchdog_session(watchdog_session&& other) noexcept
    : mapping_(std::move(other.mapping_)) { }

watchdog_session&
watchdog_session::operator=(watchdog_session&& other) noexcept {
    if (this != &other) {
        release();
        mapping_ = std::move(other.mapping_);
    }
    return *this;
}

std::optional<watchdog_session>
watchdog_session::attach(const platform::process_identity identity) {
    auto mapping = platform::open_process_mapping(identity.pid);
    if (!mapping || !valid_state(*mapping->get(), identity)) {
        return std::nullopt;
    }
    watchdog_session result;
    result.mapping_ = std::move(*mapping);
    result.renew_lease(platform::monotonic_time_ns());
    return result;
}

bool watchdog_session::valid() const noexcept {
    return static_cast<bool>(mapping_);
}

watchdog_snapshot watchdog_session::snapshot() const {
    watchdog_snapshot result;
    const shared_state* state = mapping_.get();
    if (state == nullptr) {
        return result;
    }
    const auto name_end = std::find(
        state->process_name.begin(), state->process_name.end(), '\0'
    );
    result.process_name.assign(state->process_name.begin(), name_end);
    result.process_lifecycle = static_cast<lifecycle>(
        state->process_lifecycle.load(std::memory_order_acquire)
    );
    for (std::size_t index = 0; index < result.heartbeats.size(); ++index) {
        result.heartbeats[index] = read_heartbeat(
            state->heartbeats[index],
            static_cast<channel>(static_cast<std::uint8_t>(index + 1U))
        );
    }

    const auto newest
        = state->breadcrumb_ticket.load(std::memory_order_acquire);
    const auto oldest = newest > breadcrumb_capacity
        ? newest - breadcrumb_capacity + 1U
        : std::uint64_t { 1 };
    result.breadcrumbs.reserve(
        static_cast<std::size_t>(newest >= oldest ? newest - oldest + 1U : 0)
    );
    for (std::uint64_t ticket = oldest; ticket <= newest && ticket != 0;
         ++ticket) {
        const auto& slot
            = state->breadcrumbs[(ticket - 1U) % breadcrumb_capacity];
        if (auto item = read_breadcrumb(slot, ticket)) {
            result.breadcrumbs.push_back(*item);
        }
    }
    return result;
}

void watchdog_session::renew_lease(
    const std::uint64_t now_ns, const std::uint64_t duration_ns
) noexcept {
    shared_state* state = mapping_.get();
    if (state == nullptr) {
        return;
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    state->watcher_lease_until_ns.store(
        duration_ns > maximum - now_ns ? maximum : now_ns + duration_ns,
        std::memory_order_release
    );
    state->watcher_present.store(1, std::memory_order_release);
}

void watchdog_session::release() noexcept {
    if (shared_state* state = mapping_.get()) {
        state->watcher_present.store(0, std::memory_order_release);
        state->watcher_lease_until_ns.store(0, std::memory_order_release);
    }
    mapping_ = {};
}

memory_trend::memory_trend(
    const std::size_t capacity, const std::uint64_t window_ns
)
    : capacity_(std::max<std::size_t>(capacity, 2))
    , window_ns_(window_ns) { }

void memory_trend::add(
    const std::uint64_t timestamp_ns, const std::uint64_t rss_bytes
) {
    if (!points_.empty() && timestamp_ns <= points_.back().timestamp_ns) {
        return;
    }
    points_.push_back({ timestamp_ns, rss_bytes });
    while (points_.size() > capacity_
           || (points_.size() > 2
               && timestamp_ns - points_.front().timestamp_ns > window_ns_)) {
        points_.pop_front();
    }
}

memory_trend_summary memory_trend::summary() const noexcept {
    memory_trend_summary result;
    if (points_.size() < 2) {
        return result;
    }
    result.duration_ns
        = points_.back().timestamp_ns - points_.front().timestamp_ns;
    result.rss_delta_bytes
        = points_.back().rss_bytes >= points_.front().rss_bytes
        ? static_cast<std::int64_t>(
              points_.back().rss_bytes - points_.front().rss_bytes
          )
        : -static_cast<std::int64_t>(
              points_.front().rss_bytes - points_.back().rss_bytes
          );
    if (result.duration_ns == 0) {
        return result;
    }

    // Least-squares slope describes the complete window rather than a single
    // high-water threshold.  Requiring mostly nondecreasing samples, repeated
    // positive steps in both halves of a minimum observation period, avoids
    // treating startup allocations followed by a plateau as sustained growth.
    const double origin = static_cast<double>(points_.front().timestamp_ns);
    double sum_x = 0;
    double sum_y = 0;
    for (const auto& point : points_) {
        sum_x += (static_cast<double>(point.timestamp_ns) - origin) / 1e9;
        sum_y += static_cast<double>(point.rss_bytes);
    }
    const double count = static_cast<double>(points_.size());
    const double mean_x = sum_x / count;
    const double mean_y = sum_y / count;
    double numerator = 0;
    double denominator = 0;
    std::size_t nondecreasing = 0;
    std::size_t increasing = 0;
    for (std::size_t index = 0; index < points_.size(); ++index) {
        const double x
            = (static_cast<double>(points_[index].timestamp_ns) - origin) / 1e9;
        const double y = static_cast<double>(points_[index].rss_bytes);
        numerator += (x - mean_x) * (y - mean_y);
        denominator += (x - mean_x) * (x - mean_x);
        if (index != 0
            && points_[index].rss_bytes >= points_[index - 1].rss_bytes) {
            ++nondecreasing;
        }
        if (index != 0
            && points_[index].rss_bytes > points_[index - 1].rss_bytes) {
            ++increasing;
        }
    }
    const std::size_t intervals = points_.size() - 1;
    const std::size_t midpoint = points_.size() / 2;
    constexpr std::uint64_t minimum_observation_ns = 5'000'000'000ULL;
    const std::uint64_t required_observation_ns
        = std::min(window_ns_, minimum_observation_ns);
    result.bytes_per_second = denominator > 0 ? numerator / denominator : 0;
    result.sustained_growth = points_.size() >= 4
        && result.duration_ns >= required_observation_ns
        && result.rss_delta_bytes > 0 && result.bytes_per_second > 0
        && nondecreasing * 3 >= intervals * 2 && increasing >= 2
        && increasing * 5 >= intervals
        && points_[midpoint].rss_bytes > points_.front().rss_bytes
        && points_.back().rss_bytes > points_[midpoint].rss_bytes;
    return result;
}

std::size_t memory_trend::size() const noexcept { return points_.size(); }

health classify_health(
    const watchdog_snapshot& snapshot, const std::uint64_t now_ns,
    const std::uint64_t stale_after_ns, const double cpu_percent
) noexcept {
    bool any_active = false;
    bool any_current = false;
    bool gui_active = false;
    bool gui_stale = false;
    bool non_gui_current = false;
    for (const auto& heartbeat : snapshot.heartbeats) {
        if (!heartbeat.active) {
            continue;
        }
        any_active = true;
        const bool stale = heartbeat.timestamp_ns == 0
            || (now_ns >= heartbeat.timestamp_ns
                && now_ns - heartbeat.timestamp_ns > stale_after_ns);
        any_current = any_current || !stale;
        if (heartbeat.id == channel::gui) {
            gui_active = true;
            gui_stale = stale;
        } else {
            non_gui_current = non_gui_current || !stale;
        }
    }
    if (!any_active) {
        return health::awaiting_heartbeat;
    }
    if (gui_active && gui_stale && non_gui_current) {
        return health::gui_stalled;
    }
    if (!any_current) {
        return cpu_percent >= 20.0 ? health::globally_stalled_busy
                                   : health::globally_stalled_idle;
    }
    return health::healthy;
}

const char* health_name(const health value) noexcept {
    switch (value) {
    case health::awaiting_heartbeat:
        return "AWAITING_HEARTBEAT";
    case health::healthy:
        return "HEALTHY";
    case health::gui_stalled:
        return "GUI_STALLED";
    case health::globally_stalled_idle:
        return "STALLED_IDLE";
    case health::globally_stalled_busy:
        return "STALLED_BUSY";
    }
    return "UNKNOWN";
}

const char* channel_name(const channel value) noexcept {
    switch (value) {
    case channel::main:
        return "main";
    case channel::gui:
        return "gui";
    case channel::core:
        return "core";
    case channel::worker:
        return "worker";
    }
    return "unknown";
}

const char* event_name(const std::uint16_t value) noexcept {
    switch (static_cast<event>(value)) {
    case event::configuration_loaded:
        return "configuration.loaded";
    case event::output_ready:
        return "output.ready";
    case event::stream_started:
        return "stream.started";
    case event::stream_stopped:
        return "stream.stopped";
    }
    return "unknown";
}

} // namespace monitor
