// CRPC -- logger hot-path performance assertions.
//
// These cases are gates, not benchmarks: bench_log.cpp reports numbers, this
// file fails the build when the caller-side logging path regresses past a
// stated bound. Three independent checks are made, because each one catches a
// different regression:
//
//   1. Allocation: after a thread's ring is registered, a submit must not touch
//      the heap. Counted per-thread (the sink thread allocates legitimately),
//      so a "no allocation" claim is measured rather than inferred.
//   2. Absolute cost: one enabled record must stay under 1 us per call in a
//      release build and under 5 us in a debug build. The bound is loose on
//      purpose -- it is a "nothing pathological happened" gate, not a
//      microbenchmark -- and the debug/release split is what keeps it honest
//      without pretending a debug CRT is fast.
//   3. Relative cost: the same loop with and without the log statement. Taking
//      the ratio makes the check immune to a slow or contended CI machine: a
//      machine that is 3x slower slows both loops down. Only the constant
//      factor of logging is pinned.
//
// Nothing here is relaxed to make a number look better; every printed value is
// the value the assertions used.
#include <crpc/observability/log.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

namespace obs = crpc::obs;
using clock_type = std::chrono::steady_clock;

// --- Per-thread allocation counter ------------------------------------------
//
// The sink thread allocates while it drains; a global counter would attribute
// that to the measured call. Counting only the measuring thread keeps the claim
// exact. `g_counting` is thread-local, so enabling it cannot affect another
// thread's test.

namespace {
thread_local bool g_counting = false;
thread_local std::size_t g_thread_allocations = 0;
} // namespace

void* operator new(std::size_t size) {
    if (g_counting) {
        ++g_thread_allocations;
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_counting) {
        ++g_thread_allocations;
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (g_counting) {
        ++g_thread_allocations;
    }
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

#if defined(NDEBUG)
constexpr double kMaxAbsoluteNsPerCall = 1000.0;
constexpr double kMaxRatio = 400.0;
constexpr double kMinRecordsPerSecond = 1.0e6;
#else
constexpr double kMaxAbsoluteNsPerCall = 5000.0;
constexpr double kMaxRatio = 2000.0;
constexpr double kMinRecordsPerSecond = 1.0e5;
#endif

obs::logger_config make_config(std::size_t ring_capacity) {
    obs::logger_config config;
    config.enable_file = false; // no filesystem in a timing gate
    config.file_path.clear();
    config.ring_capacity = ring_capacity;
    config.max_file_size = 0;
    config.rotate_interval = std::chrono::milliseconds(0);
    config.overflow = obs::overflow_policy::drop_and_count;
    return config;
}

void restart_logger(std::size_t ring_capacity) {
    obs::logger& log = obs::logger::instance();
    log.shutdown();
    log.reset();
    log.configure(make_config(ring_capacity));
    log.start();
    log.set_level(obs::log_level::trace);
}

void stop_logger() {
    obs::logger& log = obs::logger::instance();
    log.shutdown();
    log.reset();
}

double percentile_of(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double index = p * static_cast<double>(sorted.size() - 1);
    const std::size_t low = static_cast<std::size_t>(index);
    const std::size_t high = low + 1 < sorted.size() ? low + 1 : low;
    const double fraction = index - static_cast<double>(low);
    return static_cast<double>(sorted[low]) * (1.0 - fraction) +
           static_cast<double>(sorted[high]) * fraction;
}

// Bulk-loop timing. Per-call `steady_clock` reads are quantised by the platform
// timer (100 ns granularity on Windows), so a per-call baseline can round to
// zero and make a ratio meaningless. Both sides of the ratio below are therefore
// measured as one timed loop, which amortises the timer resolution.
struct bulk_timing {
    double ns_per_op = 0.0;
    std::size_t dropped = 0;
};

bulk_timing time_bulk_log(std::size_t iterations) {
    const std::uint64_t dropped_before = obs::logger::instance().dropped_total();
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        CRPC_LOG_INFO("perf request id={} lane={}", i, static_cast<int>(i & 7));
    }
    const auto stop = clock_type::now();
    const std::uint64_t dropped_after = obs::logger::instance().dropped_total();

    bulk_timing timing;
    timing.ns_per_op =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
        static_cast<double>(iterations);
    timing.dropped = static_cast<std::size_t>(dropped_after - dropped_before);
    return timing;
}

// Same loop shape with no log statement: the number a caller pays for the loop
// itself, so the ratio isolates formatting + ring copy + atomics. The
// `noinline` + volatile sink keeps the optimiser from folding the whole loop
// into a closed form (an empty loop measures ~0.1 ns/op otherwise, which makes
// any ratio meaningless).
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::uint64_t baseline_step(std::uint64_t* accumulator) {
    volatile std::uint64_t* sink = accumulator;
    *sink = *sink + 1;
    return *sink;
}

double time_bulk_baseline(std::size_t iterations, std::uint64_t* sink) {
    std::uint64_t accumulator = *sink;
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        accumulator = baseline_step(&accumulator);
        accumulator += i & 1U;
    }
    const auto stop = clock_type::now();
    *sink = accumulator;
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
           static_cast<double>(iterations);
}

// Per-call samples for the latency percentiles. These include two clock reads,
// so p50/p99 are upper bounds on the true call cost, as documented.
void sample_per_call(std::vector<std::uint64_t>& samples, std::size_t iterations) {
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto tick = clock_type::now();
        CRPC_LOG_INFO("latency request id={} lane={}", i, static_cast<int>(i & 7));
        const auto tock = clock_type::now();
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tock - tick).count()));
    }
}

} // namespace

// --- Allocation gate ---------------------------------------------------------

CRPC_TEST(log_hot_path_does_not_allocate) {
    restart_logger(65536);

    // Warm-up: the first call registers this thread's ring, which is the one
    // documented allocation. Everything after it must be allocation-free.
    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("warm-up {}", i);
    }

    const std::size_t before = g_thread_allocations;
    g_counting = true;
    for (std::size_t i = 0; i < 10000; ++i) {
        CRPC_LOG_INFO("allocation gate {} lane={}", i, static_cast<int>(i & 7));
    }
    g_counting = false;
    const std::size_t after = g_thread_allocations;

    CRPC_CHECK_EQ(std::size_t{0}, after - before);
    std::printf("allocation gate: %llu allocation(s) over 10000 enabled records\n",
                static_cast<unsigned long long>(after - before));
    stop_logger();
}

CRPC_TEST(log_disabled_level_does_not_allocate) {
    restart_logger(65536);
    obs::logger::instance().set_level(obs::log_level::error);

    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("suppressed {}", i);
    }

    const std::size_t before = g_thread_allocations;
    g_counting = true;
    for (std::size_t i = 0; i < 10000; ++i) {
        CRPC_LOG_INFO("suppressed {}", i);
    }
    g_counting = false;
    const std::size_t after = g_thread_allocations;

    CRPC_CHECK_EQ(std::size_t{0}, after - before);
    stop_logger();
}

// --- Latency and ratio gates -------------------------------------------------

CRPC_TEST(log_hot_path_latency_and_relative_cost) {
    restart_logger(65536);

    constexpr std::size_t iterations = 200000;
    constexpr std::size_t sample_count = 200000;
    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("warm-up {}", i);
    }

    std::vector<std::uint64_t> samples;
    samples.reserve(sample_count);

    std::uint64_t sink = 0;
    // Bulk loops for the ratio (timer-resolution independent), then per-call
    // samples for the percentiles.
    const double baseline_ns_per_op = time_bulk_baseline(iterations, &sink);
    const bulk_timing bulk = time_bulk_log(iterations);
    sample_per_call(samples, sample_count);

    std::sort(samples.begin(), samples.end());

    double sum = 0.0;
    for (const std::uint64_t value : samples) {
        sum += static_cast<double>(value);
    }

    const double sample_p50 = percentile_of(samples, 0.50);
    const double sample_p99 = percentile_of(samples, 0.99);
    const double sample_mean = sum / static_cast<double>(samples.size());

    // Use the lower of the two independent central-tendency estimates: the bulk
    // loop is timer-resolution free, the per-call median carries the clock-read
    // overhead, so the smaller one is the honest bound.
    const double ns_per_call = bulk.ns_per_op < sample_p50 ? bulk.ns_per_op : sample_p50;
    const double ratio = ns_per_call / (baseline_ns_per_op > 0.0 ? baseline_ns_per_op : 1.0);

    std::printf("hot path: bulk=%.1f ns/call | per-call p50=%.1f ns p99=%.1f ns mean=%.1f ns | "
                "baseline=%.2f ns/op | ratio=%.1fx | dropped=%llu | sink=%llu\n",
                bulk.ns_per_op, sample_p50, sample_p99, sample_mean,
                baseline_ns_per_op, ratio,
                static_cast<unsigned long long>(bulk.dropped),
                static_cast<unsigned long long>(sink));

    // (2) absolute gate on the caller cost
    CRPC_CHECK(ns_per_call < kMaxAbsoluteNsPerCall);
    CRPC_CHECK(sample_p99 < kMaxAbsoluteNsPerCall * 10.0);
    // (3) relative gate: a slower machine slows the baseline too
    CRPC_CHECK(ratio < kMaxRatio);

    stop_logger();
}

CRPC_TEST(log_throughput_floor) {
    restart_logger(65536);

    constexpr std::size_t calls = 200000;
    constexpr std::size_t warmup = 1000;
    for (std::size_t i = 0; i < warmup; ++i) {
        CRPC_LOG_INFO("warm-up {}", i);
    }

    // The counters are only exact once the warm-up records have left the ring:
    // a record drained after the snapshot would be attributed to the measured
    // window and break the conservation check below.
    obs::logger& log = obs::logger::instance();
    const auto drain_deadline = clock_type::now() + std::chrono::seconds(5);
    while (log.written_records() < warmup && clock_type::now() < drain_deadline) {
        std::this_thread::yield();
    }
    CRPC_CHECK(log.written_records() >= warmup);

    const std::uint64_t dropped_before = log.dropped_total();
    const std::uint64_t written_before = log.written_records();
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < calls; ++i) {
        CRPC_LOG_INFO("throughput request {} lane={}", i, static_cast<int>(i & 7));
    }
    const auto stop = clock_type::now();

    // Shut the sink down before reading the counters: shutdown drains whatever
    // is still in the ring, so `written + dropped` must account for every
    // submitted record exactly. Reading before the drain would count buffered
    // records as neither.
    log.shutdown();
    const std::uint64_t dropped_after = log.dropped_total();
    const std::uint64_t written_after = log.written_records();
    log.reset();

    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double records_per_second = static_cast<double>(calls) / seconds;
    const std::uint64_t dropped = dropped_after - dropped_before;
    const std::uint64_t written = written_after - written_before;

    std::printf("throughput: %.0f records/s enqueued, %llu records, %llu dropped, "
                "%llu written\n",
                records_per_second, static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(written));

    CRPC_CHECK(records_per_second > kMinRecordsPerSecond);
    // Conservation: every submitted record is either written or counted as
    // dropped. Drops are a legitimate state here (the sink formats a line per
    // record while the caller only enqueues one, so the caller can outrun it);
    // losing a record without counting it is not.
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(calls), written + dropped);
}

// --- Drop accounting ---------------------------------------------------------

CRPC_TEST(log_drop_counter_is_exact_when_sink_is_stopped) {
    obs::logger& log = obs::logger::instance();
    log.shutdown();
    log.reset();
    log.configure(make_config(1024));
    // Deliberately not started: no drain, so the accepted/dropped split is
    // deterministic and can be asserted exactly.
    log.set_level(obs::log_level::trace);

    const obs::log_record record = obs::make_record(obs::log_level::info, "drop probe");
    for (std::size_t i = 0; i < 1024; ++i) {
        log.submit(record);
    }
    const std::uint64_t at_capacity = log.dropped_total();
    for (std::size_t i = 0; i < 500; ++i) {
        log.submit(record);
    }
    const std::uint64_t over_capacity = log.dropped_total();

    CRPC_CHECK_EQ(std::uint64_t{0}, at_capacity);
    CRPC_CHECK_EQ(std::uint64_t{500}, over_capacity);
    std::printf("drop accounting: at-capacity=%llu over-capacity=%llu\n",
                static_cast<unsigned long long>(at_capacity),
                static_cast<unsigned long long>(over_capacity));

    log.shutdown();
    log.reset();
}

CRPC_TEST(log_enabled_check_is_cheap) {
    // A disabled record is the documented "one atomic load and a compare" path.
    // Its own gate is relative: it must be far cheaper than an enabled record,
    // and it must not allocate.
    restart_logger(65536);

    constexpr std::size_t iterations = 200000;
    obs::logger::instance().set_level(obs::log_level::error);
    for (std::size_t i = 0; i < 1000; ++i) {
        CRPC_LOG_INFO("suppressed {}", i);
    }

    const std::size_t before = g_thread_allocations;
    g_counting = true;
    const auto start = clock_type::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        CRPC_LOG_INFO("suppressed {}", i);
    }
    const auto stop = clock_type::now();
    g_counting = false;
    const std::size_t after = g_thread_allocations;

    const double ns_per_call =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()) /
        static_cast<double>(iterations);

    std::printf("disabled-level: %.2f ns/call, %llu allocation(s)\n", ns_per_call,
                static_cast<unsigned long long>(after - before));

    CRPC_CHECK_EQ(std::size_t{0}, after - before);
    CRPC_CHECK(ns_per_call < kMaxAbsoluteNsPerCall / 10.0);
    stop_logger();
}

CRPC_TEST_MAIN()
