// CRPC -- counters, gauges and histograms with a zero-cost disabled path.
//
// The library samples a small, fixed set of built-in metrics on paths that
// already exist: the RPC client (calls, errors, in-flight calls, pending write
// bytes) and the server dispatch (calls, errors, in-flight dispatches). The
// design mirrors the tracer:
//
//   * With no exporter installed `enabled()` is false. `add`, `set`, `observe`
//     and the static helpers perform one atomic load, one comparison and
//     return: no allocation, no lock, no clock read and no callback. The
//     pointer to the `metrics` singleton is a function-local static, so the
//     first use pays the (thread-safe) initialization and every later use is a
//     plain load.
//   * With an exporter installed each update stores the accumulated value and
//     calls `metric_exporter::on_metric` synchronously on the sampling thread.
//     The exporter is responsible for its own thread safety; it must not call
//     back into `metrics` and must not block.
//
// `metric_id` is a closed enum rather than a string so that assigning a name
// never allocates. A caller that needs a new metric adds an enumerator and a
// name in `to_string`, which keeps the exported names stable across a build.
//
// Thread safety: `metrics` is a process-wide singleton. Every update is an
// atomic operation; `set_exporter` and `reset` are lifecycle operations and
// must be called from one thread while no other thread samples (or after the
// samplers have been quiesced).
#ifndef CRPC_OBSERVABILITY_METRICS_HPP
#define CRPC_OBSERVABILITY_METRICS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <crpc/config.hpp>

namespace crpc {
namespace obs {

// Built-in sampling points. `count` is the number of ids, not a metric.
enum class metric_id : u8 {
    rpc_client_calls = 0,
    rpc_client_errors,
    rpc_client_inflight,
    rpc_client_queue_depth,
    rpc_server_calls,
    rpc_server_errors,
    rpc_server_inflight,
    rpc_server_queue_depth,
    count
};

[[nodiscard]] CRPC_API const char* to_string(metric_id id) noexcept;

enum class metric_kind : u8 {
    counter = 0,
    gauge = 1,
    histogram = 2
};

// Receives one update per sample. `value` is the accumulated counter value, the
// current gauge value, or the observed histogram sample, depending on `kind`.
class CRPC_API metric_exporter {
public:
    virtual ~metric_exporter() = default;
    virtual void on_metric(metric_id id, metric_kind kind, u64 value) noexcept = 0;
};

// Monotonic counter handle.
class CRPC_API counter {
public:
    counter() noexcept = default;
    explicit counter(metric_id id) noexcept : id_(id) {}

    void add(u64 delta = 1) const noexcept;

private:
    metric_id id_ = metric_id::count;
};

// Point-in-time value handle. `add` accepts a signed delta so an in-flight
// gauge can be raised and lowered without keeping a second counter.
class CRPC_API gauge {
public:
    gauge() noexcept = default;
    explicit gauge(metric_id id) noexcept : id_(id) {}

    void set(u64 value) const noexcept;
    void add(std::int64_t delta) const noexcept;

private:
    metric_id id_ = metric_id::count;
};

// Observation handle. The exporter receives every sample; the registry keeps
// the sample count and the last observed value.
class CRPC_API histogram {
public:
    histogram() noexcept = default;
    explicit histogram(metric_id id) noexcept : id_(id) {}

    void observe(u64 value) const noexcept;

private:
    metric_id id_ = metric_id::count;
};

class CRPC_API metrics {
public:
    static metrics& instance() noexcept;

    metrics(const metrics&) = delete;
    metrics& operator=(const metrics&) = delete;

    // Lifecycle. `set_exporter(nullptr)` disables sampling again.
    void set_exporter(metric_exporter* exporter) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool enabled() const noexcept {
        return exporter_.load(std::memory_order_acquire) != nullptr;
    }

    [[nodiscard]] counter counter_of(metric_id id) const noexcept {
        return counter(id);
    }
    [[nodiscard]] gauge gauge_of(metric_id id) const noexcept { return gauge(id); }
    [[nodiscard]] histogram histogram_of(metric_id id) const noexcept {
        return histogram(id);
    }

    // Last accumulated value for a counter or gauge, or the last histogram
    // sample. Zero while sampling is disabled.
    [[nodiscard]] u64 value(metric_id id) const noexcept;
    // Number of histogram samples collected.
    [[nodiscard]] u64 samples(metric_id id) const noexcept;

    // Static shortcuts used on the library's hot paths. They avoid naming the
    // singleton and keep the call sites short; each is exactly the handle call.
    static void increment(metric_id id, u64 delta = 1) noexcept;
    static void set_gauge(metric_id id, u64 value) noexcept;
    static void add_gauge(metric_id id, std::int64_t delta) noexcept;
    static void record(metric_id id, u64 value) noexcept;

private:
    metrics() noexcept;

    static constexpr std::size_t metric_count =
        static_cast<std::size_t>(metric_id::count);

    std::atomic<metric_exporter*> exporter_{nullptr};
    std::atomic<u64> values_[metric_count];
    std::atomic<u64> samples_[metric_count];
};

} // namespace obs
} // namespace crpc

#endif // CRPC_OBSERVABILITY_METRICS_HPP
