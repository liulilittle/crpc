// CRPC Redis -- command execution, pipelines and the transport-facing channel.
//
// The protocol layer (`resp.hpp`) knows how to turn a value tree into bytes and
// back. This header is the next layer: it sends one command or a batch of
// commands over a `connection` and returns the decoded replies.
//
// Two interfaces keep the layers testable and keep the concrete transport out of
// the feature modules:
//
//   * `command_executor` -- request/response. A pipeline, a transaction, a
//     stream helper and a publisher all talk to this, never to a socket.
//   * `message_channel` -- a channel that can also deliver unsolicited pushes.
//     Pub/Sub needs this because a subscribed connection stops being a strict
//     request/response channel.
//
// `command_channel` is the only implementation that touches `connection`; it
// owns the RESP encoder and the incremental parser for one byte stream.
//
// Async model
// -----------
// `connection::write`, `read_exact` and `read_until` park the calling coroutine
// when it runs on a CRPC shard, and wait on a condition variable on a plain
// thread. Every method here therefore has the same property: inside a coroutine
// it suspends cooperatively, outside one it blocks. There are no hidden worker
// threads and no callbacks to marshal.
//
// Concurrency
// -----------
// A `command_channel` has one request/reply stream and is not safe for
// concurrent calls. A pipeline or transaction is likewise single-owner. Two
// coroutines that need independent command streams should use two channels
// (normally two pooled connections).
#ifndef CRPC_REDIS_PIPELINE_HPP
#define CRPC_REDIS_PIPELINE_HPP

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/resp.hpp>

namespace crpc {
namespace redis {

// Defined in connection.hpp. `command_channel` only stores a pointer, so the
// feature headers do not need the transport header (and its io/runtime pulls).
class connection;

// One Redis command: the command name followed by its arguments, each a bulk
// string on the wire. Redis commands are binary safe, so `std::string` is the
// right element type (it is not treated as text).
using command = resp_command;

// Request/response side of a Redis command stream.
class CRPC_API command_executor {
public:
    command_executor() = default;
    virtual ~command_executor();

    command_executor(const command_executor&) = delete;
    command_executor& operator=(const command_executor&) = delete;

    // Sends one command and returns its reply. A Redis error reply is returned
    // as a `resp_value` of type `resp_type::error`; only a transport, framing or
    // local failure makes the `result` itself fail.
    [[nodiscard]] virtual result<resp_value> execute(const command& cmd) = 0;

    // Sends every command before reading any reply (one write when the
    // transport accepts it) and returns replies in command order. The returned
    // vector always has exactly `commands.size()` elements on success.
    [[nodiscard]] virtual result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) = 0;
};

// A command stream that can additionally receive unsolicited pushes. A Pub/Sub
// subscription uses `send` for SUBSCRIBE/UNSUBSCRIBE and `receive` for the
// messages the server pushes afterwards.
class CRPC_API message_channel {
public:
    message_channel() = default;
    virtual ~message_channel();

    message_channel(const message_channel&) = delete;
    message_channel& operator=(const message_channel&) = delete;

    // Writes one command without reading a reply.
    [[nodiscard]] virtual status send(const command& cmd) = 0;

    // Waits for and decodes the next value on the stream. A server push is
    // returned like any other value; the caller decides what it means.
    [[nodiscard]] virtual result<resp_value> receive() = 0;
};

// Concrete command stream over a CRPC Redis `connection`.
//
// The channel owns the parser because a RESP value may span several reads and
// several values may arrive in one read. It never reads ahead of the reply the
// caller asked for except into that parser buffer.
class CRPC_API command_channel final : public command_executor, public message_channel {
public:
    explicit command_channel(connection& conn) noexcept;
    ~command_channel() override;

    command_channel(const command_channel&) = delete;
    command_channel& operator=(const command_channel&) = delete;

    [[nodiscard]] result<resp_value> execute(const command& cmd) override;
    [[nodiscard]] result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override;

    [[nodiscard]] status send(const command& cmd) override;
    [[nodiscard]] result<resp_value> receive() override;

    // Drops any buffered bytes and parser error state. Call only when the
    // stream is known to be at a message boundary (for example after a
    // reconnect), never in the middle of a reply.
    void reset() noexcept;

    [[nodiscard]] connection& transport() noexcept { return *conn_; }

    // RESP dialect used for replies. Commands are always arrays of bulk strings,
    // which are identical in RESP2 and RESP3.
    void set_protocol(resp_protocol protocol) noexcept { encoder_.protocol(protocol); }
    [[nodiscard]] resp_protocol protocol() const noexcept { return encoder_.protocol(); }

private:
    [[nodiscard]] result<resp_value> read_one();
    [[nodiscard]] status write_encoded(const std::string& wire);

    connection* conn_;
    resp_parser parser_;
    resp_encoder encoder_;
};

// A batch of commands executed in one write, with replies returned in order.
//
// A pipeline is not a transaction: the server may interleave other clients'
// commands between the pipeline's commands, and a failing command does not stop
// the ones after it. Use `transaction` for atomicity.
class CRPC_API pipeline {
public:
    explicit pipeline(command_executor& executor) noexcept;

    pipeline(const pipeline&) = delete;
    pipeline& operator=(const pipeline&) = delete;

    // Appends one command. The command is copied; callers that build large
    // payloads can move a `command` in directly.
    pipeline& add(command cmd);
    pipeline& add(std::initializer_list<std::string> args);

    // Convenience: `add("SET", "key", "value")`.
    template <typename... Args>
    pipeline& add(std::string first, Args&&... rest) {
        command cmd;
        cmd.reserve(1 + sizeof...(Args));
        cmd.push_back(std::move(first));
        (cmd.push_back(std::string(std::forward<Args>(rest))), ...);
        return add(std::move(cmd));
    }

    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }

    // Removes every queued command. Does not touch the connection.
    void clear() noexcept { commands_.clear(); }

    [[nodiscard]] const std::vector<command>& commands() const noexcept {
        return commands_;
    }

    // Sends the batch and returns one reply per queued command. The queued
    // commands are consumed: a second `run()` sends nothing until more commands
    // are added. An empty pipeline returns an empty vector without touching the
    // transport.
    [[nodiscard]] result<std::vector<resp_value>> run();

private:
    command_executor* executor_;
    std::vector<command> commands_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_PIPELINE_HPP
