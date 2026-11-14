#include "monitor/platform/shared_memory.hpp"
#include "watchdog/watchdog_checks.hpp"

#include <cstdlib>
#include <iostream>

namespace monitor_test {
int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace monitor_test

using namespace monitor_test;

int main() {
    test_recording();
    test_incident_progress();
    test_incident_gaps();
    test_incident_independent_channels();
    test_incident_checkpoint_between_polls();
    test_incident_memory_and_lifetime();
    test_incident_replacement_and_bounds();
    test_observation_history();
    const auto name = monitor::platform::shared_memory_name(42);
    check(
        name.starts_with("/monitor-watchdog-") && name.ends_with("-42"),
        "producer/watchdog endpoint uses the current Monitor identity"
    );
    test_observation_owner();
    test_observed_process_exit();
    test_status_parser();
    test_command_line_parser();
    test_memory_trend();
    test_health_classification();
    test_expected_progress_classification();
    test_yodau_progress_semantics();
    test_progress_owner();
    test_concurrent_progress_publication();
    test_client_and_ring();
    test_client_lifecycle();
    test_process_discovery();
    test_breadcrumb_wrap_order();
    test_concurrent_breadcrumb_snapshots();
    if (failures == 0) {
        std::cout << "monitor watchdog focused tests passed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
