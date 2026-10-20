#pragma once

#include "monitor/observed_process.hpp"

#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace monitor {

// One serial watcher owns each selected process incarnation. No discovery,
// retry, timer, automatic eviction or output policy lives in this registry.
class observation_registry final {
public:
    explicit observation_registry(
        std::uint64_t stale_after_ns = 3'000'000'000ULL
    );
    observation_registry(const observation_registry&) = delete;
    observation_registry& operator=(const observation_registry&) = delete;
    observation_registry(observation_registry&&) = default;
    observation_registry& operator=(observation_registry&&) = default;

    // Revalidates the live identity before acquiring a lease. A rejected
    // request does not remove another entry or redirect it to a reused PID.
    [[nodiscard]] bool attach(platform::process_identity identity);
    bool detach(platform::process_identity identity);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<platform::process_identity> identities() const;
    // Value snapshots survive the next poll or membership change. Exited
    // entries remain observable until explicitly detached by the frontend.
    [[nodiscard]] std::vector<process_observation> poll();

private:
    std::uint64_t stale_after_ns_;
    std::map<std::pair<std::uint32_t, std::uint64_t>, observed_process>
        processes_;
};

} // namespace monitor
