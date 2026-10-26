// CRPC Redis -- MULTI/EXEC transaction state machine.
#include <crpc/redis/transaction.hpp>

#include <utility>

#include "reply_util.hpp"

namespace crpc {
namespace redis {
namespace {

status application_error(const resp_value& value) {
    return status{detail::application_error(value)};
}

status protocol_error(const char* operation) {
    return status{detail::protocol_error(std::string(operation) +
                                         " did not return OK")};
}

} // namespace

transaction::transaction(command_executor& executor) noexcept : executor_(&executor) {}

transaction::~transaction() = default;

status transaction::mark_dirty(rpc_error error) {
    dirty_ = true;
    queue_error_ = std::move(error);
    return status{queue_error_};
}

status transaction::expect_ok(command cmd, const char* operation) {
    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return application_error(value);
    }
    if (!detail::is_text(value) || detail::as_text(value) != "OK") {
        return protocol_error(operation);
    }
    return status{};
}

status transaction::watch(const std::vector<std::string>& keys) {
    if (active_) {
        return status{rpc_error{rpc_code::failed_precondition,
                                "WATCH cannot be issued inside MULTI"}};
    }
    if (keys.empty()) {
        return status{rpc_error{rpc_code::invalid_argument,
                                "WATCH requires at least one key"}};
    }
    command cmd;
    cmd.reserve(1 + keys.size());
    cmd.emplace_back("WATCH");
    for (const std::string& key : keys) {
        cmd.push_back(key);
    }
    return expect_ok(std::move(cmd), "WATCH");
}

status transaction::watch(std::initializer_list<std::string> keys) {
    return watch(std::vector<std::string>(keys));
}

status transaction::unwatch() {
    if (active_) {
        return status{rpc_error{rpc_code::failed_precondition,
                                "UNWATCH cannot be issued inside MULTI"}};
    }
    return expect_ok(command{"UNWATCH"}, "UNWATCH");
}

status transaction::multi() {
    if (active_) {
        return status{rpc_error{rpc_code::failed_precondition,
                                "a transaction is already active"}};
    }
    const status started = expect_ok(command{"MULTI"}, "MULTI");
    if (!started) {
        return started;
    }
    active_ = true;
    dirty_ = false;
    queued_ = 0;
    return status{};
}

status transaction::discard() {
    if (!active_) {
        return status{rpc_error{rpc_code::failed_precondition,
                                "no active transaction to discard"}};
    }
    const status discarded = expect_ok(command{"DISCARD"}, "DISCARD");
    if (!discarded) {
        return discarded;
    }
    active_ = false;
    dirty_ = false;
    queued_ = 0;
    return status{};
}

status transaction::queue(command cmd) {
    if (!active_) {
        return status{rpc_error{rpc_code::failed_precondition,
                                "commands can only be queued inside MULTI"}};
    }
    if (cmd.empty()) {
        return mark_dirty(rpc_error{rpc_code::invalid_argument,
                                    "a queued command needs a command name"});
    }

    result<resp_value> reply = executor_->execute(cmd);
    if (!reply) {
        // The write may or may not have reached the server, so the transaction
        // cannot be committed safely.
        return mark_dirty(reply.error());
    }

    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return mark_dirty(detail::application_error(value));
    }
    if (!detail::is_text(value) || detail::as_text(value) != "QUEUED") {
        return mark_dirty(detail::protocol_error(
            "expected QUEUED from a command inside MULTI"));
    }
    ++queued_;
    return status{};
}

result<transaction_exec_result> transaction::exec() {
    if (!active_) {
        return rpc_error{rpc_code::failed_precondition,
                         "EXEC requires an active transaction"};
    }

    if (dirty_) {
        // Redis would answer EXECABORT. Discard explicitly so the server queue
        // and the client state cannot drift apart, then report the original
        // queue error (text and code), not a generic message.
        (void)executor_->execute(command{"DISCARD"});
        active_ = false;
        dirty_ = false;
        queued_ = 0;
        if (queue_error_.code == rpc_code::ok) {
            return rpc_error{rpc_code::application,
                             "transaction aborted by a queue error"};
        }
        rpc_error error = queue_error_;
        queue_error_ = rpc_error{};
        return error;
    }

    result<resp_value> reply = executor_->execute(command{"EXEC"});
    active_ = false;
    queued_ = 0;

    if (!reply) {
        return reply.error();
    }

    const resp_value& value = reply.value();
    if (value.is_null()) {
        transaction_exec_result result;
        result.aborted = true;
        return result;
    }
    if (detail::is_error_reply(value)) {
        return detail::application_error(value);
    }
    if (!value.is_array()) {
        return rpc_error{rpc_code::protocol_error,
                         "EXEC returned neither an array, null, nor an error"};
    }

    transaction_exec_result result;
    result.replies = value.as_array();
    return result;
}

} // namespace redis
} // namespace crpc
