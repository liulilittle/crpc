// CRPC -- bulkhead (bounded counting semaphore).
//
// The bulkhead bounds the number of concurrent operations admitted to a
// dependency. It allocates nothing after construction and its hot path is a
// single compare-exchange loop.
//
// Thread safety:
//   * `bulkhead` is safe for concurrent use from any number of threads.
//   * `try_enter()` is non-blocking and lock-free.
//   * A `bulkhead_permit` must not outlive its bulkhead. It releases its slot
//     exactly once, either explicitly or in its destructor.
#ifndef CRPC_RESILIENCE_BULKHEAD_HPP
#define CRPC_RESILIENCE_BULKHEAD_HPP

#include <atomic>
#include <chrono>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc::resilience {

// Thread safety: a plain value type; copy it before sharing between threads.
struct bulkhead_config {
    u32 capacity{64};
    // Hint returned when the bulkhead is full.
    std::chrono::milliseconds retry_after{10};
};

class bulkhead;

// RAII slot. A rejected permit has `allowed() == false`, carries
// `rpc_code::resource_exhausted`, and does not hold a slot.
//
// Thread safety: a permit is owned by one thread at a time and is not
// internally synchronized. Moving it between threads is safe if the move itself
// is synchronized.
class [[nodiscard]] bulkhead_permit {
public:
    bulkhead_permit() noexcept = default;
    bulkhead_permit(bulkhead_permit&& other) noexcept;
    bulkhead_permit& operator=(bulkhead_permit&& other) noexcept;
    bulkhead_permit(const bulkhead_permit&) = delete;
    bulkhead_permit& operator=(const bulkhead_permit&) = delete;
    ~bulkhead_permit();

    [[nodiscard]] bool allowed() const noexcept {
        return owner_ != nullptr && held_;
    }
    [[nodiscard]] rpc_code code() const noexcept { return code_; }
    [[nodiscard]] std::chrono::milliseconds retry_after() const noexcept {
        return retry_after_;
    }

    // Releases the slot. Safe to call more than once; later calls are no-ops.
    void leave() noexcept;

private:
    friend class bulkhead;

    bulkhead* owner_ = nullptr;
    bool held_ = false;
    rpc_code code_ = rpc_code::resource_exhausted;
    std::chrono::milliseconds retry_after_{0};
};

class bulkhead {
public:
    explicit bulkhead(const bulkhead_config& config) noexcept;

    bulkhead(const bulkhead&) = delete;
    bulkhead& operator=(const bulkhead&) = delete;
    bulkhead(bulkhead&&) = delete;
    bulkhead& operator=(bulkhead&&) = delete;

    // Non-blocking, lock-free admission.
    [[nodiscard]] bulkhead_permit try_enter() noexcept;

    // Releases one slot. Prefer the RAII permit; this exists for callers that
    // manage the pairing explicitly. It never drops the count below zero.
    void leave() noexcept;

    [[nodiscard]] u32 in_use() const noexcept {
        return in_use_.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

private:
    std::atomic<u32> in_use_{0};
    u32 capacity_ = 1;
    std::chrono::milliseconds retry_after_{10};
};

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_BULKHEAD_HPP
