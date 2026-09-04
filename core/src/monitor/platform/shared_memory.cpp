#include "monitor/platform/shared_memory.hpp"

#include "monitor/platform/process_watch.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>
#include <utility>

#if defined(__linux__) || defined(__ANDROID__)
#include <unistd.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace monitor::platform {

shared_mapping::~shared_mapping() { reset(); }

shared_mapping::shared_mapping(shared_mapping&& other) noexcept
    : state_(std::exchange(other.state_, nullptr))
    , descriptor_(std::exchange(other.descriptor_, -1))
    , owner_(std::exchange(other.owner_, false))
    , name_(std::move(other.name_)) { }

shared_mapping& shared_mapping::operator=(shared_mapping&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    state_ = std::exchange(other.state_, nullptr);
    descriptor_ = std::exchange(other.descriptor_, -1);
    owner_ = std::exchange(other.owner_, false);
    name_ = std::move(other.name_);
    return *this;
}

shared_state* shared_mapping::get() const noexcept { return state_; }

shared_mapping::operator bool() const noexcept { return state_ != nullptr; }

void shared_mapping::reset() noexcept {
#if defined(__linux__) && !defined(__ANDROID__)
    if (state_ != nullptr) {
        ::munmap(state_, sizeof(shared_state));
    }
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
    if (owner_ && !name_.empty()) {
        ::shm_unlink(name_.c_str());
    }
#endif
    state_ = nullptr;
    descriptor_ = -1;
    owner_ = false;
    name_.clear();
}

std::string shared_memory_name(const std::uint32_t pid) {
    return "/cppr-watchdog-" + std::to_string(current_user_id()) + "-"
        + std::to_string(pid);
}

std::optional<shared_mapping>
create_process_mapping(const std::string_view process_name) {
#if defined(__linux__) && !defined(__ANDROID__)
    const std::uint32_t pid = current_process_id();
    const std::string name = shared_memory_name(pid);
    ::shm_unlink(name.c_str());
    const int descriptor = ::shm_open(
        name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR
    );
    if (descriptor < 0) {
        return std::nullopt;
    }
    if (::ftruncate(descriptor, static_cast<off_t>(sizeof(shared_state))) != 0
        || ::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        ::close(descriptor);
        ::shm_unlink(name.c_str());
        return std::nullopt;
    }

    void* address = ::mmap(
        nullptr, sizeof(shared_state), PROT_READ | PROT_WRITE, MAP_SHARED,
        descriptor, 0
    );
    if (address == MAP_FAILED) {
        ::close(descriptor);
        ::shm_unlink(name.c_str());
        return std::nullopt;
    }

    auto* state = new (address) shared_state {};
    state->header_size
        = static_cast<std::uint16_t>(offsetof(shared_state, heartbeats));
    state->total_size = sizeof(shared_state);
    state->pid = pid;
    state->uid = current_user_id();
    if (const auto identity = read_process_identity(pid)) {
        state->process_start_ticks = identity->start_ticks;
    }
    const std::size_t copied = std::min(
        process_name.size(), state->process_name.size() - std::size_t { 1 }
    );
    std::memcpy(state->process_name.data(), process_name.data(), copied);
    state->process_name[copied] = '\0';
    state->process_lifecycle.store(
        static_cast<std::uint32_t>(lifecycle::running),
        std::memory_order_release
    );

    shared_mapping mapping;
    mapping.state_ = state;
    mapping.descriptor_ = descriptor;
    mapping.owner_ = true;
    mapping.name_ = name;
    return mapping;
#else
    static_cast<void>(process_name);
    return std::nullopt;
#endif
}

std::optional<shared_mapping> open_process_mapping(const std::uint32_t pid) {
#if defined(__linux__) && !defined(__ANDROID__)
    const std::string name = shared_memory_name(pid);
    const int descriptor = ::shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    if (descriptor < 0) {
        return std::nullopt;
    }
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0
        || metadata.st_size != static_cast<off_t>(sizeof(shared_state))
        || metadata.st_uid != static_cast<uid_t>(current_user_id())) {
        ::close(descriptor);
        return std::nullopt;
    }
    void* address = ::mmap(
        nullptr, sizeof(shared_state), PROT_READ | PROT_WRITE, MAP_SHARED,
        descriptor, 0
    );
    if (address == MAP_FAILED) {
        ::close(descriptor);
        return std::nullopt;
    }

    shared_mapping mapping;
    mapping.state_ = static_cast<shared_state*>(address);
    mapping.descriptor_ = descriptor;
    mapping.name_ = name;
    return mapping;
#else
    static_cast<void>(pid);
    return std::nullopt;
#endif
}

std::uint64_t monotonic_time_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        )
            .count()
    );
}

std::uint32_t current_process_id() noexcept {
#if defined(__linux__) || defined(__ANDROID__)
    return static_cast<std::uint32_t>(::getpid());
#else
    return 0;
#endif
}

std::uint32_t current_user_id() noexcept {
#if defined(__linux__) || defined(__ANDROID__)
    return static_cast<std::uint32_t>(::getuid());
#else
    return 0;
#endif
}

} // namespace monitor::platform
