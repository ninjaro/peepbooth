#include "monitor/platform/process_watch.hpp"

#include "monitor/platform/shared_memory.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <csignal>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace monitor::platform {
namespace {

    std::optional<std::string> read_text_file(const std::string& path) {
        std::ifstream input(path);
        if (!input) {
            return std::nullopt;
        }
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        );
    }

    std::optional<std::uint64_t>
    parse_unsigned(std::string_view value) noexcept {
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        const std::size_t end = value.find_first_not_of("0123456789");
        value = value.substr(0, end);
        if (value.empty()) {
            return std::nullopt;
        }
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(
            value.data(), value.data() + value.size(), parsed
        );
        return result.ec == std::errc {} ? std::optional(parsed) : std::nullopt;
    }

    std::optional<std::uint64_t>
    status_value_kib(std::string_view contents, std::string_view key) noexcept {
        const std::size_t position = contents.find(key);
        if (position == std::string_view::npos
            || (position != 0 && contents[position - 1] != '\n')) {
            return std::nullopt;
        }
        std::string_view line = contents.substr(position + key.size());
        line = line.substr(0, line.find('\n'));
        const auto kib = parse_unsigned(line);
        return kib ? std::optional(*kib * 1024ULL) : std::nullopt;
    }

    struct stat_values {
        std::uint64_t cpu_ticks {};
        std::uint64_t start_ticks {};
    };

    std::optional<stat_values> parse_proc_stat(std::string_view contents) {
        const std::size_t close = contents.rfind(')');
        if (close == std::string_view::npos || close + 2 >= contents.size()) {
            return std::nullopt;
        }
        std::string tail(contents.substr(close + 2));
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (start < tail.size()) {
            while (start < tail.size() && tail[start] == ' ') {
                ++start;
            }
            if (start >= tail.size()) {
                break;
            }
            const std::size_t end = tail.find(' ', start);
            fields.emplace_back(
                tail.data() + start,
                (end == std::string::npos ? tail.size() : end) - start
            );
            start = end == std::string::npos ? tail.size() : end + 1;
        }
        // The first token after the command is field 3 (state).
        if (fields.size() <= 19) {
            return std::nullopt;
        }
        const auto user = parse_unsigned(fields[11]);
        const auto system = parse_unsigned(fields[12]);
        const auto start_ticks = parse_unsigned(fields[19]);
        if (!user || !system || !start_ticks) {
            return std::nullopt;
        }
        return stat_values {
            .cpu_ticks = *user + *system,
            .start_ticks = *start_ticks,
        };
    }

} // namespace

std::optional<process_memory>
parse_proc_status(const std::string_view contents) noexcept {
    const auto rss = status_value_kib(contents, "VmRSS:");
    if (!rss) {
        return std::nullopt;
    }
    return process_memory {
        .rss_bytes = *rss,
        .high_water_bytes = status_value_kib(contents, "VmHWM:").value_or(*rss),
        .swap_bytes = status_value_kib(contents, "VmSwap:").value_or(0),
    };
}

std::optional<process_identity> read_process_identity(const std::uint32_t pid) {
#if defined(__linux__) && !defined(__ANDROID__)
    const auto contents
        = read_text_file("/proc/" + std::to_string(pid) + "/stat");
    if (!contents) {
        return std::nullopt;
    }
    const auto values = parse_proc_stat(*contents);
    if (!values) {
        return std::nullopt;
    }
    return process_identity { .pid = pid, .start_ticks = values->start_ticks };
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

std::optional<process_sample> sample_process(const std::uint32_t pid) {
#if defined(__linux__) && !defined(__ANDROID__)
    const std::string base = "/proc/" + std::to_string(pid);
    const auto status = read_text_file(base + "/status");
    const auto stat = read_text_file(base + "/stat");
    if (!status || !stat) {
        return std::nullopt;
    }
    const auto memory = parse_proc_status(*status);
    const auto values = parse_proc_stat(*stat);
    if (!memory || !values) {
        return std::nullopt;
    }
    return process_sample {
        .identity = { .pid = pid, .start_ticks = values->start_ticks },
        .memory = *memory,
        .cpu_ticks = values->cpu_ticks,
        .sampled_at_ns = monotonic_time_ns(),
    };
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

long clock_ticks_per_second() noexcept {
#if defined(__linux__) && !defined(__ANDROID__)
    const long value = ::sysconf(_SC_CLK_TCK);
    return value > 0 ? value : 100;
#else
    return 100;
#endif
}

process_handle::~process_handle() {
#if defined(__linux__) && !defined(__ANDROID__)
    if (pid_descriptor_ >= 0) {
        ::close(pid_descriptor_);
    }
#endif
}

process_handle::process_handle(process_handle&& other) noexcept
    : identity_(other.identity_)
    , pid_descriptor_(std::exchange(other.pid_descriptor_, -1)) { }

process_handle& process_handle::operator=(process_handle&& other) noexcept {
    if (this == &other) {
        return *this;
    }
#if defined(__linux__) && !defined(__ANDROID__)
    if (pid_descriptor_ >= 0) {
        ::close(pid_descriptor_);
    }
#endif
    identity_ = other.identity_;
    pid_descriptor_ = std::exchange(other.pid_descriptor_, -1);
    return *this;
}

std::optional<process_handle> process_handle::attach(const std::uint32_t pid) {
    const auto identity = read_process_identity(pid);
    if (!identity) {
        return std::nullopt;
    }
    process_handle result;
    result.identity_ = *identity;
#if defined(__linux__) && !defined(__ANDROID__) && defined(SYS_pidfd_open)
    result.pid_descriptor_
        = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
#endif
    return result;
}

bool process_handle::alive() const {
#if defined(__linux__) && !defined(__ANDROID__)
    if (pid_descriptor_ >= 0) {
        pollfd descriptor {
            .fd = pid_descriptor_,
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN)) {
            return false;
        }
    } else if (
        ::kill(static_cast<pid_t>(identity_.pid), 0) != 0 && errno == ESRCH
    ) {
        return false;
    }
    const auto current = read_process_identity(identity_.pid);
    return current && current->start_ticks == identity_.start_ticks;
#else
    return false;
#endif
}

bool process_handle::wait_for_exit(const int timeout_ms) const {
#if defined(__linux__) && !defined(__ANDROID__)
    if (pid_descriptor_ >= 0) {
        pollfd descriptor {
            .fd = pid_descriptor_,
            .events = POLLIN,
            .revents = 0,
        };
        return ::poll(&descriptor, 1, timeout_ms) > 0
            && (descriptor.revents & POLLIN);
    }
    if (timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
    }
#else
    static_cast<void>(timeout_ms);
#endif
    return !alive();
}

process_identity process_handle::identity() const noexcept { return identity_; }

} // namespace monitor::platform
