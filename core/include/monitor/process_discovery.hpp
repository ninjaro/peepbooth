#pragma once

#include "monitor/platform/shared_memory.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace monitor {

struct process_discovery {
    bool available {};
    std::vector<platform::process_endpoint> processes;
    std::vector<std::string> diagnostics;
};

// One-shot local inventory, without leases, process sampling or cleanup.
// An endpoint may disappear afterward; observation must revalidate on attach.
[[nodiscard]] process_discovery discover_processes();

struct process_name_selection {
    std::optional<platform::process_identity> identity;
    std::string error;
};

// Exact, unique names only, from a complete inventory. Selection is read-only;
// a registry/observer must still revalidate before acquiring a lease.
[[nodiscard]] process_name_selection
select_process_name(const process_discovery& inventory, std::string_view name);

} // namespace monitor
