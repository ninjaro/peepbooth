#include "monitor/platform/shared_memory.hpp"

#include <algorithm>
#include <cerrno>
#include <system_error>

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace monitor::platform {

bool matches_process(
    const shared_state& state, const process_identity identity
) noexcept {
    return state.magic == shared_state_magic
        && state.header_size == offsetof(shared_state, heartbeats)
        && state.total_size == sizeof(shared_state) && state.pid == identity.pid
        && state.uid == current_user_id()
        && state.process_start_ticks == identity.start_ticks;
}

std::optional<process_endpoint>
inspect_process_mapping(const std::uint32_t pid, std::string* error_message) {
    error_message->clear();
#if defined(__linux__) && !defined(__ANDROID__)
    const auto identity = read_process_identity(pid);
    if (!identity) {
        *error_message = "process is unavailable";
        return std::nullopt;
    }
    shared_mapping mapping;
    mapping.descriptor_ = ::shm_open(
        shared_memory_name(pid).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0
    );
    if (mapping.descriptor_ < 0) {
        *error_message = "cannot open endpoint: "
            + std::error_code(errno, std::generic_category()).message();
        return std::nullopt;
    }

    struct stat metadata {};

    if (::fstat(mapping.descriptor_, &metadata) != 0) {
        *error_message = "cannot inspect endpoint: "
            + std::error_code(errno, std::generic_category()).message();
        return std::nullopt;
    }
    if (!S_ISREG(metadata.st_mode)
        || metadata.st_size != static_cast<off_t>(sizeof(shared_state))
        || metadata.st_uid != static_cast<uid_t>(current_user_id())) {
        *error_message = "incompatible endpoint size, type or owner";
        return std::nullopt;
    }
    void* address = ::mmap(
        nullptr, sizeof(shared_state), PROT_READ, MAP_SHARED,
        mapping.descriptor_, 0
    );
    if (address == MAP_FAILED) {
        *error_message = "cannot map endpoint: "
            + std::error_code(errno, std::generic_category()).message();
        return std::nullopt;
    }
    mapping.state_ = static_cast<shared_state*>(address);
    const auto* state = mapping.get();
    const auto phase = static_cast<monitor::lifecycle>(
        state->process_lifecycle.load(std::memory_order_acquire)
    );
    if (phase != monitor::lifecycle::running
        && phase != monitor::lifecycle::stopping
        && phase != monitor::lifecycle::clean_exit) {
        *error_message = "endpoint initialization is incomplete";
        return std::nullopt;
    }
    if (!matches_process(*state, *identity)) {
        *error_message = "incompatible layout or stale process identity";
        return std::nullopt;
    }
    const auto end = std::find(
        state->process_name.begin(), state->process_name.end(), '\0'
    );
    process_endpoint result { *identity,
                              std::string(state->process_name.begin(), end) };
    const auto current = read_process_identity(pid);
    if (!current || current->start_ticks != identity->start_ticks) {
        *error_message = "process identity changed during inspection";
        return std::nullopt;
    }
    return result;
#else
    static_cast<void>(pid);
    *error_message = "read-only endpoint inspection requires desktop Linux";
    return std::nullopt;
#endif
}

} // namespace monitor::platform
