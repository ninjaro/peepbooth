#include "monitor/client.hpp"
#include "breadcrumb_ring.hpp"

#include "monitor/platform/shared_memory.hpp"

#include <array>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

namespace monitor {
namespace {
    template <typename Change>
    void update_slot(heartbeat_slot& slot, Change change) noexcept {
        slot.revision.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_release);
        change();
        slot.revision.fetch_add(1, std::memory_order_seq_cst);
    }
}

class client::implementation final {
public:
    struct channel_state {
        std::mutex mutex;
        std::atomic<std::uint64_t> owner { 0 };
        std::atomic<std::uint64_t> next_heartbeat_ns { 0 };
    };

    platform::shared_mapping mapping;
    std::mutex mutex;
    std::atomic<shared_state*> state { nullptr };
    std::array<channel_state, heartbeat_channel_count> channels;
    std::uint64_t next_owner { 1 };

    [[nodiscard]] shared_state*
    active_state(std::uint64_t now_ns) const noexcept;
    void heartbeat(channel value, std::uint64_t identity) noexcept;
    void retire(channel value, std::uint64_t identity) noexcept;
};

client::progress_owner::progress_owner(
    const std::shared_ptr<implementation>& owner, const channel value,
    const std::uint64_t identity
) noexcept
    : owner_(owner)
    , channel_(value)
    , identity_(identity) { }

client::progress_owner::~progress_owner() { reset(); }

client::progress_owner::progress_owner(progress_owner&& other) noexcept
    : owner_(std::move(other.owner_))
    , channel_(other.channel_)
    , identity_(other.identity_.exchange(0, std::memory_order_acq_rel)) { }

client::progress_owner&
client::progress_owner::operator=(progress_owner&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::move(other.owner_);
        channel_ = other.channel_;
        identity_.store(
            other.identity_.exchange(0, std::memory_order_acq_rel),
            std::memory_order_release
        );
    }
    return *this;
}

bool client::progress_owner::valid() const noexcept {
    const auto identity = identity_.load(std::memory_order_acquire);
    const auto owner = owner_.lock();
    return identity != 0 && owner
        && owner->state.load(std::memory_order_acquire)
        && owner->channels[channel_index(channel_)].owner.load(
               std::memory_order_acquire
           )
        == identity;
}

void client::progress_owner::publish_heartbeat() noexcept {
    const auto identity = identity_.load(std::memory_order_acquire);
    if (identity != 0)
        if (const auto owner = owner_.lock())
            owner->heartbeat(channel_, identity);
}

void client::progress_owner::reset() noexcept {
    const auto identity = identity_.load(std::memory_order_acquire);
    if (identity != 0)
        if (const auto owner = owner_.lock())
            owner->retire(channel_, identity);
    // Publish invalidity after retirement has serialized with any in-flight
    // writer, so another concurrent reset cannot return before that barrier.
    identity_.store(0, std::memory_order_release);
}

client::client() {
    if constexpr (instrumentation_enabled) {
        implementation_ = std::make_shared<implementation>();
    }
}

client::~client() { stop(); }

bool client::start(const std::string_view process_name) {
    if (!instrumentation_enabled || implementation_ == nullptr) {
        static_cast<void>(process_name);
        return false;
    }
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->state.load(std::memory_order_acquire) != nullptr) {
        return true;
    }
    try {
        auto mapping = platform::create_process_mapping(process_name);
        if (!mapping) {
            return false;
        }
        implementation_->mapping = std::move(*mapping);
        for (auto& state : implementation_->channels) {
            state.next_heartbeat_ns.store(0, std::memory_order_relaxed);
            state.owner.store(0, std::memory_order_relaxed);
        }
        implementation_->state.store(
            implementation_->mapping.get(), std::memory_order_release
        );
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
    std::array<std::unique_lock<std::mutex>, heartbeat_channel_count> locks;
    for (std::size_t index = 0; index < locks.size(); ++index)
        locks[index] = std::unique_lock(implementation_->channels[index].mutex);
    shared_state* state
        = implementation_->state.exchange(nullptr, std::memory_order_acq_rel);
    if (state != nullptr) {
        for (std::size_t index = 0; index < heartbeat_channel_count; ++index) {
            implementation_->channels[index].owner.store(
                0, std::memory_order_release
            );
            auto& slot = state->heartbeats[index];
            update_slot(slot, [&] {
                slot.active.store(0, std::memory_order_relaxed);
                slot.expected_since_ns.store(0, std::memory_order_relaxed);
            });
        }
        state->watcher_present.store(0, std::memory_order_release);
        state->watcher_lease_until_ns.store(0, std::memory_order_release);
        state->process_lifecycle.store(
            static_cast<std::uint32_t>(lifecycle::clean_exit),
            std::memory_order_release
        );
    }
    implementation_->mapping = {};
}

bool client::available() const noexcept {
    return implementation_
        && implementation_->state.load(std::memory_order_acquire) != nullptr;
}

shared_state* client::implementation::active_state(
    const std::uint64_t now_ns
) const noexcept {
    shared_state* current = state.load(std::memory_order_acquire);
    if (current == nullptr
        || current->watcher_present.load(std::memory_order_relaxed) == 0) {
        return nullptr;
    }
    if (current->watcher_lease_until_ns.load(std::memory_order_acquire)
        < now_ns) {
        current->watcher_present.store(0, std::memory_order_release);
        return nullptr;
    }
    return current;
}

void client::heartbeat(const channel value) noexcept {
    if (implementation_)
        implementation_->heartbeat(value, 0);
}

void client::implementation::heartbeat(
    const channel value, const std::uint64_t identity
) noexcept {
    const auto index = channel_index(value);
    if (index >= heartbeat_channel_count
        || channels[index].owner.load(std::memory_order_acquire) != identity)
        return;
    shared_state* unchecked = state.load(std::memory_order_acquire);
    if (unchecked == nullptr
        || unchecked->watcher_present.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const std::uint64_t now = platform::monotonic_time_ns();
    shared_state* current = active_state(now);
    if (current == nullptr) {
        return;
    }
    constexpr auto minimum_interval = std::chrono::milliseconds(500);
    constexpr std::uint64_t minimum_interval_ns
        = std::chrono::duration_cast<std::chrono::nanoseconds>(minimum_interval)
              .count();
    auto& control = channels[index];
    if (now < control.next_heartbeat_ns.load(std::memory_order_relaxed))
        return;
    // Only potential publications, not suppressed calls, try the lock. Never
    // wait behind a lifecycle transition or another publisher.
    // Recheck ownership so a retired or replaced owner cannot publish late.
    std::unique_lock lock(control.mutex, std::try_to_lock);
    if (!lock.owns_lock()
        || control.owner.load(std::memory_order_acquire) != identity
        || now < control.next_heartbeat_ns.load(std::memory_order_relaxed)
        || now < current->heartbeats[index].expected_since_ns.load(
               std::memory_order_relaxed
           ))
        return;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    control.next_heartbeat_ns.store(
        minimum_interval_ns > maximum - now ? maximum
                                            : now + minimum_interval_ns,
        std::memory_order_relaxed
    );
    auto& slot = current->heartbeats[index];
    update_slot(slot, [&] {
        slot.timestamp_ns.store(now, std::memory_order_relaxed);
        slot.sequence.fetch_add(1, std::memory_order_relaxed);
    });
}

client::progress_owner client::expect_progress(const channel value) noexcept {
    if (!implementation_)
        return {};
    const auto index = channel_index(value);
    if (index >= heartbeat_channel_count)
        return {};
    std::lock_guard lifecycle_lock(implementation_->mutex);
    auto* state = implementation_->state.load(std::memory_order_acquire);
    if (!state
        || implementation_->next_owner
            == std::numeric_limits<std::uint64_t>::max())
        return {};
    auto& control = implementation_->channels[index];
    std::lock_guard channel_lock(control.mutex);
    auto& slot = state->heartbeats[index];
    if (control.owner.load(std::memory_order_acquire) != 0
        || slot.active.load(std::memory_order_acquire) != 0)
        return {};
    const auto identity = implementation_->next_owner++;
    control.owner.store(identity, std::memory_order_release);
    control.next_heartbeat_ns.store(0, std::memory_order_relaxed);
    update_slot(slot, [&] {
        slot.timestamp_ns.store(0, std::memory_order_relaxed);
        slot.sequence.store(0, std::memory_order_relaxed);
        slot.expected_since_ns.store(
            platform::monotonic_time_ns(), std::memory_order_relaxed
        );
        slot.active.store(1, std::memory_order_relaxed);
    });
    return { implementation_, value, identity };
}

void client::implementation::retire(
    const channel value, const std::uint64_t identity
) noexcept {
    auto& control = channels[channel_index(value)];
    std::lock_guard lock(control.mutex);
    if (control.owner.load(std::memory_order_acquire) != identity)
        return;
    if (auto* current = state.load(std::memory_order_acquire)) {
        auto& slot = current->heartbeats[channel_index(value)];
        update_slot(slot, [&] {
            slot.active.store(0, std::memory_order_relaxed);
            slot.expected_since_ns.store(0, std::memory_order_relaxed);
        });
    }
    control.owner.store(0, std::memory_order_release);
}

void client::set_channel_active(
    const channel value, const bool active
) noexcept {
    if (!implementation_)
        return;
    shared_state* state
        = implementation_->state.load(std::memory_order_acquire);
    const std::size_t index = channel_index(value);
    if (state == nullptr || index >= heartbeat_channel_count) {
        return;
    }
    auto& control = implementation_->channels[index];
    std::lock_guard lock(control.mutex);
    auto& slot = state->heartbeats[index];
    if (control.owner.load(std::memory_order_acquire) != 0
        || (slot.active.load(std::memory_order_acquire) != 0) == active)
        return;
    update_slot(slot, [&] {
        slot.active.store(active ? 1U : 0U, std::memory_order_relaxed);
        slot.expected_since_ns.store(
            active ? platform::monotonic_time_ns() : 0,
            std::memory_order_relaxed
        );
        if (active) {
            slot.timestamp_ns.store(0, std::memory_order_relaxed);
            slot.sequence.store(0, std::memory_order_relaxed);
            control.next_heartbeat_ns.store(0, std::memory_order_relaxed);
        }
    });
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
    if (!implementation_)
        return;
    shared_state* unchecked
        = implementation_->state.load(std::memory_order_acquire);
    if (unchecked == nullptr
        || unchecked->watcher_present.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const std::uint64_t now = platform::monotonic_time_ns();
    shared_state* state = implementation_->active_state(now);
    if (state == nullptr) {
        return;
    }

    const std::uint64_t ticket
        = state->breadcrumb_ticket.fetch_add(1, std::memory_order_relaxed) + 1;
    static_cast<void>(detail::publish_breadcrumb(
        *state, ticket,
        { .timestamp_ns = now,
          .event_id = event_id,
          .flags = flags,
          .source_id = source_id,
          .arg0 = arg0,
          .arg1 = arg1 }
    ));
}

client& client::process() noexcept {
    static client value;
    return value;
}

} // namespace monitor
