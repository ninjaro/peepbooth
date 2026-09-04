#ifndef CPPR_MONITOR_CLIENT_HPP
#define CPPR_MONITOR_CLIENT_HPP

#include "monitor/shared_state.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

namespace monitor {

inline constexpr bool instrumentation_enabled =
#if !defined(NDEBUG) && defined(__linux__) && !defined(__ANDROID__)
    true;
#else
    false;
#endif

class client final {
public:
    client();
    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    [[nodiscard]] bool start(std::string_view process_name);
    void stop() noexcept;
    [[nodiscard]] bool available() const noexcept;

    void heartbeat(channel value) noexcept;
    void set_channel_active(channel value, bool active) noexcept;
    void breadcrumb(
        event value, std::uint32_t source_id = 0, std::uint64_t arg0 = 0,
        std::uint64_t arg1 = 0, std::uint16_t flags = 0
    ) noexcept;
    void breadcrumb(
        std::uint16_t event_id, std::uint32_t source_id = 0,
        std::uint64_t arg0 = 0, std::uint64_t arg1 = 0, std::uint16_t flags = 0
    ) noexcept;

    [[nodiscard]] static client& process() noexcept;

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
    std::atomic<shared_state*> state_ { nullptr };

    [[nodiscard]] shared_state* active_state(std::uint64_t now_ns) noexcept;
};

// This is the producer-facing boundary. In unsupported builds these inline
// calls do not construct the process client or touch shared memory.
[[nodiscard]] inline bool start_process(const std::string_view process_name) {
    if constexpr (instrumentation_enabled) {
        return client::process().start(process_name);
    }
    static_cast<void>(process_name);
    return false;
}

[[nodiscard]] inline bool process_available() noexcept {
    if constexpr (instrumentation_enabled) {
        return client::process().available();
    }
    return false;
}

inline void heartbeat(const channel value) noexcept {
    if constexpr (instrumentation_enabled) {
        client::process().heartbeat(value);
    } else {
        static_cast<void>(value);
    }
}

inline void
set_channel_active(const channel value, const bool active) noexcept {
    if constexpr (instrumentation_enabled) {
        client::process().set_channel_active(value, active);
    } else {
        static_cast<void>(value);
        static_cast<void>(active);
    }
}

inline void record_breadcrumb(
    const event value, const std::uint32_t source_id = 0,
    const std::uint64_t arg0 = 0, const std::uint64_t arg1 = 0,
    const std::uint16_t flags = 0
) noexcept {
    if constexpr (instrumentation_enabled) {
        client::process().breadcrumb(value, source_id, arg0, arg1, flags);
    } else {
        static_cast<void>(value);
        static_cast<void>(source_id);
        static_cast<void>(arg0);
        static_cast<void>(arg1);
        static_cast<void>(flags);
    }
}

} // namespace monitor

#endif // CPPR_MONITOR_CLIENT_HPP
