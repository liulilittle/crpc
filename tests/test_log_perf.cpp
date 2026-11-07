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
#include <cfenv>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
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

// --- Contract-violation guard ------------------------------------------------
//
// reset() forbids other threads logging while it runs (see log.hpp). If a
// thread violates that, the generational guard must turn the dangling cache
// into a counted drop instead of a use-after-free: the first post-reset submit
// from a thread that cached its ring before the reset is dropped and counted
// (exactly one -- the cache invalidates itself), and the following submit
// re-registers. Every assertion here reads a counter, so the case fails
// deterministically when the guard is removed -- not only under ASan.

CRPC_TEST(log_reset_violation_drops_instead_of_crashing) {
    obs::logger& log = obs::logger::instance();
    log.shutdown();
    log.reset();
    log.configure(make_config(1024));
    log.start();
    log.set_level(obs::log_level::trace);

    std::atomic<bool> registered{false};
    std::atomic<bool> release{false};
    std::thread violator([&] {
        CRPC_LOG_INFO("violation warm-up {}", 0);  // registers this thread's ring
        registered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // The main thread reset the logger while this thread's cache was
        // live: the first submit must drop + count (never dereference), the
        // second re-registers and must not crash.
        CRPC_LOG_INFO("violation stale submit {}", 1);
        CRPC_LOG_INFO("violation re-registered submit {}", 2);
    });
    while (!registered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    log.reset();  // the documented violation: another thread is still logging
    release.store(true, std::memory_order_release);
    violator.join();

    // Exactly the stale submit was dropped by the guard; the re-registered
    // submit landed in the fresh ring.
    CRPC_CHECK_EQ(std::uint64_t{1}, log.stale_dropped());

    // Conservation across the violation: of the two post-reset submits, one
    // was dropped by the guard and one was accepted (drained by the
    // synchronous shutdown below).
    log.configure(make_config(1024));  // ensure shutdown drains without a file
    log.shutdown();
    const std::uint64_t written_total = log.written_records();
    log.reset();
    CRPC_CHECK_EQ(std::uint64_t{1}, written_total);
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

// --- %.6g double formatter: differential contract -----------------------------
//
// The hot path formats double fields with detail::format_double_g6 instead of
// snprintf. The replacement is only sound if its output is byte-identical to
// snprintf("%.6g") for every double, so the contract is pinned here against
// the CRT itself: one explicit boundary table (the values where %.6g semantics
// is sharpest) plus a fuzz pool over 1e6 doubles. The pools mix uniform bit
// patterns, dense normal-space sampling, exact/near decimal ties, carry-chain
// structures (999999.5-style), ulp walks around the f/e switch anchors,
// subnormals and the exponent extremes. Deterministic seeds: a failure must
// reproduce, not flicker.

namespace {

void g6_check(double value, std::size_t* checked, std::size_t* failed) {
    char reference[64];
    const int written = std::snprintf(reference, sizeof(reference), "%.6g", value);
    char actual[40];
    const std::size_t count = obs::detail::format_double_g6(actual, sizeof(actual), value);
    ++*checked;
    const bool ok = written > 0 && count == static_cast<std::size_t>(written) &&
                    std::memcmp(actual, reference, count) == 0;
    if (!ok) {
        ++*failed;
        if (*failed <= 8) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            std::printf("  G6 MISMATCH bits=%016llx value=%.17e snprintf=[%s] formatter=[%s]\n",
                        static_cast<unsigned long long>(bits), value, reference, actual);
        }
    }
}

double g6_from_bits(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double g6_next_ulp(double value, int steps) {
    const double huge = 1e308 * 10.0;
    for (int i = 0; i < (steps < 0 ? -steps : steps); ++i) {
        value = steps < 0 ? std::nextafter(value, 0.0) : std::nextafter(value, huge);
        if (!std::isfinite(value)) {
            break;
        }
    }
    return value;
}

struct g6_rng {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t x = state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state = x;
        return x * 0x9E3779B97F4A7C15ull;
    }
    double bit_pattern() { return g6_from_bits(next()); }
    std::uint64_t below(std::uint64_t bound) { return next() % bound; }
};

// Deterministic boundary anchors for the ulp-walk pool: every value where the
// %.6g semantics is known to be knife-edged (f/e switch, carry chains, exact
// powers of ten, tie forms, exponent extremes).
const double* g6_anchor_values(std::size_t* count) {
    static const double anchors[] = {
        0.5,           1.0,          1.5,               0.1,               0.2,
        1.0 / 3.0,     2.0 / 3.0,    1e-5,              9.999995e-5,       1e-4,
        1e-6,          1e-7,         1e5,               100000.5,          100001.5,
        123456.5,      123457.5,     999999.0,          999999.5,          1e6,
        1.0000005e6,   1000005.0,    1500005.0,         5000005.0,         1234567.0,
        1e7,           1e22,         1e23,              1e-22,             1e-23,
        1e-308,        1e308,        DBL_MIN,           DBL_MAX,           DBL_TRUE_MIN,
        385.0 / 64.0,  120000.0,     1.2e-7,            0.000123456789,    0.0};
    *count = sizeof(anchors) / sizeof(anchors[0]);
    return anchors;
}

// Shared boundary corpus: 338 values (explicit list, non-finites, NaN
// payloads, exact ties, f/e switches, and +/-1/2-ulp walks around every exact
// decade) plus the 10 log_buffer leg values = 348 checks. Built once under
// FE_TONEAREST so the four-mode case compares identical bit patterns.
std::vector<double> g6_boundary_values() {
    std::vector<double> values;

    // The task's explicit boundary list, both signs where meaningful.
    const double boundaries[] = {
        0.0,          -0.0,          1.0,          -1.0,          1e-5,
        -1e-5,        9.999995e-5,   -9.999995e-5, 1e-6,          -1e-6,
        999999.5,     -999999.5,     1e6,          -1e6,          1.0000005e6,
        -1.0000005e6, DBL_MIN,       -DBL_MIN,     DBL_MAX,       -DBL_MAX,
        DBL_TRUE_MIN, -DBL_TRUE_MIN, 1e-308,       -1e-308,       1e308,
        -1e308,       1.0 / 3.0,     -1.0 / 3.0};
    values.insert(values.end(), boundaries,
                  boundaries + sizeof(boundaries) / sizeof(boundaries[0]));

    // infinities as explicit bit patterns (a literal 1.0/0.0 is a compile-time
    // divide-by-zero under /W4).
    values.push_back(g6_from_bits(0x7FF0000000000000ull));
    values.push_back(g6_from_bits(0xFFF0000000000000ull));

    // NaN payloads: the CRT spelling depends on payload and sign, so every
    // form must round-trip through the same fallback as the real formatter.
    const std::uint64_t nan_patterns[] = {0x7ff8000000000000ull, 0xfff8000000000000ull,
                                          0x7ff0000000000001ull, 0xfff0000000000001ull,
                                          0x7ff8deadbeef0000ull, 0xfff8deadbeef0000ull,
                                          0x7ff4000000000000ull};
    for (const std::uint64_t bits : nan_patterns) {
        values.push_back(g6_from_bits(bits));
    }

    // Tie forms (exact .5 at the 7th significant digit): half-even on the CRT
    // side, the formatter must agree byte-for-byte.
    const double ties[] = {123456.5,  123457.5, 100000.5, 100001.5, 385.0 / 64.0,
                           5000005.0, 1500005.0, 999999.5, -999999.5, 12.5,
                           0.5,       1.5,      2.5,      3.5,       4.5};
    values.insert(values.end(), ties, ties + sizeof(ties) / sizeof(ties[0]));

    // f/e switch right at the carry edge, both directions of the 7th digit.
    const double switches[] = {9.999995e-5,       9.99999999e-5,      0.0001,            0.000123456789,
                               999999.0,          999999.99999999988, 99999.99999999999, 100000.0,
                               999999.49999999,   1000000.49999,      123456.499999999985, 120000.0,
                               1.2e-7,            1.0 / 3.0,          2.0 / 3.0,         1e23};
    values.insert(values.end(), switches,
                  switches + sizeof(switches) / sizeof(switches[0]));

    // Exact powers of ten across the whole exponent range: these are the
    // k-estimation edge cases (the fast path's decade classification is
    // computed from the binary exponent, and exact decades sit on the edge).
    for (int j = -22; j <= 22; ++j) {
        const double power = std::pow(10.0, static_cast<double>(j));
        values.push_back(power);
        values.push_back(-power);
        values.push_back(g6_next_ulp(power, -1));
        values.push_back(g6_next_ulp(power, 1));
        values.push_back(g6_next_ulp(power, -2));
        values.push_back(g6_next_ulp(power, 2));
    }
    return values;
}

void g6_check_buffer_leg(std::size_t* checked, std::size_t* failed) {
    // End-to-end through log_buffer: the append_float integration must hold
    // the same byte-identity (this is the call the logger actually makes).
    const double through_buffer[] = {3.14159265358979, 2.718281828459045, 0.0, -0.0,
                                     1e-5, 999999.5, 1e6, DBL_MIN, DBL_MAX, 1.0 / 3.0};
    for (const double value : through_buffer) {
        char reference[64];
        std::snprintf(reference, sizeof(reference), "%.6g", value);
        char buffer[64];
        obs::log_buffer out(buffer, sizeof(buffer));
        out.append_float(value);
        const std::size_t count = out.size();
        ++*checked;
        if (count != std::strlen(reference) || std::memcmp(buffer, reference, count) != 0) {
            ++*failed;
            std::printf("  G6 BUFFER MISMATCH value=%.17e snprintf=[%s] buffer=[%.*s]\n", value,
                        reference, static_cast<int>(count), buffer);
        }
    }
}

void g6_check_boundary_corpus(const std::vector<double>& values, std::size_t* checked,
                              std::size_t* failed) {
    for (const double value : values) {
        g6_check(value, checked, failed);
    }
    g6_check_buffer_leg(checked, failed);
}

} // namespace

CRPC_TEST(log_double_formatter_matches_snprintf_boundary_values) {
    std::size_t checked = 0;
    std::size_t failed = 0;

    const std::vector<double> values = g6_boundary_values();
    g6_check_boundary_corpus(values, &checked, &failed);

    std::printf("g6 boundary differential: %zu checked, %zu failures\n", checked, failed);
    CRPC_CHECK_EQ(std::size_t{0}, failed);
}

CRPC_TEST(log_double_formatter_matches_snprintf_fuzz_over_1m) {
    std::size_t checked = 0;
    std::size_t failed = 0;
    g6_rng rng{0x0B5EEDC0FFEEULL};

    // Pool A: uniform bit patterns (includes non-finites and subnormals at
    // their natural frequency -- every pool entry is checked either way).
    for (std::size_t i = 0; i < 400000; ++i) {
        g6_check(rng.bit_pattern(), &checked, &failed);
    }

    // Pool B: dense normal-space sampling (random exponent field, random
    // mantissa), sign toggled per value.
    for (std::size_t i = 0; i < 150000; ++i) {
        const std::uint64_t exponent = static_cast<std::uint64_t>(rng.below(2046)) + 1;
        const std::uint64_t bits =
            (exponent << 52) | (rng.next() & 0xFFFFFFFFFFFFFull) | ((rng.next() & 1) << 63);
        g6_check(g6_from_bits(bits), &checked, &failed);
    }

    // Pool C: decimal structures -- N, N.5 and 7-digit forms scaled by exact
    // decades. These are the carry-chain shapes (999999.5 * 10^j families)
    // where %.6g switches between %f and %e or gains a digit.
    for (std::size_t i = 0; i < 150000; ++i) {
        const int j = static_cast<int>(rng.below(31)) - 15;
        const std::uint64_t shape = rng.below(4);
        const std::uint64_t n = 1 + rng.below(2000000);
        double value;
        switch (shape) {
        case 0:
            value = static_cast<double>(n);
            break;
        case 1:
            value = static_cast<double>(n) + 0.5;
            break;
        case 2:
            value = static_cast<double>(n * 10 + 5);
            break;
        default:
            value = static_cast<double>(n) / 2.0;
            break;
        }
        const double scaled = value * std::pow(10.0, static_cast<double>(j));
        g6_check(rng.next() & 1 ? scaled : -scaled, &checked, &failed);
    }

    // Pool D: ulp walks around the anchors (both directions, 1..64 ulps),
    // which sweeps the rounding-boundary neighborhoods hard enough to catch an
    // off-by-one-ulp digit decision.
    std::size_t anchor_count = 0;
    const double* anchors = g6_anchor_values(&anchor_count);
    for (std::size_t i = 0; i < 100000; ++i) {
        const double anchor = anchors[rng.below(anchor_count)];
        const int direction = (rng.next() & 1) ? 1 : -1;
        const int steps = 1 + static_cast<int>(rng.below(64));
        g6_check(g6_next_ulp(anchor, direction * steps), &checked, &failed);
    }

    // Pool E: subnormal sweep -- dense sampling of the first 2^52 bit
    // patterns (every subnormal), plus the DBL_MIN boundary from both sides.
    for (std::size_t i = 0; i < 100000; ++i) {
        g6_check(g6_from_bits(rng.below(0x10000000000000ull)), &checked, &failed);
    }
    for (std::uint64_t bits = 0x000FFFFFFFFFFFF0ull; bits <= 0x001000000000000Full; ++bits) {
        g6_check(g6_from_bits(bits), &checked, &failed);
        g6_check(g6_from_bits(bits | 0x8000000000000000ull), &checked, &failed);
    }

    // Pool F: exponent extremes -- DBL_MAX, DBL_MIN and denorm_min
    // neighborhoods, both signs (the decade estimate and the band loop are
    // most stressed here).
    for (std::size_t i = 0; i < 50000; ++i) {
        const std::uint64_t sign = (rng.next() & 1) << 63;
        g6_check(g6_from_bits((0x7FEFFFFFFFFFFFFFull - rng.below(4096)) | sign), &checked,
                 &failed);
        g6_check(g6_from_bits((0x0010000000000000ull + rng.below(4096)) | sign), &checked,
                 &failed);
        g6_check(g6_from_bits((0x0000000000000001ull + rng.below(4096)) | sign), &checked,
                 &failed);
    }

    // Pool G: exact binary fractions i/2^j -- long finite decimal expansions
    // whose digits past the 6th are exercised deterministically.
    for (std::size_t i = 0; i < 100000; ++i) {
        const int j = static_cast<int>(rng.below(60));
        const double value =
            static_cast<double>(rng.below(1000000000000ull)) / std::pow(2.0, static_cast<double>(j));
        g6_check(rng.next() & 1 ? value : -value, &checked, &failed);
    }

    std::printf("g6 fuzz differential: %zu checked, %zu failures\n", checked, failed);
    CRPC_CHECK(checked >= 1000000);
    CRPC_CHECK_EQ(std::size_t{0}, failed);
}

// --- %.6g under every fenv rounding mode -------------------------------------
//
// The contract is same-mode byte identity: whatever the current rounding
// direction is, format_double_g6 must print exactly what snprintf("%.6g")
// prints under that same mode. Direct rounding changes %g's result for almost
// every value (snprintf rounds the 6th significant digit up/down/truncate
// whenever any digit is discarded, instead of half-even at a tie), so the
// formatter's fast path -- which decides one 7th-digit rounding -- is only
// sound under FE_TONEAREST and must delegate otherwise. This case pins both
// the delegation and the platform facts:
//
//   * fesetround is actually applied (fegetround echoes it, 1+2^-53 moves);
//   * the CRT's printf either follows the mode or not -- a platform fact, not
//     a failure: same-mode byte identity holds either way;
//   * 348 boundary checks + 100k random values per mode compare byte-for-byte.
//
// Probe on this platform (opt-float2/probe_fenv.cpp): UCRT 14.51 x64 and
// glibc 2.39 both honor fesetround in printf, e.g. 123456.5 prints
// "123456" (nearest) / "123457" (upward) / "123456" (downward) / "123456"
// (toward zero), and the ungated fast path mismatched ~50% of 200k random
// values in each directed mode.
CRPC_TEST(log_double_formatter_matches_snprintf_under_every_fenv_rounding_mode) {
    struct mode_entry {
        int mode;
        const char* name;
    };
    const mode_entry modes[] = {
        {FE_TONEAREST, "FE_TONEAREST"},
        {FE_UPWARD, "FE_UPWARD"},
        {FE_DOWNWARD, "FE_DOWNWARD"},
        {FE_TOWARDZERO, "FE_TOWARDZERO"},
    };

    const int saved_mode = std::fegetround();

    // One corpus and one random pool, built under round-to-nearest so every
    // mode formats the exact same bit patterns (pow/nextafter/division run
    // once, not once per mode).
    CRPC_CHECK_EQ(0, std::fesetround(FE_TONEAREST));
    const std::vector<double> boundaries = g6_boundary_values();

    std::vector<double> random_values;
    random_values.reserve(100000);
    g6_rng rng{0xFEEDFACE5EEDULL};
    // Uniform bit patterns: includes non-finites and subnormals at their
    // natural frequency.
    for (std::size_t i = 0; i < 50000; ++i) {
        random_values.push_back(rng.bit_pattern());
    }
    // Dense normal-space sampling, sign toggled per value.
    for (std::size_t i = 0; i < 30000; ++i) {
        const std::uint64_t exponent = static_cast<std::uint64_t>(rng.below(2046)) + 1;
        const std::uint64_t bits =
            (exponent << 52) | (rng.next() & 0xFFFFFFFFFFFFFull) | ((rng.next() & 1) << 63);
        random_values.push_back(g6_from_bits(bits));
    }
    // Decimal shapes (N.5 and 7-digit forms scaled by exact decades): the
    // carry-chain families where the 6th/7th digit is knife-edged.
    for (std::size_t i = 0; i < 20000; ++i) {
        const int j = static_cast<int>(rng.below(31)) - 15;
        const std::uint64_t n = 1 + rng.below(2000000);
        const double shape = (rng.next() & 1) != 0 ? static_cast<double>(n) + 0.5
                                                   : static_cast<double>(n * 10 + 5);
        const double scaled = shape * std::pow(10.0, static_cast<double>(j));
        random_values.push_back((rng.next() & 1) != 0 ? scaled : -scaled);
    }

    char tie_text[4][64] = {};
    for (std::size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); ++index) {
        const mode_entry& entry = modes[index];
        const int set_rc = std::fesetround(entry.mode);
        const int echo = std::fegetround();
        std::snprintf(tie_text[index], sizeof(tie_text[index]), "%.6g", 123456.5);

        // Does the hardware/CRT actually change arithmetic under this mode?
        // volatile operands keep the compiler from folding 1.0 + 2^-53.
        volatile double one = 1.0;
        volatile double half_ulp = 0x1p-53;
        volatile double neg_one = -1.0;
        const double sum = one + half_ulp;
        const double neg_sum = neg_one - half_ulp;

        std::size_t checked = 0;
        std::size_t failed = 0;
        g6_check_boundary_corpus(boundaries, &checked, &failed);
        for (const double value : random_values) {
            g6_check(value, &checked, &failed);
        }

        std::printf(
            "[%s] fesetround_rc=%d fegetround=%d(%s) 1+2^-53=%.17g -1-2^-53=%.17g "
            "snprintf_tie_123456.5=[%s] checked=%zu failures=%zu\n",
            entry.name, set_rc, echo, echo == entry.mode ? "echo-ok" : "echo-MISMATCH", sum,
            neg_sum, tie_text[index], checked, failed);

        CRPC_CHECK_EQ(0, set_rc);
        CRPC_CHECK_EQ(entry.mode, echo);
        CRPC_CHECK(checked >= 100000);
        CRPC_CHECK_EQ(std::size_t{0}, failed);
    }

    // Platform difference, recorded not asserted: a CRT whose printf ignores
    // fesetround is still contract-conforming on its own platform, because the
    // formatter delegates to that same snprintf.
    const bool printf_follows_mode = std::strcmp(tie_text[0], tie_text[1]) != 0 ||
                                     std::strcmp(tie_text[0], tie_text[2]) != 0 ||
                                     std::strcmp(tie_text[0], tie_text[3]) != 0;
    std::printf("platform fact: snprintf follows fesetround = %s\n",
                printf_follows_mode ? "yes" : "no");

    CRPC_CHECK_EQ(0, std::fesetround(saved_mode));
    CRPC_CHECK_EQ(saved_mode, std::fegetround());
}

CRPC_TEST_MAIN()
