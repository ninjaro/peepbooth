#include "monitor/client.hpp"
#include "monitor/console_watch.hpp"
#include "monitor/observed_process.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/watchdog.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::size_t batches = 4096;
constexpr std::size_t calls_per_batch = 256;

void measure(
    const std::string_view operation, const std::string_view interest,
    const std::size_t worker_count
) {
    monitor::client producer;
    monitor::client::progress_owner progress;
    std::optional<monitor::watchdog_session> observer;
    if (interest != "none" && monitor::instrumentation_enabled) {
        if (!producer.start("instrumentation-benchmark"))
            throw std::runtime_error("producer mapping unavailable");
        if (interest == "attached") {
            const auto identity = monitor::platform::read_process_identity(
                monitor::platform::current_process_id()
            );
            if (identity)
                observer = monitor::watchdog_session::attach(*identity);
            if (!observer)
                throw std::runtime_error("watcher attachment unavailable");
            // No watcher thread competes with producers. This isolates call
            // cost; live observer sampling is a separate application workload.
            observer->renew_lease(
                monitor::platform::monotonic_time_ns(), 60'000'000'000ULL
            );
        }
    }
    if (operation == "owned_heartbeat" && monitor::instrumentation_enabled) {
        progress = producer.expect_progress(monitor::channel::core);
        if (!progress.valid())
            throw std::runtime_error("progress ownership unavailable");
    }
    std::vector<std::vector<double>> samples(
        worker_count, std::vector<double>(batches)
    );
    std::vector<std::uint64_t> checksums(worker_count);
    std::barrier start(static_cast<std::ptrdiff_t>(worker_count + 1));
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back([&, index] {
            std::uint64_t checksum = index + 1;
            start.arrive_and_wait();
            for (std::size_t batch = 0; batch < batches; ++batch) {
                const auto began = clock_type::now();
                for (std::size_t call = 0; call < calls_per_batch; ++call) {
                    // Identical live workload in every mode. A baseline does
                    // not call instrumentation; Release measures the public
                    // inline API, not an explicitly constructed inert client.
                    checksum = checksum * 6364136223846793005ULL + 1;
                    std::atomic_signal_fence(std::memory_order_seq_cst);
                    if (interest == "none")
                        continue;
                    if (operation == "clock") {
                        checksum ^= monitor::platform::monotonic_time_ns();
                    } else if (operation == "owned_heartbeat") {
                        progress.heartbeat();
                    } else if (operation == "heartbeat") {
                        if constexpr (monitor::instrumentation_enabled)
                            producer.heartbeat(monitor::channel::core);
                        else
                            monitor::heartbeat(monitor::channel::core);
                    } else {
                        if constexpr (monitor::instrumentation_enabled)
                            producer.breadcrumb(
                                monitor::event::stream_stopped,
                                static_cast<std::uint32_t>(index), checksum
                            );
                        else
                            monitor::record_breadcrumb(
                                monitor::event::stream_stopped,
                                static_cast<std::uint32_t>(index), checksum
                            );
                    }
                }
                samples[index][batch]
                    = std::chrono::duration<double, std::nano>(
                          clock_type::now() - began
                      )
                          .count()
                    / static_cast<double>(calls_per_batch);
            }
            checksums[index] = checksum;
        });
    }
    const auto cpu_start = std::clock();
    const auto began = clock_type::now();
    start.arrive_and_wait();
    for (auto& worker : workers)
        worker.join();
    const double elapsed
        = std::chrono::duration<double>(clock_type::now() - began).count();
    const double cpu = static_cast<double>(std::clock() - cpu_start)
        / static_cast<double>(CLOCKS_PER_SEC);
    std::vector<double> combined;
    combined.reserve(worker_count * batches);
    for (const auto& sample : samples)
        combined.insert(combined.end(), sample.begin(), sample.end());
    std::ranges::sort(combined);
    const auto percentile = [&](const std::size_t percent) {
        return combined[(combined.size() - 1) * percent / 100];
    };
    std::uint64_t checksum = 0;
    for (const auto value : checksums)
        checksum ^= value;
    std::cout << (monitor::instrumentation_enabled ? "enabled" : "disabled")
              << ',' << operation << ',' << interest << ',' << worker_count
              << ',' << elapsed << ',' << cpu << ',' << percentile(50) << ','
              << percentile(95) << ',' << percentile(99) << ','
              << static_cast<double>(worker_count * batches * calls_per_batch)
            / elapsed
              << ',' << (observer ? observer->snapshot().breadcrumbs.size() : 0)
              << ',' << checksum << '\n';
    observer.reset();
    progress.reset();
    producer.stop();
}

void measure_incidents(
    const std::size_t capacity, const std::size_t iterations
) {
    const auto stale
        = static_cast<std::uint64_t>(monitor::console_options {}.stale_ms)
        * 1'000'000ULL;
    std::cout << "mode,scenario,capacity,observations,wall_ns_per_observation,"
                 "cpu_ns_per_observation,active,closed,overwritten,record_"
                 "bytes,incident_bytes,checksum\n";
    for (const auto scenario : { "idle", "overdue", "turnover" }) {
        for (const bool enabled : { false, true }) {
            monitor::incident_tracker tracker({ 42, 99 }, capacity);
            monitor::observation_record record;
            record.identity = { 42, 99 };
            record.observed_at_ns = 1;
            record.status = monitor::observation_status::sampled;
            record.memory = monitor::platform::process_memory { 4096, 8192, 0 };
            record.trend = monitor::memory_trend_summary {};
            record.progress = monitor::progress_evidence {};
            record.progress->process_lifecycle = monitor::lifecycle::running;
            record.progress->observation_started_ns = 1;
            record.progress->stale_after_ns = stale;
            for (std::size_t slot = 0; slot < monitor::heartbeat_channel_count;
                 ++slot)
                record.progress->channels[slot].id
                    = static_cast<monitor::channel>(slot + 1U);
            auto& core = record.progress->channels[monitor::channel_index(
                monitor::channel::core
            )];
            core.active = std::string_view(scenario) != "idle";
            core.expected_since_ns = core.active ? 1 : 0;
            std::uint64_t checksum = 1;
            const auto cpu_start = std::clock();
            const auto began = clock_type::now();
            for (std::size_t index = 0; index < iterations; ++index) {
                record.observed_at_ns += stale + 1;
                record.sampled_at_ns = record.observed_at_ns;
                if (std::string_view(scenario) == "turnover"
                    && index % 2 != 0) {
                    core.timestamp_ns = record.observed_at_ns;
                    ++core.sequence;
                }
                checksum
                    = checksum * 6364136223846793005ULL + record.observed_at_ns;
                std::atomic_signal_fence(std::memory_order_seq_cst);
                if (enabled)
                    tracker.add(record);
            }
            const double wall = std::chrono::duration<double, std::nano>(
                                    clock_type::now() - began
            )
                                    .count();
            const double cpu = static_cast<double>(std::clock() - cpu_start)
                * 1e9 / CLOCKS_PER_SEC;
            const auto active
                = std::ranges::count_if(tracker.active(), [](const auto& item) {
                      return item.has_value();
                  });
            std::cout << (enabled ? "incidents" : "carrier") << ',' << scenario
                      << ',' << capacity << ',' << iterations << ','
                      << wall / static_cast<double>(iterations) << ','
                      << cpu / static_cast<double>(iterations) << ',' << active
                      << ',' << tracker.closed().size() << ','
                      << tracker.overwritten() << ','
                      << sizeof(monitor::observation_record) << ','
                      << sizeof(monitor::diagnostic_incident) << ',' << checksum
                      << '\n';
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 1) {
            if (argc != 4 || std::string_view(argv[1]) != "--incidents")
                throw std::runtime_error(
                    "usage: monitor_instrumentation_benchmarks [--incidents "
                    "CAPACITY OBSERVATIONS]"
                );
            const auto number = [](const std::string_view text) {
                std::size_t value = 0;
                const auto result = std::from_chars(
                    text.data(), text.data() + text.size(), value
                );
                if (result.ec != std::errc {}
                    || result.ptr != text.data() + text.size())
                    throw std::runtime_error(
                        "capacity and observation count must be unsigned "
                        "integers"
                    );
                return value;
            };
            const auto capacity = number(argv[2]);
            const auto iterations = number(argv[3]);
            if (iterations == 0)
                throw std::runtime_error("observation count must be positive");
            measure_incidents(capacity, iterations);
            return 0;
        }
        std::cout << "instrumentation,operation,interest,workers,wall_s,cpu_s,"
                     "median_batch_ns_per_call,p95_batch_ns_per_call,"
                     "p99_batch_ns_per_call,calls_per_s,retained_records,"
                     "checksum\n";
        for (const std::size_t workers : { 1U, 4U }) {
            measure("baseline", "none", workers);
            measure("clock", "unattached", workers);
            for (const auto interest : { "unattached", "attached" }) {
                measure("heartbeat", interest, workers);
                measure("owned_heartbeat", interest, workers);
                measure("breadcrumb", interest, workers);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
