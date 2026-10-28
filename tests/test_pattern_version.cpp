// CRPC -- version pattern tests (optimistic concurrency control).
//
// The concurrency case is a real two-thread race: both threads read the same
// version behind a rendezvous barrier, then both attempt write_if. Exactly one
// write must commit and the other must observe a conflict (failed_precondition).
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <crpc/pattern/version.hpp>

using namespace crpc;
using namespace crpc::pattern;

namespace {

int code_id(rpc_code code) {
    return static_cast<int>(code);
}

} // namespace

CRPC_TEST(version_write_if_cas_success_and_conflict) {
    versioned<int> record(0);

    const auto first = record.read();
    CRPC_CHECK_EQ(0u, first.version);
    CRPC_CHECK_EQ(0, first.value);

    const auto ok = record.write_if(first.version, 10);
    CRPC_CHECK(ok.ok());
    CRPC_CHECK_EQ(1u, ok.value());

    // expected=0 is now stale (current version is 1): the CAS must refuse.
    const auto stale = record.write_if(0, 20);
    CRPC_CHECK_FALSE(stale.ok());
    CRPC_CHECK_EQ(code_id(rpc_code::failed_precondition), code_id(stale.code()));
    CRPC_CHECK_FALSE(stale.error().message.empty());

    // Re-reading the current version, the same write goes through.
    const auto fresh = record.write_if(record.read().version, 20);
    CRPC_CHECK(fresh.ok());
    CRPC_CHECK_EQ(2u, fresh.value());
    CRPC_CHECK_EQ(20, record.read().value);
    CRPC_CHECK_EQ(2u, record.current_version());
}

CRPC_TEST(version_concurrent_two_writers_exactly_one_commits) {
    auto entity = std::make_shared<versioned<int>>(0);
    std::atomic<int> armed{0};
    std::atomic<int> committed{0};
    std::atomic<int> conflicted{0};
    std::atomic<int> loser_code{0};

    auto writer = [&](int value) {
        // Both threads read BEFORE either writes: the rendezvous guarantees
        // both hold the same (stale) version when the write_if starts.
        const u64 expected = entity->read().version;
        armed.fetch_add(1, std::memory_order_seq_cst);
        while (armed.load(std::memory_order_seq_cst) < 2) {
            std::this_thread::yield();
        }
        const auto result = entity->write_if(expected, value);
        if (result.ok()) {
            committed.fetch_add(1, std::memory_order_relaxed);
        } else {
            conflicted.fetch_add(1, std::memory_order_relaxed);
            loser_code.store(code_id(result.code()), std::memory_order_relaxed);
        }
    };

    std::thread t1(writer, 100);
    std::thread t2(writer, 200);
    t1.join();
    t2.join();

    CRPC_CHECK_EQ(1, committed.load());
    CRPC_CHECK_EQ(1, conflicted.load());
    CRPC_CHECK_EQ(code_id(rpc_code::failed_precondition), loser_code.load());

    // Exactly one CAS committed: version advanced by one, final value belongs
    // to the winning writer.
    const auto final_state = entity->read();
    CRPC_CHECK_EQ(1u, final_state.version);
    CRPC_CHECK(final_state.value == 100 || final_state.value == 200);
}

CRPC_TEST(version_update_retries_after_conflict) {
    versioned<int> record(0);

    auto policy = default_version_policy();
    policy.max_attempts = 3;
    policy.initial_backoff = std::chrono::milliseconds(1);
    policy.max_backoff = std::chrono::milliseconds(10);
    policy.jitter_ratio = 0.0;

    int mutator_calls = 0;
    bool external_writer_ok = false;
    const auto result = record.update(
        [&](int current) {
            ++mutator_calls;
            if (mutator_calls == 1) {
                // Another writer commits between read() and write_if(): bump the
                // version out from under update() so the first attempt must
                // conflict and the loop must re-read and retry.
                const auto bump = record.write_if(record.read().version, current + 999);
                external_writer_ok = bump.ok();
            }
            return current + 1;
        },
        policy);

    CRPC_CHECK(result.ok());
    CRPC_CHECK(external_writer_ok);
    CRPC_CHECK(result.value().saw_conflict);
    CRPC_CHECK_EQ(2u, result.value().attempts);
    CRPC_CHECK_EQ(2, mutator_calls);
    CRPC_CHECK_EQ(1000, record.read().value);
    CRPC_CHECK_EQ(2u, record.read().version);
}

CRPC_TEST(version_update_honors_max_attempts) {
    versioned<int> record(0);

    auto policy = default_version_policy();
    policy.max_attempts = 2;
    policy.initial_backoff = std::chrono::nanoseconds(0);
    policy.jitter_ratio = 0.0;

    int mutator_calls = 0;
    const auto result = record.update(
        [&](int current) {
            ++mutator_calls;
            (void)record.write_if(record.read().version, current + 100);
            return current + 1;
        },
        policy);

    // Every attempt conflicts; the attempt cap must terminate the loop with
    // the conflict surfaced and the attempt count attached.
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(code_id(rpc_code::failed_precondition), code_id(result.code()));
    CRPC_CHECK_EQ(2u, result.error().attempts);
    CRPC_CHECK_EQ(2, mutator_calls);
}

CRPC_TEST(version_update_sleeps_backoff_between_attempts) {
    versioned<int> record(0);

    auto policy = default_version_policy();
    policy.max_attempts = 2;
    policy.initial_backoff = std::chrono::milliseconds(40);
    policy.max_backoff = std::chrono::milliseconds(40);
    policy.jitter_ratio = 0.0;

    int mutator_calls = 0;
    const auto start = std::chrono::steady_clock::now();
    (void)record.update(
        [&](int current) {
            ++mutator_calls;
            (void)record.write_if(record.read().version, current + 100);
            return current + 1;
        },
        policy);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    CRPC_CHECK_EQ(2, mutator_calls);
    CRPC_CHECK(elapsed_ms >= 40);
}

CRPC_TEST_MAIN()