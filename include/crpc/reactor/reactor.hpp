// CRPC -- readiness-driven dispatch (the reactor pattern).
//
// A reactor is the "readiness" half of event-driven I/O: a demultiplexer tells
// the caller which of its tokens are ready, and the reactor owns the token ->
// interest + callback table. This header keeps the two halves separate so the
// same table can sit on epoll, kqueue, select or an in-process channel without
// knowing which one it is.
//
// Demux contract (structural; there is no interface class to inherit):
//
//   * required, buffered form:
//       std::size_t wait(ready_event* out, std::size_t capacity,
//                        std::chrono::milliseconds timeout);
//     writes at most `capacity` ready events and returns how many it wrote.
//   * alternative required form (used only when the buffered form is absent):
//       std::vector<ready_event> wait(std::chrono::milliseconds timeout);
//   * optional (detected at compile time):
//       status add(token, interest);
//       status modify(token, interest);
//       status remove(token);
//     A demux with none of these is a pure readiness source; the reactor keeps
//     the whole table itself and the hooks are simply not called.
//
// Guarantees this header enforces:
//
//   1. `remove` erases the callback and the interest in the same call, before
//      the demux is told. A readiness report that a backend already harvested
//      is then filtered out by the table lookup, so a removed handler can never
//      run (no wild callback).
//   2. A dispatch notifies only tokens that are both registered and interested
//      in at least one of the ready bits. A stale writable hint for a
//      readable-only registration does not call the handler.
//   3. An exception thrown by a callback is contained. The remaining events in
//      the same batch still run and the table stays usable; the error is
//      counted in `callback_errors()`.
//   4. A callback may call `add` / `modify` / `remove` on the same reactor,
//      including removing a token whose event is later in the same batch. Each
//      event is looked up when it is reached, not snapshotted up front.
//   5. An empty table does not call the demux at all and waits out the timeout
//      on a condition variable, so `dispatch_once` is not a spin loop.
//
// Thread contract:
//
//   * The reactor is not thread-safe. `add`, `modify`, `remove` and
//     `dispatch_once` must be serialized by the owner -- normally the one loop
//     thread that also calls `dispatch_once`. The exception is guarantee 4:
//     callbacks invoked by `dispatch_once` on that same thread may call the
//     table methods.
//   * The demux is called from the dispatching thread only.
//   * The condition variable used for the empty-table wait guards no table
//     state; it is not a substitute for external synchronization.
//
// ABI: header-only template, no exported symbols, additive (MINOR).
#ifndef CRPC_REACTOR_REACTOR_HPP
#define CRPC_REACTOR_REACTOR_HPP

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace reactor {

// What a registration cares about. Both bits false is rejected by `add` and
// `modify`: a token no event can satisfy is a bug, not a state.
struct interest {
    bool readable = false;
    bool writable = false;
};

constexpr bool operator==(interest a, interest b) noexcept {
    return a.readable == b.readable && a.writable == b.writable;
}

constexpr bool operator!=(interest a, interest b) noexcept { return !(a == b); }

// True when at least one bit is set.
constexpr bool any(interest value) noexcept {
    return value.readable || value.writable;
}

// True when `ready` satisfies at least one bit of `want`. A demux may report
// both bits regardless of what was registered; this is the filter that keeps a
// pure-write registration from firing on a readable hint.
constexpr bool overlaps(interest want, interest ready) noexcept {
    return (want.readable && ready.readable) || (want.writable && ready.writable);
}

// Opaque registration key. A backend token (descriptor, slot index, generation)
// maps to one; the reactor never interprets it.
using token = std::uint64_t;

// One readiness report. `tok` is the registered token; `io` is the set of bits
// the demux observed ready.
struct ready_event {
    token tok = 0;
    interest io{};
};

namespace detail {

template <typename Demux, typename = void>
struct has_add : std::false_type {};

template <typename Demux>
struct has_add<Demux,
               std::void_t<decltype(std::declval<Demux&>().add(
                   std::declval<token>(), std::declval<interest>()))>>
    : std::true_type {};

template <typename Demux, typename = void>
struct has_modify : std::false_type {};

template <typename Demux>
struct has_modify<Demux,
                  std::void_t<decltype(std::declval<Demux&>().modify(
                      std::declval<token>(), std::declval<interest>()))>>
    : std::true_type {};

template <typename Demux, typename = void>
struct has_remove : std::false_type {};

template <typename Demux>
struct has_remove<Demux,
                  std::void_t<decltype(std::declval<Demux&>().remove(
                      std::declval<token>()))>> : std::true_type {};

template <typename Demux, typename = void>
struct has_buffered_wait : std::false_type {};

template <typename Demux>
struct has_buffered_wait<
    Demux,
    std::void_t<decltype(std::declval<Demux&>().wait(
        std::declval<ready_event*>(), std::declval<std::size_t>(),
        std::declval<std::chrono::milliseconds>()))>> : std::true_type {};

template <typename Demux, typename = void>
struct has_vector_wait : std::false_type {};

template <typename Demux>
struct has_vector_wait<Demux,
                       std::void_t<decltype(std::declval<Demux&>().wait(
                           std::declval<std::chrono::milliseconds>()))>>
    : std::true_type {};

} // namespace detail

// The token table. See the header comment for the contract; the short version
// is "readiness in, callbacks out, with removal that cannot leave a wild
// callback behind".
template <typename Demux>
class reactor {
public:
    explicit reactor(Demux& demux) : demux_(demux) {}

    reactor(const reactor&) = delete;
    reactor& operator=(const reactor&) = delete;
    reactor(reactor&&) = delete;
    reactor& operator=(reactor&&) = delete;

    // Registers `t` with `i` and `on_ready`. Fails with:
    //   * invalid_argument      -- no interest bit, or an empty callback;
    //   * failed_precondition   -- `t` is already registered;
    //   * whatever the demux's optional add hook reports (state is rolled
    //     back, so a rejected registration leaves the table untouched).
    status add(token t, interest i, std::function<void(token)> on_ready) {
        if (!any(i)) {
            return rpc_error{rpc_code::invalid_argument,
                             "reactor: an interest with no bits set is useless"};
        }
        if (!on_ready) {
            return rpc_error{rpc_code::invalid_argument,
                             "reactor: an empty readiness callback is useless"};
        }
        if (entries_.find(t) != entries_.end()) {
            return rpc_error{rpc_code::failed_precondition,
                             "reactor: the token is already registered"};
        }
        if constexpr (detail::has_add<Demux>::value) {
            const status added = demux_.add(t, i);
            if (!added.ok()) {
                return added;
            }
        }
        try {
            entries_.emplace(t, entry{i, std::move(on_ready)});
        } catch (...) {
            if constexpr (detail::has_remove<Demux>::value) {
                (void)demux_.remove(t);
            }
            return rpc_error{rpc_code::resource_exhausted,
                             "reactor: could not store the registration"};
        }
        return {};
    }

    // Changes the interest mask of an existing token. The callback is kept.
    // Fails with invalid_argument for an empty mask, not_found for an unknown
    // token, and whatever the demux reports (in which case the old mask stays).
    status modify(token t, interest i) {
        const auto it = entries_.find(t);
        if (it == entries_.end()) {
            return rpc_error{rpc_code::not_found,
                             "reactor: cannot modify an unregistered token"};
        }
        if (!any(i)) {
            return rpc_error{rpc_code::invalid_argument,
                             "reactor: an interest with no bits set is useless"};
        }
        if constexpr (detail::has_modify<Demux>::value) {
            const status changed = demux_.modify(t, i);
            if (!changed.ok()) {
                return changed;
            }
        }
        it->second.io = i;
        return {};
    }

    // Removes the token. The callback and the interest are erased *before* the
    // demux is told, so an event already harvested by the backend cannot reach
    // the handler. Fails with not_found for an unknown token; a failure from
    // the demux's optional remove hook is reported after the local entry is
    // gone, because safety (no wild callback) outranks the demux's opinion.
    status remove(token t) {
        const auto it = entries_.find(t);
        if (it == entries_.end()) {
            return rpc_error{rpc_code::not_found,
                             "reactor: cannot remove an unregistered token"};
        }
        entries_.erase(it);
        if constexpr (detail::has_remove<Demux>::value) {
            return demux_.remove(t);
        } else {
            return {};
        }
    }

    // Waits at most `timeout` for readiness and runs the callbacks that match.
    // Returns the number of callbacks invoked (not the number of events the
    // demux reported: duplicates, stale bits and unknown tokens are filtered).
    //
    // With an empty table the demux is not called at all; the call parks on a
    // condition variable for the whole timeout. `timeout == 0` therefore
    // returns immediately in both cases.
    std::size_t dispatch_once(std::chrono::milliseconds timeout) {
        if (entries_.empty()) {
            if (timeout.count() > 0) {
                std::unique_lock<std::mutex> lock(wait_mutex_);
                // The predicate is always false: the wait ends only when the
                // timeout expires. wait_for() keeps waiting through spurious
                // wake-ups, so the call cannot return early on its own.
                (void)wait_cv_.wait_for(lock, timeout, [] { return false; });
            }
            return 0;
        }

        std::size_t available = 0;
        if constexpr (detail::has_buffered_wait<Demux>::value) {
            available = demux_.wait(batch_.data(), batch_.size(), timeout);
            if (available > batch_.size()) {
                available = batch_.size();
            }
        } else {
            static_assert(detail::has_vector_wait<Demux>::value,
                          "a reactor Demux must provide wait(ready_event*, "
                          "size_t, milliseconds) or wait(milliseconds)");
            const std::vector<ready_event> events = demux_.wait(timeout);
            for (const ready_event& event : events) {
                if (available == batch_.size()) {
                    break;
                }
                batch_[available++] = event;
            }
        }

        std::size_t dispatched = 0;
        for (std::size_t index = 0; index < available; ++index) {
            const ready_event event = batch_[index];
            const auto it = entries_.find(event.tok);
            if (it == entries_.end()) {
                continue;
            }
            if (!overlaps(it->second.io, event.io)) {
                continue;
            }
            ++dispatched;
            // The callback is copied before it runs because it may remove its
            // own registration (or another one); the iterator would dangle.
            try {
                std::function<void(token)> callback = it->second.callback;
                if (callback) {
                    callback(event.tok);
                }
            } catch (...) {
                // A handler that throws must not take the loop, the table or
                // the remaining events down with it.
                ++callback_errors_;
            }
        }
        return dispatched;
    }

    [[nodiscard]] std::size_t registered() const noexcept {
        return entries_.size();
    }

    // Number of callback invocations that ended in an exception. Diagnostic
    // only; the reactor does not retry a failed handler.
    [[nodiscard]] std::uint64_t callback_errors() const noexcept {
        return callback_errors_;
    }

private:
    struct entry {
        interest io{};
        std::function<void(token)> callback;
    };

    Demux& demux_;
    std::unordered_map<token, entry> entries_;
    std::array<ready_event, 64> batch_{};
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::uint64_t callback_errors_ = 0;
};

} // namespace reactor
} // namespace crpc

#endif // CRPC_REACTOR_REACTOR_HPP
