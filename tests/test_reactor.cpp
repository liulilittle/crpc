// CRPC -- reactor (readiness-driven dispatch) tests.
//
// The reactor is the readiness-pattern half of the concurrency batch: a
// user-supplied Demux reports which tokens are ready, and the reactor owns the
// token -> interest + callback table. The tests pin the four properties that
// make that useful:
//
//   1. one dispatch notifies only the tokens that are both ready and currently
//      interested (a stale readiness bit must not fire a callback);
//   2. remove() clears the callback and the interest in the same call, so a
//      readiness report that is already in flight can never reach a removed
//      handler (the mutation target for M1);
//   3. an empty registry does not poll the demux and still honours the timeout
//      without busy-waiting (the mutation target for M4);
//   4. an exception thrown by a callback is contained: the remaining callbacks
//      still run and the table stays usable.
//
// The tests use a fake demux, not a sleeping clock: readiness is injected, so
// every case is deterministic on a loaded machine.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/reactor/reactor.hpp>

namespace {

using namespace std::chrono_literals;

using crpc::reactor::interest;
using crpc::reactor::ready_event;
using crpc::reactor::token;

struct hook_log {
    std::vector<token> added;
    std::vector<token> modified;
    std::vector<token> removed;
    std::vector<std::pair<token, interest>> registered;
};

// A Demux whose readiness is injected by the test. It implements the optional
// registration hooks as well, so the reactor's forwarding path is covered.
struct fake_demux {
    std::vector<ready_event> ready;
    hook_log log;
    std::size_t wait_calls = 0;
    std::chrono::milliseconds last_timeout{0};
    token reject_add = 0;

    std::size_t wait(ready_event* out, std::size_t capacity,
                     std::chrono::milliseconds timeout) {
        ++wait_calls;
        last_timeout = timeout;
        std::size_t count = 0;
        for (const ready_event& event : ready) {
            if (count == capacity) {
                break;
            }
            out[count++] = event;
        }
        ready.clear();
        return count;
    }

    crpc::status add(token t, interest i) {
        if (t == reject_add) {
            return crpc::rpc_error{crpc::rpc_code::internal,
                                   "the fake demux rejected the token"};
        }
        log.added.push_back(t);
        log.registered.emplace_back(t, i);
        return {};
    }

    crpc::status modify(token t, interest i) {
        log.modified.push_back(t);
        log.registered.emplace_back(t, i);
        return {};
    }

    crpc::status remove(token t) {
        log.removed.push_back(t);
        return {};
    }
};

// A Demux that only polls: no add/modify/remove hooks. This pins that the
// reactor adapts both a full demux and a pure readiness source.
struct poll_only_demux {
    std::vector<ready_event> ready;
    std::size_t wait_calls = 0;

    std::size_t wait(ready_event* out, std::size_t capacity,
                     std::chrono::milliseconds) {
        ++wait_calls;
        std::size_t count = 0;
        for (const ready_event& event : ready) {
            if (count == capacity) {
                break;
            }
            out[count++] = event;
        }
        ready.clear();
        return count;
    }
};

} // namespace

CRPC_TEST(reactor_dispatches_only_ready_and_interested_tokens) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    std::vector<token> called;
    CRPC_REQUIRE(dispatch.add(1, interest{true, true}, [&](token t) {
        called.push_back(t);
    }).ok());
    CRPC_REQUIRE(dispatch.add(2, interest{true, false}, [&](token t) {
        called.push_back(t);
    }).ok());
    CRPC_CHECK_EQ(std::size_t(2), dispatch.registered());

    demux.ready.push_back(ready_event{1, interest{true, false}});
    demux.ready.push_back(ready_event{2, interest{false, true}}); // not interested
    demux.ready.push_back(ready_event{99, interest{true, true}}); // not registered

    CRPC_CHECK_EQ(std::size_t(1), dispatch.dispatch_once(0ms));
    CRPC_REQUIRE(called.size() == 1);
    CRPC_CHECK_EQ(token(1), called[0]);
    CRPC_CHECK_EQ(std::size_t(1), demux.wait_calls);
}

CRPC_TEST(reactor_modify_changes_the_interest_mask) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    int calls = 0;
    CRPC_REQUIRE(dispatch.add(7, interest{true, false}, [&](token) {
        ++calls;
    }).ok());

    // A writable report while only readable is registered is filtered out.
    demux.ready.push_back(ready_event{7, interest{false, true}});
    CRPC_CHECK_EQ(std::size_t(0), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(0, calls);

    // After the modify the same report fires.
    CRPC_REQUIRE(dispatch.modify(7, interest{false, true}).ok());
    CRPC_CHECK_EQ(std::size_t(1), demux.log.modified.size());
    CRPC_CHECK(demux.log.modified[0] == token(7));

    demux.ready.push_back(ready_event{7, interest{false, true}});
    CRPC_CHECK_EQ(std::size_t(1), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(1, calls);

    // And the old readable bit no longer fires.
    demux.ready.push_back(ready_event{7, interest{true, false}});
    CRPC_CHECK_EQ(std::size_t(0), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(1, calls);
}

// M1 target: remove() must clear the callback and the interest in the same
// call. The injected readiness here is the event a backend had already
// harvested when remove() ran; a callback left in the table would fire.
CRPC_TEST(reactor_remove_clears_the_callback) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    int calls = 0;
    CRPC_REQUIRE(dispatch.add(5, interest{true, false}, [&](token) {
        ++calls;
    }).ok());
    CRPC_REQUIRE(dispatch.remove(5).ok());
    CRPC_CHECK_EQ(std::size_t(0), dispatch.registered());
    CRPC_REQUIRE(demux.log.removed.size() == 1);
    CRPC_CHECK(demux.log.removed[0] == token(5));

    demux.ready.push_back(ready_event{5, interest{true, false}});
    CRPC_CHECK_EQ(std::size_t(0), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(0, calls);
}

// M4 target: the empty registry must not poll the demux, and the call must
// still take the requested timeout instead of spinning. wait_calls == 0 is the
// deterministic part; the elapsed check pins "does not return early".
CRPC_TEST(reactor_empty_registry_waits_out_the_timeout_without_polling) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    const auto start = std::chrono::steady_clock::now();
    const std::size_t dispatched = dispatch.dispatch_once(40ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CRPC_CHECK_EQ(std::size_t(0), dispatched);
    CRPC_CHECK_EQ(std::size_t(0), demux.wait_calls);
    CRPC_CHECK(elapsed >= 30ms);
}

CRPC_TEST(reactor_callback_exception_does_not_break_the_table) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    int survived = 0;
    CRPC_REQUIRE(dispatch.add(1, interest{true, false}, [](token) {
        throw 42;
    }).ok());
    CRPC_REQUIRE(dispatch.add(2, interest{true, false}, [&](token) {
        ++survived;
    }).ok());

    demux.ready.push_back(ready_event{1, interest{true, false}});
    demux.ready.push_back(ready_event{2, interest{true, false}});
    CRPC_CHECK_EQ(std::size_t(2), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(1, survived);
    CRPC_CHECK_EQ(std::size_t(2), dispatch.registered());

    // The table is still usable after the throw.
    demux.ready.push_back(ready_event{2, interest{true, false}});
    CRPC_CHECK_EQ(std::size_t(1), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(2, survived);
}

CRPC_TEST(reactor_callback_can_remove_another_ready_token) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    int first = 0;
    int second = 0;
    CRPC_REQUIRE(dispatch.add(1, interest{true, false}, [&](token) {
        ++first;
        CRPC_CHECK(dispatch.remove(2).ok());
    }).ok());
    CRPC_REQUIRE(dispatch.add(2, interest{true, false}, [&](token) {
        ++second;
    }).ok());

    demux.ready.push_back(ready_event{1, interest{true, false}});
    demux.ready.push_back(ready_event{2, interest{true, false}});
    CRPC_CHECK_EQ(std::size_t(1), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(1, first);
    CRPC_CHECK_EQ(0, second);
}

CRPC_TEST(reactor_reports_duplicate_and_unknown_operations) {
    fake_demux demux;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    CRPC_REQUIRE(dispatch.add(1, interest{true, false}, [](token) {}).ok());
    const crpc::status duplicate =
        dispatch.add(1, interest{true, false}, [](token) {});
    CRPC_CHECK_FALSE(duplicate.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(duplicate.code()));

    const crpc::status missing_modify = dispatch.modify(8, interest{true, true});
    CRPC_CHECK_FALSE(missing_modify.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(missing_modify.code()));

    const crpc::status missing_remove = dispatch.remove(8);
    CRPC_CHECK_FALSE(missing_remove.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(missing_remove.code()));

    const crpc::status no_bits = dispatch.add(9, interest{false, false}, [](token) {});
    CRPC_CHECK_FALSE(no_bits.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(no_bits.code()));

    const crpc::status no_callback = dispatch.add(10, interest{true, false}, {});
    CRPC_CHECK_FALSE(no_callback.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(no_callback.code()));

    CRPC_CHECK_EQ(std::size_t(1), dispatch.registered());
}

CRPC_TEST(reactor_rolls_back_when_the_demux_rejects_a_registration) {
    fake_demux demux;
    demux.reject_add = 3;
    crpc::reactor::reactor<fake_demux> dispatch(demux);

    const crpc::status rejected = dispatch.add(3, interest{true, true}, [](token) {});
    CRPC_CHECK_FALSE(rejected.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                  static_cast<int>(rejected.code()));
    CRPC_CHECK_EQ(std::size_t(0), dispatch.registered());
}

CRPC_TEST(reactor_adapts_a_poll_only_demux) {
    poll_only_demux demux;
    crpc::reactor::reactor<poll_only_demux> dispatch(demux);

    int calls = 0;
    CRPC_REQUIRE(dispatch.add(11, interest{true, false}, [&](token) {
        ++calls;
    }).ok());
    demux.ready.push_back(ready_event{11, interest{true, false}});
    CRPC_CHECK_EQ(std::size_t(1), dispatch.dispatch_once(0ms));
    CRPC_CHECK_EQ(1, calls);
    CRPC_REQUIRE(dispatch.remove(11).ok());
    CRPC_CHECK_EQ(std::size_t(0), dispatch.registered());
}

CRPC_TEST_MAIN()
