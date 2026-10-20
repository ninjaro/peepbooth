#include "monitor/observation_registry.hpp"

namespace monitor {
namespace {
    auto identity_key(const platform::process_identity identity) noexcept {
        return std::pair { identity.pid, identity.start_ticks };
    }
}

observation_registry::observation_registry(const std::uint64_t stale_after_ns)
    : stale_after_ns_(stale_after_ns) { }

bool observation_registry::attach(const platform::process_identity identity) {
    const auto key = identity_key(identity);
    if (const auto found = processes_.find(key); found != processes_.end())
        return found->second.alive() && found->second.try_attach();
    auto handle = platform::process_handle::attach(identity.pid);
    if (!handle || handle->identity().start_ticks != identity.start_ticks)
        return false;
    observed_process process(std::move(*handle), stale_after_ns_);
    if (!process.try_attach())
        return false;
    processes_.emplace(key, std::move(process));
    return true;
}

bool observation_registry::detach(const platform::process_identity identity) {
    return processes_.erase(identity_key(identity)) != 0;
}

void observation_registry::clear() noexcept { processes_.clear(); }

std::size_t observation_registry::size() const noexcept {
    return processes_.size();
}

std::vector<platform::process_identity>
observation_registry::identities() const {
    std::vector<platform::process_identity> result;
    result.reserve(processes_.size());
    for (const auto& [key, process] : processes_)
        result.push_back(process.latest().identity);
    return result;
}

std::vector<process_observation> observation_registry::poll() {
    std::vector<process_observation> result;
    result.reserve(processes_.size());
    for (auto& [key, process] : processes_)
        result.push_back(process.poll());
    return result;
}

} // namespace monitor
