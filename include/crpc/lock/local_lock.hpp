// CRPC -- local lock manager.
//
// A keyed lock with three properties that a plain `std::mutex` map gets wrong:
//
//   * **no deadlock.** A caller may hold several keys at once, and two callers may
//     name the same keys in different orders. Every acquisition therefore sorts its
//     keys, and the whole registry is guarded by one mutex: there is never a second
//     lock held while waiting, so an ABBA cycle cannot form. (A per-key mutex design
//     is the classic way to reintroduce one.)
//   * **no leak.** Every acquisition carries a lease. A holder that forgets to
//     release, or that dies, loses the key when the lease expires; a background
//     reaper enforces it. A handle also releases on destruction, exactly once.
//   * **no lost wakeup.** Waiters are woken through one condition variable and
//     re-check the whole key set, so a release that does not happen to free *their*
//     keys cannot make them wait forever.
//
// Ownership is by string token, not by thread id: a coroutine can migrate between
// threads, and a token lets a caller express "same logical operation" across a
// suspend. Reentrancy is per token and per key.
#ifndef CRPC_LOCK_LOCAL_LOCK_HPP
#define CRPC_LOCK_LOCAL_LOCK_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace lock {

struct manager_options {
    // How long an acquisition is valid before the reaper frees it. Zero disables
    // expiry, which is only safe when every holder is known to release.
    std::chrono::nanoseconds lease{std::chrono::seconds(30)};

    // How often the reaper sweeps. Zero runs no thread and relies on the lazy
    // sweep performed by every acquire/release (enough for a single-threaded or
    // test deployment, not for a server that must reclaim a dead holder's key
    // while idle).
    std::chrono::nanoseconds reap_period{std::chrono::seconds(1)};
};

// RAII ownership of one or more keys.
//
// The handle holds the manager state by `shared_ptr`, so a handle that outlives
// its manager still releases safely instead of touching freed memory.
class CRPC_API lock_handle {
public:
    lock_handle() noexcept = default;
    ~lock_handle();

    lock_handle(const lock_handle&) = delete;
    lock_handle& operator=(const lock_handle&) = delete;
    lock_handle(lock_handle&& other) noexcept;
    lock_handle& operator=(lock_handle&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

    // Releases every key now. Idempotent (a second call is a no-op).
    void release() noexcept;

    // Keys in the order they were granted (sorted, deduplicated).
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return keys_; }
    [[nodiscard]] std::string_view owner() const noexcept { return owner_; }

private:
    friend class lock_manager;
    lock_handle(std::shared_ptr<void> state, std::vector<std::string> keys,
                std::string owner) noexcept;

    std::shared_ptr<void> state_;
    std::vector<std::string> keys_;
    std::string owner_;
};

class CRPC_API lock_manager {
public:
    explicit lock_manager(manager_options options = {});
    ~lock_manager();

    lock_manager(const lock_manager&) = delete;
    lock_manager& operator=(const lock_manager&) = delete;
    lock_manager(lock_manager&&) = delete;
    lock_manager& operator=(lock_manager&&) = delete;

    // Acquires every key or none, waiting up to `timeout` (zero means "try once").
    // Keys are sorted and deduplicated, so request order can never deadlock. The
    // same owner may re-enter a key it already holds, which increments a count.
    [[nodiscard]] result<lock_handle> acquire(span<const std::string> keys,
                                              std::chrono::nanoseconds timeout,
                                              std::string owner);

    // Convenience for one key.
    [[nodiscard]] result<lock_handle> acquire(std::string_view key,
                                              std::chrono::nanoseconds timeout,
                                              std::string owner);

    // True when `owner` currently holds every key. Used by tests and by callers
    // that must know whether they already hold a set before re-entering.
    [[nodiscard]] bool held_by(span<const std::string> keys, std::string_view owner) const;

    // Frees every lease that expired at or before `now`. The reaper calls this;
    // it is public so a test can drive expiry deterministically.
    void sweep(std::chrono::steady_clock::time_point now);

    // Diagnostics.
    [[nodiscard]] std::size_t held_keys() const;
    [[nodiscard]] std::size_t waiters() const;

private:
    friend class lock_handle;

    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace lock
} // namespace crpc

#endif // CRPC_LOCK_LOCAL_LOCK_HPP