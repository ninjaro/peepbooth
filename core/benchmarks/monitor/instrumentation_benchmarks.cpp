#include "monitor/client.hpp"
#include "monitor/platform/process_watch.hpp"
#include "monitor/platform/shared_memory.hpp"
#include "monitor/watchdog.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
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
    producer.stop();
}
} // namespace

int main() {
    try {
        std::cout << "instrumentation,operation,interest,workers,wall_s,cpu_s,"
                     "median_batch_ns_per_call,p95_batch_ns_per_call,"
                     "p99_batch_ns_per_call,calls_per_s,retained_records,"
                     "checksum\n";
        for (const std::size_t workers : { 1U, 4U }) {
            measure("baseline", "none", workers);
            measure("clock", "unattached", workers);
            for (const auto interest : { "unattached", "attached" }) {
                measure("heartbeat", interest, workers);
                measure("breadcrumb", interest, workers);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
