// CRPC -- a bounded worker executor for blocking operations.
//
// The framework's first choice is a coroutine on a shard: `crpc::net::connection`
// parks the calling coroutine and the shard thread stays free. A database driver
// or a library that only offers a blocking call cannot participate in that model,
// so this executor runs such a call off the shard and hands the result back through
// a future.
//
// Three rules keep it from becoming a deadlock source:
//
//   * **bounded queue.** `submit` refuses work when the queue is full instead of
//     growing without limit. Backpressure is the caller's decision.
//   * **no blocking a worker on its own executor.** `future::get()` called from a
//     thread of the executor it belongs to would wait for a task that can never be
//     scheduled; it returns `failed_precondition` instead of hanging.
//   * **shutdown drains.** Destroying the executor runs the queued tasks and then
//     joins, so a future that was handed out always completes.
//
// A task that returns `crpc::result<T>` produces `future<T>` whose `get()` returns
// `result<T>` — the result is flattened, not nested, so a caller never unwraps two
// layers to find one error.
#ifndef CRPC_ASYNC_EXECUTOR_HPP
#define CRPC_ASYNC_EXECUTOR_HPP

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace async {

class executor;

namespace detail {

template <typename T>
struct is_result : std::false_type {};

template <typename T>
struct is_result<crpc::result<T>> : std::true_type {};

// Maps a callable's return type to the future's value type, flattening
// esult<T> to T. Declared here rather than inside executor because a
// member's trailing return type cannot see a nested type declared later.
template <typename Produced>
struct produced_value {
    using type = Produced;
};
template <typename U>
struct produced_value<crpc::result<U>> {
    using type = U;
};

} // namespace detail

// A one-shot result channel. `T` is the task's value type after flattening.
template <typename T>
class future {
public:
    future() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

    [[nodiscard]] bool ready() const {
        if (!state_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->ready;
    }

    // Waits for the result.
    //
    // From a worker thread of the executor that owns this future it returns
    // `failed_precondition`: waiting there could block every thread that could run
    // the task it is waiting for. Reporting that is better than a hang that looks
    // like a slow query.
    [[nodiscard]] result<T> get() {
        if (!state_) {
            return rpc_error{rpc_code::failed_precondition, "future is not valid"};
        }
        if (owner_ != nullptr && executor::current() == owner_) {
            return rpc_error{rpc_code::failed_precondition,
                             "a worker must not block on its own executor"};
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait(lock, [&] { return state_->ready; });
        return outcome();
    }

    // Waits up to `timeout`.
    [[nodiscard]] result<T> wait_for(std::chrono::nanoseconds timeout) {
        if (!state_) {
            return rpc_error{rpc_code::failed_precondition, "future is not valid"};
        }
        if (owner_ != nullptr && executor::current() == owner_) {
            return rpc_error{rpc_code::failed_precondition,
                             "a worker must not block on its own executor"};
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->cv.wait_for(lock, timeout, [&] { return state_->ready; })) {
            return rpc_error{rpc_code::deadline_exceeded,
                             "the operation did not finish in time"};
        }
        return outcome();
    }

    // Non-blocking. `failed_precondition` while the task is still running.
    [[nodiscard]] result<T> try_get() {
        if (!state_) {
            return rpc_error{rpc_code::failed_precondition, "future is not valid"};
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->ready) {
            return rpc_error{rpc_code::failed_precondition,
                             "the operation has not finished"};
        }
        return outcome();
    }

    struct state {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false;
        // A task that returned an error completes its future with that error rather
        // than with a default value.
        bool failed = false;
        rpc_error error;
        std::optional<T> value;
    };

private:
    friend class executor;

    future(std::shared_ptr<state> shared, executor* owner) noexcept
        : state_(std::move(shared)), owner_(owner) {}

    // Called with the state locked.
    [[nodiscard]] result<T> outcome() const {
        if (state_->failed) {
            return state_->error;
        }
        return *state_->value;
    }

    std::shared_ptr<state> state_;
    executor* owner_ = nullptr;
};

struct executor_options {
    // Worker threads. One is enough for a serialised workload; more only helps when
    // the blocking calls actually overlap.
    std::size_t threads = 4;

    // Queue capacity. A submit beyond it is refused.
    std::size_t queue_capacity = 4096;
};

class CRPC_API executor {
public:
    explicit executor(executor_options options = {});
    ~executor();

    executor(const executor&) = delete;
    executor& operator=(const executor&) = delete;
    executor(executor&&) = delete;
    executor& operator=(executor&&) = delete;

    // Runs `callable` on a worker and returns a future for its result.
    //
    // A callable returning `result<T>` yields `future<T>`: a failure is stored in
    // the future and reported by `get()`, so a failed task still completes and no
    // exception ever crosses the thread boundary.
    template <typename Callable>
    auto submit(Callable&& callable) -> future<typename detail::produced_value<decltype(callable())>::type> {
        using produced = decltype(callable());
        using value_type = typename detail::produced_value<produced>::type;
        using state_type = typename future<value_type>::state;

        auto shared = std::make_shared<state_type>();
        auto task = [shared, callable = std::forward<Callable>(callable)]() mutable {
            store<value_type, produced>(*shared, callable);
            shared->cv.notify_all();
        };

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_ || queue_.size() >= options_.queue_capacity) {
                // The task never runs, so an invalid future is returned: the caller
                // must check `valid()`. A ready default value would fabricate a
                // successful result for work that never happened.
                return future<value_type>();
            }
            queue_.push_back(std::move(task));
            cv_.notify_one();
        }
        return future<value_type>(std::move(shared), this);
    }

    // The executor whose worker is running the current thread, or nullptr. Used by
    // `future::get` to refuse a self-deadlock.
    [[nodiscard]] static executor* current() noexcept;

    // Runs the queued tasks, then joins. Idempotent.
    void shutdown() noexcept;

    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] std::size_t threads() const noexcept { return options_.threads; }
    [[nodiscard]] bool is_closed() const;

private:
    // Stores the task's outcome. A `result<T>` is flattened: success becomes the
    // value, failure becomes the future's error, so `get()` never returns a result
    // nested inside a result.
    template <typename ValueType, typename Produced, typename Callable>
    static void store(typename future<ValueType>::state& state, Callable& callable) {
        if constexpr (detail::is_result<Produced>::value) {
            Produced produced = callable();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (produced.ok()) {
                state.value = std::move(produced.value());
            } else {
                state.failed = true;
                state.error = produced.error();
            }
            state.ready = true;
        } else {
            ValueType produced = callable();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.value = std::move(produced);
            state.ready = true;
        }
    }
    void run_worker();

    executor_options options_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool closed_ = false;
};

} // namespace async
} // namespace crpc

#endif // CRPC_ASYNC_EXECUTOR_HPP
