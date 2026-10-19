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
        delivering_on = this;
        return target_;
    }

    // Announces that the call started by `enter()` has returned.
    void leave() noexcept {
        std::lock_guard<std::mutex> lk(m_);
        delivering_on = nullptr;
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
        if (delivering_on != this) {
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
    // The guard this thread is currently delivering into, if any.
    static thread_local const bound_guard* delivering_on;

    mutable std::mutex m_;
    std::condition_variable cv_;
    source* target_ = nullptr;
    int in_flight_ = 0;
    bool retired_ = false;
};

inline thread_local const bound_guard* bound_guard::delivering_on = nullptr;

} // namespace detail
} // namespace io
} // namespace crpc

#endif // CRPC_IO_BOUND_GUARD_HPP
