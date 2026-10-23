#pragma once

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
    class implementation;

public:
    // One independently supervised progress domain per fixed channel. A failed
    // claim is invalid, never a second contributor to somebody else's owner.
    class progress_owner final {
    public:
        progress_owner() = default;
        ~progress_owner();
        progress_owner(const progress_owner&) = delete;
        progress_owner& operator=(const progress_owner&) = delete;
        progress_owner(progress_owner&& other) noexcept;
        progress_owner& operator=(progress_owner&& other) noexcept;

        // heartbeat/valid may overlap reset. Join publishers before moving or
        // destroying this handle; producer shutdown retains its host contract.
        [[nodiscard]] bool valid() const noexcept;

        void heartbeat() noexcept {
            if constexpr (instrumentation_enabled)
                publish_heartbeat();
        }

        void reset() noexcept;

    private:
        friend class client;
        progress_owner(
            const std::shared_ptr<implementation>& owner, channel value,
            std::uint64_t identity
        ) noexcept;
        std::weak_ptr<implementation> owner_;
        channel channel_ { channel::main };
        std::atomic<std::uint64_t> identity_ { 0 };
        void publish_heartbeat() noexcept;
    };

    client();
    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    // One lifecycle owner starts before producers, then stops/joins all
    // producers before stop() or destruction. Publication may be concurrent;
    // unmapping concurrently with publication is unsupported.
    [[nodiscard]] bool start(std::string_view process_name);
    void stop() noexcept;
    [[nodiscard]] bool available() const noexcept;

    [[nodiscard]] progress_owner expect_progress(channel value) noexcept;
    // Passive evidence unless an explicit manual expectation is active.
    // Neither operation can publish for, or retire, a scoped owner.
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
    std::shared_ptr<implementation> implementation_;
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

[[nodiscard]] inline client::progress_owner
expect_progress(const channel value) noexcept {
    if constexpr (instrumentation_enabled) {
        return client::process().expect_progress(value);
    }
    static_cast<void>(value);
    return {};
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
