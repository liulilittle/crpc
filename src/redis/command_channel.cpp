// CRPC Redis -- the concrete RESP command stream over a CRPC connection.
#include <crpc/redis/pipeline.hpp>

#include <crpc/redis/connection.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "reply_util.hpp"

namespace crpc {
namespace redis {
namespace {

span<const std::byte> as_bytes(const std::string& text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

} // namespace

command_channel::command_channel(connection& conn) noexcept : conn_(&conn) {}

command_channel::~command_channel() = default;

status command_channel::write_encoded(const std::string& wire) {
    return conn_->write(as_bytes(wire));
}

result<resp_value> command_channel::read_one() {
    for (;;) {
        resp_value value;
        const resp_parser::result decoded = parser_.next(value);
        if (decoded == resp_parser::result::value_ready) {
            return value;
        }
        if (decoded == resp_parser::result::error) {
            return rpc_error{parser_.error(), "RESP decoding failed"};
        }

        // The parser needs more bytes. Read one byte (which parks until at
        // least one arrives), then drain everything already buffered. This
        // avoids `read_until('\n')`'s "a line must fit in the read buffer"
        // limitation: a large bulk payload is consumed in buffer-sized chunks
        // and the parser reassembles it. Any bytes beyond the current value
        // stay in the parser buffer for the next reply.
        result<std::vector<std::byte>> first = conn_->read_exact(1);
        if (!first) {
            return first.error();
        }
        parser_.feed(span<const std::byte>(first.value().data(), first.value().size()));

        std::vector<std::byte> rest = conn_->read_available();
        if (!rest.empty()) {
            parser_.feed(span<const std::byte>(rest.data(), rest.size()));
        }
    }
}

result<resp_value> command_channel::execute(const command& cmd) {
    const status written = send(cmd);
    if (!written) {
        return written.error();
    }
    return read_one();
}

result<std::vector<resp_value>> command_channel::execute_batch(
    const std::vector<command>& commands) {
    if (commands.empty()) {
        return std::vector<resp_value>{};
    }

    std::string wire;
    for (const command& cmd : commands) {
        if (cmd.empty()) {
            return rpc_error{rpc_code::invalid_argument,
                             "an empty Redis command cannot be sent"};
        }
        wire.append(encoder_.encode_command_to_string(cmd));
    }

    const status written = write_encoded(wire);
    if (!written) {
        return written.error();
    }

    std::vector<resp_value> replies;
    replies.reserve(commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        result<resp_value> reply = read_one();
        if (!reply) {
            return reply.error();
        }
        replies.push_back(std::move(reply).value());
    }
    return replies;
}

status command_channel::send(const command& cmd) {
    if (cmd.empty()) {
        return status{rpc_error{rpc_code::invalid_argument,
                                "an empty Redis command cannot be sent"}};
    }
    return write_encoded(encoder_.encode_command_to_string(cmd));
}

result<resp_value> command_channel::receive() {
    return read_one();
}

void command_channel::reset() noexcept {
    parser_.reset();
}

} // namespace redis
} // namespace crpc
