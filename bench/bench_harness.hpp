// CRPC -- benchmark harness.
//
// Rules this harness follows, because a benchmark that breaks them produces
// numbers nobody can act on:
//
//   * every case is warmed up before it is measured, so the first-touch page
//     faults and the cold branch predictors do not land in the sample;
//   * each case is run `runs` times and the **median** is reported, not the
//     best. A best-of-N number describes a machine that does not exist;
//   * the harness reports the environment (compiler, build type, CPU) next to
//     the numbers, because a number without its environment is not a
//     measurement;
//   * nothing is extrapolated. If a case measures a round trip, the result is
//     labelled as a round trip; it is never converted into "RPC/s" by dividing
//     by a message count that was never actually exercised.
#ifndef CRPC_BENCH_HARNESS_HPP
#define CRPC_BENCH_HARNESS_HPP

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace crpc_bench {

struct result {
    std::string name;
    std::string unit;      // "ns/op", "ops/s", "MiB/s"
    double value = 0.0;
    double spread = 0.0;   // max-min, to show stability
};

inline std::vector<result>& results() {
    static std::vector<result> list;
    return list;
}

inline const char*& filter() {
    static const char* value = nullptr;
    return value;
}

inline bool enabled(const char* name) {
    const char* f = filter();
    return f == nullptr || std::string(name).find(f) != std::string::npos;
}

// Runs `body` `iterations` times, `runs` times, and records the median ns/op.
template <class Body>
void measure(const char* name, std::size_t iterations, int runs, Body&& body) {
    if (!enabled(name)) {
        return;
    }
    // Warm-up: one full pass, discarded.
    body(iterations);

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(runs));
    for (int r = 0; r < runs; ++r) {
        const auto start = std::chrono::steady_clock::now();
        body(iterations);
        const auto stop = std::chrono::steady_clock::now();
        const double ns =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                    .count());
        samples.push_back(ns / static_cast<double>(iterations));
    }
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    const double spread = samples.back() - samples.front();

    result r;
    r.name = name;
    r.unit = "ns/op";
    r.value = median;
    r.spread = spread;
    results().push_back(r);

    std::printf("%-46s %10.1f ns/op   (spread %.1f, runs %d)\n", name, median,
                spread, runs);
    std::fflush(stdout);
}

// Records a throughput-style number the caller has already computed. Used where
// ns/op would be misleading (for example a batched transfer).
inline void record(const char* name, const char* unit, double value,
                   double spread = 0.0) {
    if (!enabled(name)) {
        return;
    }
    result r;
    r.name = name;
    r.unit = unit;
    r.value = value;
    r.spread = spread;
    results().push_back(r);
    std::printf("%-46s %10.2f %s\n", name, value, unit);
    std::fflush(stdout);
}

inline void print_environment() {
    std::printf("=== CRPC benchmark ===\n");
#if defined(_MSC_VER)
    std::printf("compiler: MSVC %d\n", _MSC_VER);
#elif defined(__clang__)
    std::printf("compiler: Clang %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__)
    std::printf("compiler: GCC %d.%d\n", __GNUC__, __GNUC_MINOR__);
#endif
#if defined(NDEBUG)
    std::printf("build: Release (NDEBUG)\n");
#else
    std::printf("build: Debug (assertions enabled, results not representative)\n");
#endif
#if defined(CRPC_ARCH_X64)
    std::printf("arch: x64\n");
#elif defined(CRPC_ARCH_X86)
    std::printf("arch: x86\n");
#elif defined(CRPC_ARCH_ARM64)
    std::printf("arch: arm64\n");
#endif
    std::printf("note: single-process loopback unless the case name says otherwise\n\n");
}

// Defined once per benchmark binary by the benchmark file itself.
void run_all_cases();

inline int summary() {
    std::printf("\n%d case(s) recorded\n", static_cast<int>(results().size()));
    return 0;
}

} // namespace crpc_bench

#define CRPC_BENCH_MAIN()                                                      \
    int main(int argc, char** argv) {                                          \
        ::crpc_bench::filter() = argc > 1 ? argv[1] : nullptr;                 \
        ::crpc_bench::print_environment();                                     \
        ::run_all_cases();                                         \
        return ::crpc_bench::summary();                                        \
    }

#endif // CRPC_BENCH_HARNESS_HPP

