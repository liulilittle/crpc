// CRPC -- Redis distributed lock.
//
// A lease-based mutual exclusion over Redis. The safety of a distributed lock
// rests on three things, and all three are enforced here rather than left to the
// caller:
//
//   * **acquire is atomic.** `SET key token NX PX ttl` is one round trip; there is
//     no "get then set" window in which two owners can both believe they won.
//   * **release is owner-checked.** Unlocking with a plain `DEL` is the classic
//     bug: owner A's lease expires, owner B acquires, then A's delayed `DEL` frees
//     B's lock. Release and renew therefore run a compare-and-delete script that
//     only touches the key when the stored token is still ours.
//   * **the caller can order operations by a fence.** Every acquisition takes a
//     monotonically increasing number from `INCR`, and the token embeds it. A
//     resource guarded by the lock can compare the fence against the last one it
//     saw and reject a stale holder that woke up after its lease expired. A lease
//     alone cannot give that guarantee: a paused process cannot be told its lease
//     ended, but it can be prevented from doing damage if every write carries the
//     fence.
//
// What this is not: a quorum lock. A single Redis instance is a single point of
// failure, and a failover can lose a lock that was acknowledged (asynchronous
// replication). If that matters, run Redis in a configuration whose loss of
// durability is acceptable, or run the lock over several independent instances and
// require a majority — `distributed_lock` deliberately implements the single
// instance case, and the header says so instead of implying a guarantee it cannot
// make.
#ifndef CRPC_REDIS_DISTRIBUTED_LOCK_HPP
#define CRPC_REDIS_DISTRIBUTED_LOCK_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>

namespace crpc {
namespace redis {

struct lock_options {
    // Lease length. Longer tolerates a slow owner, shorter recovers faster from a
    // dead one. There is no "correct" value: choose it from the expected pause
    // between renewals, and rely on the fence for the rest.
    std::chrono::milliseconds ttl{10000};

    // How long `acquire()` keeps retrying before giving up.
    std::chrono::milliseconds acquire_timeout{5000};

    // Pause between attempts. Small enough to be responsive, large enough not to
    // hammer the server. Values below one millisecond are raised to one: a zero
    // pause would make the retry loop spin and burn a core until the budget runs
    // out, so the floor is part of the contract, not an implementation detail.
    std::chrono::milliseconds retry_delay{50};

    // Key prefixes. Separating the fence key from the lock key means a lock and
    // its counter cannot be confused by a scan.
    std::string key_prefix = "crpc:lock:";
    std::string fence_prefix = "crpc:lock:fence:";
};

// RAII ownership of one lock. Releases on destruction, exactly once.
class CRPC_API lock_handle {
public:
    lock_handle() noexcept = default;
    ~lock_handle();

    lock_handle(const lock_handle&) = delete;
    lock_handle& operator=(const lock_handle&) = delete;
    lock_handle(lock_handle&& other) noexcept;
    lock_handle& operator=(lock_handle&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    [[nodiscard]] std::string_view resource() const noexcept { return resource_; }
    [[nodiscard]] std::string_view token() const noexcept { return token_; }
    // The monotonic value taken at acquisition. Pass it to whatever the lock
    // protects and reject anything older.
    [[nodiscard]] std::uint64_t fence() const noexcept { return fence_; }

    // Releases the lock now (owner-checked). Idempotent.
    void release() noexcept;

private:
    friend class distributed_lock;
    lock_handle(std::shared_ptr<void> state, std::string resource, std::string token,
                std::uint64_t fence);

    std::shared_ptr<void> state_;
    std::string resource_;
    std::string token_;
    std::uint64_t fence_ = 0;
};

class CRPC_API distributed_lock {
public:
    // `executor` is usually a `client`; the interface is what makes the lock
    // testable without a server.
    distributed_lock(command_executor& executor, lock_options options = {});
    ~distributed_lock();

    distributed_lock(const distributed_lock&) = delete;
    distributed_lock& operator=(const distributed_lock&) = delete;
    distributed_lock(distributed_lock&&) = delete;
    distributed_lock& operator=(distributed_lock&&) = delete;

    // One attempt. Succeeds or reports `failed_precondition` when another owner
    // holds the resource.
    [[nodiscard]] result<lock_handle> try_acquire(std::string_view resource,
                                                  std::string_view owner);

    // Retries until `acquire_timeout`; failure is `deadline_exceeded`.
    [[nodiscard]] result<lock_handle> acquire(std::string_view resource, std::string_view owner);

    // Owner-checked renew of an existing token. False means the lock is no longer
    // ours (expired or taken), which the caller must treat as lost ownership, not
    // as a transient error.
    [[nodiscard]] result<bool> renew(std::string_view resource, std::string_view token);

    // Owner-checked release. False means the lock was already gone or belonged to
    // someone else; releasing an expired lease is not an error.
    [[nodiscard]] result<bool> release(std::string_view resource, std::string_view token);

    // Starts or stops the renewal thread. It renews every handle this object
    // handed out at one third of the TTL. Without it a holder that is busy longer
    // than the TTL loses the lock; with it, a paused holder still loses it (by
    // design -- a renewer that cannot run must not be able to keep a lease alive).
    //
    // Both are ref-counted in the sense that repeated calls are safe: starting
    // twice does not spawn a second thread, and stopping is a no-op once the
    // watchdog is already down. What is not automatic is "renew for as long as I
    // hold the resource" -- use `renewal_guard` for that scope.
    void start_watchdog();
    void stop_watchdog();

    // Diagnostics.
    [[nodiscard]] std::size_t held() const;

private:
    friend class lock_handle;

    struct impl;
    std::shared_ptr<impl> impl_;
};

// RAII scope guard: brings the renewal thread up for its lifetime and tears it
// down on exit. This is the deferred "auto-renew" convenience (plan.md 方案 2):
// a caller that wants "renew this lock for as long as this scope runs" wraps the
// acquisition in a `renewal_guard` instead of calling start/stop_watchdog itself
// and (too often) forgetting one end. Additive API, no `lock_options` layout
// change, so it is a MINOR addition.
class CRPC_API renewal_guard {
public:
    explicit renewal_guard(distributed_lock& lock) noexcept : lock_(&lock) {
        lock_->start_watchdog();
    }

    ~renewal_guard() {
        if (lock_) {
            lock_->stop_watchdog();
        }
    }

    renewal_guard(const renewal_guard&) = delete;
    renewal_guard& operator=(const renewal_guard&) = delete;

private:
    distributed_lock* lock_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_DISTRIBUTED_LOCK_HPP