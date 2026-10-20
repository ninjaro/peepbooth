#pragma once

#include "monitor/shared_state.hpp"

#include <limits>

namespace monitor::detail {

// A reserved ticket can be delayed while other publishers wrap the ring.
// Kept private so tests can exercise that ordering without timing or sleeps.
inline bool publish_breadcrumb(
    shared_state& state, const std::uint64_t ticket, const breadcrumb& payload
) noexcept {
    if (ticket == 0
        || ticket > (std::numeric_limits<std::uint64_t>::max() >> 1U))
        return false;
    auto& slot = state.breadcrumbs[(ticket - 1) % breadcrumb_capacity];
    auto previous = slot.commit.load(std::memory_order_seq_cst);
    // An odd commit belongs to a live publisher. A newer/equal even commit
    // supersedes this reservation. Try once; diagnostic publication never
    // waits for another producer or retries a contested slot.
    if ((previous & 1U) != 0 || (previous >> 1U) >= ticket
        || !slot.commit.compare_exchange_strong(
            previous, (ticket << 1U) | 1U, std::memory_order_seq_cst,
            std::memory_order_seq_cst
        ))
        return false;
    // Paired with the snapshot acquire fence: observing any replacement
    // payload must also observe its preceding in-progress commit.
    std::atomic_thread_fence(std::memory_order_release);
    slot.ticket.store(ticket, std::memory_order_relaxed);
    slot.timestamp_ns.store(payload.timestamp_ns, std::memory_order_relaxed);
    slot.arg0.store(payload.arg0, std::memory_order_relaxed);
    slot.arg1.store(payload.arg1, std::memory_order_relaxed);
    slot.source_id.store(payload.source_id, std::memory_order_relaxed);
    slot.event_and_flags.store(
        static_cast<std::uint32_t>(payload.event_id)
            | (static_cast<std::uint32_t>(payload.flags) << 16U),
        std::memory_order_relaxed
    );
    slot.commit.store(ticket << 1U, std::memory_order_seq_cst);
    return true;
}

} // namespace monitor::detail
