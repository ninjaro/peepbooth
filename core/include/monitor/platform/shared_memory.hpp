#ifndef CPPR_MONITOR_PLATFORM_SHARED_MEMORY_HPP
#define CPPR_MONITOR_PLATFORM_SHARED_MEMORY_HPP

#include "monitor/shared_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace monitor::platform {

class shared_mapping final {
public:
    shared_mapping() = default;
    ~shared_mapping();
    shared_mapping(const shared_mapping&) = delete;
    shared_mapping& operator=(const shared_mapping&) = delete;
    shared_mapping(shared_mapping&& other) noexcept;
    shared_mapping& operator=(shared_mapping&& other) noexcept;

    [[nodiscard]] shared_state* get() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

private:
    friend std::optional<shared_mapping>
    create_process_mapping(std::string_view process_name);
    friend std::optional<shared_mapping>
    open_process_mapping(std::uint32_t pid);

    shared_state* state_ { nullptr };
    int descriptor_ { -1 };
    bool owner_ { false };
    std::string name_;

    void reset() noexcept;
};

[[nodiscard]] std::optional<shared_mapping>
create_process_mapping(std::string_view process_name);
[[nodiscard]] std::optional<shared_mapping>
open_process_mapping(std::uint32_t pid);
[[nodiscard]] std::string shared_memory_name(std::uint32_t pid);
[[nodiscard]] std::uint64_t monotonic_time_ns() noexcept;
[[nodiscard]] std::uint32_t current_process_id() noexcept;
[[nodiscard]] std::uint32_t current_user_id() noexcept;

} // namespace monitor::platform

#endif // CPPR_MONITOR_PLATFORM_SHARED_MEMORY_HPP
