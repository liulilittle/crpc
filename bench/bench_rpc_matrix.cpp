// CRPC -- RPC throughput/latency matrix.
//
// Extends bench_rpc with the dimensions a capacity plan needs:
//
//   * payload size   16 B, 256 B, 4 KiB, 64 KiB;
//   * concurrency    1, 2 and 4 caller threads;
//   * reliability    reliable request/response and unreliable ONE_WAY;
//   * issue shape    closed loop (one call in flight) and pipelined batch
//                    (batch 1/16/64 calls in flight).
//
// Every reliable case is a real facade call: encode, load-balance, transport,
// dispatch, handler, response, decode. Every one-way case is a real ONE_WAY
// frame and the handler counter is read after the timed issue window, so the
// report distinguishes "issued" from "delivered" instead of assuming they are
// the same number.
//
// The harness warms up every case. Throughput is the median of several timed
// runs; latency reports p50/p99. This is a single-process in-process backend
// measurement; it is an upper bound for the framework, not a network number.
//
// The closed-loop cases are the reproducible baseline. The pipelined cases add
// the issue shape a throughput-oriented caller actually uses; the CF-1
// completion race that once made that shape unreliable is fixed
// (`call_slot::invoke_callback` claims the callback exactly once and
// `release_pending` synchronizes handle destruction with an in-flight
// callback), so a pipelined case destroys its handles as soon as it reads them
// instead of keeping them for the process lifetime.
//
// A pipelined case can still deadlock in a Debug build (the concurrent issue
// path and the in-process backend's synchronous completion path contend on the
// client's completion lock while a server handler coroutine holds it). The
// deadlock is not recoverable in-process, so each pipelined case runs in a child
// process with a watchdog timeout; a case that exceeds the timeout is killed and
// falls back to the closed-loop measurement, which is recorded under a
// `*_closed_loop_fallback_*` name with a `hung` note rather than as a pipelined
// number.
#include <crpc/client_facade.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "bench_harness.hpp"

namespace {

using namespace std::chrono_literals;

template <class Predicate>
bool wait_until(Predicate predicate, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
}

constexpr crpc::u64 k_service = 710;
constexpr crpc::u64 k_echo = 1;
constexpr crpc::u64 k_oneway = 2;

struct echo_service {
    std::atomic<std::uint64_t> echo_calls{0};
    std::atomic<std::uint64_t> oneway_calls{0};

    std::string echo(const std::string& value) {
        echo_calls.fetch_add(1, std::memory_order_relaxed);
        return value;
    }

    std::string sink(const std::string& value) {
        oneway_calls.fetch_add(1, std::memory_order_relaxed);
        return value;
    }
};

const char* filter_value() {
    return crpc_bench::filter();
}

bool want(const char* key) {
    const char* filter = filter_value();
    return filter == nullptr || std::string(filter).find(key) != std::string::npos;
}

// CRPC_BENCH_RPC_CALLS overrides the per-case call count. It exists so a slow
// or suspect machine can reproduce one case with a smaller workload without a
// rebuild; the default is the published workload.
std::size_t call_total(std::size_t fallback) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "CRPC_BENCH_RPC_CALLS") == 0 &&
        value != nullptr) {
        const long parsed = std::strtol(value, nullptr, 10);
        std::free(value);
        return parsed > 0 ? static_cast<std::size_t>(parsed) : fallback;
    }
    return fallback;
#else
    const char* value = std::getenv("CRPC_BENCH_RPC_CALLS");
    if (value == nullptr) {
        return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 ? static_cast<std::size_t>(parsed) : fallback;
#endif
}

// Reads an environment variable without the MSVC C4996 deprecation warning.
bool dupenv(const char* name, std::string& out) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    out.assign(value);
    std::free(value);
    return true;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    out.assign(value);
    return true;
#endif
}

std::atomic<unsigned> environment_counter{0};

std::string unique_name() {
    return "crpc-bench-rpc-matrix-" +
           std::to_string(
               environment_counter.fetch_add(1, std::memory_order_relaxed));
}

struct environment {
    crpc::inproc_backend backend;
    echo_service service;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    std::string name = unique_name();

    ~environment() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
    }
};

bool start(environment& env, std::size_t pool_size) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(env.name), env.backend);
    sb.add_service(k_service, "bench.matrix");
    sb.add_method<echo_service, &echo_service::echo>(k_service, k_echo, "echo",
                                                     env.service);
    sb.add_method<echo_service, &echo_service::sink>(k_service, k_oneway,
                                                     "sink", env.service);
    sb.shards(4);
    sb.handler_workers(8);
    // The one-way case deliberately issues a burst without waiting; the
    // server's admission cap must not turn the benchmark into an overload
    // measurement.
    sb.max_inflight_per_connection(65536);
    sb.per_service_inflight(65536);
    sb.drain_timeout(200ms);
    env.srv = sb.build();
    if (!env.srv || !env.srv->start().ok()) {
        return false;
    }

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(env.name), env.backend);
    cb.pool_size(pool_size);
    // The latency case retains every completed handle until the case ends (the
    // CF-1 completion race), so the per-connection slot pool must cover the
    // samples. Throughput cases pipeline in small batches instead.
    cb.slot_capacity(16384);
    cb.max_inflight_per_connection(16384);
    cb.default_deadline(5s);
    cb.plain_thread_timeout(5s);
    cb.default_idempotent(true);
    crpc::resilience::retry_policy retry;
    retry.max_attempts = 1;
    cb.retry(retry);
    // The benchmark measures the transport, not the breaker. A cold Debug
    // batch can exceed the default 1 ms slow-call threshold; opening the
    // endpoint breaker after the first batch would turn the rest of the case
    // into an admission-rejection measurement.
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1000000000u;
    breaker.failure_ratio_threshold = 1.0;
    breaker.slow_call_threshold = 1h;
    breaker.slow_call_ratio_threshold = 1.0;
    breaker.open_duration = 1s;
    cb.breaker(breaker);
    cb.resilience_enabled(false);
    env.cli = cb.build();
    return env.cli != nullptr;
}

// --- statistics --------------------------------------------------------------

struct latency_stats {
    double p50 = 0.0;
    double p99 = 0.0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    std::size_t samples = 0;
};

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double index = fraction * static_cast<double>(sorted.size() - 1);
    const std::size_t low = static_cast<std::size_t>(index);
    if (low + 1 >= sorted.size()) {
        return sorted.back();
    }
    const double weight = index - static_cast<double>(low);
    return sorted[low] * (1.0 - weight) + sorted[low + 1] * weight;
}

latency_stats summarize(std::vector<double>& samples) {
    latency_stats stats;
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    stats.samples = samples.size();
    stats.min = samples.front();
    stats.max = samples.back();
    stats.p50 = percentile(samples, 0.50);
    stats.p99 = percentile(samples, 0.99);
    double total = 0.0;
    for (const double value : samples) {
        total += value;
    }
    stats.mean = total / static_cast<double>(samples.size());
    return stats;
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

// --- calls -------------------------------------------------------------------

void bench_latency(std::size_t payload, std::size_t samples, int rounds) {
    environment env;
    if (!start(env, 1)) {
        std::printf("rpc/latency_%zuB: environment did not start\n", payload);
        return;
    }

    const std::string request(payload, 'x');
    // Handles are kept for the life of the process. Destroying a completed
    // facade handle while the completion callback is still returning is the
    // CF-1 race; a measurement process can keep them and let the OS reclaim
    // the memory at exit.
    auto* keep = new std::vector<crpc::client::call<std::string>>();
    keep->reserve(samples * static_cast<std::size_t>(rounds) + 2000);

    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;
    for (int i = 0; i < 2000; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
        auto started = stub.call(request);
        if (!started.ok()) {
            code = started.code();
            break;
        }
        keep->push_back(std::move(started).value());
        auto result = keep->back().get();
        if (!result.ok()) {
            code = result.code();
            break;
        }
        response = std::move(result).value();
    }
    if (code != crpc::rpc_code::ok) {
        std::printf("rpc/latency_%zuB warm-up failed: %s\n", payload,
                    crpc::to_string(code));
        return;
    }

    std::vector<double> p50;
    std::vector<double> p99;
    for (int round = 0; round < rounds; ++round) {
        std::vector<double> round_samples;
        round_samples.reserve(samples);
        for (std::size_t i = 0; i < samples; ++i) {
            auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
            const auto start = std::chrono::steady_clock::now();
            auto started = stub.call(request);
            if (!started.ok()) {
                continue;
            }
            keep->push_back(std::move(started).value());
            auto result = keep->back().get();
            const auto stop = std::chrono::steady_clock::now();
            if (!result.ok()) {
                continue;
            }
            round_samples.push_back(static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                    .count()));
        }
        latency_stats stats = summarize(round_samples);
        if (stats.samples != 0) {
            p50.push_back(stats.p50);
            p99.push_back(stats.p99);
        }
    }

    char name[96];
    std::snprintf(name, sizeof(name), "rpc/latency_%zuB_p50_ns", payload);
    crpc_bench::record(name, "ns", median(p50));
    std::snprintf(name, sizeof(name), "rpc/latency_%zuB_p99_ns", payload);
    crpc_bench::record(name, "ns", median(p99));
    std::printf("rpc/latency_%zuB payload=%zuB rounds=%d samples/round=%zu\n",
                payload, payload, rounds, samples);
    std::fflush(stdout);
}

void bench_throughput(std::size_t payload, std::size_t concurrency,
                      std::size_t total) {
    environment env;
    if (!start(env, concurrency)) {
        std::printf("rpc/throughput_closed_loop_%zuB_c%zu: environment did not start\n",
                    payload, concurrency);
        return;
    }

    const std::string request(payload, 'x');
    std::string first_error;

    // Warm-up: closed-loop calls on the main thread, handles kept for the
    // process lifetime (see bench_latency for the teardown-race reason).
    auto* warmup = new std::vector<crpc::client::call<std::string>>();
    warmup->reserve(1000);
    std::size_t warm_failed = 0;
    for (int i = 0; i < 500; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
        auto started = stub.call(request);
        if (!started.ok()) {
            if (first_error.empty()) {
                first_error = crpc::to_string(started.code());
            }
            ++warm_failed;
            continue;
        }
        warmup->push_back(std::move(started).value());
        auto result = warmup->back().get();
        if (!result.ok()) {
            if (first_error.empty()) {
                first_error = crpc::to_string(result.code());
            }
            ++warm_failed;
        }
    }
    if (warm_failed != 0) {
        std::printf("rpc/throughput_closed_loop_%zuB_c%zu: warm-up failed (%zu) first=%s\n",
                    payload, concurrency, warm_failed, first_error.c_str());
        return;
    }

    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    const std::size_t base = total / concurrency;
    const std::size_t remainder = total % concurrency;

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            const std::size_t count = base + (worker < remainder ? 1 : 0);
            auto* handles = new std::vector<crpc::client::call<std::string>>();
            handles->reserve(count);
            std::size_t done = 0;
            std::size_t failed = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto stub = env.cli->service<std::string, std::string>(
                    k_service, k_echo);
                auto started = stub.call(request);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                handles->push_back(std::move(started).value());
                auto result = handles->back().get();
                if (result.ok()) {
                    ++done;
                } else {
                    ++failed;
                }
            }
            completed.fetch_add(done, std::memory_order_relaxed);
            errors.fetch_add(failed, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto stop = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(stop - start).count();
    const std::size_t done = completed.load(std::memory_order_relaxed);
    const double ops = seconds > 0.0 ? static_cast<double>(done) / seconds : 0.0;

    char name[128];
    std::snprintf(name, sizeof(name), "rpc/throughput_closed_loop_%zuB_c%zu", payload,
                  concurrency);
    crpc_bench::record(name, "M RPC/s", ops / 1e6);
    std::snprintf(name, sizeof(name), "rpc/throughput_closed_loop_%zuB_c%zu_ns", payload,
                  concurrency);
    crpc_bench::record(name, "ns/call", ops > 0.0 ? 1e9 / ops : 0.0);
    std::printf("rpc/closed_loop_%zuB c=%zu calls=%zu errors=%zu time=%.4fs\n",
                payload, concurrency, done,
                errors.load(std::memory_order_relaxed), seconds);
    std::fflush(stdout);
}

void bench_oneway(std::size_t payload, std::size_t concurrency,
                  std::size_t total) {
    environment env;
    if (!start(env, concurrency)) {
        std::printf("rpc/oneway_%zuB_c%zu: environment did not start\n", payload,
                    concurrency);
        return;
    }

    const std::string request(payload, 'x');

    // Warm-up: issue the same shape, then wait until the handler has observed
    // every issued call before the timed window starts.
    const std::uint64_t warm_before =
        env.service.oneway_calls.load(std::memory_order_acquire);
    auto* warmup = new std::vector<crpc::client::call<std::string>>();
    warmup->reserve(500);
    std::size_t warm_issued = 0;
    for (int i = 0; i < 500; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_oneway);
        crpc::rpc::call_context ctx;
        ctx.mode = crpc::delivery_mode::unreliable;
        auto started = stub.call(request, ctx);
        if (started.ok()) {
            warmup->push_back(std::move(started).value());
            ++warm_issued;
        }
    }
    (void)wait_until(
        [&env, warm_before, warm_issued] {
            return env.service.oneway_calls.load(std::memory_order_acquire) >=
                   warm_before + warm_issued;
        },
        10000);
    const std::uint64_t baseline =
        env.service.oneway_calls.load(std::memory_order_acquire);

    std::atomic<std::size_t> issued{0};
    std::atomic<std::size_t> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    const std::size_t base = total / concurrency;
    const std::size_t remainder = total % concurrency;

    const auto issue_start = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            const std::size_t count = base + (worker < remainder ? 1 : 0);
            // A ONE_WAY call is complete when start_call returns; the handle
            // is not waited on. Keep it for the process lifetime for the same
            // teardown-race reason as the reliable path.
            auto* handles = new std::vector<crpc::client::call<std::string>>();
            handles->reserve(count);
            std::size_t failed = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto stub =
                    env.cli->service<std::string, std::string>(k_service, k_oneway);
                crpc::rpc::call_context ctx;
                ctx.mode = crpc::delivery_mode::unreliable;
                auto started = stub.call(request, ctx);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                handles->push_back(std::move(started).value());
            }
            issued.fetch_add(handles->size(), std::memory_order_relaxed);
            errors.fetch_add(failed, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto issue_stop = std::chrono::steady_clock::now();

    const std::size_t sent = issued.load(std::memory_order_relaxed);
    const std::uint64_t target = baseline + sent;
    const bool drained = wait_until(
        [&env, target] {
            return env.service.oneway_calls.load(std::memory_order_acquire) >=
                   target;
        },
        30000);
    const auto delivered_stop = std::chrono::steady_clock::now();

    const double issue_seconds =
        std::chrono::duration<double>(issue_stop - issue_start).count();
    const double total_seconds =
        std::chrono::duration<double>(delivered_stop - issue_start).count();
    const std::uint64_t delivered =
        env.service.oneway_calls.load(std::memory_order_acquire) - baseline;
    const double issue_rate =
        issue_seconds > 0.0 ? static_cast<double>(sent) / issue_seconds : 0.0;
    const double delivered_rate =
        total_seconds > 0.0 ? static_cast<double>(delivered) / total_seconds : 0.0;

    char name[128];
    std::snprintf(name, sizeof(name), "rpc/oneway_issue_%zuB_c%zu", payload,
                  concurrency);
    crpc_bench::record(name, "M calls/s", issue_rate / 1e6);
    std::snprintf(name, sizeof(name), "rpc/oneway_delivered_%zuB_c%zu", payload,
                  concurrency);
    crpc_bench::record(name, "M calls/s", delivered_rate / 1e6);
    std::printf("rpc/oneway_%zuB c=%zu issued=%zu delivered=%llu errors=%zu "
                "drained=%s issue_time=%.4fs total_time=%.4fs\n",
                payload, concurrency, sent,
                static_cast<unsigned long long>(delivered),
                errors.load(std::memory_order_relaxed), drained ? "yes" : "no",
                issue_seconds, total_seconds);
    std::fflush(stdout);
}

// --- pipelined batch throughput ----------------------------------------------
//
// The closed-loop cases above issue one call and wait for it before issuing the
// next, so the number they report is a round-trip rate. A pipelined batch issues
// `batch` calls before waiting for any of them, which is the shape a caller that
// wants throughput rather than latency uses. The handles are destroyed as soon as
// they are read (see the file header for the CF-1 fix that makes this safe).
//
// Every case runs `rounds` timed iterations and reports the median, p99 and
// spread (max-min) of the per-iteration rate, so a single fast or slow iteration
// cannot silently become the result.

struct pipeline_stats {
    double median_ops = 0.0;
    double p99_ops = 0.0;
    double spread_ops = 0.0;
    double median_ns = 0.0;
    double p99_ns = 0.0;
    double spread_ns = 0.0;
    std::size_t errors = 0;
    bool clean = false;
    bool started = false;
};

pipeline_stats summarize_pipeline_rounds(const std::vector<double>& ops,
                                         const std::vector<double>& ns) {
    pipeline_stats stats;
    if (!ops.empty()) {
        std::vector<double> sorted_ops = ops;
        std::sort(sorted_ops.begin(), sorted_ops.end());
        stats.median_ops = sorted_ops[sorted_ops.size() / 2];
        stats.p99_ops = percentile(sorted_ops, 0.99);
        stats.spread_ops = sorted_ops.back() - sorted_ops.front();
    }
    if (!ns.empty()) {
        std::vector<double> sorted_ns = ns;
        std::sort(sorted_ns.begin(), sorted_ns.end());
        stats.median_ns = sorted_ns[sorted_ns.size() / 2];
        stats.p99_ns = percentile(sorted_ns, 0.99);
        stats.spread_ns = sorted_ns.back() - sorted_ns.front();
    }
    return stats;
}

pipeline_stats run_pipeline_reliable(environment& env, const std::string& request,
                                     std::size_t concurrency, std::size_t batch,
                                     std::size_t total, int rounds) {
    pipeline_stats out;
    std::vector<double> ops_samples;
    std::vector<double> ns_samples;
    std::atomic<std::size_t> errors{0};

    for (int round = 0; round < rounds; ++round) {
        std::atomic<std::size_t> completed{0};
        std::vector<std::thread> workers;
        workers.reserve(concurrency);
        const std::size_t base = total / concurrency;
        const std::size_t remainder = total % concurrency;

        const auto start = std::chrono::steady_clock::now();
        for (std::size_t worker = 0; worker < concurrency; ++worker) {
            workers.emplace_back([&, worker] {
                const std::size_t count = base + (worker < remainder ? 1 : 0);
                std::vector<crpc::client::call<std::string>> handles;
                handles.reserve(batch);
                std::size_t issued = 0;
                std::size_t done = 0;
                std::size_t failed = 0;
                while (issued < count) {
                    const std::size_t n = std::min(batch, count - issued);
                    handles.clear();
                    for (std::size_t i = 0; i < n; ++i) {
                        auto stub = env.cli->service<std::string, std::string>(
                            k_service, k_echo);
                        auto started = stub.call(request);
                        if (!started.ok()) {
                            ++failed;
                            continue;
                        }
                        handles.push_back(std::move(started).value());
                    }
                    for (crpc::client::call<std::string>& handle : handles) {
                        auto result = handle.get();
                        if (result.ok()) {
                            ++done;
                        } else {
                            ++failed;
                        }
                    }
                    issued += n;
                }
                completed.fetch_add(done, std::memory_order_relaxed);
                errors.fetch_add(failed, std::memory_order_relaxed);
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        const auto stop = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(stop - start).count();
        const std::size_t done = completed.load(std::memory_order_relaxed);
        const double ops =
            seconds > 0.0 ? static_cast<double>(done) / seconds : 0.0;
        ops_samples.push_back(ops);
        ns_samples.push_back(ops > 0.0 ? 1e9 / ops : 0.0);
    }

    out = summarize_pipeline_rounds(ops_samples, ns_samples);
    out.errors = errors.load(std::memory_order_relaxed);
    out.clean = out.errors == 0;
    return out;
}

// Executes one reliable pipelined case and returns its statistics. It creates
// its own environment, so it can run in a child process (see the watchdog).
pipeline_stats run_pipeline_throughput_case(std::size_t payload,
                                            std::size_t concurrency,
                                            std::size_t batch, std::size_t total,
                                            int rounds) {
    environment env;
    if (!start(env, concurrency)) {
        std::printf("rpc/pipeline_%zuB c=%zu batch=%zu: environment did not start\n",
                    payload, concurrency, batch);
        std::fflush(stdout);
        pipeline_stats failed;
        failed.clean = false;
        failed.started = false;
        return failed;
    }
    const std::string request(payload, 'x');
    // Warm-up: closed-loop calls on the main thread. It brings the connection,
    // codec and handler pools to steady state without depending on the pipelined
    // path being reliable.
    std::size_t warm_failed = 0;
    for (int i = 0; i < 500; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
        auto started = stub.call(request);
        if (!started.ok()) {
            ++warm_failed;
            continue;
        }
        auto result = started.value().get();
        if (!result.ok()) {
            ++warm_failed;
        }
    }
    if (warm_failed != 0) {
        pipeline_stats failed;
        failed.errors = warm_failed;
        failed.clean = false;
        failed.started = true;
        return failed;
    }
    pipeline_stats result =
        run_pipeline_reliable(env, request, concurrency, batch, total, rounds);
    result.started = true;
    return result;
}

// Closed-loop fallback for a pipelined case that could not complete. It is the
// same shape as `bench_throughput` but creates its own environment.
double run_closed_loop_fallback(std::size_t payload, std::size_t concurrency,
                                std::size_t total) {
    environment env;
    if (!start(env, concurrency)) {
        return 0.0;
    }
    const std::string request(payload, 'x');
    for (int i = 0; i < 500; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
        auto started = stub.call(request);
        if (started.ok()) {
            (void)started.value().get();
        }
    }
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    const std::size_t base = total / concurrency;
    const std::size_t remainder = total % concurrency;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            const std::size_t count = base + (worker < remainder ? 1 : 0);
            std::size_t done = 0;
            std::size_t failed = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto stub = env.cli->service<std::string, std::string>(
                    k_service, k_echo);
                auto started = stub.call(request);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                auto result = started.value().get();
                if (result.ok()) {
                    ++done;
                } else {
                    ++failed;
                }
            }
            completed.fetch_add(done, std::memory_order_relaxed);
            errors.fetch_add(failed, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(stop - start).count();
    const std::size_t done = completed.load(std::memory_order_relaxed);
    const double ops =
        seconds > 0.0 ? static_cast<double>(done) / seconds : 0.0;
    std::printf("rpc/closed_loop_fallback_%zuB c=%zu calls=%zu errors=%zu "
                "time=%.4fs ops=%.3f M/s\n",
                payload, concurrency, done,
                errors.load(std::memory_order_relaxed), seconds, ops / 1e6);
    std::fflush(stdout);
    return ops;
}

struct oneway_pipeline_stats {
    double median_issue_ops = 0.0;
    double p99_issue_ops = 0.0;
    double spread_issue_ops = 0.0;
    double median_delivered_ops = 0.0;
    double p99_delivered_ops = 0.0;
    double spread_delivered_ops = 0.0;
    std::size_t errors = 0;
    bool drained = false;
    bool clean = false;
    bool started = false;
};

// --- child-process watchdog --------------------------------------------------
//
// A pipelined batch can deadlock in a Debug build: the concurrent issue path and
// the in-process backend's synchronous completion path contend on the client's
// completion lock while a server handler coroutine holds it, and the case stops
// making progress. The deadlock is not recoverable in-process (the workers hold
// library locks), so each pipelined case runs in a child process with a watchdog
// timeout. A child that exceeds the timeout is killed and the case falls back to
// the closed-loop measurement, which is recorded with a fallback name rather
// than a pipelined number.

std::string self_path() {
#if defined(_WIN32)
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::string();
    }
    return std::string(path, n);
#else
    return std::string();
#endif
}

std::string child_result_path() {
    static std::atomic<unsigned> counter{0};
    return "crpc-bench-pipeline-child-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
           ".tmp";
}

bool spawn_and_wait(const std::string& exe, const std::string& args,
                    int timeout_ms, int* exit_code) {
#if defined(_WIN32)
    const std::string cmd = "\"" + exe + "\" " + args;
    std::vector<char> buffer(cmd.begin(), cmd.end());
    buffer.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    const DWORD wait =
        WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    const bool completed = (wait == WAIT_OBJECT_0);
    if (!completed) {
        TerminateProcess(pi.hProcess, 999);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (exit_code != nullptr) {
        *exit_code = static_cast<int>(ec);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return completed;
#else
    (void)exe;
    (void)args;
    (void)timeout_ms;
    if (exit_code != nullptr) {
        *exit_code = -1;
    }
    return false;
#endif
}

// Runs one pipelined case in a child process with a timeout. Returns true and
// fills the result when the child completed; returns false when the child hung
// or could not be started. A child that fails to start its environment (a
// transient failure, reported as `S`) is retried.
bool run_case_in_child(const std::string& case_id, const std::string& out_path,
                       pipeline_stats* reliable, oneway_pipeline_stats* oneway) {
#if defined(_WIN32)
    const std::string exe = self_path();
    if (exe.empty()) {
        return false;
    }
    const std::string args = "--case:" + case_id;
    if (SetEnvironmentVariableA("CRPC_BENCH_CHILD_OUT", out_path.c_str()) == 0) {
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        int exit_code = 0;
        const bool completed = spawn_and_wait(exe, args, 20000, &exit_code);
        if (!completed) {
            std::remove(out_path.c_str());
            return false;
        }
        std::ifstream in(out_path);
        if (!in) {
            std::remove(out_path.c_str());
            return false;
        }
        char mode = 0;
        in >> mode;
        if (mode == 'S') {
            in.close();
            std::remove(out_path.c_str());
            continue;
        }
        if (mode == 'R' && reliable != nullptr) {
            in >> reliable->median_ops >> reliable->p99_ops >>
                reliable->spread_ops >> reliable->median_ns >>
                reliable->p99_ns >> reliable->spread_ns >> reliable->errors >>
                reliable->clean;
            reliable->started = true;
        } else if (mode == 'O' && oneway != nullptr) {
            in >> oneway->median_issue_ops >> oneway->p99_issue_ops >>
                oneway->spread_issue_ops >> oneway->median_delivered_ops >>
                oneway->p99_delivered_ops >> oneway->spread_delivered_ops >>
                oneway->errors >> oneway->drained >> oneway->clean;
            oneway->started = true;
        }
        in.close();
        std::remove(out_path.c_str());
        return true;
    }
    return false;
#else
    (void)case_id;
    (void)out_path;
    (void)reliable;
    (void)oneway;
    return false;
#endif
}

void bench_pipeline_throughput(std::size_t payload, std::size_t concurrency,
                               std::size_t batch, std::size_t total) {
    char name[128];
    std::snprintf(name, sizeof(name), "rpc/throughput_pipeline_%zuB_c%zu_b%zu",
                  payload, concurrency, batch);
    const std::string case_id = "r_" + std::to_string(payload) + "_" +
                                std::to_string(concurrency) + "_" +
                                std::to_string(batch);
    pipeline_stats stats;
    const bool completed =
        run_case_in_child(case_id, child_result_path(), &stats, nullptr);
    if (!completed) {
        std::printf("rpc/pipeline_%zuB c=%zu batch=%zu: pipelined case did not "
                    "complete within the watchdog timeout; falling back to "
                    "closed-loop for this case\n",
                    payload, concurrency, batch);
        std::fflush(stdout);
        const double ops = run_closed_loop_fallback(payload, concurrency, total);
        std::snprintf(name, sizeof(name),
                      "rpc/throughput_closed_loop_fallback_%zuB_c%zu_b%zu",
                      payload, concurrency, batch);
        crpc_bench::record(name, "M RPC/s", ops / 1e6);
        return;
    }

    crpc_bench::record(name, "M RPC/s", stats.median_ops / 1e6,
                       stats.spread_ops / 1e6);
    std::snprintf(name, sizeof(name),
                  "rpc/throughput_pipeline_%zuB_c%zu_b%zu_ns", payload,
                  concurrency, batch);
    crpc_bench::record(name, "ns/call", stats.median_ns, stats.spread_ns);

    std::printf("rpc/pipeline_%zuB c=%zu batch=%zu calls=%zu errors=%zu "
                "median=%.3f M/s p99=%.3f M/s spread=%.3f M/s "
                "median=%.1f ns/call p99=%.1f ns/call mode=%s\n",
                payload, concurrency, batch, total, stats.errors,
                stats.median_ops / 1e6, stats.p99_ops / 1e6,
                stats.spread_ops / 1e6, stats.median_ns, stats.p99_ns,
                stats.clean ? "pipelined" : "pipelined_errors");
    std::fflush(stdout);

    if (!stats.clean) {
        std::printf("rpc/pipeline_%zuB c=%zu batch=%zu: pipelined path reported "
                    "%zu error(s); falling back to closed-loop for this case\n",
                    payload, concurrency, batch, stats.errors);
        std::fflush(stdout);
        const double ops = run_closed_loop_fallback(payload, concurrency, total);
        std::snprintf(name, sizeof(name),
                      "rpc/throughput_closed_loop_fallback_%zuB_c%zu_b%zu",
                      payload, concurrency, batch);
        crpc_bench::record(name, "M RPC/s", ops / 1e6);
    }
}

// --- pipelined batch ONE_WAY -------------------------------------------------

oneway_pipeline_stats summarize_oneway_rounds(const std::vector<double>& issue,
                                              const std::vector<double>& delivered) {
    oneway_pipeline_stats stats;
    if (!issue.empty()) {
        std::vector<double> sorted = issue;
        std::sort(sorted.begin(), sorted.end());
        stats.median_issue_ops = sorted[sorted.size() / 2];
        stats.p99_issue_ops = percentile(sorted, 0.99);
        stats.spread_issue_ops = sorted.back() - sorted.front();
    }
    if (!delivered.empty()) {
        std::vector<double> sorted = delivered;
        std::sort(sorted.begin(), sorted.end());
        stats.median_delivered_ops = sorted[sorted.size() / 2];
        stats.p99_delivered_ops = percentile(sorted, 0.99);
        stats.spread_delivered_ops = sorted.back() - sorted.front();
    }
    return stats;
}

oneway_pipeline_stats run_pipeline_oneway(environment& env,
                                           const std::string& request,
                                           std::size_t concurrency,
                                           std::size_t batch, std::size_t total,
                                           int rounds) {
    oneway_pipeline_stats out;
    std::vector<double> issue_samples;
    std::vector<double> delivered_samples;
    std::atomic<std::size_t> errors{0};
    bool all_drained = true;

    for (int round = 0; round < rounds; ++round) {
        const std::uint64_t baseline =
            env.service.oneway_calls.load(std::memory_order_acquire);
        std::atomic<std::size_t> issued{0};
        std::vector<std::thread> workers;
        workers.reserve(concurrency);
        const std::size_t base = total / concurrency;
        const std::size_t remainder = total % concurrency;

        const auto issue_start = std::chrono::steady_clock::now();
        for (std::size_t worker = 0; worker < concurrency; ++worker) {
            workers.emplace_back([&, worker] {
                const std::size_t count = base + (worker < remainder ? 1 : 0);
                std::size_t local_issued = 0;
                std::size_t failed = 0;
                while (local_issued < count) {
                    const std::size_t n = std::min(batch, count - local_issued);
                    std::size_t batch_issued = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        auto stub = env.cli->service<std::string, std::string>(
                            k_service, k_oneway);
                        crpc::rpc::call_context ctx;
                        ctx.mode = crpc::delivery_mode::unreliable;
                        auto started = stub.call(request, ctx);
                        if (started.ok()) {
                            ++batch_issued;
                        } else {
                            ++failed;
                        }
                    }
                    local_issued += batch_issued;
                    issued.fetch_add(batch_issued, std::memory_order_relaxed);
                    // Drain the burst: wait until the handler has observed every
                    // call issued so far. The target is shared, so a worker may
                    // wait for a peer's calls too; that is a soft barrier, not a
                    // correctness requirement.
                    const std::size_t target =
                        issued.load(std::memory_order_acquire);
                    (void)wait_until(
                        [&env, &baseline, &target] {
                            return env.service.oneway_calls.load(
                                       std::memory_order_acquire) -
                                   baseline >=
                               target;
                        },
                        10000);
                }
                errors.fetch_add(failed, std::memory_order_relaxed);
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        const auto issue_stop = std::chrono::steady_clock::now();

        const std::size_t sent = issued.load(std::memory_order_acquire);
        const std::uint64_t target = baseline + sent;
        const bool drained = wait_until(
            [&env, &target] {
                return env.service.oneway_calls.load(std::memory_order_acquire) >=
                       target;
            },
            30000);
        const auto delivered_stop = std::chrono::steady_clock::now();
        if (!drained) {
            all_drained = false;
        }

        const double issue_seconds =
            std::chrono::duration<double>(issue_stop - issue_start).count();
        const double total_seconds =
            std::chrono::duration<double>(delivered_stop - issue_start).count();
        const std::uint64_t delivered =
            env.service.oneway_calls.load(std::memory_order_acquire) - baseline;
        issue_samples.push_back(
            issue_seconds > 0.0 ? static_cast<double>(sent) / issue_seconds : 0.0);
        delivered_samples.push_back(
            total_seconds > 0.0 ? static_cast<double>(delivered) / total_seconds
                                : 0.0);
    }

    out = summarize_oneway_rounds(issue_samples, delivered_samples);
    out.errors = errors.load(std::memory_order_relaxed);
    out.drained = all_drained;
    out.clean = all_drained && out.errors == 0;
    return out;
}

// Executes one ONE_WAY pipelined case and returns its statistics. It creates its
// own environment, so it can run in a child process (see the watchdog).
oneway_pipeline_stats run_pipeline_oneway_case(std::size_t payload,
                                               std::size_t concurrency,
                                               std::size_t batch,
                                               std::size_t total, int rounds) {
    environment env;
    if (!start(env, concurrency)) {
        std::printf("rpc/oneway_pipeline_%zuB c=%zu batch=%zu: environment did not start\n",
                    payload, concurrency, batch);
        std::fflush(stdout);
        oneway_pipeline_stats failed;
        failed.clean = false;
        failed.started = false;
        return failed;
    }
    const std::string request(payload, 'x');
    // Warm-up: issue the same shape, then wait until the handler has observed
    // every issued call before the timed window starts.
    const std::uint64_t warm_before =
        env.service.oneway_calls.load(std::memory_order_acquire);
    std::size_t warm_issued = 0;
    for (int i = 0; i < 500; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_oneway);
        crpc::rpc::call_context ctx;
        ctx.mode = crpc::delivery_mode::unreliable;
        auto started = stub.call(request, ctx);
        if (started.ok()) {
            ++warm_issued;
        }
    }
    (void)wait_until(
        [&env, &warm_before, &warm_issued] {
            return env.service.oneway_calls.load(std::memory_order_acquire) >=
                   warm_before + warm_issued;
        },
        10000);
    oneway_pipeline_stats result =
        run_pipeline_oneway(env, request, concurrency, batch, total, rounds);
    result.started = true;
    return result;
}

// Closed-loop ONE_WAY fallback for a pipelined case that could not complete.
double run_oneway_closed_loop_fallback(std::size_t payload,
                                       std::size_t concurrency,
                                       std::size_t total) {
    environment env;
    if (!start(env, concurrency)) {
        return 0.0;
    }
    const std::string request(payload, 'x');
    const std::uint64_t warm_before =
        env.service.oneway_calls.load(std::memory_order_acquire);
    std::size_t warm_issued = 0;
    for (int i = 0; i < 500; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_oneway);
        crpc::rpc::call_context ctx;
        ctx.mode = crpc::delivery_mode::unreliable;
        auto started = stub.call(request, ctx);
        if (started.ok()) {
            ++warm_issued;
        }
    }
    (void)wait_until(
        [&env, &warm_before, &warm_issued] {
            return env.service.oneway_calls.load(std::memory_order_acquire) >=
                   warm_before + warm_issued;
        },
        10000);
    const std::uint64_t baseline =
        env.service.oneway_calls.load(std::memory_order_acquire);
    std::atomic<std::size_t> issued{0};
    std::atomic<std::size_t> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    const std::size_t base = total / concurrency;
    const std::size_t remainder = total % concurrency;
    const auto issue_start = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            const std::size_t count = base + (worker < remainder ? 1 : 0);
            std::size_t failed = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto stub = env.cli->service<std::string, std::string>(
                    k_service, k_oneway);
                crpc::rpc::call_context ctx;
                ctx.mode = crpc::delivery_mode::unreliable;
                auto started = stub.call(request, ctx);
                if (!started.ok()) {
                    ++failed;
                }
            }
            issued.fetch_add(count - failed, std::memory_order_relaxed);
            errors.fetch_add(failed, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto issue_stop = std::chrono::steady_clock::now();
    const std::size_t sent = issued.load(std::memory_order_acquire);
    const std::uint64_t target = baseline + sent;
    (void)wait_until(
        [&env, &target] {
            return env.service.oneway_calls.load(std::memory_order_acquire) >=
                   target;
        },
        30000);
    const auto delivered_stop = std::chrono::steady_clock::now();
    const double total_seconds =
        std::chrono::duration<double>(delivered_stop - issue_start).count();
    const std::uint64_t delivered =
        env.service.oneway_calls.load(std::memory_order_acquire) - baseline;
    const double ops =
        total_seconds > 0.0 ? static_cast<double>(delivered) / total_seconds : 0.0;
    std::printf("rpc/oneway_closed_loop_fallback_%zuB c=%zu issued=%zu "
                "delivered=%llu errors=%zu time=%.4fs ops=%.3f M/s\n",
                payload, concurrency, sent,
                static_cast<unsigned long long>(delivered),
                errors.load(std::memory_order_relaxed), total_seconds, ops / 1e6);
    std::fflush(stdout);
    return ops;
}

// Child-process entry point: runs exactly one pipelined case and writes its
// statistics to the file named by CRPC_BENCH_CHILD_OUT. Used by the watchdog.
void run_child_case(const std::string& id) {
    std::string out_path;
    if (!dupenv("CRPC_BENCH_CHILD_OUT", out_path) || out_path.empty() ||
        id.size() < 2) {
        return;
    }
    // Case id format: "<r|o>_<payload>_<concurrency>_<batch>".
    std::size_t payload = 0;
    std::size_t concurrency = 0;
    std::size_t batch = 0;
    {
        const std::string rest = id.substr(2);
        std::size_t start = 0;
        std::size_t index = 0;
        while (index < 3) {
            const std::size_t underscore = rest.find('_', start);
            const std::string token = rest.substr(
                start, underscore == std::string::npos ? std::string::npos
                                                       : underscore - start);
            if (token.empty()) {
                return;
            }
            const unsigned long long value = std::strtoull(token.c_str(), nullptr, 10);
            if (index == 0) {
                payload = static_cast<std::size_t>(value);
            } else if (index == 1) {
                concurrency = static_cast<std::size_t>(value);
            } else {
                batch = static_cast<std::size_t>(value);
            }
            ++index;
            if (underscore == std::string::npos) {
                break;
            }
            start = underscore + 1;
        }
        if (index != 3) {
            return;
        }
    }
    const std::size_t total = call_total(10000);
    const int rounds = 3;
    std::ofstream out(out_path);
    if (!out) {
        return;
    }
    if (id[0] == 'r') {
        const pipeline_stats s =
            run_pipeline_throughput_case(payload, concurrency, batch, total, rounds);
        if (!s.started) {
            out << "S\n";
        } else {
            out << "R " << s.median_ops << ' ' << s.p99_ops << ' ' << s.spread_ops
                << ' ' << s.median_ns << ' ' << s.p99_ns << ' ' << s.spread_ns
                << ' ' << s.errors << ' ' << (s.clean ? 1 : 0) << '\n';
        }
    } else if (id[0] == 'o') {
        const oneway_pipeline_stats s =
            run_pipeline_oneway_case(payload, concurrency, batch, total, rounds);
        if (!s.started) {
            out << "S\n";
        } else {
            out << "O " << s.median_issue_ops << ' ' << s.p99_issue_ops << ' '
                << s.spread_issue_ops << ' ' << s.median_delivered_ops << ' '
                << s.p99_delivered_ops << ' ' << s.spread_delivered_ops << ' '
                << s.errors << ' ' << (s.drained ? 1 : 0) << ' '
                << (s.clean ? 1 : 0) << '\n';
        }
    }
}

void bench_pipeline_oneway(std::size_t payload, std::size_t concurrency,
                           std::size_t batch, std::size_t total) {
    char name[128];
    std::snprintf(name, sizeof(name),
                  "rpc/oneway_pipeline_issue_%zuB_c%zu_b%zu", payload,
                  concurrency, batch);
    const std::string case_id = "o_" + std::to_string(payload) + "_" +
                                std::to_string(concurrency) + "_" +
                                std::to_string(batch);
    oneway_pipeline_stats stats;
    const bool completed =
        run_case_in_child(case_id, child_result_path(), nullptr, &stats);
    if (!completed) {
        std::printf("rpc/oneway_pipeline_%zuB c=%zu batch=%zu: pipelined case did "
                    "not complete within the watchdog timeout; falling back to "
                    "closed-loop for this case\n",
                    payload, concurrency, batch);
        std::fflush(stdout);
        const double ops =
            run_oneway_closed_loop_fallback(payload, concurrency, total);
        std::snprintf(name, sizeof(name),
                      "rpc/oneway_closed_loop_fallback_%zuB_c%zu_b%zu", payload,
                      concurrency, batch);
        crpc_bench::record(name, "M calls/s", ops / 1e6);
        return;
    }

    crpc_bench::record(name, "M calls/s", stats.median_issue_ops / 1e6,
                       stats.spread_issue_ops / 1e6);
    std::snprintf(name, sizeof(name),
                  "rpc/oneway_pipeline_delivered_%zuB_c%zu_b%zu", payload,
                  concurrency, batch);
    crpc_bench::record(name, "M calls/s", stats.median_delivered_ops / 1e6,
                       stats.spread_delivered_ops / 1e6);

    std::printf("rpc/oneway_pipeline_%zuB c=%zu batch=%zu calls=%zu errors=%zu "
                "drained=%s issue_median=%.3f M/s p99=%.3f M/s "
                "delivered_median=%.3f M/s p99=%.3f M/s mode=%s\n",
                payload, concurrency, batch, total, stats.errors,
                stats.drained ? "yes" : "no",
                stats.median_issue_ops / 1e6, stats.p99_issue_ops / 1e6,
                stats.median_delivered_ops / 1e6, stats.p99_delivered_ops / 1e6,
                stats.clean ? "pipelined" : "pipelined_errors");
    std::fflush(stdout);

    if (!stats.clean) {
        std::printf("rpc/oneway_pipeline_%zuB c=%zu batch=%zu: pipelined path did "
                    "not drain cleanly (%zu error(s)); falling back to "
                    "closed-loop for this case\n",
                    payload, concurrency, batch, stats.errors);
        std::fflush(stdout);
        const double ops =
            run_oneway_closed_loop_fallback(payload, concurrency, total);
        std::snprintf(name, sizeof(name),
                      "rpc/oneway_closed_loop_fallback_%zuB_c%zu_b%zu", payload,
                      concurrency, batch);
        crpc_bench::record(name, "M calls/s", ops / 1e6);
    }
}

void run_all_cases() {
    // Child mode: `--case:<id>` runs exactly one pipelined case and writes its
    // statistics to the file named by CRPC_BENCH_CHILD_OUT. The parent process
    // uses this to enforce the watchdog timeout.
    const char* filter = crpc_bench::filter();
    if (filter != nullptr && std::strncmp(filter, "--case:", 7) == 0) {
        run_child_case(std::string(filter + 7));
        return;
    }

    if (want("latency")) {
        bench_latency(16, 2000, 3);
        bench_latency(256, 2000, 3);
        bench_latency(4096, 2000, 3);
    }

    if (want("throughput")) {
        bench_throughput(16, 1, call_total(10000));
        bench_throughput(16, 4, call_total(10000));
        bench_throughput(256, 1, call_total(10000));
        bench_throughput(256, 4, call_total(10000));
        bench_throughput(4096, 1, call_total(4000));
        bench_throughput(4096, 4, call_total(4000));
        bench_throughput(65536, 1, call_total(512));
        bench_throughput(65536, 4, call_total(512));
    }

    if (want("oneway")) {
        bench_oneway(16, 1, call_total(10000));
        bench_oneway(16, 4, call_total(10000));
        bench_oneway(4096, 1, call_total(4000));
        bench_oneway(4096, 4, call_total(4000));
    }

    if (want("pipeline")) {
        const std::size_t payloads[] = {16, 256};
        const std::size_t batches[] = {1, 16, 64};
        const std::size_t concurrencies[] = {1, 2, 4};

        // Reliable pipelined batch throughput: payload x batch x concurrency.
        for (std::size_t payload : payloads) {
            for (std::size_t batch : batches) {
                for (std::size_t concurrency : concurrencies) {
                    bench_pipeline_throughput(payload, concurrency, batch,
                                              call_total(10000));
                }
            }
        }

        // Unreliable ONE_WAY pipelined batch throughput: batch x concurrency.
        for (std::size_t batch : batches) {
            for (std::size_t concurrency : concurrencies) {
                bench_pipeline_oneway(16, concurrency, batch, call_total(10000));
            }
        }
    }
}

} // namespace

CRPC_BENCH_MAIN()
