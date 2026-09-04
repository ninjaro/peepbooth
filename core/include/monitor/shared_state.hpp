#ifndef CPPR_MONITOR_SHARED_STATE_HPP
#define CPPR_MONITOR_SHARED_STATE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace monitor {

inline constexpr std::uint64_t shared_state_magic = 0x4350505257444f47ULL;
inline constexpr std::uint16_t shared_state_version = 1;
inline constexpr std::size_t heartbeat_channel_count = 4;
inline constexpr std::size_t breadcrumb_capacity = 128;
inline constexpr std::size_t process_name_capacity = 32;

enum class channel : std::uint8_t {
    main = 1,
    gui = 2,
    core = 3,
    worker = 4,
};

enum class event : std::uint16_t {
    configuration_loaded = 10,
    output_ready = 12,
    stream_started = 22,
    stream_stopped = 23,
};

enum class lifecycle : std::uint32_t {
    starting = 1,
    running = 2,
    stopping = 3,
    clean_exit = 4,
};

struct breadcrumb {
    std::uint64_t timestamp_ns {};
    std::uint16_t event_id {};
    std::uint16_t flags {};
    std::uint32_t source_id {};
    std::uint64_t arg0 {};
    std::uint64_t arg1 {};
};

struct heartbeat_slot {
    std::atomic<std::uint64_t> timestamp_ns { 0 };
    std::atomic<std::uint64_t> sequence { 0 };
    std::atomic<std::uint32_t> active { 0 };
    std::uint32_t reserved {};
};

struct breadcrumb_slot {
    std::atomic<std::uint64_t> commit { 0 };
    std::atomic<std::uint64_t> ticket { 0 };
    std::atomic<std::uint64_t> timestamp_ns { 0 };
    std::atomic<std::uint64_t> arg0 { 0 };
    std::atomic<std::uint64_t> arg1 { 0 };
    std::atomic<std::uint32_t> source_id { 0 };
    std::atomic<std::uint32_t> event_and_flags { 0 };
};

struct shared_state {
    std::uint64_t magic { shared_state_magic };
    std::uint16_t version { shared_state_version };
    std::uint16_t header_size {};
    std::uint32_t total_size {};
    std::uint32_t pid {};
    std::uint32_t uid {};
    std::uint64_t process_start_ticks {};
    std::array<char, process_name_capacity> process_name {};
    std::atomic<std::uint32_t> process_lifecycle {
        static_cast<std::uint32_t>(lifecycle::starting)
    };
    std::atomic<std::uint32_t> watcher_present { 0 };
    std::atomic<std::uint64_t> watcher_lease_until_ns { 0 };
    std::atomic<std::uint64_t> breadcrumb_ticket { 0 };
    std::array<heartbeat_slot, heartbeat_channel_count> heartbeats {};
    std::array<breadcrumb_slot, breadcrumb_capacity> breadcrumbs {};
};

[[nodiscard]] constexpr std::size_t channel_index(channel value) noexcept {
    const auto raw = static_cast<std::uint8_t>(value);
    return raw == 0 || raw > heartbeat_channel_count
        ? heartbeat_channel_count
        : static_cast<std::size_t>(raw - 1U);
}

static_assert(sizeof(breadcrumb) == 32);
static_assert(std::is_standard_layout_v<shared_state>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

} // namespace monitor

#endif // CPPR_MONITOR_SHARED_STATE_HPP
