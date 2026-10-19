// CRPC -- lifetime guard for a borrowed io::source.
//
// A backend installs the framework source with `connection::bind_source` and
// calls into it from whichever thread drives traffic. The source is borrowed:
// the caller may destroy it as soon as `bind_source` returns, including
// `bind_source(nullptr)`. This guard makes that contract explicit.
//
// `enter()` announces one call and returns the source to invoke (or null when
// none is bound). `leave()` publishes its completion. `rebind()` installs a
// new source and waits until every announced call has finished before it
// returns. No lock is held while the user callback runs.
//
// The wait is skipped when the caller is already delivering into this guard on
// the same thread; a callback that unbinds its own connection would otherwise
// wait for itself forever.
#ifndef CRPC_IO_BOUND_GUARD_HPP
#define CRPC_IO_BOUND_GUARD_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace crpc {
namespace io {

class source;

namespace detail {

class bound_guard {
public:
    bound_guard() = default;
    bound_guard(const bound_guard&) = delete;
    bound_guard& operator=(const bound_guard&) = delete;

    // Returns the source to call, or null when no source is bound. On a
    // non-null return the caller must call `leave()` after the call.
    source* enter() noexcept {
        std::lock_guard<std::mutex> lk(m_);
        if (target_ == nullptr || retired_) {
            return nullptr;
        }
        ++in_flight_;
        push_delivery(this);
        return target_;
    }

    // Announces that the call started by `enter()` has returned.
    void leave() noexcept {
        std::lock_guard<std::mutex> lk(m_);
        pop_delivery();
        if (--in_flight_ == 0) {
            cv_.notify_all();
        }
    }

    // Installs `s` and waits for calls already running into the previous
    // source. A null source retires the guard; a non-null source clears the
    // retired state. The wait releases the mutex, so an in-flight callback can
    // finish its own `leave()`.
    void rebind(source* s) noexcept {
        std::unique_lock<std::mutex> lk(m_);
        target_ = s;
        retired_ = (s == nullptr);
        if (!is_delivering_on(this)) {
            cv_.wait(lk, [this] { return in_flight_ == 0; });
        }
    }

    // Equivalent to `rebind(nullptr)`.
    void retire() noexcept { rebind(nullptr); }

    [[nodiscard]] bool active() const noexcept {
        std::lock_guard<std::mutex> lk(m_);
        return target_ != nullptr && !retired_;
    }

private:
    // Delivery markers are a stack: nested delivery on one thread (A calls
    // into B) must restore A's marker when B's call returns. A single slot
    // would clear the outer marker and turn a later self-unbind into a
    // self-deadlock.
    static constexpr std::size_t kMaxDeliveryDepth = 16;
    struct delivery_stack {
        const bound_guard* slots[kMaxDeliveryDepth] = {};
        std::size_t depth = 0;
        // Entries beyond the capacity are not recorded. Any rebind on this
        // thread then conservatively skips its wait (a deep nesting is
        // pathological; avoiding a self-deadlock matters more than delaying a
        // return in that case).
        std::size_t overflow = 0;
    };
    static thread_local delivery_stack delivery_;

    static void push_delivery(const bound_guard* guard) noexcept {
        if (delivery_.depth < kMaxDeliveryDepth) {
            delivery_.slots[delivery_.depth++] = guard;
        } else {
            ++delivery_.overflow;
        }
    }

    static void pop_delivery() noexcept {
        if (delivery_.overflow > 0) {
            --delivery_.overflow;
        } else if (delivery_.depth > 0) {
            --delivery_.depth;
        }
    }

    static bool is_delivering_on(const bound_guard* guard) noexcept {
        if (delivery_.overflow > 0) {
            return true;
        }
        for (std::size_t i = 0; i < delivery_.depth; ++i) {
            if (delivery_.slots[i] == guard) {
                return true;
            }
        }
        return false;
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    source* target_ = nullptr;
    int in_flight_ = 0;
    bool retired_ = false;
};

inline thread_local bound_guard::delivery_stack bound_guard::delivery_ = {};

} // namespace detail
} // namespace io
} // namespace crpc

#endif // CRPC_IO_BOUND_GUARD_HPP
