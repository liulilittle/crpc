// CRPC Redis -- pipeline and the command-executor interfaces.
#include <crpc/redis/pipeline.hpp>

#include <utility>

namespace crpc {
namespace redis {

command_executor::~command_executor() = default;
message_channel::~message_channel() = default;

pipeline::pipeline(command_executor& executor) noexcept : executor_(&executor) {}

pipeline& pipeline::add(command cmd) {
    commands_.push_back(std::move(cmd));
    return *this;
}

pipeline& pipeline::add(std::initializer_list<std::string> args) {
    command cmd;
    cmd.reserve(args.size());
    for (const auto& arg : args) {
        cmd.push_back(arg);
    }
    return add(std::move(cmd));
}

result<std::vector<resp_value>> pipeline::run() {
    if (commands_.empty()) {
        return std::vector<resp_value>{};
    }
    result<std::vector<resp_value>> replies = executor_->execute_batch(commands_);
    if (!replies) {
        commands_.clear();
        return replies.error();
    }
    if (replies.value().size() != commands_.size()) {
        commands_.clear();
        return rpc_error{rpc_code::protocol_error,
                         "pipeline received a different number of replies than commands"};
    }
    commands_.clear();
    return replies;
}

} // namespace redis
} // namespace crpc
