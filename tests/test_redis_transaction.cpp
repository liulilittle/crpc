// CRPC Redis -- MULTI/EXEC/WATCH state machine.
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <crpc/redis/transaction.hpp>

#include "redis_test_support.hpp"
#include "test_framework.hpp"

using namespace redis_test;

namespace {

struct tx_state {
    bool in_multi = false;
    bool abort_watch = false;
    bool queue_error = false;
    bool exec_error = false;
    bool transport_error = false;
    std::vector<resp_value> exec_replies;
    std::vector<std::string> log;
};

scripted_executor::handler_fn make_handler(tx_state& state) {
    return [&state](const command& cmd) -> crpc::result<resp_value> {
        state.log.push_back(cmd[0]);

        if (cmd[0] == "WATCH" || cmd[0] == "UNWATCH" || cmd[0] == "MULTI" ||
            cmd[0] == "DISCARD") {
            if (cmd[0] == "MULTI") {
                state.in_multi = true;
            } else if (cmd[0] == "DISCARD") {
                state.in_multi = false;
            }
            return simple("OK");
        }

        if (cmd[0] == "EXEC") {
            state.in_multi = false;
            if (state.exec_error) {
                return error("EXECABORT Transaction discarded");
            }
            if (state.abort_watch) {
                return null_value();
            }
            return array(state.exec_replies);
        }

        if (!state.in_multi) {
            return error("ERR command not allowed outside MULTI");
        }
        if (state.transport_error) {
            return crpc::rpc_error{crpc::rpc_code::connection_reset,
                                   "connection lost while queueing"};
        }
        if (state.queue_error) {
            return error("ERR unknown command");
        }
        return simple("QUEUED");
    };
}

} // namespace

CRPC_TEST(transaction_commit_returns_all_replies) {
    tx_state state;
    state.exec_replies = {simple("OK"), integer(2)};
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.watch({"watched"}).ok());
    CRPC_CHECK(tx.multi().ok());
    CRPC_CHECK(tx.active());
    CRPC_CHECK(tx.queue({"SET", "key", "value"}).ok());
    CRPC_CHECK(tx.queue({"INCR", "counter"}).ok());
    CRPC_CHECK_EQ(std::size_t{2}, tx.queued());

    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK(result.ok());
    CRPC_CHECK_FALSE(result.value().aborted);
    CRPC_CHECK_EQ(std::size_t{2}, result.value().replies.size());
    CRPC_CHECK_EQ(std::string("OK"),
                  result.value().replies[0].as_simple_string());
    CRPC_CHECK_EQ(std::int64_t{2}, result.value().replies[1].as_integer());
    CRPC_CHECK_FALSE(tx.active());
    CRPC_CHECK_EQ(std::size_t{0}, tx.queued());

    CRPC_CHECK_EQ(std::size_t{5}, state.log.size());
    CRPC_CHECK_EQ(std::string("WATCH"), state.log[0]);
    CRPC_CHECK_EQ(std::string("MULTI"), state.log[1]);
    CRPC_CHECK_EQ(std::string("SET"), state.log[2]);
    CRPC_CHECK_EQ(std::string("INCR"), state.log[3]);
    CRPC_CHECK_EQ(std::string("EXEC"), state.log[4]);
}

CRPC_TEST(transaction_watch_conflict_reports_aborted) {
    tx_state state;
    state.abort_watch = true;
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.watch({"key"}).ok());
    CRPC_CHECK(tx.multi().ok());
    CRPC_CHECK(tx.queue({"SET", "key", "value"}).ok());

    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK(result.ok());
    CRPC_CHECK(result.value().aborted);
    CRPC_CHECK(result.value().replies.empty());
    CRPC_CHECK_FALSE(tx.active());
}

CRPC_TEST(transaction_discard_clears_state) {
    tx_state state;
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.multi().ok());
    CRPC_CHECK(tx.queue({"SET", "key", "value"}).ok());
    CRPC_CHECK(tx.discard().ok());
    CRPC_CHECK_FALSE(tx.active());
    CRPC_CHECK_EQ(std::size_t{0}, tx.queued());

    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(result.code()));
}

CRPC_TEST(transaction_rejects_misordered_calls) {
    tx_state state;
    scripted_executor executor(make_handler(state));
    crpc::redis::transaction tx(executor);

    crpc::result<crpc::redis::transaction_exec_result> early = tx.exec();
    CRPC_CHECK_FALSE(early.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(early.code()));

    crpc::status queue_outside = tx.queue({"SET", "a", "1"});
    CRPC_CHECK_FALSE(queue_outside.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(queue_outside.code()));

    CRPC_CHECK(tx.multi().ok());
    crpc::status second_multi = tx.multi();
    CRPC_CHECK_FALSE(second_multi.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(second_multi.code()));

    crpc::status watch_inside = tx.watch({"key"});
    CRPC_CHECK_FALSE(watch_inside.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(watch_inside.code()));

    crpc::status unwatch_inside = tx.unwatch();
    CRPC_CHECK_FALSE(unwatch_inside.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(unwatch_inside.code()));
}

CRPC_TEST(transaction_watch_requires_a_key) {
    tx_state state;
    scripted_executor executor(make_handler(state));
    crpc::redis::transaction tx(executor);

    crpc::status empty = tx.watch(std::vector<std::string>{});
    CRPC_CHECK_FALSE(empty.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(empty.code()));
    CRPC_CHECK(executor.executed.empty());
}

CRPC_TEST(transaction_watch_builds_all_keys) {
    tx_state state;
    scripted_executor executor(make_handler(state));
    crpc::redis::transaction tx(executor);

    CRPC_CHECK(tx.watch({"a", "b", "c"}).ok());
    CRPC_CHECK_EQ(std::size_t{1}, executor.executed.size());
    CRPC_CHECK_EQ(std::size_t{4}, executor.executed[0].size());
    CRPC_CHECK_EQ(std::string("WATCH"), executor.executed[0][0]);
    CRPC_CHECK_EQ(std::string("a"), executor.executed[0][1]);
    CRPC_CHECK_EQ(std::string("b"), executor.executed[0][2]);
    CRPC_CHECK_EQ(std::string("c"), executor.executed[0][3]);
}

CRPC_TEST(transaction_queue_error_marks_dirty_and_discards) {
    tx_state state;
    state.queue_error = true;
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.multi().ok());
    crpc::status queued = tx.queue({"BOGUS"});
    CRPC_CHECK_FALSE(queued.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(queued.code()));
    CRPC_CHECK(tx.dirty());
    CRPC_CHECK_EQ(std::size_t{0}, tx.queued());

    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(result.code()));
    CRPC_CHECK_EQ(std::string("ERR unknown command"), result.error().message);
    CRPC_CHECK_FALSE(tx.active());
    CRPC_CHECK_FALSE(tx.dirty());
    CRPC_CHECK_EQ(std::string("DISCARD"), state.log.back());
}

CRPC_TEST(transaction_transport_error_marks_dirty) {
    tx_state state;
    state.transport_error = true;
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.multi().ok());
    crpc::status queued = tx.queue({"SET", "a", "1"});
    CRPC_CHECK_FALSE(queued.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                  static_cast<int>(queued.code()));
    CRPC_CHECK(tx.dirty());

    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK_FALSE(result.ok());
    // The original transport error is preserved, not flattened to `application`.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                  static_cast<int>(result.code()));
    CRPC_CHECK_EQ(std::string("connection lost while queueing"),
                  result.error().message);
}

CRPC_TEST(transaction_empty_queue_is_dirty) {
    tx_state state;
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.multi().ok());
    crpc::status queued = tx.queue(crpc::redis::command{});
    CRPC_CHECK_FALSE(queued.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(queued.code()));
    CRPC_CHECK(tx.dirty());
}

CRPC_TEST(transaction_exec_error_reply_is_application_error) {
    tx_state state;
    state.exec_error = true;
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.multi().ok());
    CRPC_CHECK(tx.queue({"SET", "a", "1"}).ok());
    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(result.code()));
    CRPC_CHECK_FALSE(tx.active());
}

CRPC_TEST(transaction_exec_replies_may_contain_errors) {
    tx_state state;
    state.exec_replies = {error("WRONGTYPE Operation against a key"), integer(1)};
    scripted_executor executor(make_handler(state));

    crpc::redis::transaction tx(executor);
    CRPC_CHECK(tx.multi().ok());
    CRPC_CHECK(tx.queue({"LPUSH", "key", "x"}).ok());
    CRPC_CHECK(tx.queue({"INCR", "counter"}).ok());

    crpc::result<crpc::redis::transaction_exec_result> result = tx.exec();
    CRPC_CHECK(result.ok());
    CRPC_CHECK_FALSE(result.value().aborted);
    CRPC_CHECK_EQ(std::size_t{2}, result.value().replies.size());
    CRPC_CHECK(result.value().replies[0].is_error());
    CRPC_CHECK_EQ(std::int64_t{1}, result.value().replies[1].as_integer());
}

CRPC_TEST(transaction_discard_without_multi_is_rejected) {
    tx_state state;
    scripted_executor executor(make_handler(state));
    crpc::redis::transaction tx(executor);

    crpc::status discarded = tx.discard();
    CRPC_CHECK_FALSE(discarded.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(discarded.code()));
}

CRPC_TEST_MAIN()
