// CRPC -- circuit breaker.
//
// The breaker keeps a sliding window of outcome counters and a single atomic
// state word. `allow()` is lock-free: it performs one acquire load of the state
// word and, only when the breaker is open or half-open, a compare-exchange. The
// steady-state closed path therefore never takes a lock and never allocates.
//
// Time is always supplied by an injected monotonic clock. No wall clock is
// read anywhere in this module. `manual_clock` exists so tests can advance time
// deterministically.
//
// Thread safety:
//   * `circuit_breaker` is safe for concurrent use from any number of threads.
//   * `allow()` and permit recording are lock-free (atomic loads and CAS loops).
//   * The state-change callback runs on the thread that won the transition and
//     may run concurrently with callbacks for other transitions. It must be
//     noexcept, non-blocking, thread-safe, and must not call back into the same
//     breaker.
//   * A `breaker_permit` must not outlive its breaker. Permits are move-only.
#ifndef CRPC_RESILIENCE_CIRCUIT_BREAKER_HPP
#define CRPC_RESILIENCE_CIRCUIT_BREAKER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc::resilience {

// --- Clock -------------------------------------------------------------------

// A clock function returns monotonic nanoseconds. The context is owned by the
// caller and must outlive every object that received the clock.
using clock_fn = u64 (*)(void* context) noexcept;

class clock_source {
public:
    constexpr clock_source() noexcept = default;
    constexpr clock_source(clock_fn read, void* context) noexcept
        : read_(read), context_(context) {}

    [[nodiscard]] u64 now_ns() const noexcept {
        return read_ != nullptr ? read_(context_) : 0;
    }
    [[nodiscard]] bool valid() const noexcept { return read_ != nullptr; }

private:
    clock_fn read_ = nullptr;
    void* context_ = nullptr;
};

// Returns a clock backed by `std::chrono::steady_clock` (monotonic, never the
// wall clock).
CRPC_API clock_source monotonic_clock() noexcept;

// A deterministic clock for tests. It is thread-safe.
class manual_clock {
public:
    explicit manual_clock(u64 start_ns = 0) noexcept : now_(start_ns) {}

    manual_clock(const manual_clock&) = delete;
    manual_clock& operator=(const manual_clock&) = delete;

    void advance(std::chrono::nanoseconds delta) noexcept {
        now_.fetch_add(static_cast<u64>(delta.count()), std::memory_order_relaxed);
    }
    void set(u64 now_ns) noexcept { now_.store(now_ns, std::memory_order_relaxed); }

    [[nodiscard]] u64 now_ns() const noexcept {
        return now_.load(std::memory_order_relaxed);
    }

    // Returns a clock_source that points at this clock. The clock must outlive
    // the consumer.
    [[nodiscard]] clock_source source() noexcept {
        return clock_source(&manual_clock::read, this);
    }

private:
    static u64 read(void* context) noexcept {
        return static_cast<manual_clock*>(context)->now_ns();
    }

    std::atomic<u64> now_;
};

// --- Configuration -----------------------------------------------------------

// All durations are monotonic. Zero `slow_call_threshold` means "derive it from
// p99_target": max(1 ms, 5 * p99_target).
//
// Thread safety: a plain value type; copy it before sharing between threads.
struct breaker_config {
    std::chrono::nanoseconds window{std::chrono::seconds(30)};
    u32 buckets{10};
    u32 min_requests{20};
    double failure_ratio_threshold{0.5};
    std::chrono::nanoseconds p99_target{std::chrono::microseconds(100)};
    // Zero means "derive": max(1 ms, 5 * p99_target).
    std::chrono::nanoseconds slow_call_threshold{std::chrono::nanoseconds(0)};
    double slow_call_ratio_threshold{0.5};
    std::chrono::nanoseconds open_duration{std::chrono::seconds(10)};
    u32 half_open_max_probes{3};
    u32 half_open_successes_to_close{5};
};

enum class breaker_state : u8 {
    closed = 0,
    open = 1,
    half_open = 2
};

enum class breaker_outcome : u8 {
    success = 0,
    failure = 1,
    slow = 2,
    ignored = 3
};

// Thread safety: a plain value type; each call to `stats()` returns a snapshot.
struct breaker_stats {
    u64 success = 0;
    u64 failure = 0;
    u64 slow = 0;

    [[nodiscard]] u64 total() const noexcept { return success + failure + slow; }
};

using state_change_fn = void (*)(void* context, breaker_state old_state,
                                 breaker_state new_state) noexcept;

class circuit_breaker;

// RAII admission ticket. Exactly one outcome is recorded:
//   * `success()`, `failure()`, `slow()` record that outcome;
//   * `ignore()` explicitly records `ignored`;
//   * destroying an unrecorded permit records `ignored`.
// An `ignored` outcome is not counted in any ratio. A slow success is classified
// as `slow` (not `success`) when its measured duration reaches the configured
// slow-call threshold.
//
// A rejected permit has `allowed() == false` and carries `rpc_code::unavailable`
// plus an optional `retry_after` hint.
//
// Thread safety: a permit is owned by one thread at a time and is not
// internally synchronized. Moving it between threads is safe if the move itself
// is synchronized; concurrent recording on the same permit is not.
class [[nodiscard]] breaker_permit {
public:
    breaker_permit() noexcept = default;

    breaker_permit(breaker_permit&& other) noexcept;
    breaker_permit& operator=(breaker_permit&& other) noexcept;
    breaker_permit(const breaker_permit&) = delete;
    breaker_permit& operator=(const breaker_permit&) = delete;
    ~breaker_permit();

    [[nodiscard]] bool allowed() const noexcept {
        return breaker_ != nullptr && code_ == rpc_code::ok;
    }
    [[nodiscard]] rpc_code code() const noexcept { return code_; }
    [[nodiscard]] std::chrono::milliseconds retry_after() const noexcept {
        return retry_after_;
    }

    void success() noexcept;
    void failure() noexcept;
    void slow() noexcept;
    void ignore() noexcept;
    void record(breaker_outcome outcome) noexcept;

private:
    friend class circuit_breaker;

    void release_unrecorded() noexcept;

    circuit_breaker* breaker_ = nullptr;
    u64 generation_ = 0;
    u64 start_ns_ = 0;
    u64 slow_threshold_ns_ = 0;
    bool probe_ = false;
    bool recorded_ = false;
    rpc_code code_ = rpc_code::unavailable;
    std::chrono::milliseconds retry_after_{0};
};

// Thread-safe circuit breaker. Non-copyable and non-movable because its state
// is shared with outstanding permits.
class circuit_breaker {
public:
    explicit circuit_breaker(
        const breaker_config& config, clock_source clock = monotonic_clock(),
        state_change_fn on_change = nullptr, void* on_change_context = nullptr) noexcept;
    ~circuit_breaker() = default;

    circuit_breaker(const circuit_breaker&) = delete;
    circuit_breaker& operator=(const circuit_breaker&) = delete;
    circuit_breaker(circuit_breaker&&) = delete;
    circuit_breaker& operator=(circuit_breaker&&) = delete;

    // Lock-free admission. Never blocks and never allocates.
    [[nodiscard]] breaker_permit allow() noexcept;

    [[nodiscard]] breaker_state state() const noexcept;
    [[nodiscard]] breaker_stats stats() const noexcept;

    // Forces the breaker closed and clears the sliding window. Outstanding
    // permits from older generations are ignored.
    void reset() noexcept;

    [[nodiscard]] const breaker_config& config() const noexcept { return config_; }

private:
    friend class breaker_permit;

    static constexpr u32 k_max_buckets = 64;

    struct bucket {
        // (epoch << 2) | flags. flag bit 0 = locked, bit 1 = ready.
        std::atomic<u64> state{0};
        std::atomic<u64> success{0};
        std::atomic<u64> failure{0};
        std::atomic<u64> slow{0};
    };

    void record_permit(const breaker_permit& permit, breaker_outcome outcome) noexcept;
    void record_bucket(breaker_outcome outcome) noexcept;
    void maybe_open() noexcept;
    void handle_probe(const breaker_permit& permit, breaker_outcome outcome) noexcept;
    bool claim_bucket(bucket& slot, u64 epoch) noexcept;
    void reset_window() noexcept;
    bool transition_to(breaker_state expected, breaker_state next, u32 probes,
                       u32 successes) noexcept;
    void notify(breaker_state old_state, breaker_state new_state) noexcept;

    breaker_config config_;
    clock_source clock_;
    state_change_fn on_change_ = nullptr;
    void* on_change_context_ = nullptr;

    u32 bucket_count_ = 1;
    u64 bucket_width_ns_ = 1;
    u64 slow_threshold_ns_ = 1;
    u64 open_duration_ns_ = 0;
    u32 max_probes_ = 1;
    u32 successes_to_close_ = 1;

    std::atomic<u64> state_word_{0};
    std::atomic<u64> open_until_ns_{0};
    std::atomic<u64> window_origin_{0};
    bucket buckets_[k_max_buckets];
};

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_CIRCUIT_BREAKER_HPP
