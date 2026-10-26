// CRPC Redis -- MULTI/EXEC transactions.
//
// A Redis transaction is a server-side queue. The client sends MULTI, then the
// commands; the server answers QUEUED for each and executes the whole queue
// atomically on EXEC. WATCH adds optimistic locking: if any watched key changes
// before EXEC, the server aborts the transaction and EXEC returns a null reply.
//
// This wrapper keeps the client-side state (`active`, `queued`, `dirty`) in
// lockstep with the server. It does not hide the two failure modes Redis itself
// exposes:
//
//   * a command rejected while queuing makes the transaction dirty; EXEC would
//     fail with EXECABORT, so `exec()` sends DISCARD and reports the original
//     queue error;
//   * a WATCH conflict makes EXEC return null; `exec()` reports that as
//     `transaction_exec_result::aborted` with no replies, which is a successful
//     protocol outcome, not an error.
//
// A transaction is single-owner and not thread safe, like the executor it uses.
#ifndef CRPC_REDIS_TRANSACTION_HPP
#define CRPC_REDIS_TRANSACTION_HPP

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>

namespace crpc {
namespace redis {

// The outcome of EXEC.
struct transaction_exec_result {
    // True when WATCH detected a conflict and the server discarded the queue.
    bool aborted = false;

    // Replies in queue order. Empty when `aborted` is true. Individual entries
    // may be RESP error values: Redis executes every queued command and reports
    // a runtime error per command, without aborting the others.
    std::vector<resp_value> replies;
};

class CRPC_API transaction {
public:
    explicit transaction(command_executor& executor) noexcept;

    // Does not perform I/O: an active transaction must be closed with
    // `discard()` (or `exec()`) explicitly. A destructor cannot park a
    // coroutine or block a plain thread safely, so it leaves the server's
    // MULTI state to the next command on that connection.
    ~transaction();

    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;

    // WATCH one or more keys. Must be called outside MULTI. An empty key list is
    // rejected locally because Redis rejects it too.
    [[nodiscard]] status watch(const std::vector<std::string>& keys);
    [[nodiscard]] status watch(std::initializer_list<std::string> keys);

    // UNWATCH flushes all watched keys. Must be called outside MULTI.
    [[nodiscard]] status unwatch();

    // Starts a transaction. Fails with `failed_precondition` when one is
    // already active.
    [[nodiscard]] status multi();

    // Discards an active transaction. Fails with `failed_precondition` when no
    // transaction is active.
    [[nodiscard]] status discard();

    // Queues one command inside MULTI. The server validates the command and
    // answers QUEUED; a validation error marks the transaction dirty and is
    // returned. Fails with `failed_precondition` outside MULTI.
    [[nodiscard]] status queue(command cmd);

    // Executes the queue. The transaction becomes inactive whatever the result.
    [[nodiscard]] result<transaction_exec_result> exec();

    [[nodiscard]] bool active() const noexcept { return active_; }

    // True when a queued command was rejected; `exec()` will not commit.
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    // Number of commands the server accepted as QUEUED.
    [[nodiscard]] std::size_t queued() const noexcept { return queued_; }

private:
    [[nodiscard]] status expect_ok(command cmd, const char* operation);

    // Marks the transaction dirty and remembers the original queue error so
    // `exec()` can report it instead of a generic message.
    status mark_dirty(rpc_error error);

    command_executor* executor_;
    bool active_ = false;
    bool dirty_ = false;
    std::size_t queued_ = 0;
    rpc_error queue_error_{};
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_TRANSACTION_HPP
