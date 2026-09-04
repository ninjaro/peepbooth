#include "monitor/client.hpp"

#include "monitor/platform/shared_memory.hpp"

#include <array>
#include <chrono>
#include <mutex>
#include <utility>

namespace monitor {

class client::implementation final {
public:
    platform::shared_mapping mapping;
    std::mutex mutex;
    std::array<std::atomic<std::uint64_t>, heartbeat_channel_count>
        next_heartbeat_ns {};
};

client::client() {
    if constexpr (instrumentation_enabled) {
        implementation_ = std::make_unique<implementation>();
    }
}

client::~client() { stop(); }

bool client::start(const std::string_view process_name) {
    if (!instrumentation_enabled || implementation_ == nullptr) {
        static_cast<void>(process_name);
        return false;
    }
    std::lock_guard lock(implementation_->mutex);
    if (state_.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    try {
        auto mapping = platform::create_process_mapping(process_name);
        if (!mapping) {
            return false;
        }
        implementation_->mapping = std::move(*mapping);
        state_.store(implementation_->mapping.get(), std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

void client::stop() noexcept {
    if (implementation_ == nullptr) {
        return;
    }
    std::lock_guard lock(implementation_->mutex);
    shared_state* state = state_.exchange(nullptr, std::memory_order_acq_rel);
    if (state != nullptr) {
        state->process_lifecycle.store(
            static_cast<std::uint32_t>(lifecycle::clean_exit),
            std::memory_order_release
        );
    }
    implementation_->mapping = {};
}

bool client::available() const noexcept {
    return state_.load(std::memory_order_acquire) != nullptr;
}

shared_state* client::active_state(const std::uint64_t now_ns) noexcept {
    shared_state* state = state_.load(std::memory_order_acquire);
    if (state == nullptr
        || state->watcher_present.load(std::memory_order_relaxed) == 0) {
        return nullptr;
    }
    if (state->watcher_lease_until_ns.load(std::memory_order_acquire)
        < now_ns) {
        state->watcher_present.store(0, std::memory_order_release);
        return nullptr;
    }
    return state;
}

void client::heartbeat(const channel value) noexcept {
    shared_state* unchecked = state_.load(std::memory_order_acquire);
    if (unchecked == nullptr
        || unchecked->watcher_present.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const std::uint64_t now = platform::monotonic_time_ns();
    shared_state* state = active_state(now);
    const std::size_t index = channel_index(value);
    if (state == nullptr || index >= heartbeat_channel_count) {
        return;
    }
    heartbeat_slot& slot = state->heartbeats[index];
    constexpr auto minimum_interval = std::chrono::milliseconds(500);
    constexpr std::uint64_t minimum_interval_ns
        = std::chrono::duration_cast<std::chrono::nanoseconds>(minimum_interval)
              .count();
    auto& next_heartbeat = implementation_->next_heartbeat_ns[index];
    std::uint64_t next = next_heartbeat.load(std::memory_order_relaxed);
    for (;;) {
        if (now < next) {
            return;
        }
        if (next_heartbeat.compare_exchange_weak(
                next, now + minimum_interval_ns, std::memory_order_relaxed,
                std::memory_order_relaxed
            )) {
            break;
        }
    }
    slot.active.store(1, std::memory_order_relaxed);
    slot.timestamp_ns.store(now, std::memory_order_release);
    slot.sequence.fetch_add(1, std::memory_order_release);
}

void client::set_channel_active(
    const channel value, const bool active
) noexcept {
    shared_state* state = state_.load(std::memory_order_acquire);
    const std::size_t index = channel_index(value);
    if (state == nullptr || index >= heartbeat_channel_count) {
        return;
    }
    state->heartbeats[index].active.store(
        active ? 1U : 0U, std::memory_order_release
    );
    if (active) {
        heartbeat(value);
    }
}

void client::breadcrumb(
    const event value, const std::uint32_t source_id, const std::uint64_t arg0,
    const std::uint64_t arg1, const std::uint16_t flags
) noexcept {
    breadcrumb(static_cast<std::uint16_t>(value), source_id, arg0, arg1, flags);
}

void client::breadcrumb(
    const std::uint16_t event_id, const std::uint32_t source_id,
    const std::uint64_t arg0, const std::uint64_t arg1,
    const std::uint16_t flags
) noexcept {
    shared_state* unchecked = state_.load(std::memory_order_acquire);
    if (unchecked == nullptr
        || unchecked->watcher_present.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const std::uint64_t now = platform::monotonic_time_ns();
    shared_state* state = active_state(now);
    if (state == nullptr) {
        return;
    }

    const std::uint64_t ticket
        = state->breadcrumb_ticket.fetch_add(1, std::memory_order_relaxed) + 1;
    breadcrumb_slot& slot
        = state->breadcrumbs[(ticket - 1) % breadcrumb_capacity];
    // The odd value closes the slot to readers before any payload field is
    // changed.  Sequential consistency is deliberate here: breadcrumbs are
    // infrequent and the stronger ordering keeps this inter-process seqlock
    // straightforward on architectures weaker than x86.
    slot.commit.store((ticket << 1U) | 1U, std::memory_order_seq_cst);
    slot.ticket.store(ticket, std::memory_order_relaxed);
    slot.timestamp_ns.store(now, std::memory_order_relaxed);
    slot.arg0.store(arg0, std::memory_order_relaxed);
    slot.arg1.store(arg1, std::memory_order_relaxed);
    slot.source_id.store(source_id, std::memory_order_relaxed);
    slot.event_and_flags.store(
        static_cast<std::uint32_t>(event_id)
            | (static_cast<std::uint32_t>(flags) << 16U),
        std::memory_order_relaxed
    );
    slot.commit.store(ticket << 1U, std::memory_order_seq_cst);
}

client& client::process() noexcept {
    static client value;
    return value;
}

} // namespace monitor
