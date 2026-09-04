#ifndef CPPR_MONITOR_PLATFORM_PROCESS_WATCH_HPP
#define CPPR_MONITOR_PLATFORM_PROCESS_WATCH_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace monitor::platform {

struct process_identity {
    std::uint32_t pid {};
    std::uint64_t start_ticks {};
};

struct process_memory {
    std::uint64_t rss_bytes {};
    std::uint64_t high_water_bytes {};
    std::uint64_t swap_bytes {};
};

struct process_sample {
    process_identity identity;
    process_memory memory;
    std::uint64_t cpu_ticks {};
    std::uint64_t sampled_at_ns {};
};

[[nodiscard]] std::optional<process_memory>
parse_proc_status(std::string_view contents) noexcept;
[[nodiscard]] std::optional<process_identity>
read_process_identity(std::uint32_t pid);
[[nodiscard]] std::optional<process_sample> sample_process(std::uint32_t pid);
[[nodiscard]] long clock_ticks_per_second() noexcept;

class process_handle final {
public:
    process_handle() = default;
    ~process_handle();
    process_handle(const process_handle&) = delete;
    process_handle& operator=(const process_handle&) = delete;
    process_handle(process_handle&& other) noexcept;
    process_handle& operator=(process_handle&& other) noexcept;

    [[nodiscard]] static std::optional<process_handle>
    attach(std::uint32_t pid);
    [[nodiscard]] bool alive() const;
    [[nodiscard]] bool wait_for_exit(int timeout_ms) const;
    [[nodiscard]] process_identity identity() const noexcept;

private:
    process_identity identity_;
    int pid_descriptor_ { -1 };
};

} // namespace monitor::platform

#endif // CPPR_MONITOR_PLATFORM_PROCESS_WATCH_HPP
