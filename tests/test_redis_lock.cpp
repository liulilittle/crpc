// CRPC -- Redis distributed lock tests.
//
// The lock is written against `command_executor`, so the tests drive it with a
// scripted executor and assert the exact command sequence: INCR for the fence, SET
// NX PX for the acquisition, EVAL for the owner-checked release and renew. The
// safety properties (atomic acquire, CAS release, monotonic fence) are what these
// cases pin.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/redis/distributed_lock.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

#include "redis_test_support.hpp"

namespace {

using crpc::redis::command;
using crpc::redis::distributed_lock;
using crpc::redis::lock_options;
using crpc::redis::resp_value;
using redis_test::integer;
using redis_test::null_value;
using redis_test::simple;

[[nodiscard]] lock_options quick_options() {
    lock_options options;
    options.ttl = std::chrono::milliseconds(1000);
    options.acquire_timeout = std::chrono::milliseconds(30);
    options.retry_delay = std::chrono::milliseconds(5);
    return options;
}

// Answers INCR with a rising counter, SET NX according to a script, and EVAL with
// a truthy integer. Every command is recorded here.
//
// All state is behind `mutex` because `execute` runs on whichever thread drives
// the lock: the test thread for acquire/renew/release, and the watchdog thread
// for renewals. TSan caught the resulting race on `executed` -- `push_back` in
// `execute` against the `size()`/`operator[]` reads in the assertions -- and it
// is also why `redis_lock_watchdog_renews_past_one_ttl` failed intermittently
// under `ctest -j 4` with MSVC's "vector subscript out of range": a reallocation
// racing a read hands back a stale element pointer. The accessors below are the
// only way the cases touch the state, so a future case cannot reintroduce it by
// reading a member directly.
class lock_executor final : public crpc::redis::command_executor {
public:
    // Programs the replies for the upcoming SET NX calls.
    void push_set_result(bool acquired) {
        std::lock_guard<std::mutex> guard(mutex_);
        set_results_.push_back(acquired);
    }

    // Whether an EVAL that renews reports success.
    void set_renew_succeeds(bool value) {
        std::lock_guard<std::mutex> guard(mutex_);
        renew_succeeds_ = value;
    }

    // Snapshot of everything executed so far, in order.
    [[nodiscard]] std::vector<command> executed() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return executed_;
    }

    [[nodiscard]] std::size_t executed_count(std::string_view name) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return counted_locked(name);
    }

    // The last recorded command; the caller must check `executed()` is not empty.
    [[nodiscard]] command last_executed() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return executed_.empty() ? command{} : executed_.back();
    }

    crpc::result<resp_value> execute(const command& cmd) override {
        std::lock_guard<std::mutex> guard(mutex_);
        executed_.push_back(cmd);
        if (cmd.empty()) {
            return crpc::rpc_error{crpc::rpc_code::invalid_argument, "empty command"};
        }
        if (cmd[0] == "INCR") {
            return integer(static_cast<std::int64_t>(next_fence_++));
        }
        if (cmd[0] == "SET") {
            const bool acquired = !set_results_.empty() && set_results_.front();
            if (!set_results_.empty()) {
                set_results_.pop_front();
            }
            return acquired ? simple("OK") : null_value();
        }
        if (cmd[0] == "EVAL") {
            // The script text distinguishes renew from release; both answer 1/0.
            const bool renew = cmd.size() > 1 && cmd[1].find("pexpire") != std::string::npos;
            return integer(renew ? (renew_succeeds_ ? 1 : 0) : 1);
        }
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "unexpected command"};
    }

    crpc::result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override {
        std::vector<resp_value> replies;
        replies.reserve(commands.size());
        for (const command& cmd : commands) {
            auto reply = execute(cmd);
            if (!reply) {
                return reply.error();
            }
            replies.push_back(std::move(reply.value()));
        }
        return replies;
    }

private:
    [[nodiscard]] std::size_t counted_locked(std::string_view name) const {
        std::size_t total = 0;
        for (const command& cmd : executed_) {
            if (!cmd.empty() && cmd[0] == name) {
                ++total;
            }
        }
        return total;
    }

    mutable std::mutex mutex_;
    std::deque<bool> set_results_; // true = acquired, false = someone else holds it
    bool renew_succeeds_ = true;
    std::uint64_t next_fence_ = 1;
    std::vector<command> executed_;
};

} // namespace

CRPC_TEST(redis_lock_acquires_with_fence_and_cas_token) {
    lock_executor executor;
    executor.push_set_result(true);
    distributed_lock lock(executor, quick_options());

    auto handle = lock.try_acquire("orders", "worker-1");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        return;
    }
    // The fence is taken before the attempt, and the token carries it.
    CRPC_CHECK_EQ(std::uint64_t{1}, handle.value().fence());
    CRPC_CHECK_EQ(std::string("worker-1:1"), handle.value().token());
    CRPC_CHECK_EQ(std::string("orders"), handle.value().resource());
    CRPC_CHECK_EQ(std::size_t{1}, lock.held());

    // Command order: INCR, then SET NX PX.
    CRPC_CHECK_EQ(std::size_t{2}, executor.executed().size());
    if (executor.executed().size() != 2) {
        return;
    }
    CRPC_CHECK_EQ(std::string("INCR"), executor.executed()[0][0]);
    CRPC_CHECK_EQ(std::string("crpc:lock:fence:orders"), executor.executed()[0][1]);

    const command set = executor.executed()[1];
    CRPC_CHECK_EQ(std::string("SET"), set[0]);
    CRPC_CHECK_EQ(std::string("crpc:lock:orders"), set[1]);
    CRPC_CHECK_EQ(std::string("worker-1:1"), set[2]);
    CRPC_CHECK_EQ(std::string("NX"), set[3]);
    CRPC_CHECK_EQ(std::string("PX"), set[4]);
    CRPC_CHECK_EQ(std::string("1000"), set[5]);
}

CRPC_TEST(redis_lock_reports_contention_as_precondition) {
    lock_executor executor;
    executor.push_set_result(false);
    distributed_lock lock(executor, quick_options());

    auto handle = lock.try_acquire("orders", "worker-2");
    CRPC_CHECK_FALSE(handle.ok());
    if (!handle.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(handle.error().code));
    }
    // A failed attempt leaves nothing held, but the fence was still consumed:
    // monotonicity matters, density does not.
    CRPC_CHECK_EQ(std::size_t{0}, lock.held());
    CRPC_CHECK_EQ(std::size_t{1}, executor.executed_count("INCR"));
}

CRPC_TEST(redis_lock_acquire_retries_until_it_wins) {
    lock_executor executor;
    executor.push_set_result(false);
    executor.push_set_result(false);
    executor.push_set_result(true);
    lock_options options = quick_options();
    // This case pins the retry-until-success behaviour, not the budget edge, so the
    // budget is wide on purpose. With the 30ms default a single sleep_for(5ms) whose
    // actual duration reaches the 30ms budget is enough to end the loop before the
    // third attempt, because the deadline is checked right before each pause, not
    // after. The failing condition is one long pause, not two short ones summed: two
    // 5ms pauses can total more than 30ms and still pass. At 5000ms only one pause
    // would have to run for five seconds to end the loop early.
    options.acquire_timeout = std::chrono::milliseconds(5000);
    options.ttl = std::chrono::milliseconds(10000);
    distributed_lock lock(executor, options);

    auto handle = lock.acquire("orders", "worker-3");
    CRPC_CHECK(handle.ok());
    if (handle.ok()) {
        // Three attempts: two contended, one won, and the fence is the third value.
        CRPC_CHECK_EQ(std::uint64_t{3}, handle.value().fence());
        CRPC_CHECK_EQ(std::size_t{3}, executor.executed_count("SET"));
    }
}

CRPC_TEST(redis_lock_acquire_uses_last_slack_before_deadline) {
    lock_executor executor;
    executor.push_set_result(false);
    executor.push_set_result(true);
    lock_options options = quick_options();
    // The retry delay R (2000ms) is deliberately larger than the budget A (200ms),
    // so the outcome is pure arithmetic and never depends on how long a sleep runs.
    //
    // Old code decided whether to retry with `now + R >= deadline`, which is
    // `0 + 2000 >= 200` on the first iteration: always true, so it gave up after a
    // single attempt and returned deadline_exceeded. It issued SET once and never
    // reached the attempt that would have won.
    //
    // The fixed loop asks only whether the budget is already spent, so it sleeps
    // `min(R, A - now)`, which clamps to about A, and then re-enters the loop body,
    // where try_acquire runs before the deadline check. The second attempt is
    // therefore sent even if that sleep overshot the deadline; only the attempt
    // count and the fence are asserted, never elapsed time.
    options.acquire_timeout = std::chrono::milliseconds(200);
    options.retry_delay = std::chrono::milliseconds(2000);
    distributed_lock lock(executor, options);

    auto handle = lock.acquire("orders", "worker-3b");
    CRPC_CHECK(handle.ok());
    if (handle.ok()) {
        // The second attempt won, so the fence is the second value consumed.
        CRPC_CHECK_EQ(std::uint64_t{2}, handle.value().fence());
    }
    CRPC_CHECK_EQ(std::size_t{2}, executor.executed_count("SET"));
}

CRPC_TEST(redis_lock_acquire_times_out) {
    lock_executor executor;
    // Never acquired: every SET answers nil.
    executor.push_set_result(false);
    executor.push_set_result(false);
    executor.push_set_result(false);
    executor.push_set_result(false);
    lock_options options = quick_options();
    options.acquire_timeout = std::chrono::milliseconds(20);
    distributed_lock lock(executor, options);

    const auto started = std::chrono::steady_clock::now();
    auto handle = lock.acquire("orders", "worker-4");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(handle.ok());
    if (!handle.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(handle.error().code));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(15));
    CRPC_CHECK_EQ(std::size_t{0}, lock.held());
}

// A zero pause must not turn the retry loop into a spin. The bound below is a
// command count, not a duration, so it does not depend on how fast the host is:
// a paced loop issues roughly one SET per retry_delay, while an unpaced one issues
// as many as the executor can answer for the whole budget.
CRPC_TEST(redis_lock_zero_retry_delay_does_not_spin) {
    lock_executor executor;
    lock_options options = quick_options();
    options.acquire_timeout = std::chrono::milliseconds(200);
    options.retry_delay = std::chrono::milliseconds(0);
    distributed_lock lock(executor, options);

    auto handle = lock.acquire("orders", "worker-spin");
    CRPC_CHECK_FALSE(handle.ok());
    if (!handle.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(handle.error().code));
    }
    // The bound is derived from the budget, not guessed: a one-millisecond floor
    // admits about one attempt per millisecond, so 200 ms of budget admits about 201.
    // An unthrottled loop reaches thousands in the same window (13k measured), so the
    // two are two orders of magnitude apart. Doubling the budget rather than adding a
    // few attempts keeps that separation while leaving room for scheduling noise: a cap
    // of exactly 202 would turn a single off-by-one in the deadline comparison red.
    const std::size_t attempts_cap =
        2 * static_cast<std::size_t>(options.acquire_timeout.count());
    CRPC_CHECK(executor.executed_count("SET") <= attempts_cap);
}

// The watchdog renews every handle at one third of the TTL. Without the renewal
// thread a holder that outlives the lease silently loses the lock -- the caller
// still believes it holds it, and the next owner can take it concurrently. This
// case pins the renewal itself: after several periods the lease must have been
// extended more than once and the handle must still be held.
CRPC_TEST(redis_lock_watchdog_renews_past_one_ttl) {
    lock_executor executor;
    executor.push_set_result(true);
    lock_options options = quick_options();
    options.acquire_timeout = std::chrono::milliseconds(3000);
    options.ttl = std::chrono::milliseconds(30);
    options.retry_delay = std::chrono::milliseconds(1);
    distributed_lock lock(executor, options);
    lock.start_watchdog();

    auto handle = lock.acquire("orders", "worker-ttl");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        lock.stop_watchdog();
        return;
    }
    // The renewal period is one third of the TTL, so 300 ms spans several of
    // them and more than one renewal must have landed.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CRPC_CHECK(executor.executed_count("EVAL") >= 2);
    CRPC_CHECK_EQ(std::size_t{1}, lock.held());
    lock.stop_watchdog();
}

// The counterpart: without the watchdog nothing renews, and that is the behaviour
// callers get by default because nothing in the library starts it. Pinning it here
// makes the contract explicit -- an acquire() alone does not keep a lease alive past
// its TTL, so a caller that outlives the lease must ask for renewal.
CRPC_TEST(redis_lock_without_watchdog_lets_the_lease_lapse) {
    lock_executor executor;
    executor.push_set_result(true);
    lock_options options = quick_options();
    options.acquire_timeout = std::chrono::milliseconds(3000);
    options.ttl = std::chrono::milliseconds(30);
    options.retry_delay = std::chrono::milliseconds(1);
    distributed_lock lock(executor, options);

    auto handle = lock.acquire("orders", "worker-nowd");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // No renewal command was issued, so the lease ran out unobserved.
    CRPC_CHECK_EQ(std::size_t{0}, executor.executed_count("EVAL"));
}

CRPC_TEST(redis_lock_renew_uses_cas_script) {
    lock_executor executor;
    executor.push_set_result(true);
    distributed_lock lock(executor, quick_options());
    auto handle = lock.try_acquire("orders", "worker-5");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        return;
    }
    const std::string token(handle.value().token());

    const auto renewed = lock.renew("orders", token);
    CRPC_CHECK(renewed.ok());
    if (renewed.ok()) {
        CRPC_CHECK(renewed.value());
    }
    const command eval = executor.last_executed();
    CRPC_CHECK_EQ(std::string("EVAL"), eval[0]);
    // The script compares the stored token before extending the TTL, so a stale
    // owner cannot renew someone else's lease.
    CRPC_CHECK(eval[1].find("pexpire") != std::string::npos);
    CRPC_CHECK(eval[1].find("get") != std::string::npos);
    CRPC_CHECK_EQ(std::string("1"), eval[2]);
    CRPC_CHECK_EQ(std::string("crpc:lock:orders"), eval[3]);
    CRPC_CHECK_EQ(token, eval[4]);
    CRPC_CHECK_EQ(std::string("1000"), eval[5]);

    // A renew that the script refuses means ownership was lost.
    executor.set_renew_succeeds(false);
    const auto lost = lock.renew("orders", token);
    CRPC_CHECK(lost.ok());
    if (lost.ok()) {
        CRPC_CHECK_FALSE(lost.value());
    }
}

CRPC_TEST(redis_lock_release_uses_cas_script) {
    lock_executor executor;
    executor.push_set_result(true);
    distributed_lock lock(executor, quick_options());
    auto handle = lock.try_acquire("orders", "worker-6");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        return;
    }
    const std::string token(handle.value().token());

    const auto released = lock.release("orders", token);
    CRPC_CHECK(released.ok());
    if (released.ok()) {
        CRPC_CHECK(released.value());
    }
    const command eval = executor.last_executed();
    // The unlock script must compare before deleting; a plain DEL would free a
    // lock that had already been granted to another owner.
    CRPC_CHECK(eval[1].find("get") != std::string::npos);
    CRPC_CHECK(eval[1].find("del") != std::string::npos);
    CRPC_CHECK_EQ(token, eval[4]);
    CRPC_CHECK_EQ(std::size_t{0}, lock.held());
}

CRPC_TEST(redis_lock_handle_releases_on_destruction) {
    lock_executor executor;
    executor.push_set_result(true);
    distributed_lock lock(executor, quick_options());
    {
        auto handle = lock.try_acquire("orders", "worker-7");
        CRPC_CHECK(handle.ok());
        CRPC_CHECK_EQ(std::size_t{1}, lock.held());
        const std::size_t before = executor.executed_count("EVAL");
        (void)before;
    }
    // The destructor released through the CAS script, and the registry is empty.
    CRPC_CHECK_EQ(std::size_t{1}, executor.executed_count("EVAL"));
    CRPC_CHECK_EQ(std::size_t{0}, lock.held());
}

CRPC_TEST(redis_lock_handle_outliving_the_lock_is_safe) {
    lock_executor executor;
    executor.push_set_result(true);
    crpc::redis::lock_handle escaped;
    {
        distributed_lock lock(executor, quick_options());
        auto handle = lock.try_acquire("orders", "worker-8");
        CRPC_CHECK(handle.ok());
        if (!handle.ok()) {
            return;
        }
        escaped = std::move(handle.value());
    }
    // The lock object is gone; the handle still owns its state and must release
    // without touching freed memory.
    CRPC_CHECK(escaped.valid());
    escaped.release();
    CRPC_CHECK_FALSE(escaped.valid());
    escaped.release(); // idempotent
    CRPC_CHECK_EQ(std::size_t{1}, executor.executed_count("EVAL"));
}

CRPC_TEST(redis_lock_rejects_bad_arguments) {
    lock_executor executor;
    distributed_lock lock(executor, quick_options());

    auto no_resource = lock.try_acquire("", "owner");
    CRPC_CHECK_FALSE(no_resource.ok());
    if (!no_resource.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(no_resource.error().code));
    }

    auto no_owner = lock.try_acquire("orders", "");
    CRPC_CHECK_FALSE(no_owner.ok());

    auto bad_renew = lock.renew("orders", "");
    CRPC_CHECK_FALSE(bad_renew.ok());

    auto bad_release = lock.release("", "token");
    CRPC_CHECK_FALSE(bad_release.ok());

    // Nothing reached the server.
    CRPC_CHECK_EQ(std::size_t{0}, executor.executed().size());
}

CRPC_TEST(redis_lock_fences_are_monotonic_across_owners) {
    lock_executor executor;
    executor.push_set_result(true);
    distributed_lock lock(executor, quick_options());
    auto first = lock.try_acquire("orders", "owner-a");
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    const std::uint64_t first_fence = first.value().fence();
    first.value().release();

    executor.push_set_result(true);
    auto second = lock.try_acquire("orders", "owner-b");
    CRPC_CHECK(second.ok());
    if (!second.ok()) {
        return;
    }
    // A new owner must always see a strictly higher fence, even after the previous
    // lease was released normally.
    CRPC_CHECK(second.value().fence() > first_fence);
    CRPC_CHECK(second.value().token().find(std::to_string(second.value().fence())) !=
               std::string::npos);
}

CRPC_TEST_MAIN()