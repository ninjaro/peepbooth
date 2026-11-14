#pragma once

#include <string_view>

namespace monitor_test {
extern int failures;
void check(bool condition, std::string_view message);

void test_status_parser();
void test_observation_history();
void test_incident_progress();
void test_incident_gaps();
void test_incident_checkpoint_between_polls();
void test_incident_independent_channels();
void test_incident_memory_and_lifetime();
void test_incident_replacement_and_bounds();
void test_recording();
void test_command_line_parser();
void test_memory_trend();
void test_health_classification();
void test_expected_progress_classification();
void test_yodau_progress_semantics();
void test_progress_owner();
void test_client_and_ring();
void test_concurrent_progress_publication();
void test_observation_owner();
void test_client_lifecycle();
void test_observed_process_exit();
void test_process_discovery();
void test_breadcrumb_wrap_order();
void test_concurrent_breadcrumb_snapshots();
} // namespace monitor_test
