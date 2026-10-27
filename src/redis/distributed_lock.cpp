// CRPC -- Redis distributed lock implementation.
#include <crpc/redis/distributed_lock.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace crpc {
namespace redis {
namespace {

// Unlock: delete only when the stored token is still ours. A plain DEL here is the
// bug that lets an owner whose lease expired free someone else's lock.
constexpr const char* unlock_script =
    "if redis.call('get', KEYS[1]) == ARGV[1] then "
    "return redis.call('del', KEYS[1]) else return 0 end";

// Renew: extend the TTL only when the token is still ours.
constexpr const char* renew_script =
    "if redis.call('get', KEYS[1]) == ARGV[1] then "
    "return redis.call('pexpire', KEYS[1], ARGV[2]) else return 0 end";

[[nodiscard]] std::string to_decimal(std::uint64_t value) {
    return std::to_string(value);
}

[[nodiscard]] bool reply_truthy(const resp_value& reply) {
    if (reply.is_integer()) {
        return reply.as_integer() != 0;
    }
    if (reply.is_boolean()) {
        return reply.as_boolean();
    }
    return false;
}

// A Redis error reply is a value, not a transport failure; turning it into a
// typed error here keeps every call site from re-checking.
[[nodiscard]] status reply_status(const resp_value& reply, std::string_view what) {
    if (!reply.is_error()) {
        return status{};
    }
    return status{rpc_code::application, std::string(what) + ": " + reply.as_error()};
}

} // namespace

struct distributed_lock::impl {
    command_executor* executor = nullptr;
    lock_options options;

    mutable std::mutex mutex;
    // Live tokens by lock key, so the watchdog knows what to renew.
    std::map<std::string, std::string> live;
    std::thread watchdog;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    bool running = false;

    impl(command_executor& exec, lock_options opts) : executor(&exec), options(std::move(opts)) {}

    ~impl() {
        stop.store(true, std::memory_order_release);
        cv.notify_all();
        if (watchdog.joinable()) {
            watchdog.join();
        }
    }

    [[nodiscard]] std::string lock_key(std::string_view resource) const {
        std::string key = options.key_prefix;
        key.append(resource.data(), resource.size());
        return key;
    }

    [[nodiscard]] std::string fence_key(std::string_view resource) const {
        std::string key = options.fence_prefix;
        key.append(resource.data(), resource.size());
        return key;
    }

    // Executes one command, keeping a Redis error reply as an error value.
    [[nodiscard]] result<resp_value> run(std::string_view what, command cmd) {
        result<resp_value> reply = executor->execute(cmd);
        if (!reply) {
            return reply.error();
        }
        const status failed = reply_status(reply.value(), what);
        if (!failed) {
            return failed.error();
        }
        return reply;
    }

    void run_watchdog() noexcept {
        // Renew at one third of the TTL: two renewals may be lost before the lease
        // actually expires, which tolerates a slow round trip without weakening the
        // lease itself.
        const auto period =
            std::max(std::chrono::milliseconds(50), options.ttl / 3);
        while (!stop.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (cv.wait_for(lock, period, [&] { return stop.load(std::memory_order_acquire); })) {
                    return;
                }
            }
            std::vector<std::string> keys;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& entry : live) {
                    keys.push_back(entry.first);
                }
            }
            for (const std::string& key : keys) {
                std::string token;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto found = live.find(key);
                    if (found == live.end()) {
                        continue;
                    }
                    token = found->second;
                }
                command cmd{"EVAL", renew_script, "1", key, token,
                            to_decimal(static_cast<std::uint64_t>(options.ttl.count()))};
                result<resp_value> renewed = executor->execute(cmd);
                // A lock that is no longer ours must be dropped from the renewal
                // set: retrying would keep stealing a renew of someone else's key
                // (the CAS makes it a no-op, but the entry is dead weight and hides
                // that ownership was lost).
                if (!renewed || !reply_truthy(renewed.value())) {
                    std::lock_guard<std::mutex> lock(mutex);
                    live.erase(key);
                }
            }
        }
    }

    // Owner-checked renew/release live here so a handle can call them without
    // reaching back into the public object (which it does not own).
    [[nodiscard]] bool renew_token(std::string_view resource, std::string_view token) {
        command cmd{"EVAL", renew_script, "1", lock_key(resource), std::string(token),
                    to_decimal(static_cast<std::uint64_t>(options.ttl.count()))};
        result<resp_value> reply = executor->execute(cmd);
        return reply && reply_truthy(reply.value());
    }

    [[nodiscard]] bool release_token(std::string_view resource, std::string_view token) {
        command cmd{"EVAL", unlock_script, "1", lock_key(resource), std::string(token)};
        result<resp_value> reply = executor->execute(cmd);
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = live.find(lock_key(resource));
            if (found != live.end() && found->second == token) {
                live.erase(found);
            }
        }
        return reply && reply_truthy(reply.value());
    }
};

// --- lock_handle -------------------------------------------------------------

lock_handle::lock_handle(std::shared_ptr<void> state, std::string resource, std::string token,
                         std::uint64_t fence)
    : state_(std::move(state)),
      resource_(std::move(resource)),
      token_(std::move(token)),
      fence_(fence) {}

lock_handle::~lock_handle() { release(); }

lock_handle::lock_handle(lock_handle&& other) noexcept
    : state_(std::move(other.state_)),
      resource_(std::move(other.resource_)),
      token_(std::move(other.token_)),
      fence_(other.fence_) {}

lock_handle& lock_handle::operator=(lock_handle&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        resource_ = std::move(other.resource_);
        token_ = std::move(other.token_);
        fence_ = other.fence_;
    }
    return *this;
}

void lock_handle::release() noexcept {
    if (!state_) {
        return;
    }
    auto* state = static_cast<distributed_lock::impl*>(state_.get());
    // Keep the state alive across the release: this handle may hold the last
    // reference, and dropping it first would destroy the object under the call.
    std::shared_ptr<void> keep = state_;
    state_.reset();
    (void)state->release_token(resource_, token_);
    resource_.clear();
    token_.clear();
    fence_ = 0;
    (void)keep;
}

// --- distributed_lock --------------------------------------------------------

distributed_lock::distributed_lock(command_executor& executor, lock_options options)
    : impl_(std::make_shared<impl>(executor, std::move(options))) {}

distributed_lock::~distributed_lock() {
    stop_watchdog();
    impl_.reset();
}

result<lock_handle> distributed_lock::try_acquire(std::string_view resource,
                                                   std::string_view owner) {
    if (resource.empty()) {
        return rpc_error{rpc_code::invalid_argument, "a lock needs a resource name"};
    }
    if (owner.empty()) {
        return rpc_error{rpc_code::invalid_argument, "a lock needs an owner token"};
    }

    const std::string key = impl_->lock_key(resource);

    // The fence is taken before the lock attempt. A failed attempt burns a number,
    // which is fine: the guarantee is monotonicity, not density.
    result<resp_value> fence_reply = impl_->run("INCR", command{"INCR", impl_->fence_key(resource)});
    if (!fence_reply) {
        return fence_reply.error();
    }
    if (!fence_reply.value().is_integer()) {
        return rpc_error{rpc_code::protocol_error, "INCR did not return an integer"};
    }
    const std::uint64_t fence = static_cast<std::uint64_t>(fence_reply.value().as_integer());

    // The token carries the fence so a release can prove ownership and a resource
    // can order writers by the number it has already seen.
    std::string token(owner);
    token.push_back(':');
    token.append(to_decimal(fence));

    result<resp_value> set_reply = impl_->run(
        "SET NX",
        command{"SET", key, token, "NX", "PX",
                to_decimal(static_cast<std::uint64_t>(impl_->options.ttl.count()))});
    if (!set_reply) {
        return set_reply.error();
    }
    // Redis answers a failed SET NX with a null bulk reply, not an error.
    if (set_reply.value().is_null()) {
        return rpc_error{rpc_code::failed_precondition, "the lock is held by another owner"};
    }
    if (!set_reply.value().is_simple_string() && !set_reply.value().is_bulk_string()) {
        return rpc_error{rpc_code::protocol_error, "SET NX returned an unexpected reply"};
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->live[key] = token;
    }

    // The token is embedded in the handle, so the destructor releases the exact
    // lease it was granted rather than whatever is there now.
    return lock_handle(impl_, std::string(resource), std::move(token), fence);
}

result<lock_handle> distributed_lock::acquire(std::string_view resource, std::string_view owner) {
    const auto deadline = std::chrono::steady_clock::now() + impl_->options.acquire_timeout;
    for (;;) {
        result<lock_handle> attempt = try_acquire(resource, owner);
        if (attempt) {
            return attempt;
        }
        // Only contention is retried; a transport or protocol failure is reported
        // immediately, because retrying it would just repeat the same failure.
        if (attempt.error().code != rpc_code::failed_precondition) {
            return attempt.error();
        }
        if (std::chrono::steady_clock::now() + impl_->options.retry_delay >= deadline) {
            return rpc_error{rpc_code::deadline_exceeded, "timed out acquiring the lock"};
        }
        std::this_thread::sleep_for(impl_->options.retry_delay);
    }
}

result<bool> distributed_lock::renew(std::string_view resource, std::string_view token) {
    if (resource.empty() || token.empty()) {
        return rpc_error{rpc_code::invalid_argument, "renew needs a resource and a token"};
    }
    return impl_->renew_token(resource, token);
}

result<bool> distributed_lock::release(std::string_view resource, std::string_view token) {
    if (resource.empty() || token.empty()) {
        return rpc_error{rpc_code::invalid_argument, "release needs a resource and a token"};
    }
    return impl_->release_token(resource, token);
}

void distributed_lock::start_watchdog() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) {
        return;
    }
    impl_->running = true;
    impl_->stop.store(false, std::memory_order_release);
    impl* raw = impl_.get();
    impl_->watchdog = std::thread([raw] { raw->run_watchdog(); });
}

void distributed_lock::stop_watchdog() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        impl_->running = false;
        impl_->stop.store(true, std::memory_order_release);
    }
    impl_->cv.notify_all();
    if (impl_->watchdog.joinable()) {
        impl_->watchdog.join();
    }
}

std::size_t distributed_lock::held() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->live.size();
}

} // namespace redis
} // namespace crpc