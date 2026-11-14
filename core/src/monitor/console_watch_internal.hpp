#pragma once

#include "monitor/observed_process.hpp"

#include <iosfwd>

namespace monitor::console_detail {

/// Stable tags shared by console diagnostics and recorded JSON frames.
const char* status_name(observation_status value) noexcept;
const char* kind_name(incident_kind kind) noexcept;
const char* state_name(incident_state state) noexcept;
const char* ending_name(incident_ending ending) noexcept;

void write_history(std::ostream& output, const observation_history& history);

} // namespace monitor::console_detail
