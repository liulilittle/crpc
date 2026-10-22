// CRPC -- server facade implementation.
#include <crpc/server.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/endian.hpp>
#include <crpc/resilience/admission.hpp>
#include <crpc/resilience/fallback.hpp>
#include <crpc/rpc/call.hpp>

namespace crpc {
namespace {

[[nodiscard]] std::chrono::steady_clock::time_point steady_now() noexcept {
    return std::chrono::steady_clock::now();
}

} // namespace

struct server_connection;

// Called when a queued frame could not be written (e.g. a second push while one
// is in flight violates I5, or the peer is gone). Defined after the connection
// type below; a failure must never leave the peer waiting silently (R18).
void report_send_failure(server_connection* sc, std::error_code ec) noexcept;

// Finishes one outgoing frame and starts the next queued one. Defined after the
// connection type below.
void finish_send(server_connection* sc, io::io_result result) noexcept;

// Resumes the push pump after a PUSH frame leaves the connection's staging
// queue. Defined after `server::impl` (it needs the full type).
void wake_push_pump(server_connection* sc) noexcept;

// Queues the hub detach on the connection's shard. Defined after
// `server::impl` (it needs the full type).
void schedule_detach_connection(server_connection* sc) noexcept;

// Owns the bytes of one outgoing frame until the backend completes the write.
struct send_op final : io::completion {
    std::unique_ptr<std::byte[]> bytes;
    std::size_t size = 0;
    bool is_push = false;
    std::shared_ptr<server_connection> keep_alive;

    void complete(io::io_result result) noexcept override;
};

// One decoded request waiting for a handler coroutine.
struct request_job {
    std::shared_ptr<server_connection> conn;
    frame_header header{};
    buffer payload;
    bool counted = false;
    bool forced_error = false;
    rpc_code forced_code = rpc_code::ok;
    std::chrono::milliseconds forced_retry_after{0};
};

// One immutable admission record per registered service, built in `start()`
// after the registry is frozen. The handler path only reads it: no lock, no
// `operator[]`, no allocation. A service that is not in the registry has no
// entry and therefore never creates state.
struct service_guard {
    std::optional<resilience::bulkhead> bulkhead;
    std::optional<resilience::rate_limiter> rate;
    std::unique_ptr<resilience::circuit_breaker> breaker;
    // Existing per-service in-flight cap. Atomic so the handler path never
    // takes a lock. Ignored (not counted) when the cap is unlimited.
    std::atomic<std::size_t> in_flight{0};
};

struct server_connection {
    server::impl* owner = nullptr;
    runtime* shard = nullptr;

    // Connection-level flood bucket. It lives on the connection, is read only
    // by that connection's reader, and is absent unless configured.
    std::unique_ptr<resilience::rate_limiter> rate;

    struct reader_source final : io::source {
        explicit reader_source(server_connection* owner) noexcept : owner(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            return owner->on_data(data);
        }
        void on_eof() noexcept override { owner->on_terminal(); }
        void on_error(std::error_code ec) noexcept override {
            (void)ec;
            owner->on_terminal();
        }
        server_connection* owner;
    };

    // Declared before `conn` so reverse destruction tears the connection down
    // before the source it borrows. The destructor also unbinds explicitly:
    // bind_source(nullptr) waits for in-flight delivery to finish.
    std::unique_ptr<reader_source> source;
    std::unique_ptr<connection> conn;
    frame_decoder decoder{max_frame_default};
    std::mutex decode_mutex;
    std::atomic<std::uint32_t> in_flight{0};
    std::atomic<bool> closed{false};
    std::weak_ptr<server_connection> self;

    // The transport contract allows exactly one outstanding push per connection
    // (a second push while one is in flight is refused with
    // operation_not_supported by both epoll and inproc). Responses are therefore
    // queued here and handed to the sink one at a time. The queue is bounded by
    // per_connection_inflight: a handler sends at most one response.
    std::mutex send_mutex;
    std::deque<std::unique_ptr<send_op>> send_queue;
    bool send_busy = false;

    // Bytes of PUSH frames staged for the transport (in-flight plus queued).
    // Bounded by `options::push_egress_max_bytes`; the pump stops taking from
    // the hub when it reaches the bound. Responses are not counted: their bound
    // is the existing per-connection in-flight cap.
    std::atomic<std::size_t> push_egress_bytes{0};

    // --- push state ---------------------------------------------------------
    // Everything below is guarded by `server::impl::push_mutex`, which also
    // serializes every call into the single-threaded hub. The group deliberately
    // covers the caller thread (publish), the shard thread (subscribe), the
    // reader thread (PUSH_ACK) and the control thread (close/detach).
    u64 push_token = 0;      // hub::connection_id; 0 when detached
    u64 push_subscriber = 0;
    bool push_draining = false;
    std::atomic<bool> detach_scheduled{false};

    struct push_stream_state {
        u32 stream_id = 0;
        std::string topic;
        u16 topic_id = 0;
        push::delivery_mode mode = push::delivery_mode::reliable;
        bool ordered = false;
    };

    // One message already taken from the hub that did not fit under the egress
    // bound. Holding it here (rather than dropping it in the server) is what
    // lets the pump pause without losing a message.
    struct pending_message {
        push::push_message message;
        u32 stream_id = 0;
    };

    std::unordered_map<u32, push_stream_state> push_streams;
    std::unordered_map<std::string, u32> push_stream_by_topic;
    std::optional<pending_message> pending_push;
    buffer push_scratch;

    server_connection() = default;
    ~server_connection();
    void on_terminal() noexcept;

    std::size_t on_data(span<const std::byte> data) noexcept;
    void submit(frame& request) noexcept;
    void complete_call_safe() noexcept;
};

void report_send_failure(server_connection* sc, std::error_code ec) noexcept {
    (void)ec;
    if (sc == nullptr) {
        return;
    }
    bool expected = false;
    if (!sc->closed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    // A lost response would otherwise strand the peer until its deadline. Close
    // explicitly so every in-flight call observes EOF/failure; the drop is then
    // visible to the client instead of disappearing silently (R18).
    schedule_detach_connection(sc);
    if (sc->conn) {
        sc->conn->close();
    }
}

void finish_send(server_connection* sc, io::io_result result) noexcept {
    if (sc == nullptr) {
        return;
    }
    if (!result.ok()) {
        report_send_failure(sc, result.ec);
    }
    send_op* next = nullptr;
    {
        std::lock_guard<std::mutex> lk(sc->send_mutex);
        sc->send_busy = false;
        if (!sc->closed.load(std::memory_order_acquire) &&
            !sc->send_queue.empty()) {
            std::unique_ptr<send_op> head = std::move(sc->send_queue.front());
            sc->send_queue.pop_front();
            sc->send_busy = true;
            next = head.release();
        } else if (sc->closed.load(std::memory_order_acquire)) {
            // The peer is gone: nobody can consume these frames.
            sc->send_queue.clear();
        }
    }
    // Push outside the lock: a synchronous completion re-enters finish_send.
    if (next != nullptr && sc->conn) {
        sc->conn->sink().push(
            span<const std::byte>(next->bytes.get(), next->size), next);
    }
}

struct server::impl {
    options opt;
    rpc::service_registry registry;
    std::unique_ptr<rpc::dispatcher> dispatch;
    std::vector<std::unique_ptr<runtime>> shards;
    std::vector<std::unique_ptr<listener>> listeners;
    std::vector<listener_config> pending_listeners;
    std::vector<server_interceptor*> interceptors;

    std::atomic<bool> running{false};
    std::atomic<bool> accepting{false};
    std::atomic<bool> shutting_down{false};
    std::atomic<std::uint64_t> next_shard{0};

    std::atomic<std::size_t> in_flight{0};
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> handled{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> failures{0};
    std::atomic<std::uint64_t> rate_limited_connection{0};
    std::atomic<std::uint64_t> rate_limited_service{0};
    std::atomic<std::uint64_t> bulkhead_rejected{0};
    std::atomic<std::uint64_t> breaker_open{0};
    std::atomic<std::uint64_t> deadline_rejected{0};
    std::atomic<std::uint64_t> probe_admitted{0};

    std::mutex connections_mutex;
    std::vector<std::shared_ptr<server_connection>> connections;

    std::mutex drain_mutex;
    std::condition_variable drain_cv;

    // Frozen in `start()` before any listener can accept, and destroyed only
    // after every shard has been joined. The index is read concurrently after
    // freeze; `guard_index` itself is never mutated while handlers run.
    std::vector<std::unique_ptr<service_guard>> guards;
    std::unordered_map<u64, std::size_t> guard_index;

    // --- push ---------------------------------------------------------------
    // One hub for the (single) shard. The hub itself has no internal locking;
    // `push_mutex` is the single lock every access goes through. It is
    // recursive because a synchronous in-process send can re-enter the server
    // through the peer's PUSH_ACK before the sending call returns.
    //
    // Lock order: push_mutex may be held while taking send_mutex (send_payload).
    // No path takes push_mutex while holding send_mutex: finish_send releases
    // it before resuming the pump.
    std::recursive_mutex push_mutex;
    std::unique_ptr<push::hub> push_hub;
    std::unordered_map<std::string, u16> push_topic_ids;
    u16 next_push_topic_id = 1;
    u32 next_push_stream_id = 1;

    std::atomic<u64> push_published{0};
    std::atomic<u64> push_delivered{0};
    std::atomic<u64> push_overflow_dropped{0};
    std::atomic<u64> push_queued_messages{0};
    std::atomic<u64> push_queued_bytes{0};
    std::atomic<u64> push_active_connections{0};
    std::atomic<std::size_t> push_egress_high_water{0};
    std::atomic<u64> push_acks{0};

    struct acceptor final : accept_sink {
        explicit acceptor(impl* owner) noexcept : owner(owner) {}
        void on_accept(std::unique_ptr<connection> c) noexcept override {
            owner->on_accept(std::move(c));
        }
        impl* owner;
    };
    std::unique_ptr<acceptor> accept_sink_impl;

    explicit impl(options o) : opt(std::move(o)) {}

    // Built once after `registry.freeze()`. Every registered service gets a
    // breaker (the pre-existing behaviour); the rate and bulkhead stages exist
    // only when explicitly configured and enabled.
    void build_guards() {
        guards.clear();
        guard_index.clear();
        guards.reserve(registry.service_count());
        registry.for_each_service([this](const rpc::service_slot& service) {
            auto guard = std::make_unique<service_guard>();
            if (opt.resilience_enabled) {
                if (opt.per_service_rate) {
                    guard->rate.emplace(*opt.per_service_rate);
                }
                if (opt.per_service_bulkhead) {
                    resilience::bulkhead_config config = *opt.per_service_bulkhead;
                    if (config.capacity == 0) {
                        if (opt.per_service_inflight == 0) {
                            config.capacity = 0;
                        } else {
                            config.capacity = static_cast<u32>(std::min(
                                opt.per_service_inflight,
                                static_cast<std::size_t>(
                                    std::numeric_limits<u32>::max())));
                        }
                    }
                    if (config.capacity != 0) {
                        guard->bulkhead.emplace(config);
                    }
                }
            }
            guard->breaker =
                std::make_unique<resilience::circuit_breaker>(opt.breaker);
            guard_index.emplace(service.id(), guards.size());
            guards.push_back(std::move(guard));
        });
    }

    [[nodiscard]] service_guard* find_guard(u64 service_id) const noexcept {
        const auto entry = guard_index.find(service_id);
        if (entry == guard_index.end()) {
            return nullptr;
        }
        return guards[entry->second].get();
    }

    // Existing per-service cap, now lock-free and stored on the guard. A zero
    // cap means unlimited and is not counted.
    [[nodiscard]] bool enter_service_cap(service_guard& guard) noexcept {
        if (opt.per_service_inflight == 0) {
            return true;
        }
        std::size_t current = guard.in_flight.load(std::memory_order_relaxed);
        for (;;) {
            if (current >= opt.per_service_inflight) {
                return false;
            }
            if (guard.in_flight.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void leave_service_cap(service_guard& guard) noexcept {
        if (opt.per_service_inflight != 0) {
            guard.in_flight.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    // Called after the shards are joined. Debug builds assert the drain
    // invariant before the table is released.
    void clear_guards() noexcept {
        for (const std::unique_ptr<service_guard>& guard : guards) {
            if (!guard) {
                continue;
            }
#ifndef NDEBUG
            assert(guard->in_flight.load(std::memory_order_acquire) == 0);
            if (guard->bulkhead) {
                assert(guard->bulkhead->in_use() == 0);
            }
#else
            (void)guard;
#endif
        }
        guards.clear();
        guard_index.clear();
    }

    void on_accept(std::unique_ptr<connection> c) noexcept {
        if (c == nullptr) {
            return;
        }
        if (!accepting.load(std::memory_order_acquire) ||
            shards.empty()) {
            c->close();
            return;
        }
        try {
            const std::size_t index =
                static_cast<std::size_t>(
                    next_shard.fetch_add(1, std::memory_order_relaxed)) %
                shards.size();
            auto sc = std::make_shared<server_connection>();
            sc->owner = this;
            sc->shard = shards[index].get();
            if (opt.resilience_enabled && opt.per_connection_rate) {
                sc->rate = std::make_unique<resilience::rate_limiter>(
                    *opt.per_connection_rate);
            }
            sc->conn = std::move(c);
            sc->source = std::make_unique<server_connection::reader_source>(sc.get());
            sc->self = sc;
            {
                std::lock_guard<std::mutex> lk(connections_mutex);
                connections.push_back(sc);
            }
            accepted.fetch_add(1, std::memory_order_relaxed);
            sc->conn->bind_source(sc->source.get());
        } catch (...) {
            c.reset();
            ++failures;
        }
    }

    void notify_drained() noexcept {
        drain_cv.notify_all();
    }

    void complete_call(const std::shared_ptr<server_connection>& sc, bool counted) noexcept {
        if (counted) {
            sc->in_flight.fetch_sub(1, std::memory_order_acq_rel);
            in_flight.fetch_sub(1, std::memory_order_acq_rel);
            notify_drained();
        }
    }

    // Returns true when the frame was accepted into the staging pipeline (handed
    // to the sink or queued). A false return means the caller must keep its
    // message (the pump parks it in `pending_push`) or that the connection is
    // gone. `is_push` charges the byte count against the PUSH egress bound.
    [[nodiscard]] bool send_payload(const std::shared_ptr<server_connection>& sc,
                                     std::uint32_t stream_id, frame_type type,
                                     span<const std::byte> payload,
                                     bool is_push = false) noexcept {
        if (sc->closed.load(std::memory_order_acquire) || !sc->conn) {
            return false;
        }
        std::unique_ptr<send_op> op(new (std::nothrow) send_op());
        if (!op) {
            return false;
        }
        frame value;
        value.header.type = type;
        value.header.stream_id = stream_id;
        value.payload = payload;
        const std::size_t size = encoded_frame_size(value);
        op->bytes.reset(new (std::nothrow) std::byte[size]);
        if (!op->bytes) {
            return false;
        }
        std::size_t written = 0;
        const status encoded =
            encode_frame(span<std::byte>(op->bytes.get(), size), value, written);
        if (!encoded.ok()) {
            return false;
        }
        op->size = written;
        op->is_push = is_push;
        op->keep_alive = sc;

        if (is_push) {
            const std::size_t total =
                sc->push_egress_bytes.fetch_add(op->size, std::memory_order_acq_rel) +
                op->size;
            std::size_t high = push_egress_high_water.load(std::memory_order_relaxed);
            while (high < total &&
                   !push_egress_high_water.compare_exchange_weak(
                       high, total, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        }

        send_op* ready = nullptr;
        {
            std::lock_guard<std::mutex> lk(sc->send_mutex);
            if (sc->closed.load(std::memory_order_acquire)) {
                if (is_push) {
                    sc->push_egress_bytes.fetch_sub(op->size);
                }
                return false;
            }
            if (sc->send_busy) {
                // One push is already outstanding: hold this frame until its
                // completion arrives. The transport refuses a second push.
                sc->send_queue.push_back(std::move(op));
                return true;
            }
            sc->send_busy = true;
            ready = op.release();
        }
        sc->conn->sink().push(
            span<const std::byte>(ready->bytes.get(), ready->size), ready);
        return true;
    }

    // --- push helpers (all called with push_mutex held) ---------------------

    [[nodiscard]] u32 next_push_stream_locked() noexcept {
        const u32 id = next_push_stream_id;
        ++next_push_stream_id;
        if (next_push_stream_id == 0 || next_push_stream_id > max_stream_id) {
            next_push_stream_id = 1;
        }
        return id;
    }

    [[nodiscard]] u16 topic_id_locked(const std::string& topic) {
        const auto known = push_topic_ids.find(topic);
        if (known != push_topic_ids.end()) {
            return known->second;
        }
        const u16 id = next_push_topic_id;
        ++next_push_topic_id;
        if (next_push_topic_id == 0) {
            next_push_topic_id = 1;
        }
        push_topic_ids.emplace(topic, id);
        return id;
    }

    void refresh_push_stats_locked() noexcept {
        if (!push_hub) {
            push_published.store(0, std::memory_order_relaxed);
            push_delivered.store(0, std::memory_order_relaxed);
            push_overflow_dropped.store(0, std::memory_order_relaxed);
            push_queued_messages.store(0, std::memory_order_relaxed);
            push_queued_bytes.store(0, std::memory_order_relaxed);
            push_active_connections.store(0, std::memory_order_relaxed);
            return;
        }
        const push::hub_stats counters = push_hub->stats();
        push_published.store(counters.published, std::memory_order_relaxed);
        push_delivered.store(counters.delivered, std::memory_order_relaxed);
        push_overflow_dropped.store(counters.dropped_overflow,
                                    std::memory_order_relaxed);
        push_queued_messages.store(counters.queued_messages,
                                   std::memory_order_relaxed);
        push_queued_bytes.store(counters.queued_bytes, std::memory_order_relaxed);
        push_active_connections.store(counters.active_connections,
                                      std::memory_order_relaxed);
    }

    static push::push_envelope make_push_envelope(const push::push_message& message,
                                                  u16 topic_id) noexcept {
        push::push_envelope envelope;
        envelope.version = push::push_wire_version;
        envelope.mode = message.mode;
        envelope.topic_id = topic_id;
        envelope.publisher_id = message.publisher_id;
        envelope.epoch = message.epoch;
        envelope.seq = message.seq;
        envelope.topic_seq = message.topic_seq;
        envelope.body = message.data();
        return envelope;
    }

    [[nodiscard]] static std::size_t push_frame_size(
        const push::push_message& message, u16 topic_id) noexcept {
        return frame_header_size +
               push::encoded_push_envelope_size(make_push_envelope(message, topic_id));
    }

    [[nodiscard]] bool send_push_locked(
        const std::shared_ptr<server_connection>& sc,
        const push::push_message& message,
        const server_connection::push_stream_state& stream) noexcept {
        const push::push_envelope envelope =
            make_push_envelope(message, stream.topic_id);
        const std::size_t size = push::encoded_push_envelope_size(envelope);
        sc->push_scratch.clear();
        if (!sc->push_scratch.reserve(size)) {
            return false;
        }
        std::size_t written = 0;
        if (!push::encode_push_envelope(
                 span<std::byte>(sc->push_scratch.data(), size), envelope, written)
                 .ok()) {
            return false;
        }
        return send_payload(
            sc, stream.stream_id, frame_type::push,
            span<const std::byte>(sc->push_scratch.data(), written), true);
    }

    [[nodiscard]] bool flush_pending_push_locked(
        const std::shared_ptr<server_connection>& sc) noexcept {
        if (!sc->pending_push.has_value()) {
            return true;
        }
        server_connection::pending_message& pending = *sc->pending_push;
        const auto stream = sc->push_streams.find(pending.stream_id);
        if (stream == sc->push_streams.end()) {
            sc->pending_push.reset();
            return true;
        }
        const std::size_t frame =
            push_frame_size(pending.message, stream->second.topic_id);
        const std::size_t egress =
            sc->push_egress_bytes.load(std::memory_order_relaxed);
        if (opt.push_egress_max_bytes != 0 && egress != 0 &&
            egress + frame > opt.push_egress_max_bytes) {
            return false;
        }
        if (!send_push_locked(sc, pending.message, stream->second)) {
            return false;
        }
        sc->pending_push.reset();
        return true;
    }

    // Takes messages from the hub while the connection has PUSH egress
    // headroom. The loop is the only place `try_take` is called, so reaching
    // the bound really does stop the pump rather than dropping in this layer.
    void drain_connection_locked(
        const std::shared_ptr<server_connection>& sc) noexcept {
        if (!sc || !push_hub || sc->closed.load(std::memory_order_acquire) ||
            sc->push_token == 0) {
            return;
        }
        if (sc->push_draining) {
            // A synchronous completion re-entered while the outer pump is
            // running; the outer loop re-evaluates the bound itself.
            return;
        }
        sc->push_draining = true;
        for (;;) {
            if (sc->closed.load(std::memory_order_acquire) || sc->push_token == 0) {
                break;
            }
            if (sc->pending_push.has_value()) {
                if (!flush_pending_push_locked(sc)) {
                    break;
                }
                continue;
            }
            if (opt.push_egress_max_bytes != 0 &&
                sc->push_egress_bytes.load(std::memory_order_relaxed) >=
                    opt.push_egress_max_bytes) {
                break;
            }
            push::push_message message;
            if (!push_hub->try_take(sc->push_token, message)) {
                break;
            }
            const auto by_topic = sc->push_stream_by_topic.find(message.topic);
            if (by_topic == sc->push_stream_by_topic.end()) {
                continue;
            }
            const auto stream = sc->push_streams.find(by_topic->second);
            if (stream == sc->push_streams.end()) {
                continue;
            }
            const std::size_t frame =
                push_frame_size(message, stream->second.topic_id);
            const std::size_t egress =
                sc->push_egress_bytes.load(std::memory_order_relaxed);
            // A single message larger than the bound still makes progress; the
            // bound pauses the pump, it never refuses a message outright.
            if (opt.push_egress_max_bytes != 0 && egress != 0 &&
                egress + frame > opt.push_egress_max_bytes) {
                sc->pending_push =
                    server_connection::pending_message{std::move(message),
                                                       stream->second.stream_id};
                break;
            }
            if (!send_push_locked(sc, message, stream->second)) {
                sc->pending_push =
                    server_connection::pending_message{std::move(message),
                                                       stream->second.stream_id};
                break;
            }
        }
        sc->push_draining = false;
        refresh_push_stats_locked();
    }

    void wake_push_pump(server_connection* raw) noexcept {
        if (raw == nullptr) {
            return;
        }
        std::shared_ptr<server_connection> sc = raw->self.lock();
        if (!sc) {
            return;
        }
        std::lock_guard<std::recursive_mutex> lk(push_mutex);
        if (sc->push_draining) {
            return;
        }
        drain_connection_locked(sc);
    }

    // Idempotent. Called from the terminal path, the destructor and close_all;
    // detaching here (before the connection and its token disappear) is the
    // order the hub contract requires.
    void detach_connection(server_connection* sc) noexcept {
        if (sc == nullptr) {
            return;
        }
        std::lock_guard<std::recursive_mutex> lk(push_mutex);
        if (sc->push_token != 0) {
            if (push_hub) {
                (void)push_hub->detach(sc->push_token);
            }
            sc->push_token = 0;
            sc->push_subscriber = 0;
        }
        sc->pending_push.reset();
        sc->push_streams.clear();
        sc->push_stream_by_topic.clear();
        refresh_push_stats_locked();
    }

    void handle_push_ack_locked(const std::shared_ptr<server_connection>& sc,
                                u32 stream_id, u64 seq) noexcept {
        if (!sc || !push_hub || sc->push_token == 0) {
            return;
        }
        const auto stream = sc->push_streams.find(stream_id);
        if (stream == sc->push_streams.end()) {
            return;
        }
        if (push_hub->ack(sc->push_subscriber, stream->second.topic, seq).ok()) {
            push_acks.fetch_add(1, std::memory_order_relaxed);
        }
        refresh_push_stats_locked();
        drain_connection_locked(sc);
    }

    void handle_push_ack(const std::shared_ptr<server_connection>& sc,
                         u32 stream_id, u64 seq) noexcept {
        std::lock_guard<std::recursive_mutex> lk(push_mutex);
        handle_push_ack_locked(sc, stream_id, seq);
    }

    struct detach_task {
        impl* owner = nullptr;
        std::shared_ptr<server_connection> conn;
    };
    static void run_detach_task(void* ctx) noexcept;
    static void drop_detach_task(void* ctx) noexcept;

    // Schedules the detach on the shard instead of taking push_mutex on the
    // closing thread. The client destructor holds its own mutex while closing
    // the transport; taking push_mutex there would invert against the send path
    // (push_mutex -> client mutex), which TSan reports as a potential deadlock.
    // The task keeps the connection alive, so the hub detach still happens
    // before the connection object is destroyed.
    void schedule_detach(server_connection* sc) noexcept {
        if (sc == nullptr || sc->owner == nullptr) {
            return;
        }
        if (sc->detach_scheduled.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        std::shared_ptr<server_connection> keep = sc->self.lock();
        if (!keep) {
            sc->detach_scheduled.store(false, std::memory_order_release);
            return;
        }
        auto* task = new (std::nothrow) detach_task{this, std::move(keep)};
        if (task == nullptr || sc->shard == nullptr ||
            !sc->shard->post(&run_detach_task, task, &drop_detach_task)) {
            delete task;
            sc->detach_scheduled.store(false, std::memory_order_release);
        }
    }

    // Handles the reserved push control REQUEST and fills `response` with an
    // encoded RESPONSE payload. Returns the outcome code.
    rpc_code handle_push_control_locked(request_job& job,
                                        const call_envelope& envelope,
                                        buffer& response) noexcept {
        rpc_code code = rpc_code::ok;
        rpc_error error;
        std::vector<std::byte> body;
        try {
            if (opt.shards != 1 || !push_hub) {
                code = rpc_code::unavailable;
                error = rpc_error(code, "push requires a single-shard server");
            } else if (envelope.method_id != push::push_control_method_subscribe) {
                code = rpc_code::no_such_method;
                error = rpc_error(code, "unknown push control method");
            } else {
                push::subscribe_request request;
                if (!push::decode_subscribe_request(envelope.body, request).ok()) {
                    code = rpc_code::protocol_error;
                    error = rpc_error(code, "malformed subscribe request");
                } else {
                    const status subscribed = push_hub->subscribe(
                        request.topic, request.subscriber_id, request.mode,
                        request.ordered, opt.push_subscribe);
                    if (!subscribed.ok() &&
                        subscribed.code() != rpc_code::failed_precondition) {
                        code = subscribed.code();
                        error = subscribed.error();
                    } else {
                        const push::hub::connection_id token =
                            push_hub->attach(request.subscriber_id);
                        server_connection* sc = job.conn.get();
                        if (token == push::hub::invalid_connection || sc == nullptr) {
                            code = rpc_code::resource_exhausted;
                            error = rpc_error(code, "push attach failed");
                        } else {
                            u32 stream_id = 0;
                            const auto existing =
                                sc->push_stream_by_topic.find(request.topic);
                            if (existing != sc->push_stream_by_topic.end()) {
                                stream_id = existing->second;
                            } else {
                                stream_id = next_push_stream_locked();
                                server_connection::push_stream_state stream;
                                stream.stream_id = stream_id;
                                stream.topic = request.topic;
                                stream.topic_id = topic_id_locked(request.topic);
                                stream.mode = request.mode;
                                stream.ordered = request.ordered;
                                sc->push_streams[stream_id] = std::move(stream);
                                sc->push_stream_by_topic[request.topic] = stream_id;
                            }
                            sc->push_token = token;
                            sc->push_subscriber = request.subscriber_id;

                            const server_connection::push_stream_state& state =
                                sc->push_streams.at(stream_id);
                            push::push_subscription reply;
                            reply.stream_id = stream_id;
                            reply.topic_id = state.topic_id;
                            reply.mode = state.mode;
                            reply.ordered = state.ordered;
                            reply.topic = request.topic;
                            const std::size_t size =
                                push::encoded_push_subscription_size(reply);
                            body.resize(size);
                            std::size_t written = 0;
                            const status encoded = push::encode_push_subscription(
                                span<std::byte>(body.data(), body.size()), reply,
                                written);
                            if (!encoded.ok()) {
                                code = encoded.code();
                                error = encoded.error();
                            }
                        }
                    }
                }
            }
        } catch (...) {
            code = rpc_code::internal;
            error = rpc_error(code, "push control allocation failed");
        }

        const status encoded =
            code == rpc_code::ok
                ? rpc::encode_response(
                      rpc_code::ok, rpc_error{},
                      span<const std::byte>(body.data(), body.size()), response)
                : rpc::encode_response(code, error, span<const std::byte>(),
                                       response);
        if (!encoded.ok()) {
            code = rpc_code::encode_error;
            response.clear();
        }
        refresh_push_stats_locked();
        return code;
    }

    void send_error(const std::shared_ptr<server_connection>& sc, std::uint32_t stream_id,
                    rpc_code code, std::chrono::milliseconds retry_after,
                    const char* message, bool degradable = false) noexcept {
        rpc_error error(code, message != nullptr ? std::string(message) : std::string());
        error.retryable = is_retryable(code);
        error.retry_after = retry_after;
        // The single place the reserved marker is written. Only the four
        // pre-execution admission refusals pass `degradable = true`; every other
        // call site leaves it false, so a handler that returns the same code
        // never advertises degradability. See resilience/admission.hpp (6).
        if (degradable) {
            error.metadata.set(resilience::degradable_metadata_key,
                               resilience::degraded_metadata_value);
        }
        buffer payload;
        if (!rpc::encode_response(code, error, span<const std::byte>(), payload).ok()) {
            return;
        }
        (void)send_payload(sc, stream_id, frame_type::response, payload.readable());
    }

    // True when the request's wire deadline is below the floor the dispatcher
    // uses to refuse work (resilience::min_remaining_ns). Such a request may
    // still be refused by an admission stage that runs first (the breaker, or
    // the rate bucket whose hint happens to fit the remaining time), but a
    // fallback value could not reach the caller in time either, so those
    // refusals must not advertise degradability.
    //
    // All four marked call sites pass `one_way = false` as a literal: each sits
    // inside `if (wants_response)`, so the request is never one-way there.
    // `fallback_eligible` still takes the flag so the one-way decision stays in
    // one place; do not replace the literal with a second test of the frame
    // type, which could drift from `wants_response`.
    [[nodiscard]] static bool wire_deadline_expired(const call_envelope& envelope) noexcept {
        return envelope.extensions.has(trace_extension::has_deadline) &&
               envelope.extensions.deadline_ns < resilience::min_remaining_ns;
    }

    void handle_request(request_job& job) noexcept {
        const std::shared_ptr<server_connection>& sc = job.conn;
        const bool wants_response = job.header.type != frame_type::one_way;
        if (job.forced_error) {
            if (wants_response) {
                const char* message =
                    job.forced_code == rpc_code::resource_exhausted
                        ? "rate limit exceeded"
                        : "overloaded";
                send_error(sc, job.header.stream_id, job.forced_code,
                           job.forced_retry_after, message);
            }
            complete_call(sc, job.counted);
            return;
        }
        if (shutting_down.load(std::memory_order_acquire)) {
            if (wants_response) {
                send_error(sc, job.header.stream_id, rpc_code::shutting_down,
                           std::chrono::milliseconds(0), "server is shutting down");
            }
            complete_call(sc, job.counted);
            return;
        }

        frame request;
        request.header = job.header;
        request.payload = job.payload.readable();

        for (server_interceptor* interceptor : interceptors) {
            if (interceptor != nullptr) {
                interceptor->on_request(request);
            }
        }

        // Per-service cap and breaker. A malformed envelope is left to the
        // dispatcher, which produces the decode_error response.
        call_envelope envelope;
        const bool decoded = decode_call_envelope(request.payload, envelope).ok();
        const u64 service_id = decoded ? envelope.service_id : 0;

        if (decoded && service_id == push::push_control_service_id) {
            buffer response;
            rpc_code outcome = rpc_code::ok;
            {
                // The push lock also orders the subscription reply before the
                // first replayed PUSH on this connection.
                std::lock_guard<std::recursive_mutex> lk(push_mutex);
                outcome = handle_push_control_locked(job, envelope, response);
                if (wants_response) {
                    if (response.empty()) {
                        send_error(sc, job.header.stream_id, rpc_code::internal,
                                   std::chrono::milliseconds(0),
                                   "push control response encoding failed");
                    } else {
                        (void)send_payload(sc, job.header.stream_id,
                                           frame_type::response, response.readable());
                    }
                }
                if (outcome == rpc_code::ok) {
                    if (std::shared_ptr<server_connection> keep = sc->self.lock()) {
                        drain_connection_locked(keep);
                    }
                }
            }
            for (server_interceptor* interceptor : interceptors) {
                if (interceptor != nullptr) {
                    interceptor->on_response(outcome);
                }
            }
            handled.fetch_add(1, std::memory_order_relaxed);
            complete_call(sc, job.counted);
            return;
        }

        // Validate the service id against the frozen registry before touching
        // any per-service guard. An arbitrary client-chosen id must not
        // allocate state: the frozen guard index is keyed by registered ids
        // only. The dispatcher still runs for an unknown id so it produces the
        // canonical `not_found` response.
        const bool guarded =
            decoded && registry.find_service(service_id) != nullptr;
        service_guard* guard = guarded ? find_guard(service_id) : nullptr;

        // Breaker-first: a half-open probe must be able to reach the service
        // even when the per-service rate bucket or bulkhead is saturated.
        // `is_probe()` comes from the permit itself, so a concurrent state
        // transition cannot misclassify a closed-state admission.
        resilience::breaker_permit permit;
        bool probe = false;
        if (guard != nullptr && guard->breaker) {
            permit = guard->breaker->allow();
            if (!permit.allowed()) {
                breaker_open.fetch_add(1, std::memory_order_relaxed);
                if (wants_response) {
                    const rpc_code code = resilience::reject_code(
                        resilience::stage_kind::breaker);
                    send_error(sc, job.header.stream_id, code,
                               permit.retry_after(), "service circuit breaker is open",
                               !wire_deadline_expired(envelope) &&
                                   resilience::fallback_eligible(
                                       code, resilience::stage_kind::breaker,
                                       /*one_way=*/false));
                }
                complete_call(sc, job.counted);
                return;
            }
            probe = permit.is_probe();
        }

        // Admission: rate -> bulkhead -> existing per-service cap. A half-open
        // probe skips the per-service rate and bulkhead stages only; the cap
        // is a real capacity bound and applies to probes too (this is also the
        // pre-pipeline behaviour when resilience is disabled).
        bool cap_entered = false;
        resilience::bulkhead_permit bulkhead_permit;
        if (guard != nullptr) {
            if (opt.resilience_enabled && !probe) {
                if (guard->rate) {
                    resilience::rate_limit_result limited;
                    if (decoded && envelope.extensions.has(trace_extension::has_deadline)) {
                        const auto now = steady_now();
                        const auto now_ns = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now.time_since_epoch())
                                .count());
                        const std::uint64_t remaining =
                            envelope.extensions.deadline_ns;
                        const std::uint64_t deadline_ns =
                            remaining >
                                    std::numeric_limits<std::uint64_t>::max() - now_ns
                                ? std::numeric_limits<std::uint64_t>::max()
                                : now_ns + remaining;
                        limited = guard->rate->acquire_for(
                            1, std::chrono::nanoseconds(deadline_ns));
                    } else {
                        limited = guard->rate->try_acquire(1);
                    }
                    if (!limited.ok()) {
                        // Local admission: never record it as a service
                        // failure.
                        permit.ignore();
                        rate_limited_service.fetch_add(1, std::memory_order_relaxed);
                        if (!limited.deadline_met) {
                            deadline_rejected.fetch_add(1, std::memory_order_relaxed);
                            if (wants_response) {
                                send_error(sc, job.header.stream_id,
                                           resilience::reject_code(
                                               resilience::stage_kind::service_rate,
                                               limited.deadline_met),
                                           std::chrono::milliseconds(0),
                                           "service rate limit cannot meet the deadline");
                            }
                        } else if (wants_response) {
                            const rpc_code code = resilience::reject_code(
                                resilience::stage_kind::service_rate,
                                limited.deadline_met);
                            send_error(sc, job.header.stream_id, code,
                                       limited.retry_after,
                                       "service rate limit reached",
                                       !wire_deadline_expired(envelope) &&
                                           resilience::fallback_eligible(
                                               code,
                                               resilience::stage_kind::service_rate,
                                               /*one_way=*/false));
                        }
                        complete_call(sc, job.counted);
                        return;
                    }
                }
                if (guard->bulkhead) {
                    bulkhead_permit = guard->bulkhead->try_enter();
                    if (!bulkhead_permit.allowed()) {
                        permit.ignore();
                        bulkhead_rejected.fetch_add(1, std::memory_order_relaxed);
                        if (wants_response) {
                            const std::chrono::milliseconds hint =
                                bulkhead_permit.retry_after().count() > 0
                                    ? bulkhead_permit.retry_after()
                                    : opt.overload_retry_after;
                            const rpc_code code = resilience::reject_code(
                                resilience::stage_kind::service_bulkhead);
                            send_error(sc, job.header.stream_id, code, hint,
                                       "service bulkhead is full",
                                       !wire_deadline_expired(envelope) &&
                                           resilience::fallback_eligible(
                                               code,
                                               resilience::stage_kind::service_bulkhead,
                                               /*one_way=*/false));
                        }
                        complete_call(sc, job.counted);
                        return;
                    }
                }
            }
            if (!enter_service_cap(*guard)) {
                permit.ignore();
                rejected.fetch_add(1, std::memory_order_relaxed);
                if (wants_response) {
                    const rpc_code code = resilience::reject_code(
                        resilience::stage_kind::service_capacity);
                    send_error(sc, job.header.stream_id, code,
                               opt.overload_retry_after,
                               "service concurrency limit reached",
                               !wire_deadline_expired(envelope) &&
                                   resilience::fallback_eligible(
                                       code,
                                       resilience::stage_kind::service_capacity,
                                       /*one_way=*/false));
                }
                complete_call(sc, job.counted);
                return;
            }
            cap_entered = true;
        }
        if (probe) {
            // Count probes that got past admission and reached dispatch; a
            // probe rejected by the per-service cap is not an admitted probe.
            probe_admitted.fetch_add(1, std::memory_order_relaxed);
        }

        buffer response;
        bool has_response = false;
        bool handler_ran = false;
        const status dispatched = dispatch->dispatch(request, response, has_response,
                                                     steady_now(), handler_ran);

        rpc_code outcome_code = rpc_code::ok;
        if (!dispatched.ok()) {
            outcome_code = dispatched.code();
        } else if (has_response) {
            rpc_code coded = rpc_code::ok;
            rpc_error ignored;
            span<const std::byte> body;
            if (rpc::decode_response(response.readable(), coded, ignored, body).ok()) {
                outcome_code = coded;
            }
        }
        if (outcome_code == rpc_code::deadline_exceeded) {
            deadline_rejected.fetch_add(1, std::memory_order_relaxed);
        } else if (outcome_code == rpc_code::overload && !handler_ran) {
            // The dispatcher's per-method concurrency limit. Application
            // handlers that return `overload` are not local admissions.
            rejected.fetch_add(1, std::memory_order_relaxed);
        }
        if (permit.allowed()) {
            switch (outcome_code) {
            case rpc_code::ok:
                permit.success();
                break;
            case rpc_code::deadline_exceeded:
                // The dispatcher refuses an expired wire deadline before the
                // handler starts; a handler can also report one while running.
                // `handler_ran` is the explicit signal for that difference:
                // a pre-execution refusal says nothing about the service, while
                // one produced by a running handler is a slow call.
                if (handler_ran) {
                    permit.slow();
                } else {
                    permit.ignore();
                }
                break;
            case rpc_code::internal:
            case rpc_code::unavailable:
            case rpc_code::not_acknowledged:
                permit.failure();
                break;
            default:
                // Local admission and application outcomes (not_found,
                // no_such_method, invalid_argument, failed_precondition,
                // decode_error, protocol_error, canceled, shutting_down,
                // overload, resource_exhausted, application, ...) are not
                // dependency failures and must not open the service breaker.
                permit.ignore();
                break;
            }
        }
        if (!dispatched.ok()) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }

        if (has_response && wants_response) {
            (void)send_payload(sc, job.header.stream_id, frame_type::response,
                               response.readable());
        } else if (!dispatched.ok() && wants_response) {
            send_error(sc, job.header.stream_id, dispatched.code(),
                       std::chrono::milliseconds(0), "dispatch failed");
        }

        for (server_interceptor* interceptor : interceptors) {
            if (interceptor != nullptr) {
                interceptor->on_response(outcome_code);
            }
        }
        handled.fetch_add(1, std::memory_order_relaxed);
        if (cap_entered) {
            leave_service_cap(*guard);
        }
        complete_call(sc, job.counted);
    }

    static void run_job(void* ctx) noexcept {
        std::unique_ptr<request_job> job(static_cast<request_job*>(ctx));
        if (job->conn && job->conn->owner != nullptr) {
            job->conn->owner->handle_request(*job);
        }
    }

    // The shard stopped before the job ran. Release the job and give back the
    // in-flight accounting it took, so shutdown neither leaks nor leaves the
    // server's drain counter stuck above zero.
    static void drop_job(void* ctx) noexcept {
        std::unique_ptr<request_job> job(static_cast<request_job*>(ctx));
        if (job->counted && job->conn && job->conn->owner != nullptr) {
            job->conn->owner->complete_call(job->conn, true);
        }
    }

    void send_goaway() noexcept {
        std::vector<std::shared_ptr<server_connection>> snapshot;
        {
            std::lock_guard<std::mutex> lk(connections_mutex);
            snapshot = connections;
        }
        std::byte payload[8];
        endian::store_le32(payload, 0);
        endian::store_le32(payload + 4, static_cast<u32>(rpc_code::shutting_down));
        for (const std::shared_ptr<server_connection>& sc : snapshot) {
            (void)send_payload(sc, 0, frame_type::goaway,
                         span<const std::byte>(payload, sizeof(payload)));
        }
    }

    void close_all() noexcept {
        std::vector<std::shared_ptr<server_connection>> snapshot;
        {
            std::lock_guard<std::mutex> lk(connections_mutex);
            snapshot.swap(connections);
        }
        for (const std::shared_ptr<server_connection>& sc : snapshot) {
            sc->closed.store(true, std::memory_order_release);
            sc->detach_scheduled.store(true, std::memory_order_release);
            // Detach first: the hub must never target a connection that is
            // about to be closed (and later destroyed). The control thread
            // holds no client mutex here, so a synchronous detach is safe.
            detach_connection(sc.get());
            if (sc->conn) {
                sc->conn->close();
            }
        }
    }
};

struct push_ack_task {
    server::impl* owner = nullptr;
    std::shared_ptr<server_connection> conn;
    u32 stream_id = 0;
    u64 seq = 0;
};

void run_push_ack_task(void* ctx) noexcept {
    std::unique_ptr<push_ack_task> task(static_cast<push_ack_task*>(ctx));
    if (task->owner != nullptr) {
        task->owner->handle_push_ack(task->conn, task->stream_id, task->seq);
    }
}

void drop_push_ack_task(void* ctx) noexcept {
    delete static_cast<push_ack_task*>(ctx);
}

void server::impl::run_detach_task(void* ctx) noexcept {
    std::unique_ptr<detach_task> task(static_cast<detach_task*>(ctx));
    if (task->owner != nullptr && task->conn) {
        task->owner->detach_connection(task->conn.get());
    }
}

void server::impl::drop_detach_task(void* ctx) noexcept {
    delete static_cast<detach_task*>(ctx);
}

void send_op::complete(io::io_result result) noexcept {
    // Keep the connection alive across the hand-off to the next frame: the
    // completion may be the last owner once this object is gone.
    std::shared_ptr<server_connection> keep = keep_alive;
    if (keep && is_push) {
        // Release the per-connection PUSH egress budget before the pump is
        // allowed to take another message.
        keep->push_egress_bytes.fetch_sub(size, std::memory_order_acq_rel);
    }
    if (keep) {
        finish_send(keep.get(), result);
        if (keep->owner != nullptr) {
            wake_push_pump(keep.get());
        }
    }
    delete this;
}

server_connection::~server_connection() {
    // Connection teardown order is part of the contract: detach from the hub
    // before the connection and its push token can disappear. The detach is
    // synchronous under push_mutex; a no-op when the terminal path already ran.
    if (owner != nullptr) {
        owner->detach_connection(this);
    }
    if (conn) {
        conn->bind_source(nullptr);
    }
}

void server_connection::on_terminal() noexcept {
    if (closed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // The detach is queued on the shard. It holds this connection alive until
    // it runs, so the hub detach still precedes destruction; the closed flag
    // already stops the pump from targeting this connection in the meantime.
    if (owner != nullptr) {
        owner->schedule_detach(this);
    }
}

void wake_push_pump(server_connection* sc) noexcept {
    if (sc != nullptr && sc->owner != nullptr) {
        sc->owner->wake_push_pump(sc);
    }
}

void schedule_detach_connection(server_connection* sc) noexcept {
    if (sc != nullptr && sc->owner != nullptr) {
        sc->owner->schedule_detach(sc);
    }
}

std::size_t server_connection::on_data(span<const std::byte> data) noexcept {
    if (closed.load(std::memory_order_acquire)) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(decode_mutex);
    decoder.feed(data);
    for (;;) {
        frame value;
        const frame_decoder::result result = decoder.next(value);
        if (result == frame_decoder::result::need_more) {
            break;
        }
        if (result == frame_decoder::result::error) {
            on_terminal();
            if (conn) {
                conn->close();
            }
            break;
        }
        if (value.header.type == frame_type::request ||
            value.header.type == frame_type::one_way) {
            submit(value);
        } else if (value.header.type == frame_type::push_ack) {
            if (value.payload.size() != push::push_ack_size) {
                // A wrong-sized acknowledgement is a protocol violation; fail
                // closed instead of acknowledging a guessed sequence.
                on_terminal();
                if (conn) {
                    conn->close();
                }
                break;
            }
            if (shard != nullptr) {
                push_ack_task* task = new (std::nothrow) push_ack_task();
                const std::shared_ptr<server_connection> keep = self.lock();
                if (task == nullptr || !keep) {
                    delete task;
                } else {
                    task->owner = owner;
                    task->conn = keep;
                    task->stream_id = value.header.stream_id;
                    task->seq = endian::load_le64(value.payload.data());
                    // The hub is not thread-safe: the acknowledgement is
                    // applied on the shard thread, never on the reader.
                    if (!shard->post(&run_push_ack_task, task,
                                     &drop_push_ack_task)) {
                        delete task;
                    }
                }
            }
        }
    }
    return data.size();
}

void server_connection::submit(frame& request) noexcept {
    // Connection-level flood protection, before the in-flight cap and before
    // any job allocation. The bucket belongs to this connection; it is not the
    // per-service rate stage.
    if (rate) {
        const resilience::rate_limit_result limited = rate->try_acquire(1);
        if (!limited.ok()) {
            owner->rate_limited_connection.fetch_add(1,
                                                     std::memory_order_relaxed);
            if (request.header.type != frame_type::one_way) {
                request_job* reject = new (std::nothrow) request_job();
                if (reject != nullptr) {
                    reject->conn = self.lock();
                    reject->header = request.header;
                    reject->counted = false;
                    reject->forced_error = true;
                    reject->forced_code = rpc_code::resource_exhausted;
                    reject->forced_retry_after = limited.retry_after;
                    if (!reject->conn ||
                        !shard->submit(&server::impl::run_job, reject,
                                       &server::impl::drop_job)) {
                        delete reject;
                    }
                }
            }
            return;
        }
    }

    const std::size_t cap = owner->opt.per_connection_inflight;
    const std::uint32_t current =
        in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (cap != 0 && static_cast<std::size_t>(current) > cap) {
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
        owner->rejected.fetch_add(1, std::memory_order_relaxed);
        if (request.header.type != frame_type::one_way) {
            request_job* reject = new (std::nothrow) request_job();
            if (reject != nullptr) {
                reject->conn = self.lock();
                reject->header = request.header;
                reject->counted = false;
                reject->forced_error = true;
                reject->forced_code = rpc_code::overload;
                reject->forced_retry_after = owner->opt.overload_retry_after;
                if (!reject->conn ||
                    !shard->submit(&server::impl::run_job, reject,
                                  &server::impl::drop_job)) {
                    delete reject;
                }
            }
        }
        return;
    }

    owner->in_flight.fetch_add(1, std::memory_order_acq_rel);
    request_job* job = new (std::nothrow) request_job();
    if (job == nullptr || !job->payload.append(request.payload)) {
        delete job;
        complete_call_safe();
        return;
    }
    job->conn = self.lock();
    job->header = request.header;
    job->counted = true;
    if (!job->conn ||
        !shard->submit(&server::impl::run_job, job, &server::impl::drop_job)) {
        delete job;
        complete_call_safe();
    }
}

void server_connection::complete_call_safe() noexcept {
    in_flight.fetch_sub(1, std::memory_order_acq_rel);
    owner->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    owner->notify_drained();
}

// --- builder -----------------------------------------------------------------

server::builder& server::builder::add_listener(endpoint ep, backend_factory& backend) {
    listener_config config;
    config.ep = std::move(ep);
    config.backend = &backend;
    listeners_.push_back(std::move(config));
    return *this;
}

server::builder& server::builder::add_service(u64 service_id, std::string name) {
    registrations_.push_back(
        [service_id, name](rpc::service_registry& registry) -> status {
            return registry.register_service(service_id, name);
        });
    return *this;
}

server::builder& server::builder::add_raw_method(u64 service_id, u64 method_id,
                                                 std::string name,
                                                 rpc::method_handler handler, void* self,
                                                 rpc::method_options options) {
    registrations_.push_back(
        [service_id, method_id, name, handler, self, options](
            rpc::service_registry& registry) -> status {
            return registry.register_method(service_id, method_id, name, handler, self,
                                            options);
        });
    return *this;
}

server::builder& server::builder::add_interceptor(server_interceptor& interceptor) {
    interceptors_.push_back(&interceptor);
    return *this;
}

server::builder& server::builder::shards(std::size_t count) {
    options_.shards = count == 0 ? 1 : count;
    return *this;
}

server::builder& server::builder::handler_workers(std::size_t count) {
    options_.runtime.handler_workers = count;
    return *this;
}

server::builder& server::builder::handler_stack_bytes(std::size_t bytes) {
    options_.runtime.handler_stack_bytes = bytes;
    return *this;
}

server::builder& server::builder::per_connection_inflight(std::size_t count) {
    options_.per_connection_inflight = count;
    return *this;
}

server::builder& server::builder::per_service_inflight(std::size_t count) {
    options_.per_service_inflight = count;
    return *this;
}

server::builder& server::builder::drain_timeout(std::chrono::milliseconds value) {
    options_.drain_timeout = value;
    return *this;
}

server::builder& server::builder::overload_retry_after(std::chrono::milliseconds value) {
    options_.overload_retry_after = value;
    return *this;
}

server::builder& server::builder::breaker(resilience::breaker_config config) {
    options_.breaker = config;
    return *this;
}

server::builder& server::builder::per_connection_rate(
    resilience::rate_limit_config config) {
    options_.per_connection_rate = config;
    return *this;
}

server::builder& server::builder::per_service_rate(
    resilience::rate_limit_config config) {
    options_.per_service_rate = config;
    return *this;
}

server::builder& server::builder::per_service_bulkhead(
    resilience::bulkhead_config config) {
    options_.per_service_bulkhead = config;
    return *this;
}

server::builder& server::builder::resilience_enabled(bool enabled) {
    options_.resilience_enabled = enabled;
    return *this;
}

server::builder& server::builder::push_egress_max_bytes(std::size_t bytes) {
    options_.push_egress_max_bytes = bytes;
    return *this;
}

server::builder& server::builder::push_subscribe_options(
    push::subscribe_options options) {
    options_.push_subscribe = options;
    return *this;
}

std::unique_ptr<server> server::builder::build() {
    auto result = std::unique_ptr<server>(new server(options_));
    impl& state = *result->impl_;
    state.interceptors = interceptors_;
    for (auto& registration : registrations_) {
        const status registered = registration(state.registry);
        if (!registered.ok()) {
            return nullptr;
        }
    }
    state.dispatch = std::make_unique<rpc::dispatcher>(state.registry);
    state.opt.shards = options_.shards == 0 ? 1 : options_.shards;

    // Keep the listener configurations for start().
    state.listeners.clear();
    state.shards.clear();
    if (!listeners_.empty()) {
        result->impl_->pending_listeners = listeners_;
    }
    return result;
}

// --- server ------------------------------------------------------------------

server::server(options opt) : impl_(std::make_unique<impl>(std::move(opt))) {
    impl_->accept_sink_impl = std::make_unique<impl::acceptor>(impl_.get());
}

server::~server() {
    stop();
}

status server::start() {
    if (impl_->running.exchange(true, std::memory_order_acq_rel)) {
        return status{};
    }
    const status frozen = impl_->registry.freeze();
    if (!frozen.ok()) {
        impl_->running.store(false, std::memory_order_release);
        return frozen;
    }
    try {
        impl_->build_guards();
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        return status{rpc_code::resource_exhausted};
    }

    try {
        {
            std::lock_guard<std::recursive_mutex> lk(impl_->push_mutex);
            impl_->push_hub = std::make_unique<push::hub>(push::hub_options{});
            impl_->push_topic_ids.clear();
            impl_->next_push_topic_id = 1;
            impl_->next_push_stream_id = 1;
            impl_->push_egress_high_water.store(0, std::memory_order_relaxed);
            impl_->push_acks.store(0, std::memory_order_relaxed);
            impl_->refresh_push_stats_locked();
        }
        for (std::size_t i = 0; i < impl_->opt.shards; ++i) {
            runtime_options ro = impl_->opt.runtime;
            ro.start_handler_pool = true;
            auto shard = std::make_unique<runtime>(ro);
            shard->start_thread();
            impl_->shards.push_back(std::move(shard));
        }
        for (std::size_t i = 0; i < impl_->pending_listeners.size(); ++i) {
            const listener_config& config = impl_->pending_listeners[i];
            if (config.backend == nullptr) {
                continue;
            }
            std::unique_ptr<listener> bound =
                config.backend->listen(config.ep, listen_options{});
            if (!bound) {
                stop();
                return status{rpc_error(rpc_code::invalid_argument,
                                        "listener could not bind endpoint")};
            }
            impl_->listeners.push_back(std::move(bound));
        }
        impl_->accepting.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < impl_->listeners.size(); ++i) {
            impl_->listeners[i]->start(impl_->accept_sink_impl.get());
        }
    } catch (...) {
        stop();
        return status{rpc_code::resource_exhausted};
    }
    return status{};
}

void server::stop() noexcept {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        // Still close a partially started server.
        impl_->accepting.store(false, std::memory_order_release);
        impl_->close_all();
        for (std::size_t i = 0; i < impl_->shards.size(); ++i) {
            impl_->shards[i]->stop();
            impl_->shards[i]->join();
        }
        {
            std::lock_guard<std::recursive_mutex> lk(impl_->push_mutex);
            impl_->push_hub.reset();
            impl_->push_topic_ids.clear();
            impl_->push_acks.store(0, std::memory_order_relaxed);
            impl_->refresh_push_stats_locked();
        }
        // Every shard is joined: no handler can still hold a guard permit.
        impl_->clear_guards();
        return;
    }

    impl_->accepting.store(false, std::memory_order_release);
    for (std::size_t i = 0; i < impl_->listeners.size(); ++i) {
        if (impl_->listeners[i]) {
            impl_->listeners[i]->stop();
        }
    }
    impl_->listeners.clear();

    // New work is refused from this point; calls already admitted keep running
    // until the drain deadline. GOAWAY is sent after the drain because the
    // low-level client treats it as a terminal frame for every in-flight call.
    impl_->shutting_down.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lk(impl_->drain_mutex);
        impl_->drain_cv.wait_for(lk, impl_->opt.drain_timeout, [this] {
            return impl_->in_flight.load(std::memory_order_acquire) == 0;
        });
    }
    if (impl_->opt.send_goaway_on_stop) {
        impl_->send_goaway();
    }
    impl_->close_all();

    for (std::size_t i = 0; i < impl_->shards.size(); ++i) {
        impl_->shards[i]->stop();
    }
    for (std::size_t i = 0; i < impl_->shards.size(); ++i) {
        impl_->shards[i]->join();
    }
    impl_->shards.clear();
    // `pending_listeners` is builder configuration, not live state: keep it so
    // a later start() can bind the same listeners again. Clearing it made
    // start() return ok() after stop() while binding nothing. The stored
    // backend pointers are borrowed: the caller must keep every backend alive
    // for the whole server lifetime, including any stop()/start() cycle (see
    // server::builder::add_listener()).
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->push_mutex);
        impl_->push_hub.reset();
        impl_->push_topic_ids.clear();
        impl_->push_acks.store(0, std::memory_order_relaxed);
        impl_->refresh_push_stats_locked();
    }
    // The guard table is the last resilience object to go: every shard has
    // been joined and every admitted call has released its permits.
    impl_->clear_guards();
}

bool server::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

server::statistics server::stats() const noexcept {
    statistics out;
    {
        std::lock_guard<std::mutex> lk(impl_->connections_mutex);
        out.active_connections = impl_->connections.size();
        for (const std::shared_ptr<server_connection>& sc : impl_->connections) {
            out.push_egress_bytes +=
                sc->push_egress_bytes.load(std::memory_order_relaxed);
        }
    }
    out.in_flight_calls = impl_->in_flight.load(std::memory_order_acquire);
    out.accepted_connections = impl_->accepted.load(std::memory_order_relaxed);
    out.requests_handled = impl_->handled.load(std::memory_order_relaxed);
    out.rejected_overload = impl_->rejected.load(std::memory_order_relaxed);
    out.handler_failures = impl_->failures.load(std::memory_order_relaxed);
    out.rate_limited_connection =
        impl_->rate_limited_connection.load(std::memory_order_relaxed);
    out.rate_limited_service =
        impl_->rate_limited_service.load(std::memory_order_relaxed);
    out.bulkhead_rejected =
        impl_->bulkhead_rejected.load(std::memory_order_relaxed);
    out.breaker_open = impl_->breaker_open.load(std::memory_order_relaxed);
    out.deadline_rejected =
        impl_->deadline_rejected.load(std::memory_order_relaxed);
    out.probe_admitted = impl_->probe_admitted.load(std::memory_order_relaxed);
    out.push_active_connections = static_cast<std::size_t>(
        impl_->push_active_connections.load(std::memory_order_relaxed));
    out.push_queued_messages = static_cast<std::size_t>(
        impl_->push_queued_messages.load(std::memory_order_relaxed));
    out.push_queued_bytes = static_cast<std::size_t>(
        impl_->push_queued_bytes.load(std::memory_order_relaxed));
    out.push_egress_high_water =
        impl_->push_egress_high_water.load(std::memory_order_relaxed);
    out.push_dropped_overflow = static_cast<std::size_t>(
        impl_->push_overflow_dropped.load(std::memory_order_relaxed));
    out.push_acks = static_cast<std::size_t>(
        impl_->push_acks.load(std::memory_order_relaxed));
    return out;
}

bool server::publish(std::string_view topic, span<const std::byte> body,
                     push::delivery_mode mode) noexcept {
    if (topic.empty() || impl_->opt.shards != 1 ||
        !impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    try {
        std::vector<std::shared_ptr<server_connection>> snapshot;
        {
            std::lock_guard<std::mutex> lk(impl_->connections_mutex);
            snapshot = impl_->connections;
        }
        std::lock_guard<std::recursive_mutex> lk(impl_->push_mutex);
        if (!impl_->push_hub) {
            return false;
        }
        impl_->push_hub->publish(topic, body, mode);
        impl_->refresh_push_stats_locked();
        for (const std::shared_ptr<server_connection>& sc : snapshot) {
            if (!sc->closed.load(std::memory_order_acquire)) {
                impl_->drain_connection_locked(sc);
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace crpc