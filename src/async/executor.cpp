// CRPC -- bounded worker executor implementation.
#include <crpc/async/executor.hpp>

#include <utility>

namespace crpc {
namespace async {
namespace {

// Set on a worker thread to the executor it serves. A future consults it to refuse
// a wait that could starve the very pool that would run its task.
thread_local executor* current_executor = nullptr;

} // namespace

executor::executor(executor_options options) : options_(options) {
    if (options_.threads == 0) {
        options_.threads = 1;
    }
    if (options_.queue_capacity == 0) {
        options_.queue_capacity = 1;
    }
    workers_.reserve(options_.threads);
    for (std::size_t i = 0; i < options_.threads; ++i) {
        workers_.emplace_back([this] { run_worker(); });
    }
}

executor::~executor() { shutdown(); }

executor* executor::current() noexcept { return current_executor; }

void executor::run_worker() {
    current_executor = this;
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
            if (queue_.empty()) {
                // Closed and drained: the shutdown contract is that every queued
                // task runs before the join, so this is the only exit.
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // The task runs without the queue locked: a task that submits more work must
        // not deadlock against its own executor's queue.
        task();
    }
}

void executor::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::size_t executor::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool executor::is_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

} // namespace async
} // namespace crpc