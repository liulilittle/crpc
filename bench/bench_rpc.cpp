// CRPC -- end-to-end RPC benchmark over the real client facade and dispatcher.
//
// What this measures
// ------------------
// A real call: the typed stub encodes a `std::string` payload with
// `call_codec<std::string>` (the crpc-binary path), the facade admits it
// through its circuit breaker and load balancer, the request crosses the
// configured backend, the server shard decodes it, runs a registered echo
// method and encodes the response, and the response crosses back and is decoded
// by `call<Resp>::get()`. Nothing here is a no-op stub and no number is derived
// from a batch divided by an RTT.
//
// Cases
// -----
//   * single-thread synchronous latency (p50/p90/p99/p999, full histogram
//     summary, no averaging away of the tail);
//   * single-thread throughput with pipelining depth 1/16/64;
//   * multi-shard throughput (1/2/4 server shards, one runtime thread each);
//   * Windows only: the same workload over a real TCP loopback using the IOCP
//     backend.
//
// Every case prints the environment (CPU, cores, compiler, build type) and the
// exact workload (payload bytes, method, in-flight depth). The in-process
// backend result is an upper bound for the framework, not a network result.
#include <crpc/client_facade.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#if defined(_WIN32)
#include <crpc/io/iocp_backend.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench_harness.hpp"

namespace {

using namespace std::chrono_literals;

constexpr crpc::u64 k_service = 300;
constexpr crpc::u64 k_echo = 1;

// The method under test: an echo of a real payload. The handler is registered
// through the typed trampoline, so the request/response are encoded and decoded
// with call_codec<std::string> on both sides.
struct echo_service {
    std::string echo(const std::string& value) { return value; }
};

// --- environment -------------------------------------------------------------

std::string cpu_model() {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "PROCESSOR_IDENTIFIER") == 0 && value != nullptr) {
        std::string model(value);
        std::free(value);
        return model;
    }
    return "unknown";
#else
    std::FILE* file = std::fopen("/proc/cpuinfo", "rb");
    if (file == nullptr) {
        return "unknown";
    }
    char line[512];
    std::string model = "unknown";
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const char* key = "model name";
        const std::size_t key_len = std::strlen(key);
        if (std::strncmp(line, key, key_len) == 0) {
            const char* colon = std::strchr(line, ':');
            if (colon != nullptr) {
                const char* start = colon + 1;
                while (*start == ' ' || *start == '\t') {
                    ++start;
                }
                model = start;
                while (!model.empty() &&
                       (model.back() == '\n' || model.back() == '\r')) {
                    model.pop_back();
                }
            }
            break;
        }
    }
    std::fclose(file);
    return model;
#endif
}

void print_environment_extended() {
    crpc_bench::print_environment();
    std::printf("cpu: %s\n", cpu_model().c_str());
    std::printf("cores (hardware_concurrency): %u\n",
                std::thread::hardware_concurrency());
#if defined(_WIN32)
    std::printf("backend under test: inproc (in-process loopback) + iocp (tcp loopback)\n\n");
#else
    std::printf("backend under test: inproc (in-process loopback)\n\n");
#endif
}

// --- case selection ----------------------------------------------------------

std::string env_var(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) {
        std::string result(value);
        std::free(value);
        return result;
    }
    return std::string();
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// CRPC_BENCH_ONLY selects a subset of cases by substring, so a single case can
// be reproduced (and a crash isolated) without rebuilding.
std::string g_only;

bool want(const char* key) {
    return g_only.empty() || g_only.find(key) != std::string::npos;
}

// --- statistics --------------------------------------------------------------

struct latency_stats {
    double p50 = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    std::size_t samples = 0;
};

double percentile_ns(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    double index = fraction * static_cast<double>(sorted.size() - 1);
    std::size_t low = static_cast<std::size_t>(index);
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
    stats.p50 = percentile_ns(samples, 0.50);
    stats.p90 = percentile_ns(samples, 0.90);
    stats.p99 = percentile_ns(samples, 0.99);
    stats.p999 = percentile_ns(samples, 0.999);
    double total = 0.0;
    for (double value : samples) {
        total += value;
    }
    stats.mean = total / static_cast<double>(samples.size());
    return stats;
}

// --- fixture -----------------------------------------------------------------

struct environment {
    crpc::inproc_backend* inproc = nullptr;
#if defined(_WIN32)
    crpc::iocp_backend* iocp = nullptr;
#endif
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    echo_service service;

    ~environment() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
    }
};

bool start_server(environment& env, crpc::backend_factory& backend,
                  const crpc::endpoint& ep, std::size_t shards,
                  std::size_t handler_workers) {
    crpc::server::builder sb;
    sb.add_listener(ep, backend);
    sb.add_service(k_service, "bench");
    sb.add_method<echo_service, &echo_service::echo>(k_service, k_echo, "echo",
                                                     env.service);
    sb.shards(shards);
    sb.handler_workers(handler_workers);
    sb.drain_timeout(2000ms);
    env.srv = sb.build();
    if (!env.srv || !env.srv->start().ok()) {
        std::printf("bench: server did not start on %s\n", ep.to_string().c_str());
        return false;
    }
    return true;
}

bool start_environment(environment& env, crpc::backend_factory& backend,
                       const crpc::endpoint& ep, std::size_t shards,
                       std::size_t pool_size, std::size_t handler_workers) {
    if (!start_server(env, backend, ep, shards, handler_workers)) {
        return false;
    }

    crpc::client::builder cb;
    cb.add_endpoint(ep, backend);
    cb.pool_size(pool_size);
    cb.max_inflight_per_connection(4096);
    cb.default_deadline(5s);
    env.cli = cb.build();
    if (!env.cli) {
        std::printf("bench: client did not build\n");
        return false;
    }
    return true;
}

// --- cases -------------------------------------------------------------------

// One synchronous call: encode, dispatch, decode, wait if needed.
bool one_call(crpc::client& cli, const std::string& request, std::string& out,
              crpc::rpc_code& code) {
    auto stub = cli.service<std::string, std::string>(k_service, k_echo);
    auto started = stub.call(request);
    if (!started.ok()) {
        code = started.code();
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        code = result.code();
        return false;
    }
    out = std::move(result).value();
    code = crpc::rpc_code::ok;
    return true;
}

void bench_latency(environment& env, std::size_t payload_bytes,
                   std::size_t iterations) {
    const std::string request(payload_bytes, 'x');
    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;

    // Warm-up, discarded.
    for (int i = 0; i < 2000; ++i) {
        (void)one_call(*env.cli, request, response, code);
    }
    if (code != crpc::rpc_code::ok) {
        std::printf("rpc/inproc_latency warm-up failed: %s\n",
                    crpc::to_string(code));
        return;
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    // Retained handles: see bench_throughput (defect CF-1). A handle is only
    // destroyed at the end of the case, after the completion callbacks have
    // drained, so the timing measures the call and not a use-after-free.
    std::vector<crpc::client::call<std::string>> all;
    all.reserve(iterations + 2000);
    std::size_t errors = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
        auto started = stub.call(request);
        if (!started.ok()) {
            code = started.code();
            ++errors;
            continue;
        }
        auto handle = std::move(started).value();
        const auto start = std::chrono::steady_clock::now();
        auto result = handle.get();
        const auto stop = std::chrono::steady_clock::now();
        all.push_back(std::move(handle));
        if (!result.ok()) {
            code = result.code();
            if (errors == 0) {
                std::printf("rpc/inproc_latency: first error code=%s\n",
                            crpc::to_string(code));
            }
            ++errors;
            continue;
        }
        samples.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count()));
    }

    latency_stats stats = summarize(samples);
    std::printf("rpc/inproc_latency_%zuB  payload=%zuB method=bench.echo "
                "depth=1 samples=%zu errors=%zu\n",
                payload_bytes, payload_bytes, stats.samples, errors);
    std::printf("    p50=%.2fus p90=%.2fus p99=%.2fus p999=%.2fus "
                "min=%.2fus max=%.2fus mean=%.2fus\n",
                stats.p50 / 1000.0, stats.p90 / 1000.0, stats.p99 / 1000.0,
                stats.p999 / 1000.0, stats.min / 1000.0, stats.max / 1000.0,
                stats.mean / 1000.0);
    char name[96];
    std::snprintf(name, sizeof(name), "rpc/inproc_echo_%zuB_p99_ns", payload_bytes);
    crpc_bench::record(name, "ns", stats.p99);
    std::this_thread::sleep_for(100ms);
}

void bench_throughput(environment& env, std::size_t payload_bytes,
                      std::size_t depth, std::size_t total) {
    const std::string request(payload_bytes, 'x');
    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;

    for (std::size_t i = 0; i < std::min<std::size_t>(2000, total); ++i) {
        (void)one_call(*env.cli, request, response, code);
    }

    // Every successfully started call handle is kept alive until the end of the
    // case. Destroying the last handle immediately after get() returns races
    // with the completion callback still finishing call_core::signal() on
    // another thread (see .opencode/verify-round.md, defect CF-1); keeping the
    // handles alive removes the harness-induced use-after-free while measuring
    // the same real calls.
    std::vector<crpc::client::call<std::string>> all;
    all.reserve(total);
    std::size_t issued = 0;
    std::size_t completed = 0;
    std::size_t errors = 0;

    const auto start = std::chrono::steady_clock::now();
    while (issued < total) {
        const std::size_t batch = std::min(depth, total - issued);
        const std::size_t base = all.size();
        for (std::size_t i = 0; i < batch; ++i) {
            auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
            auto started = stub.call(request);
            if (!started.ok()) {
                ++errors;
                continue;
            }
            all.push_back(std::move(started).value());
            ++issued;
        }
        for (std::size_t i = base; i < all.size(); ++i) {
            auto result = all[i].get();
            if (!result.ok()) {
                ++errors;
            } else {
                ++completed;
            }
        }
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(stop - start).count();
    const double ops = seconds > 0.0 ? static_cast<double>(completed) / seconds
                                     : 0.0;

    std::printf("rpc/inproc_throughput_%zuB depth=%zu payload=%zuB "
                "method=bench.echo calls=%zu errors=%zu time=%.4fs\n",
                payload_bytes, depth, payload_bytes, completed, errors, seconds);
    std::printf("    %.3f M RPC/s  (%.1f ns/call)\n", ops / 1e6,
                ops > 0.0 ? 1e9 / ops : 0.0);
    char name[96];
    std::snprintf(name, sizeof(name), "rpc/inproc_throughput_%zuB_depth%zu",
                  payload_bytes, depth);
    crpc_bench::record(name, "M RPC/s", ops / 1e6);
    std::this_thread::sleep_for(100ms);
}

void bench_multishard(crpc::inproc_backend& backend, std::size_t payload_bytes,
                      std::size_t shards, std::size_t depth, std::size_t total) {
    environment env;
    env.inproc = &backend;
    const std::string name = "crpc-bench-shards-" + std::to_string(shards);
    const crpc::endpoint ep = crpc::endpoint::inproc(name);
    if (!start_environment(env, backend, ep, shards, shards, 64)) {
        return;
    }

    const std::string request(payload_bytes, 'x');
    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;
    for (std::size_t i = 0; i < 2000; ++i) {
        (void)one_call(*env.cli, request, response, code);
    }

    std::vector<crpc::client::call<std::string>> all;
    all.reserve(total);
    std::size_t issued = 0;
    std::size_t completed = 0;
    std::size_t errors = 0;

    const auto start = std::chrono::steady_clock::now();
    while (issued < total) {
        const std::size_t batch = std::min(depth, total - issued);
        const std::size_t base = all.size();
        for (std::size_t i = 0; i < batch; ++i) {
            auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
            auto started = stub.call(request);
            if (!started.ok()) {
                ++errors;
                continue;
            }
            all.push_back(std::move(started).value());
            ++issued;
        }
        for (std::size_t i = base; i < all.size(); ++i) {
            auto result = all[i].get();
            if (!result.ok()) {
                ++errors;
            } else {
                ++completed;
            }
        }
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double ops = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;

    std::printf("rpc/inproc_multishard shards=%zu pool=%zu depth=%zu "
                "payload=%zuB calls=%zu errors=%zu time=%.4fs\n",
                shards, shards, depth, payload_bytes, completed, errors, seconds);
    std::printf("    %.3f M RPC/s  (%.1f ns/call)\n", ops / 1e6,
                ops > 0.0 ? 1e9 / ops : 0.0);
    char label[96];
    std::snprintf(label, sizeof(label), "rpc/inproc_multishard_%zu_shards", shards);
    crpc_bench::record(label, "M RPC/s", ops / 1e6);
    std::this_thread::sleep_for(100ms);
}

// --- diagnostics: where the per-call cost goes --------------------------------
//
// These cases do not measure RPC. They measure the individual pieces the
// throughput cases are made of, so the bottleneck can be named with numbers
// instead of guessed: the per-call shared_ptr allocation, the per-connection
// recursive mutex, the string codec, the issue-vs-wait split of a pipelined
// call, and whether extra client threads scale with extra server shards.

void bench_diag_alloc() {
    crpc_bench::measure("diag/make_shared_call_state", 2'000'000, 7,
                        [](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            auto state =
                std::make_shared<crpc::client_detail::call_state<std::string>>();
            (void)state;
        }
    });
}

void bench_diag_lock() {
    static std::recursive_mutex mutex;
    crpc_bench::measure("diag/recursive_mutex_lock_unlock", 5'000'000, 7,
                        [](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            std::lock_guard<std::recursive_mutex> lock(mutex);
        }
    });
}

void bench_diag_codec() {
    const std::string value(64, 'x');
    crpc::buffer encoded;
    (void)crpc::rpc::call_codec<std::string>::encode(value, encoded);
    crpc::buffer scratch;
    scratch.reserve(256);
    crpc_bench::measure("diag/string_codec_encode", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            scratch.clear();
            (void)crpc::rpc::call_codec<std::string>::encode(value, scratch);
        }
    });
    std::string decoded;
    crpc_bench::measure("diag/string_codec_decode", 2'000'000, 7, [&](std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            (void)crpc::rpc::call_codec<std::string>::decode(encoded.readable(),
                                                             decoded);
        }
    });
}

// Splits the pipelined cost into "issue" (start_call: encode + push) and
// "wait" (get: completion + decode).
void bench_diag_phase_split(environment& env, std::size_t payload_bytes,
                            std::size_t depth, std::size_t total) {
    const std::string request(payload_bytes, 'x');
    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;
    for (int i = 0; i < 2000; ++i) {
        (void)one_call(*env.cli, request, response, code);
    }

    std::vector<crpc::client::call<std::string>> all;
    all.reserve(total);
    double issue_ns = 0.0;
    double wait_ns = 0.0;
    std::size_t issued = 0;
    std::size_t completed = 0;
    std::size_t errors = 0;
    std::size_t ready_at_issue = 0;
    while (issued < total) {
        const std::size_t batch = std::min(depth, total - issued);
        const std::size_t base = all.size();
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < batch; ++i) {
            auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
            auto started = stub.call(request);
            if (!started.ok()) {
                ++errors;
                continue;
            }
            const bool ready_now = started.value().ready();
            all.push_back(std::move(started).value());
            ++issued;
            if (ready_now) {
                ++ready_at_issue;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        for (std::size_t i = base; i < all.size(); ++i) {
            auto result = all[i].get();
            if (!result.ok()) {
                ++errors;
            } else {
                ++completed;
            }
        }
        const auto t2 = std::chrono::steady_clock::now();
        issue_ns += static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        wait_ns += static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
    }
    const double issue_per =
        issued > 0 ? issue_ns / static_cast<double>(issued) : 0.0;
    const double wait_per =
        completed > 0 ? wait_ns / static_cast<double>(completed) : 0.0;
    std::printf("diag/phase_split depth=%zu payload=%zuB calls=%zu errors=%zu "
                "ready_at_issue=%zu\n",
                depth, payload_bytes, completed, errors, ready_at_issue);
    std::printf("    issue=%.1f ns/call  wait=%.1f ns/call  total=%.1f ns/call\n",
                issue_per, wait_per, issue_per + wait_per);
    crpc_bench::record("diag/phase_split_issue_ns", "ns", issue_per);
    crpc_bench::record("diag/phase_split_wait_ns", "ns", wait_per);
    std::this_thread::sleep_for(100ms);
}

// Each worker thread owns its own client (one connection, depth-pipelined) and
// the server gets `threads` shards, so a scaling curve separates "one client
// thread is the ceiling" from "the server/dispatcher is the ceiling".
void bench_diag_multithread(crpc::inproc_backend& backend,
                            std::size_t payload_bytes, std::size_t threads,
                            std::size_t depth, std::size_t per_thread) {
    environment server;
    const std::string name = "crpc-bench-mt-" + std::to_string(threads);
    const crpc::endpoint ep = crpc::endpoint::inproc(name);
    if (!start_server(server, backend, ep, threads, 64)) {
        return;
    }

    const std::string request(payload_bytes, 'x');
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> errors{0};
    std::vector<std::thread> workers;
    workers.reserve(threads);
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&] {
            crpc::client::builder cb;
            cb.add_endpoint(ep, backend);
            cb.pool_size(1);
            cb.max_inflight_per_connection(4096);
            cb.default_deadline(5s);
            auto cli = cb.build();
            if (!cli) {
                errors.fetch_add(per_thread, std::memory_order_relaxed);
                return;
            }
            std::string response;
            crpc::rpc_code code = crpc::rpc_code::ok;
            for (int i = 0; i < 1000; ++i) {
                (void)one_call(*cli, request, response, code);
            }
            std::vector<crpc::client::call<std::string>> all;
            all.reserve(per_thread);
            std::size_t issued = 0;
            std::size_t done = 0;
            std::size_t failed = 0;
            while (issued < per_thread) {
                const std::size_t batch = std::min(depth, per_thread - issued);
                const std::size_t base = all.size();
                for (std::size_t i = 0; i < batch; ++i) {
                    auto stub = cli->service<std::string, std::string>(k_service, k_echo);
                    auto started = stub.call(request);
                    if (!started.ok()) {
                        ++failed;
                        continue;
                    }
                    all.push_back(std::move(started).value());
                    ++issued;
                }
                for (std::size_t i = base; i < all.size(); ++i) {
                    auto result = all[i].get();
                    if (!result.ok()) {
                        ++failed;
                    } else {
                        ++done;
                    }
                }
            }
            cli->close();
            completed.fetch_add(done, std::memory_order_relaxed);
            errors.fetch_add(failed, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    const std::size_t total = completed.load(std::memory_order_relaxed);
    const double ops = seconds > 0.0 ? static_cast<double>(total) / seconds : 0.0;
    std::printf("diag/multithread clients=%zu shards=%zu depth=%zu calls=%zu "
                "errors=%zu time=%.4fs -> %.3f M RPC/s\n",
                threads, threads, depth, total,
                errors.load(std::memory_order_relaxed), seconds, ops / 1e6);
    char label[96];
    std::snprintf(label, sizeof(label), "diag/multithread_%zu_clients", threads);
    crpc_bench::record(label, "M RPC/s", ops / 1e6);
    std::this_thread::sleep_for(100ms);
}

#if defined(_WIN32)
void bench_tcp(crpc::iocp_backend& backend, std::size_t payload_bytes,
               std::size_t depth, std::size_t total) {
    // Bind the first free loopback port. start_environment leaves a started
    // server in `env` on success; on failure it is discarded and retried.
    environment env;
    bool started = false;
    for (crpc::u16 port = 45200; port < 45250 && !started; ++port) {
        const crpc::endpoint ep = crpc::endpoint::tcp("127.0.0.1", port);
        started = start_environment(env, backend, ep, 1, 1, 8);
    }
    if (!started) {
        std::printf("rpc/iocp: no free loopback port for the TCP benchmark\n");
        return;
    }

    const std::string request(payload_bytes, 'x');
    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;

    // Latency samples (synchronous, depth 1).
    for (int i = 0; i < 200; ++i) {
        (void)one_call(*env.cli, request, response, code);
    }
    std::vector<double> samples;
    samples.reserve(std::min<std::size_t>(total, 20000));
    for (std::size_t i = 0; i < std::min<std::size_t>(total, 20000); ++i) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = one_call(*env.cli, request, response, code);
        const auto stop = std::chrono::steady_clock::now();
        if (ok) {
            samples.push_back(static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                    .count()));
        }
    }
    latency_stats stats = summarize(samples);
    std::printf("rpc/iocp_loopback_latency_%zuB payload=%zuB method=bench.echo "
                "depth=1 samples=%zu\n",
                payload_bytes, payload_bytes, stats.samples);
    std::printf("    p50=%.2fus p90=%.2fus p99=%.2fus p999=%.2fus "
                "min=%.2fus max=%.2fus mean=%.2fus\n",
                stats.p50 / 1000.0, stats.p90 / 1000.0, stats.p99 / 1000.0,
                stats.p999 / 1000.0, stats.min / 1000.0, stats.max / 1000.0,
                stats.mean / 1000.0);

    // Pipelined throughput. Handles are kept alive for the whole case for the
    // reason documented in bench_throughput (defect CF-1).
    std::vector<crpc::client::call<std::string>> all;
    all.reserve(total);
    std::size_t issued = 0;
    std::size_t completed = 0;
    std::size_t errors = 0;
    const auto start = std::chrono::steady_clock::now();
    while (issued < total) {
        const std::size_t batch = std::min(depth, total - issued);
        const std::size_t base = all.size();
        for (std::size_t i = 0; i < batch; ++i) {
            auto stub = env.cli->service<std::string, std::string>(k_service, k_echo);
            auto call_started = stub.call(request);
            if (!call_started.ok()) {
                ++errors;
                continue;
            }
            all.push_back(std::move(call_started).value());
            ++issued;
        }
        for (std::size_t i = base; i < all.size(); ++i) {
            auto result = all[i].get();
            if (!result.ok()) {
                ++errors;
            } else {
                ++completed;
            }
        }
    }
    const auto stop = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(stop - start).count();
    const double ops = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
    std::printf("rpc/iocp_loopback_throughput_%zuB depth=%zu calls=%zu "
                "errors=%zu time=%.4fs -> %.3f M RPC/s\n",
                payload_bytes, depth, completed, errors, seconds, ops / 1e6);
    char name[96];
    std::snprintf(name, sizeof(name), "rpc/iocp_loopback_tput_%zuB_d%zu",
                  payload_bytes, depth);
    crpc_bench::record(name, "M RPC/s", ops / 1e6);
    std::this_thread::sleep_for(100ms);
}
#endif

// Percolates a bad configuration early instead of reporting a fake zero rate.
bool preflight(crpc::inproc_backend& backend, environment& env) {
    const crpc::endpoint ep = crpc::endpoint::inproc("crpc-bench-preflight");
    if (!start_environment(env, backend, ep, 1, 1, 8)) {
        return false;
    }
    const std::string request(64, 'x');
    std::string response;
    crpc::rpc_code code = crpc::rpc_code::ok;
    const bool ok = one_call(*env.cli, request, response, code);
    if (!ok) {
        std::printf("bench: preflight call failed: %s\n", crpc::to_string(code));
    } else if (response != request) {
        std::printf("bench: preflight echo mismatch\n");
        return false;
    }
    return ok;
}

void run_all() {
    g_only = env_var("CRPC_BENCH_ONLY");
    print_environment_extended();

    const std::size_t payload = 64;
    const std::size_t latency_iterations = 50000;
    const std::size_t throughput_total = 200000;

    crpc::inproc_backend backend;
    environment env;
    if (!preflight(backend, env)) {
        return;
    }

    if (want("inproc_latency")) {
        bench_latency(env, payload, latency_iterations);
    }
    if (want("inproc_tput")) {
        bench_throughput(env, payload, 1, throughput_total);
        bench_throughput(env, payload, 16, throughput_total);
        bench_throughput(env, payload, 64, throughput_total);
    }
    if (want("inproc_multi")) {
        bench_multishard(backend, payload, 1, 16, throughput_total);
        bench_multishard(backend, payload, 2, 16, throughput_total);
        bench_multishard(backend, payload, 4, 16, throughput_total);
    }
    if (want("diag")) {
        bench_diag_alloc();
        bench_diag_lock();
        bench_diag_codec();
        bench_diag_phase_split(env, payload, 64, 200000);
        bench_diag_multithread(backend, payload, 1, 64, 50000);
        bench_diag_multithread(backend, payload, 2, 64, 50000);
        bench_diag_multithread(backend, payload, 4, 64, 50000);
        bench_diag_multithread(backend, payload, 8, 64, 50000);
    }

#if defined(_WIN32)
    if (want("iocp_single")) {
        crpc::iocp_backend iocp;
        bench_tcp(iocp, payload, 1, 20000);
    }
    if (want("iocp_pipeline")) {
        crpc::iocp_backend iocp;
        bench_tcp(iocp, payload, 16, 100000);
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    crpc_bench::filter() = argc > 1 ? argv[1] : nullptr;
    run_all();
    return crpc_bench::summary();
}
