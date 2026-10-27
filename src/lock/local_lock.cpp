// CRPC -- local lock manager implementation.
#include <crpc/lock/local_lock.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace crpc {
namespace lock {
namespace {

// One key's holder. `count` is the reentrancy depth for `owner`.
struct key_state {
    std::string owner;
    std::uint32_t count = 0;
    std::chrono::steady_clock::time_point expires{};
};

[[nodiscard]] std::vector<std::string> normalise(span<const std::string> keys) {
    std::vector<std::string> out;
    out.reserve(keys.size());
    for (const std::string& key : keys) {
        if (key.empty()) {
            continue;
        }
        out.push_back(key);
    }
    // A fixed global order is what makes a multi-key acquisition acyclic: two
    // callers that ask for the same set in different orders still queue behind
    // each other instead of each holding half.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

struct lock_manager::impl {
    manager_options options;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, key_state> held;
    // Waiters are counted so a release can skip the notify when nobody is waiting.
    std::size_t waiters = 0;
    bool closed = false;
    std::thread reaper;
    std::atomic<bool> stop{false};

    explicit impl(manager_options opts) : options(opts) {}

    ~impl() {
        {
            std::lock_guard<std::mutex> guard(mutex);
            closed = true;
        }
        cv.notify_all();
        stop.store(true, std::memory_order_release);
        if (reaper.joinable()) {
            reaper.join();
        }
    }

    // The reaper runs on this raw pointer, not on a shared_ptr: a thread holding a
    // reference would keep the state alive forever, so the destructor could never
    // run and the join below could never happen. The destructor joins before any
    // member is destroyed, so the raw pointer stays valid for the thread's life.
    void run_reaper() noexcept {
        while (!stop.load(std::memory_order_acquire)) {
            // Sleep in slices so shutdown is prompt without a second wake-up
            // mechanism; this thread owns nothing else.
            const auto slice = std::chrono::milliseconds(50);
            auto slept = std::chrono::nanoseconds(0);
            while (slept < options.reap_period) {
                if (stop.load(std::memory_order_acquire)) {
                    return;
                }
                std::this_thread::sleep_for(slice);
                slept += slice;
            }
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            {
                std::lock_guard<std::mutex> guard(mutex);
                sweep_locked(std::chrono::steady_clock::now());
            }
            cv.notify_all();
        }
    }

    // Frees leases that have expired. Called with the mutex held.
    void sweep_locked(std::chrono::steady_clock::time_point now) {
        if (options.lease.count() <= 0) {
            return;
        }
        for (auto it = held.begin(); it != held.end();) {
            if (it->second.expires <= now) {
                it = held.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] bool free_for(const std::vector<std::string>& keys,
                                std::string_view owner) const {
        for (const std::string& key : keys) {
            const auto found = held.find(key);
            if (found != held.end() && found->second.owner != owner) {
                return false;
            }
        }
        return true;
    }

    void grant(const std::vector<std::string>& keys, const std::string& owner,
               std::chrono::steady_clock::time_point now) {
        const auto expires =
            options.lease.count() > 0 ? now + options.lease
                                      : std::chrono::steady_clock::time_point::max();
        for (const std::string& key : keys) {
            key_state& state = held[key];
            // Empty owner means the key was not held; a re-entrant grant keeps the
            // original owner and increments the depth.
            if (state.count == 0) {
                state.owner = owner;
            }
            ++state.count;
            state.expires = expires;
        }
    }

    void release_keys(const std::vector<std::string>& keys, const std::string& owner) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            for (const std::string& key : keys) {
                const auto found = held.find(key);
                if (found == held.end() || found->second.owner != owner) {
                    // A handle whose lease already expired has nothing to release;
                    // silently ignoring it is correct, and it must not free a key
                    // that now belongs to someone else.
                    continue;
                }
                if (--found->second.count == 0) {
                    held.erase(found);
                }
            }
        }
        // Wake every waiter: a release may unblock a multi-key acquisition that
        // only now finds all of its keys free.
        cv.notify_all();
    }
};

// --- lock_handle -------------------------------------------------------------

lock_handle::lock_handle(std::shared_ptr<void> state, std::vector<std::string> keys,
                         std::string owner) noexcept
    : state_(std::move(state)), keys_(std::move(keys)), owner_(std::move(owner)) {}

lock_handle::~lock_handle() { release(); }

lock_handle::lock_handle(lock_handle&& other) noexcept
    : state_(std::move(other.state_)),
      keys_(std::move(other.keys_)),
      owner_(std::move(other.owner_)) {}

lock_handle& lock_handle::operator=(lock_handle&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        keys_ = std::move(other.keys_);
        owner_ = std::move(other.owner_);
    }
    return *this;
}

void lock_handle::release() noexcept {
    if (!state_) {
        // Idempotent: a second release, or a release of a moved-from handle, does
        // nothing.
        return;
    }
    auto* state = static_cast<lock_manager::impl*>(state_.get());
    // Copy what is needed before clearing, then drop the state reference after the
    // manager has been touched: the handle may hold the last reference, and
    // releasing it while inside the manager would destroy the state from under the
    // call.
    std::shared_ptr<void> keep = state_;
    state_.reset();
    state->release_keys(keys_, owner_);
    keys_.clear();
    owner_.clear();
    (void)keep;
}

// --- lock_manager ------------------------------------------------------------

lock_manager::lock_manager(manager_options options)
    : impl_(std::make_shared<impl>(options)) {
    if (impl_->options.reap_period.count() > 0) {
        impl* raw = impl_.get();
        impl_->reaper = std::thread([raw] { raw->run_reaper(); });
    }
}

lock_manager::~lock_manager() {
    // The reaper holds a shared_ptr, so the state lives until the thread exits;
    // `impl::~impl` joins it.
    impl_.reset();
}

result<lock_handle> lock_manager::acquire(span<const std::string> keys,
                                          std::chrono::nanoseconds timeout,
                                          std::string owner) {
    const std::vector<std::string> wanted = normalise(keys);
    if (wanted.empty()) {
        return rpc_error{rpc_code::invalid_argument, "a lock needs at least one non-empty key"};
    }
    if (owner.empty()) {
        return rpc_error{rpc_code::invalid_argument, "a lock needs an owner token"};
    }

    std::unique_lock<std::mutex> guard(impl_->mutex);
    const auto now = std::chrono::steady_clock::now();
    impl_->sweep_locked(now);

    if (impl_->free_for(wanted, owner)) {
        impl_->grant(wanted, owner, now);
        return lock_handle(impl_, wanted, owner);
    }

    if (timeout.count() <= 0) {
        return rpc_error{rpc_code::unavailable, "lock is held by another owner"};
    }

    const auto deadline = now + timeout;
    ++impl_->waiters;
    // One wait per wake-up, re-checking the whole set: a notify that frees a
    // different key must not be mistaken for this acquisition's turn.
    for (;;) {
        const auto woke = impl_->cv.wait_until(guard, deadline);
        if (impl_->closed) {
            --impl_->waiters;
            return rpc_error{rpc_code::shutting_down, "lock manager is shutting down"};
        }
        const auto current = std::chrono::steady_clock::now();
        impl_->sweep_locked(current);
        if (impl_->free_for(wanted, owner)) {
            // Re-entry is legal even while waiting: another call by the same owner
            // may have granted part of the set in the meantime.
            impl_->grant(wanted, owner, current);
            --impl_->waiters;
            return lock_handle(impl_, wanted, owner);
        }
        if (woke == std::cv_status::timeout) {
            --impl_->waiters;
            return rpc_error{rpc_code::deadline_exceeded, "lock acquisition timed out"};
        }
    }
}

result<lock_handle> lock_manager::acquire(std::string_view key,
                                          std::chrono::nanoseconds timeout,
                                          std::string owner) {
    const std::string single(key);
    return acquire(span<const std::string>(&single, 1), timeout, std::move(owner));
}

bool lock_manager::held_by(span<const std::string> keys, std::string_view owner) const {
    const std::vector<std::string> wanted = normalise(keys);
    std::lock_guard<std::mutex> guard(impl_->mutex);
    for (const std::string& key : wanted) {
        const auto found = impl_->held.find(key);
        if (found == impl_->held.end() || found->second.owner != owner) {
            return false;
        }
    }
    return !wanted.empty();
}

void lock_manager::sweep(std::chrono::steady_clock::time_point now) {
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->sweep_locked(now);
    }
    impl_->cv.notify_all();
}

std::size_t lock_manager::held_keys() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->held.size();
}

std::size_t lock_manager::waiters() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->waiters;
}

} // namespace lock
} // namespace crpc