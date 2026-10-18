// CRPC -- spans, deterministic head sampling and asynchronous export.
//
// The tracer keeps the caller's path cheap:
//
//   * With no exporter registered `enabled()` is false and `start_span()`
//     returns an inactive span. The cost paid is one atomic load, one
//     comparison and the construction of an empty span object. No clock is
//     read, no identifier is generated, no queue is touched and nothing is
//     allocated.
//   * With an exporter registered, an active span costs two identifier
//     generations, a deterministic hash (root spans only), two clock reads
//     (start and finish) and one lock-free queue push. The queue is bounded;
//     when full the span is dropped and counted, never blocked.
//   * `export_spans()` is called only on the tracer's background thread, in
//     batches, never from a call thread.
//
// Parent/child relationship is established from the send/receive pair: the
// sender copies its `trace_context` into the call envelope and the receiver
// passes that context to `start_span()`. Timestamps are recorded for display
// only and are never used to infer a parent. A local monotonic clock is used
// for start/end so that a wall-clock adjustment cannot produce a negative
// duration.
//
// Thread safety: `tracer::instance()` is the only documented mutable singleton
// of this module. `start_span()` and `span::finish()` are safe from any thread.
// `configure()`, `set_exporter()`, `start()`, `shutdown()` and `reset()` are
// lifecycle operations; call them from one thread before serving (or after
// joining all call threads).
#ifndef CRPC_OBSERVABILITY_TRACER_HPP
#define CRPC_OBSERVABILITY_TRACER_HPP

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/observability/context.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace obs {

namespace detail {
class span_queue;
}

// Bounded attribute slot. No allocation.
struct span_attribute {
    static constexpr std::size_t max_key = 24;
    static constexpr std::size_t max_value = 96;

    char key[max_key];
    char value[max_value];
    u8 key_len;
    u8 value_len;
    u8 truncated;
};

// Immutable snapshot handed to an exporter. Trivially copyable so the queue can
// move it by value without allocation.
struct span_data {
    static constexpr std::size_t max_name = 64;
    static constexpr std::size_t max_attributes = 4;

    trace_id trace;
    span_id span;
    span_id parent;
    u8 flags;
    rpc_code status;
    u8 attribute_count;
    u8 dropped_attributes;
    u8 name_len;
    std::uint64_t start_ns;
    std::uint64_t end_ns;
    char name[max_name];
    span_attribute attributes[max_attributes];

    [[nodiscard]] bool sampled() const noexcept {
        return (flags & trace_context::sampled_flag) != 0;
    }
};

// Exporter contract. `export_spans` runs on the tracer background thread; it
// may block that thread, but must not call back into the tracer. Exceptions
// are caught and swallowed so that an exporter bug cannot terminate the
// process.
class exporter {
public:
    virtual ~exporter() = default;
    virtual void export_spans(::crpc::span<const span_data> spans) = 0;
};

// Tail-sampling hook, invoked on the background thread after a span has been
// dequeued and before it is exported. Return false to drop the span.
using tail_sampler_fn = bool (*)(void* user, const span_data& data) noexcept;

// Injectable monotonic clock, in nanoseconds. `nullptr` selects the default
// steady clock.
using monotonic_clock_fn = std::uint64_t (*)(void* user) noexcept;

struct tracer_config {
    // Head-sampling rate in [0, 1]. The decision is a pure function of the
    // trace id, so two processes configured with the same rate agree.
    double sample_rate = 0.01;
    // Rounded up to a power of two (minimum two spans).
    std::size_t queue_capacity = 4096;
    monotonic_clock_fn clock = nullptr;
    void* clock_user = nullptr;
};

// Forward declaration: `friend class tracer;` inside `span` does not introduce
// a name visible to ordinary lookup before the definition (MSVC accepts it,
// GCC 13 rejects it with "tracer does not name a type").
class tracer;

// RAII span. Not copyable; movable. A span is tied to the thread that created
// it and must be finished (or destroyed) on that thread.
class [[nodiscard]] span {
public:
    span() noexcept;
    span(span&& other) noexcept;
    span& operator=(span&& other) noexcept;
    span(const span&) = delete;
    span& operator=(const span&) = delete;
    ~span();

    // Idempotent. Records the end timestamp and enqueues the span when it is
    // sampled.
    void finish() noexcept;

    [[nodiscard]] bool active() const noexcept { return tracer_ != nullptr && !finished_; }
    [[nodiscard]] bool sampled() const noexcept { return sampled_; }

    // Returns a zero context for an inactive span. Otherwise the context is
    // suitable for a `traceparent` header or a call envelope.
    [[nodiscard]] trace_context context() const noexcept;

    // Bounded. When the attribute table is full the call is ignored and
    // `dropped_attributes` is incremented (saturating at 255).
    void set_attribute(std::string_view key, std::string_view value) noexcept;
    void set_status(rpc_code code) noexcept;

    // Starts a child in the same trace. The parent link is this span's id,
    // not a timestamp relation.
    [[nodiscard]] span start_child(std::string_view name) noexcept;

private:
    friend class tracer;

    tracer* tracer_;
    span_data data_;
    bool finished_;
    bool sampled_;
};

class tracer {
public:
    static tracer& instance() noexcept {
        static tracer value;
        return value;
    }

    tracer(const tracer&) = delete;
    tracer& operator=(const tracer&) = delete;

    // Lifecycle. Configure before start. Registering a non-null exporter
    // enables the tracer and starts the worker (start() is idempotent); call
    // configure() first so the queue capacity and sampling rate are applied.
    void configure(const tracer_config& config) noexcept;
    void set_exporter(exporter* sink) noexcept;
    void set_tail_sampler(tail_sampler_fn function, void* user) noexcept;
    void start() noexcept;
    void shutdown() noexcept;

    // Testing/administrative reset. Stops the worker, clears the queue and
    // counters and restores defaults. No span may be alive.
    void reset() noexcept;

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    // Starts a root or child span. A valid `parent` makes the new span a child
    // of it and inherits its sampled flag. An invalid parent creates a root
    // whose sampling decision is derived from the trace id.
    [[nodiscard]] span start_span(std::string_view name,
                                  const trace_context& parent = trace_context{}) noexcept;

    // Deterministic head-sampling decision. Pure function of the id and rate;
    // exposed so tests and other modules can make the same decision.
    [[nodiscard]] static bool sample_trace(trace_id trace, double rate) noexcept;

    [[nodiscard]] std::uint64_t dropped_spans() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t exported_spans() const noexcept {
        return exported_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t tail_dropped_spans() const noexcept {
        return tail_dropped_.load(std::memory_order_relaxed);
    }

private:
    tracer() noexcept;
    ~tracer();

    friend class span;

    [[nodiscard]] std::uint64_t current_time() const noexcept;
    void enqueue(const span_data& data) noexcept;
    void export_loop() noexcept;
    std::size_t drain(std::size_t maximum) noexcept;
    void export_batch(std::size_t count) noexcept;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> sample_threshold_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> exported_{0};
    std::atomic<std::uint64_t> tail_dropped_{0};

    std::size_t queue_capacity_ = 4096;
    monotonic_clock_fn clock_ = nullptr;
    void* clock_user_ = nullptr;
    exporter* exporter_ = nullptr;
    tail_sampler_fn tail_sampler_ = nullptr;
    void* tail_sampler_user_ = nullptr;

    std::unique_ptr<detail::span_queue> queue_;
    std::thread worker_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::array<span_data, 256> batch_;
};

} // namespace obs
} // namespace crpc

#endif // CRPC_OBSERVABILITY_TRACER_HPP
