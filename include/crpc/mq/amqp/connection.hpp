// CRPC -- synchronous AMQP 0-9-1 client.
//
// The connection owns one byte channel (the same `crpc::db::channel` every
// database adapter uses), performs the protocol header + Connection.Start/Tune/
// Open handshake, and then gives out logical channels. All protocol work is
// synchronous: a method either gets its reply or returns a `result`/`status`
// error; nothing here throws.
//
// Threading
// ---------
// The public API is not safe to call concurrently. One thread drives the
// connection (methods and reads). The only additional thread is an internal
// heartbeat writer, created after Tune when the negotiated interval is non-zero:
// AMQP requires a peer that is not sending frames to send heartbeats, and a
// purely demand-driven API cannot do that while the caller is idle. That thread
// writes only complete heartbeat frames under the same write mutex the caller
// uses, and it is stopped and joined before the link is closed.
//
// Channel lifetime
// ----------------
// A `channel` borrows its `connection`. It must be destroyed before the
// connection; the destructor performs a best-effort Channel.Close, so keeping a
// channel past the connection would be use-after-free rather than a leak.
//
// Heartbeat default
// -----------------
// `connection_options::host` defaults to `localhost`. On the development host
// this client was verified on, WSL2's relay binds only `[::1]:5672`, so the
// IPv4 loopback does not reach RabbitMQ. That is a property of that host, not of
// AMQP: a deployment passes its own host through the options or the environment.
#ifndef CRPC_MQ_AMQP_CONNECTION_HPP
#define CRPC_MQ_AMQP_CONNECTION_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/mq/amqp/protocol.hpp>
#include <crpc/mq/amqp/types.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace mq {
namespace amqp {

struct connection_options {
    // See the header comment: `localhost` is what this machine's WSL2 relay
    // forwards. A real deployment sets the broker address explicitly.
    std::string host = "localhost";
    std::uint16_t port = 5672;
    std::string user = "guest";
    std::string password = "guest";
    std::string virtual_host = "/";

    std::string client_product = "crpc";
    std::string client_version = "2.0.0";

    // Zero means "accept the server's proposal". A non-zero frame_max and
    // channel_max are additionally capped by the server's value, because a
    // client cannot exceed what the server offered.
    std::uint16_t heartbeat = 0;
    std::uint16_t channel_max = 0;
    std::uint32_t frame_max = 0;

    // Bounds on one inbound frame and one reassembled content body. A peer that
    // announces more is refused with `resource_exhausted` instead of growing
    // memory without limit.
    std::size_t max_inbound_frame = max_frame_payload;
    std::size_t max_body_size = 64u * 1024u * 1024u;

    // Used by `connection::open` for the underlying transport.
    std::chrono::nanoseconds operation_timeout{std::chrono::seconds(10)};
};

class connection;

// One logical AMQP channel. Methods never throw; a channel the broker closed is
// reported on the call that observed it and `is_open()` turns false.
class CRPC_API channel {
public:
    ~channel();

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    channel(channel&&) = delete;
    channel& operator=(channel&&) = delete;

    [[nodiscard]] std::uint16_t id() const noexcept { return id_; }
    [[nodiscard]] bool is_open() const noexcept;

    // Queue.Declare/DeclareOk. An empty name asks the broker to generate one.
    // `passive=true` only checks that the queue exists, and returns its counts.
    [[nodiscard]] result<queue_declare_result> queue_declare(
        std::string_view queue, bool passive = false, bool durable = false,
        bool exclusive = false, bool auto_delete = false,
        const field_table& arguments = field_table{});

    // Queue.Delete/DeleteOk.
    [[nodiscard]] status queue_delete(std::string_view queue, bool if_unused = false,
                                      bool if_empty = false);

    // Queue.Bind/BindOk. Needed by every exchange routing test: declaring an
    // exchange is not enough to route to a queue.
    [[nodiscard]] status queue_bind(std::string_view queue, std::string_view exchange,
                                    std::string_view routing_key,
                                    const field_table& arguments = field_table{});

    // Exchange.Declare/DeclareOk. `type` is "direct", "fanout", "topic" or
    // "headers"; this client does not interpret it.
    [[nodiscard]] status exchange_declare(std::string_view exchange,
                                          std::string_view type, bool passive = false,
                                          bool durable = false, bool auto_delete = false,
                                          bool internal = false,
                                          const field_table& arguments = field_table{});

    // Basic.Publish. The method frame and the content header/body frames are
    // written in one buffer so a partial publication cannot be split by another
    // writer (the heartbeat thread takes the same mutex).
    //
    // `mandatory=true` asks the broker to return an unroutable message as
    // Basic.Return instead of dropping it; read it with `next_return`.
    [[nodiscard]] status basic_publish(std::string_view exchange,
                                       std::string_view routing_key,
                                       const message_properties& properties,
                                       span<const std::byte> body,
                                       bool mandatory = false, bool immediate = false);

    // Basic.Get/GetOk/GetEmpty. An empty queue is `empty=true`, not an error.
    [[nodiscard]] result<get_result> basic_get(std::string_view queue, bool no_ack = false);

    // Basic.Consume/ConsumeOk. An empty consumer tag asks the broker to generate
    // one, which is returned.
    [[nodiscard]] result<std::string> basic_consume(
        std::string_view queue, std::string_view consumer_tag = "", bool no_ack = false,
        const field_table& arguments = field_table{});

    // Basic.Cancel/CancelOk. With `no_wait` the broker sends nothing, so the
    // call only writes and does not wait.
    [[nodiscard]] status basic_cancel(std::string_view consumer_tag, bool no_wait = false);

    // Basic.Ack, Basic.Nack, Basic.Reject. None of them has a reply.
    [[nodiscard]] status basic_ack(std::uint64_t delivery_tag, bool multiple = false);
    [[nodiscard]] status basic_nack(std::uint64_t delivery_tag, bool multiple = false,
                                    bool requeue = true);
    [[nodiscard]] status basic_reject(std::uint64_t delivery_tag, bool requeue = true);

    // Basic.Qos/QosOk.
    [[nodiscard]] status basic_qos(std::uint16_t prefetch_count,
                                   std::uint32_t prefetch_size = 0,
                                   bool global = false);

    // Waits up to `timeout` for a Basic.Deliver on this channel. A timeout is a
    // normal value (`present=false`), not an error. Frames that belong to another
    // channel are queued for that channel.
    [[nodiscard]] result<delivery> next_delivery(std::chrono::milliseconds timeout);

    // Waits up to `timeout` for a Basic.Return caused by a mandatory
    // publication.
    [[nodiscard]] result<returned_message> next_return(std::chrono::milliseconds timeout);

    // Channel.Close/CloseOk. Idempotent. A channel the broker already closed is
    // simply marked closed.
    [[nodiscard]] status close();

private:
    friend class connection;
    channel(connection* owner, std::uint16_t id) noexcept;

    connection* owner_ = nullptr;
    std::uint16_t id_ = 0;
    bool open_ = true;
};

// One AMQP 0-9-1 connection.
class CRPC_API connection {
public:
    // Runs the handshake on an already-open channel. The constructor never
    // throws; a failed handshake is reported by `is_open()` and
    // `connect_error()`.
    connection(connection_options options, std::unique_ptr<db::channel> link);
    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&&) = delete;
    connection& operator=(connection&&) = delete;

    // Creates the TCP channel and handshakes. Returns the handshake error as the
    // result, so a caller can distinguish "broker refused" from "not connected".
    [[nodiscard]] static result<std::unique_ptr<connection>> open(
        connection_options options, crpc::backend_factory& backend,
        crpc::runtime* shard = nullptr);

    // Channel.Open/OpenOk. The returned channel borrows this connection.
    [[nodiscard]] result<std::unique_ptr<channel>> open_channel();

    // Connection.Close/CloseOk. Stops the heartbeat writer first, then closes
    // the link; idempotent.
    [[nodiscard]] status close();

    [[nodiscard]] bool is_open() const noexcept {
        return open_.load(std::memory_order_acquire) && link_ != nullptr;
    }
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }

    // Negotiated in Tune/TuneOk.
    [[nodiscard]] std::uint16_t heartbeat_interval() const noexcept { return heartbeat_; }
    [[nodiscard]] std::uint16_t channel_max() const noexcept { return channel_max_; }
    [[nodiscard]] std::uint32_t frame_max() const noexcept { return frame_max_; }

    // "product version" from the server's Connection.Start properties.
    [[nodiscard]] std::string_view server_version() const noexcept { return server_version_; }
    [[nodiscard]] const field_table& server_properties() const noexcept {
        return server_properties_;
    }

private:
    friend class channel;

    struct queued_method {
        std::uint16_t channel = 0;
        std::uint16_t class_id = 0;
        std::uint16_t method_id = 0;
        std::vector<std::byte> arguments;
    };

    [[nodiscard]] status perform_handshake();
    [[nodiscard]] status send_bytes(span<const std::byte> data);
    [[nodiscard]] status send_method(std::uint16_t channel, std::uint16_t method_class,
                                     std::uint16_t method,
                                     span<const std::byte> arguments);
    [[nodiscard]] result<frame> read_frame();
    // Waits for one of `methods` on `channel`. `matched` receives the method id
    // that answered. Asynchronous frames (Deliver, Return, server Close) are
    // dispatched while waiting.
    [[nodiscard]] result<std::vector<std::byte>> await_method(
        std::uint16_t channel, std::uint16_t method_class,
        std::initializer_list<std::uint16_t> methods, std::uint16_t* matched);
    [[nodiscard]] result<bool> handle_incoming(std::uint16_t frame_channel,
                                               std::uint16_t method_class,
                                               std::uint16_t method,
                                               span<const std::byte> arguments);
    [[nodiscard]] status read_content(std::uint16_t expected_channel,
                                      message_properties& properties,
                                      std::vector<std::byte>& body);
    [[nodiscard]] status close_channel(std::uint16_t id);
    [[nodiscard]] bool channel_closed(std::uint16_t id) const noexcept;
    [[nodiscard]] result<delivery> wait_delivery(std::uint16_t id,
                                                 std::chrono::milliseconds timeout);
    [[nodiscard]] result<returned_message> wait_return(std::chrono::milliseconds timeout);

    void start_heartbeat();
    void stop_heartbeat() noexcept;
    void heartbeat_loop(std::chrono::milliseconds period);
    void fail() noexcept;

    connection_options options_;
    std::unique_ptr<db::channel> link_;
    std::atomic<bool> open_{false};
    rpc_error connect_error_;

    field_table server_properties_;
    std::string server_version_;
    std::uint16_t heartbeat_ = 0;
    std::uint16_t channel_max_ = 0;
    std::uint32_t frame_max_ = default_frame_max;
    std::uint16_t next_channel_ = 1;

    std::deque<queued_method> pending_methods_;
    std::map<std::uint16_t, std::deque<delivery>> deliveries_;
    std::deque<returned_message> returns_;
    std::set<std::uint16_t> closed_channels_;
    std::map<std::uint16_t, rpc_error> channel_errors_;

    // Serialises whole frames. The heartbeat writer only takes this lock.
    std::mutex write_mutex_;
    std::chrono::steady_clock::time_point last_write_{};

    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    bool heartbeat_stop_ = false;
};

} // namespace amqp
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_AMQP_CONNECTION_HPP