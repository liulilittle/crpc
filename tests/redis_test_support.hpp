// CRPC Redis feature tests -- shared scripted executor.
//
// The feature layer (pipeline, transaction, streams, Pub/Sub) talks to the
// `command_executor` / `message_channel` interfaces, not to a socket. These
// helpers script replies and record the commands that were sent, so each test
// can pin both the command it builds and the reply it decodes without a server.
#ifndef CRPC_TESTS_REDIS_TEST_SUPPORT_HPP
#define CRPC_TESTS_REDIS_TEST_SUPPORT_HPP

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <crpc/redis/pipeline.hpp>

namespace redis_test {

using crpc::redis::command;
using crpc::redis::resp_value;

inline resp_value text(std::string value) {
    return resp_value(crpc::redis::resp_bulk_string{std::move(value)});
}

inline resp_value simple(std::string value) {
    return resp_value(crpc::redis::resp_simple_string{std::move(value)});
}

inline resp_value error(std::string message) {
    return resp_value(crpc::redis::resp_error{std::move(message)});
}

inline resp_value integer(std::int64_t value) {
    return resp_value(crpc::redis::resp_integer{value});
}

inline resp_value array(std::vector<resp_value> items) {
    return resp_value(crpc::redis::resp_array_value{std::move(items)});
}

inline resp_value push(std::vector<resp_value> items) {
    return resp_value(crpc::redis::resp_push_value{std::move(items)});
}

inline resp_value map(std::vector<std::pair<resp_value, resp_value>> entries) {
    return resp_value(crpc::redis::resp_map_value{std::move(entries)});
}

inline resp_value null_value() {
    return resp_value(crpc::redis::resp_null{});
}

class scripted_executor final : public crpc::redis::command_executor,
                                public crpc::redis::message_channel {
public:
    using handler_fn = std::function<crpc::result<resp_value>(const command&)>;

    explicit scripted_executor(handler_fn handler = {})
        : handler_(std::move(handler)) {}

    void set_handler(handler_fn handler) { handler_ = std::move(handler); }

    crpc::result<resp_value> execute(const command& cmd) override {
        executed.push_back(cmd);
        if (!handler_) {
            return crpc::rpc_error{crpc::rpc_code::internal,
                                   "scripted executor has no handler"};
        }
        return handler_(cmd);
    }

    crpc::result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override {
        ++batch_calls;
        batches.push_back(commands);
        std::vector<resp_value> replies;
        replies.reserve(commands.size());
        for (const command& cmd : commands) {
            executed.push_back(cmd);
            if (!handler_) {
                return crpc::rpc_error{crpc::rpc_code::internal,
                                       "scripted executor has no handler"};
            }
            crpc::result<resp_value> reply = handler_(cmd);
            if (!reply) {
                return reply.error();
            }
            replies.push_back(std::move(reply).value());
        }
        return replies;
    }

    crpc::status send(const command& cmd) override {
        sent.push_back(cmd);
        return crpc::status{};
    }

    crpc::result<resp_value> receive() override {
        if (incoming.empty()) {
            return crpc::rpc_error{crpc::rpc_code::eof,
                                   "no scripted push remains"};
        }
        resp_value value = std::move(incoming.front());
        incoming.pop_front();
        return value;
    }

    handler_fn handler_;
    std::vector<command> executed;
    std::vector<command> sent;
    std::vector<std::vector<command>> batches;
    std::deque<resp_value> incoming;
    std::size_t batch_calls = 0;
};

} // namespace redis_test

#endif // CRPC_TESTS_REDIS_TEST_SUPPORT_HPP
