// CRPC -- logger microbenchmark.
//
// What this measures, and what it does not:
//
//   * "enqueue" numbers are the cost of the caller-side hot path
//     (format into a stack buffer, copy one 512-byte record into the
//     per-thread ring, two atomics). The sink thread runs concurrently and is
//     not part of the timed loop, so a number here is the price a caller pays,
//     not a file-write rate. Records that the sink could not drain before the
//     ring filled are reported separately as drops, never folded into a rate.
//   * Latency is sampled with `steady_clock::now()` on both sides of each call.
//     That adds the clock read overhead (tens of nanoseconds) to every sample,
//     which makes p50/p99 an upper bound on the true call cost. Both the raw
//     and the derived numbers are reported so the bound is visible.
//   * Drop accounting is measured with the sink stopped: a stopped logger is
//     the only state in which the accepted/dropped split is deterministic, so
//     an exact expected count can be asserted instead of observed and believed.
#include <crpc/observability/log.hpp>

#include "bench_harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace obs = crpc::obs;

namespace {

using clock_type = std::chrono::steady_clock;

double seconds_between(clock_type::time_point start, clock_type::time_point stop) {
    return std::chrono::duration<double>(stop - start).count();
}

obs::logger_config make_config(std::size_t ring_capacity) {
    obs::logger_config config;
    config.enable_file = false; // keep the disk out of every number
    config.file_path.clear();
    config.ring_capacity = ring_capacity;
    config.max_file_size = 0;
    config.rotate_interval = std::chrono::milliseconds(0);
    config.overflow = obs::overflow_policy::drop_and_count;
    return config;
}

void reset_logger() {
    obs::logger& log = obs::logger::instance();
    log.shutdown();
    log.reset();
}

// --- Single-thread enqueue throughput ---------------------------------------

void case_single_thread_throughput() {
    reset_logger();
    obs::logger_config config = make_config(65536);
    obs::logger::instance().configure(config);
    obs::logger::instance().start();
    obs::logger::instance().set_level(obs::log_level::trace);

    constexpr std::size_t calls = 200000;
    // Warm-up: the thread-local ring registration (the one allocation) and the
    // first formatting path all land here, outside the measurement.
    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("warm-up {}", i);
    }

    const std::uint64_t dropped_before = obs::logger::instance().dropped_total();
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < calls; ++i) {
        CRPC_LOG_INFO("single-thread request id={} lane={}", i, static_cast<int>(i & 7));
    }
    const auto stop = clock_type::now();
    const std::uint64_t dropped_after = obs::logger::instance().dropped_total();

    const double seconds = seconds_between(start, stop);
    const double rate = static_cast<double>(calls) / seconds;
    crpc_bench::record("log.enqueue.single-thread", "records/s", rate);
    std::printf("%-46s %10llu dropped during the timed loop\n", "log.enqueue.drops",
                static_cast<unsigned long long>(dropped_after - dropped_before));
    reset_logger();
}

// --- Multi-thread enqueue throughput ----------------------------------------

void case_multi_thread_throughput() {
    constexpr std::size_t per_thread = 100000;
    const int thread_counts[] = {1, 2, 4, 8};

    for (const int threads : thread_counts) {
        reset_logger();
        obs::logger_config config = make_config(65536);
        obs::logger::instance().configure(config);
        obs::logger::instance().start();
        obs::logger::instance().set_level(obs::log_level::trace);

        std::atomic<bool> go{false};
        std::atomic<std::size_t> ready{0};
        std::vector<std::thread> workers;
        workers.reserve(static_cast<std::size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&go, &ready, per_thread, t]() {
                obs::logger& log = obs::logger::instance();
                (void)log;
                // Warm-up inside the worker: ring registration is per thread.
                for (std::size_t i = 0; i < 1000; ++i) {
                    CRPC_LOG_DEBUG("worker {} warm {}", t, i);
                }
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::size_t i = 0; i < per_thread; ++i) {
                    CRPC_LOG_INFO("worker {} request {} lane {}", t, i,
                                  static_cast<int>(i & 7));
                }
            });
        }
        while (ready.load(std::memory_order_acquire) <
               static_cast<std::size_t>(threads)) {
            std::this_thread::yield();
        }

        const std::uint64_t dropped_before = obs::logger::instance().dropped_total();
        const auto start = clock_type::now();
        go.store(true, std::memory_order_release);
        for (std::thread& worker : workers) {
            worker.join();
        }
        const auto stop = clock_type::now();
        const std::uint64_t dropped_after = obs::logger::instance().dropped_total();

        const double seconds = seconds_between(start, stop);
        const double total_calls = static_cast<double>(threads) *
                                   static_cast<double>(per_thread);
        const double total_rate = total_calls / seconds;
        const double per_thread_rate = total_rate / static_cast<double>(threads);

        char name[64];
        std::snprintf(name, sizeof(name), "log.enqueue.%d-thread.total", threads);
        crpc_bench::record(name, "records/s", total_rate);
        std::snprintf(name, sizeof(name), "log.enqueue.%d-thread.per-thread", threads);
        crpc_bench::record(name, "records/s", per_thread_rate);
        std::printf("%-46s %10llu dropped during the timed loop\n", "log.enqueue.drops",
                    static_cast<unsigned long long>(dropped_after - dropped_before));
        reset_logger();
    }
}

// --- Per-call latency --------------------------------------------------------

void case_latency() {
    reset_logger();
    obs::logger_config config = make_config(65536);
    obs::logger::instance().configure(config);
    obs::logger::instance().start();
    obs::logger::instance().set_level(obs::log_level::trace);

    constexpr std::size_t samples = 200000;
    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("warm-up {}", i);
    }

    std::vector<std::uint64_t> nanos;
    nanos.reserve(samples);

    // Empty-loop baseline: the same loop shape with no logging, so the ratio
    // below includes an identical amount of loop/clock overhead on both sides.
    std::uint64_t guard = 0;
    std::vector<std::uint64_t> baseline;
    baseline.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const auto tick = clock_type::now();
        guard += i & 1U;
        const auto tock = clock_type::now();
        baseline.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tock - tick).count()));
    }
    std::printf("%-46s %10llu (sink for the baseline loop)\n", "baseline.guard",
                static_cast<unsigned long long>(guard));

    for (std::size_t i = 0; i < samples; ++i) {
        const auto tick = clock_type::now();
        CRPC_LOG_INFO("latency request id={} lane={}", i, static_cast<int>(i & 7));
        const auto tock = clock_type::now();
        nanos.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tock - tick).count()));
    }

    std::sort(nanos.begin(), nanos.end());
    std::sort(baseline.begin(), baseline.end());

    const auto percentile = [](const std::vector<std::uint64_t>& values, double p) {
        if (values.empty()) {
            return 0.0;
        }
        const double index = p * static_cast<double>(values.size() - 1);
        const std::size_t low = static_cast<std::size_t>(index);
        const std::size_t high = low + 1 < values.size() ? low + 1 : low;
        const double fraction = index - static_cast<double>(low);
        return static_cast<double>(values[low]) * (1.0 - fraction) +
               static_cast<double>(values[high]) * fraction;
    };

    double sum = 0.0;
    for (const std::uint64_t value : nanos) {
        sum += static_cast<double>(value);
    }
    const double mean = sum / static_cast<double>(nanos.size());

    crpc_bench::record("log.latency.mean", "ns/call", mean);
    crpc_bench::record("log.latency.p50", "ns/call", percentile(nanos, 0.50));
    crpc_bench::record("log.latency.p99", "ns/call", percentile(nanos, 0.99));
    crpc_bench::record("log.latency.p999", "ns/call", percentile(nanos, 0.999));
    crpc_bench::record("log.latency.max", "ns/call",
                       static_cast<double>(nanos.back()));
    crpc_bench::record("baseline.p50", "ns/iter", percentile(baseline, 0.50));
    crpc_bench::record("baseline.p99", "ns/iter", percentile(baseline, 0.99));

    reset_logger();
}

// --- Disabled level ----------------------------------------------------------

void case_disabled_level() {
    reset_logger();
    obs::logger_config config = make_config(65536);
    obs::logger::instance().configure(config);
    obs::logger::instance().start();
    obs::logger::instance().set_level(obs::log_level::error);
    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("suppressed {}", i);
    }

    constexpr std::size_t calls = 1000000;
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < calls; ++i) {
        CRPC_LOG_INFO("suppressed {}", i);
    }
    const auto stop = clock_type::now();
    const double ns_per_call =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
        static_cast<double>(calls);
    crpc_bench::record("log.disabled-level", "ns/call", ns_per_call);
    reset_logger();
}

// --- Drop accounting ---------------------------------------------------------

void case_drop_accounting() {
    reset_logger();
    constexpr std::size_t capacity = 1024;
    constexpr std::size_t extra = 500;
    obs::logger_config config = make_config(capacity);
    obs::logger::instance().configure(config);
    // Deliberately not started: nothing drains the ring, so exactly `capacity`
    // records are accepted and every later submit is dropped and counted.
    obs::logger::instance().set_level(obs::log_level::trace);

    const obs::log_record record = obs::make_record(obs::log_level::info, "drop probe");
    for (std::size_t i = 0; i < capacity; ++i) {
        obs::logger::instance().submit(record);
    }
    const std::uint64_t dropped_at_capacity = obs::logger::instance().dropped_total();
    for (std::size_t i = 0; i < extra; ++i) {
        obs::logger::instance().submit(record);
    }
    const std::uint64_t dropped_total = obs::logger::instance().dropped_total();

    std::printf("%-46s %10llu (expected 0)\n", "log.drops.at-capacity",
                static_cast<unsigned long long>(dropped_at_capacity));
    std::printf("%-46s %10llu (expected %llu)\n", "log.drops.over-capacity",
                static_cast<unsigned long long>(dropped_total),
                static_cast<unsigned long long>(extra));
    std::printf("%-46s %10s\n", "log.drops.accounting",
                dropped_at_capacity == 0 && dropped_total == extra ? "EXACT" : "MISMATCH");
    reset_logger();
}

} // namespace

void run_all_cases() {
    case_single_thread_throughput();
    case_multi_thread_throughput();
    case_latency();
    case_disabled_level();
    case_drop_accounting();
}

CRPC_BENCH_MAIN()
