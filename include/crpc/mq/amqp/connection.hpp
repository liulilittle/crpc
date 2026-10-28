// CRPC -- synchronous AMQP 0-9-1 client.
//
// The connection owns one byte channel (the same `crpc::db::channel` every
// database adapter uses), performs the protocol header + Connection.Start/Tune/
// Open handshake, and then gives out logical channels. All protocol work is
// synchronous: a method either gets its reply or returns a `result`/`status`
// error; nothing here throws.
//
// High availability
// -----------------
// `connection_options::nodes` may list several brokers. `connection::open`
// visits them in a stable rotation that starts at the node that succeeded last
// time, and only fails when every node refuses. A transport failure is not
// terminal: the next public call on any channel automatically reconnects with
// exponential backoff (capped, with jitter), and a successful reconnect
// re-opens every channel and re-declares the topology that was active before
// the drop -- exchanges, queues, bindings, Qos, the reconnect-aware consumers
// and publisher-confirm mode. This is what keeps a consumer running after a
// broker restart; without the re-declaration the queue would exist only on the
// old broker and consumption would silently stop.
//
// Publisher confirms (`confirm.select`) add a synchronous "the broker
// processed the message" barrier: `channel::basic_publish_confirm` returns
// once the broker acknowledges the publication, and reports a Basic.Nack the
// same way it reports an error. `channel::next_cancel` surfaces the broker's
// consumer_cancel_notify notification instead of letting a cancelled consumer
// die silently.
//
// Threading
// ---------
// The public API is not safe to call concurrently. One thread drives the
// connection (methods and reads). The only additional thread is an internal
// heartbeat writer, created after Tune when the negotiated interval is non-zero:
// AMQP requires a peer that is not sending frames to send heartbeats, and a
// purely demand-driven API cannot do that while the caller is idle. That thread
// writes only complete heartbeat frames under the same write mutex the caller
// uses, and it is stopped and joined before the link is closed. It stays alive
// across reconnects and stays silent while the connection is not open.
//
// Channel lifetime
// ----------------
// A `channel` borrows its `connection`. It must be destroyed before the
// connection; the destructor performs a best-effort Channel.Close, so keeping a
// channel past the connection would be use-after-free rather than a leak.
// A channel survives a connection drop and a reconnect; only a broker-initiated
// Channel.Close marks it permanently dead.
//
// Quorum queues
// -------------
// `channel::queue_declare(..., arguments)` passes its arguments through to
// Queue.Declare, so a quorum queue is declared with `x-queue-type` = "quorum"
// in the arguments table. Quorum queues are replicated with Raft and survive a
// single node loss, unlike a classic queue whose whole state lives on one node.
// Practical differences this client must observe: a quorum queue must be
// durable and must not be exclusive or auto-delete (RabbitMQ answers 406
// PRECONDITION_FAILED otherwise), and -- because consumers re-register on
// reconnect -- a quorum queue needs a re-registered consumer, exactly like a
// classic one.
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

// One broker in a connection's node list. The client tries them in a rotation
// that starts at the last successful node. `port == 0` falls back to 5672.
struct node_address {
    std::string host;
    std::uint16_t port = 5672;

    [[nodiscard]] bool operator==(const node_address& other) const noexcept {
        return host == other.host && port == other.port;
    }
    [[nodiscard]] bool operator!=(const node_address& other) const noexcept {
        return !(*this == other);
    }
};

// Order in which a connection visits `count` nodes: `last_success` first, then
// the remaining indices in ascending order. The first node is the one that
// succeeded last, so a healthy broker stays the primary and a recovering one is
// probed before the clients pile on another node.
[[nodiscard]] CRPC_API std::vector<std::size_t> node_visit_order(
    std::size_t count, std::size_t last_success) noexcept;

// Exponential reconnect delay for retry `attempt` (0 = first retry):
// `min(base * 2^attempt, cap)` plus a jitter of up to `base - 1` derived from
// `jitter_basis`. A caller that wants deterministic tests hands a fixed basis.
[[nodiscard]] CRPC_API std::chrono::milliseconds node_backoff_delay(
    std::size_t attempt, std::chrono::milliseconds base,
    std::chrono::milliseconds cap, std::uint64_t jitter_basis) noexcept;

struct connection_options {
    // See the header comment: `localhost` is what this machine's WSL2 relay
    // forwards. A real deployment sets the broker address explicitly.
    std::string host = "localhost";
    std::uint16_t port = 5672;
    std::string user = "guest";
    std::string password = "guest";
    std::string virtual_host = "/";

    // Broker list for failover. When non-empty, it replaces `host`/`port` for
    // both the initial connect and every reconnect. The order inside the list
    // is not a priority order: the node that connected last wins the first
    // attempt of the next cycle.
    std::vector<node_address> nodes;

    // Reconnect policy. After a transport failure the next public call waits up
    // to `reconnect_attempts` full passes over the node list with an
    // exponential, capped, jittered delay between passes. A reconnect
    // re-declares the channel topology, so these bounds also bound how long a
    // publisher or consumer can be parked waiting for the broker to come back.
    std::size_t reconnect_attempts = 8;
    std::chrono::milliseconds reconnect_delay_base{std::chrono::milliseconds(100)};
    std::chrono::milliseconds reconnect_delay_cap{std::chrono::milliseconds(2000)};

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

    // Puts this channel in publisher-confirm mode (Confirm.Select/SelectOk).
    // After this, `basic_publish_confirm` waits for the broker's per-message
    // acknowledgement; plain `basic_publish` keeps working but is no longer
    // fire-and-forget on the broker's side (unsynchronised publishes are
    // still tracked by sequence number). Confirm mode is re-entered on
    // reconnect.
    [[nodiscard]] status confirm_select();

    [[nodiscard]] bool publisher_confirms_enabled() const noexcept {
        return confirm_mode_;
    }

    // Publishes a message and blocks until the broker confirms it. Returns
    // `acknowledged=true` on Basic.Ack and `acknowledged=false` on
    // Basic.Nack, so an unconfirmed or nack-ed publication is visible to the
    // caller. Needs `confirm_select` first. The confirm sequence number in the
    // result is this channel's next confirm tag. `mandatory` publications
    // that come back as Basic.Return are nack-ed by RabbitMQ, which surfaces
    // here as `acknowledged=false`; the returned message itself is still
    // readable through `next_return`.
    [[nodiscard]] result<publish_confirm> basic_publish_confirm(
        std::string_view exchange, std::string_view routing_key,
        const message_properties& properties, span<const std::byte> body,
        bool mandatory = false, bool immediate = false,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // Waits up to `timeout` for the broker to cancel a consumer on this
    // channel (consumer_cancel_notify, e.g. because the queue was deleted).
    // A timeout is `present=false`, not an error. This is how a caller learns
    // that consumption stopped; it must re-declare and re-consume itself.
    [[nodiscard]] result<consumer_cancel> next_cancel(
        std::chrono::milliseconds timeout);

    // Channel.Close/CloseOk. Idempotent. A channel the broker already closed is
    // simply marked closed.
    [[nodiscard]] status close();

private:
    friend class connection;
    channel(connection* owner, std::uint16_t id) noexcept;

    connection* owner_ = nullptr;
    std::uint16_t id_ = 0;
    bool open_ = true;
    // Set when the broker closed this specific channel (Channel.Close). Such a
    // channel is not re-opened on reconnect.
    bool dead_ = false;
    bool confirm_mode_ = false;
    // Confirm sequence number the broker will attach to the next Basic.Publish
    // on this channel. Reset to 1 after a reconnect, which is how a fresh
    // broker channel numbers its confirms.
    std::uint64_t next_publish_tag_ = 1;

    // The topology this channel re-declares after a reconnect, in a form that
    // only keeps the *current* intent: a deleted queue no longer produces a
    // declare, a cancelled consumer no longer produces a consume.
    struct queue_state {
        std::string name;  // resolved name; server-generated names stored here
        bool passive = false;
        bool durable = false;
        bool exclusive = false;
        bool auto_delete = false;
        field_table arguments;
    };
    struct exchange_state {
        std::string name;
        std::string type;
        bool passive = false;
        bool durable = false;
        bool auto_delete = false;
        bool internal = false;
        field_table arguments;
    };
    struct bind_state {
        std::string queue;
        std::string exchange;
        std::string routing_key;
        field_table arguments;
    };
    struct consume_state {
        std::string queue;
        std::string consumer_tag;  // granted tag after ConsumeOk
        bool no_ack = false;
        field_table arguments;
    };
    struct qos_state {
        std::uint16_t prefetch_count = 0;
        std::uint32_t prefetch_size = 0;
        bool global = false;
    };

    std::map<std::string, queue_state> queues_;
    std::map<std::string, exchange_state> exchanges_;
    std::vector<bind_state> binds_;
    std::map<std::string, consume_state> consumes_;
    bool qos_applied_ = false;
    qos_state qos_;

    // Reconnects on behalf of a public method when the link is not open.
    [[nodiscard]] status ensure_ready();
    // Guard for operations that must never retry across a reconnect (conv: a
    // Basic.Ack for a delivery belongs to the generation that delivered it).
    [[nodiscard]] status require_live();
    // Re-declares exchange/queue/bind/consume/QoS/confirm on a fresh link.
    [[nodiscard]] status replay_topology();
    // Sends one method and waits for one of `replies`; the encoding used by the
    // public call and by the reconnect replay must not drift apart.
    [[nodiscard]] result<std::vector<std::byte>> exchange_method(
        std::uint16_t method_class, std::uint16_t method,
        span<const std::byte> arguments,
        std::initializer_list<std::uint16_t> replies,
        std::uint16_t* matched = nullptr);
    // Writes the Basic.Publish method and content frames without touching the
    // confirm sequence.
    [[nodiscard]] status send_publish(std::string_view exchange,
                                      std::string_view routing_key,
                                      const message_properties& properties,
                                      span<const std::byte> body, bool mandatory,
                                      bool immediate);
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
    // the link; idempotent. A closed connection does not reconnect; channels
    // borrowed from it are unusable.
    [[nodiscard]] status close();

    // Reconnects now: visits the node list in rotation (the last successful
    // node first) with exponential, capped, jittered backoff between passes,
    // then re-opens every live channel and re-declares its topology. This is
    // also what every public channel method calls automatically when it finds
    // the link down, so the call is normally only needed to force a reconnect
    // on the caller's own schedule. Returns the last transport error when the
    // reconnect budget is exhausted.
    [[nodiscard]] status reconnect();

    [[nodiscard]] bool is_open() const noexcept {
        return open_.load(std::memory_order_acquire) && link_ != nullptr;
    }
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }

    // Failover state.
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    // The node the current link is connected to. Valid while `is_open()`.
    [[nodiscard]] const node_address& active_node() const noexcept;
    // Index of the node that connected last, i.e. the one a reconnect probes
    // first. Never greater than `node_count() - 1`.
    [[nodiscard]] std::size_t last_success_index() const noexcept {
        return last_success_index_;
    }

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

    struct confirm_event {
        std::uint64_t delivery_tag = 0;
        bool acknowledged = false;
        bool multiple = false;
    };

    [[nodiscard]] status perform_handshake();
    [[nodiscard]] status send_bytes(span<const std::byte> data);
    [[nodiscard]] status send_method(std::uint16_t channel, std::uint16_t method_class,
                                     std::uint16_t method,
                                     span<const std::byte> arguments);
    [[nodiscard]] result<frame> read_frame();
    // Waits for one of `methods` on `channel`. `matched` receives the method id
    // that answered. Asynchronous frames (Deliver, Return, Cancel, confirm,
    // server Close) are dispatched while waiting.
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
    [[nodiscard]] result<confirm_event> wait_confirm(std::uint16_t id,
                                                     std::uint64_t target_tag,
                                                     std::chrono::milliseconds timeout);
    [[nodiscard]] result<consumer_cancel> wait_cancel(std::uint16_t id,
                                                      std::chrono::milliseconds timeout);

    void start_heartbeat();
    void stop_heartbeat() noexcept;
    void heartbeat_loop();
    void fail() noexcept;

    // HA machinery.
    [[nodiscard]] status ensure_connected();
    [[nodiscard]] status reconnect_with_backoff();
    [[nodiscard]] status attach_to_node(std::size_t index);
    void reset_protocol_state() noexcept;
    void register_channel(channel* ch);
    void unregister_channel(channel* ch);
    [[nodiscard]] channel* channel_with_id(std::uint16_t id) noexcept;
    [[nodiscard]] status replay_channels();
    void set_node(std::size_t index) noexcept;

    connection_options options_;
    std::unique_ptr<db::channel> link_;
    std::atomic<bool> open_{false};
    rpc_error connect_error_;

    // Failover state.
    std::vector<node_address> nodes_;
    std::size_t node_index_ = 0;
    std::size_t last_success_index_ = 0;
    crpc::backend_factory* backend_ = nullptr;
    crpc::runtime* shard_ = nullptr;
    bool user_closed_ = false;
    rpc_error last_reconnect_error_;
    std::deque<channel*> channels_;
    std::map<std::uint16_t, channel*> channels_by_id_;

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
    std::map<std::uint16_t, std::deque<confirm_event>> confirm_events_;
    std::map<std::uint16_t, std::deque<std::string>> consumer_cancels_;
    std::map<std::uint16_t, std::map<std::uint64_t, bool>> confirm_results_;

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