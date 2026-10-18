// CRPC -- server facade implementation.
#include <crpc/server.hpp>

#include <algorithm>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/endian.hpp>
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

// Owns the bytes of one outgoing frame until the backend completes the write.
struct send_op final : io::completion {
    std::unique_ptr<std::byte[]> bytes;
    std::size_t size = 0;
    std::shared_ptr<server_connection> keep_alive;

    void complete(io::io_result result) noexcept override {
        if (!result.ok() && keep_alive) {
            report_send_failure(keep_alive.get(), result.ec);
        }
        delete this;
    }
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

struct server_connection {
    server::impl* owner = nullptr;
    runtime* shard = nullptr;
    std::unique_ptr<connection> conn;
    frame_decoder decoder{max_frame_default};
    std::mutex decode_mutex;
    std::atomic<std::uint32_t> in_flight{0};
    std::atomic<bool> closed{false};
    std::weak_ptr<server_connection> self;

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

    std::unique_ptr<reader_source> source;

    server_connection() = default;

    void on_terminal() noexcept {
        closed.store(true, std::memory_order_release);
    }

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
    if (sc->conn) {
        sc->conn->close();
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

    std::mutex connections_mutex;
    std::vector<std::shared_ptr<server_connection>> connections;

    std::mutex drain_mutex;
    std::condition_variable drain_cv;

    std::mutex service_mutex;
    std::unordered_map<u64, std::size_t> service_in_flight;
    std::unordered_map<u64, std::unique_ptr<resilience::circuit_breaker>> breakers;

    struct acceptor final : accept_sink {
        explicit acceptor(impl* owner) noexcept : owner(owner) {}
        void on_accept(std::unique_ptr<connection> c) noexcept override {
            owner->on_accept(std::move(c));
        }
        impl* owner;
    };
    std::unique_ptr<acceptor> accept_sink_impl;

    explicit impl(options o) : opt(std::move(o)) {}

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

    void send_payload(const std::shared_ptr<server_connection>& sc, std::uint32_t stream_id,
                      frame_type type, span<const std::byte> payload) noexcept {
        if (sc->closed.load(std::memory_order_acquire) || !sc->conn) {
            return;
        }
        send_op* op = new (std::nothrow) send_op();
        if (op == nullptr) {
            return;
        }
        frame value;
        value.header.type = type;
        value.header.stream_id = stream_id;
        value.payload = payload;
        const std::size_t size = encoded_frame_size(value);
        op->bytes.reset(new (std::nothrow) std::byte[size]);
        if (!op->bytes) {
            delete op;
            return;
        }
        std::size_t written = 0;
        const status encoded =
            encode_frame(span<std::byte>(op->bytes.get(), size), value, written);
        if (!encoded.ok()) {
            delete op;
            return;
        }
        op->size = written;
        op->keep_alive = sc;
        sc->conn->sink().push(span<const std::byte>(op->bytes.get(), op->size), op);
    }

    void send_error(const std::shared_ptr<server_connection>& sc, std::uint32_t stream_id,
                    rpc_code code, std::chrono::milliseconds retry_after,
                    const char* message) noexcept {
        rpc_error error(code, message != nullptr ? std::string(message) : std::string());
        error.retryable = is_retryable(code);
        error.retry_after = retry_after;
        buffer payload;
        if (!rpc::encode_response(code, error, span<const std::byte>(), payload).ok()) {
            return;
        }
        send_payload(sc, stream_id, frame_type::response, payload.readable());
    }

    void handle_request(request_job& job) noexcept {
        const std::shared_ptr<server_connection>& sc = job.conn;
        const bool wants_response = job.header.type != frame_type::one_way;
        if (job.forced_error) {
            if (wants_response) {
                send_error(sc, job.header.stream_id, job.forced_code,
                           job.forced_retry_after, "overloaded");
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

        if (decoded && opt.per_service_inflight != 0) {
            bool over = false;
            {
                std::lock_guard<std::mutex> lk(service_mutex);
                std::size_t& count = service_in_flight[service_id];
                if (count >= opt.per_service_inflight) {
                    over = true;
                } else {
                    ++count;
                }
            }
            if (over) {
                rejected.fetch_add(1, std::memory_order_relaxed);
                if (wants_response) {
                    send_error(sc, job.header.stream_id, rpc_code::overload,
                               opt.overload_retry_after, "service concurrency limit reached");
                }
                complete_call(sc, job.counted);
                return;
            }
        } else if (decoded) {
            std::lock_guard<std::mutex> lk(service_mutex);
            ++service_in_flight[service_id];
        }

        resilience::circuit_breaker* breaker = nullptr;
        resilience::breaker_permit permit;
        if (decoded) {
            std::lock_guard<std::mutex> lk(service_mutex);
            std::unique_ptr<resilience::circuit_breaker>& slot = breakers[service_id];
            if (!slot) {
                slot.reset(new (std::nothrow) resilience::circuit_breaker(opt.breaker));
            }
            breaker = slot.get();
        }
        if (breaker != nullptr) {
            permit = breaker->allow();
            if (!permit.allowed()) {
                rejected.fetch_add(1, std::memory_order_relaxed);
                if (wants_response) {
                    send_error(sc, job.header.stream_id, rpc_code::unavailable,
                               permit.retry_after(), "service circuit breaker is open");
                }
                release_service(decoded, service_id);
                complete_call(sc, job.counted);
                return;
            }
        }

        buffer response;
        bool has_response = false;
        const status dispatched =
            dispatch->dispatch(request, response, has_response, steady_now());

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
        if (permit.allowed()) {
            if (outcome_code == rpc_code::ok) {
                permit.success();
            } else {
                permit.failure();
            }
        }
        if (!dispatched.ok()) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }

        if (has_response && wants_response) {
            send_payload(sc, job.header.stream_id, frame_type::response, response.readable());
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
        release_service(decoded, service_id);
        complete_call(sc, job.counted);
    }

    void release_service(bool decoded, u64 service_id) noexcept {
        if (!decoded) {
            return;
        }
        std::lock_guard<std::mutex> lk(service_mutex);
        auto it = service_in_flight.find(service_id);
        if (it != service_in_flight.end() && it->second > 0) {
            --it->second;
        }
    }

    static void run_job(void* ctx) noexcept {
        std::unique_ptr<request_job> job(static_cast<request_job*>(ctx));
        if (job->conn && job->conn->owner != nullptr) {
            job->conn->owner->handle_request(*job);
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
            send_payload(sc, 0, frame_type::goaway,
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
            if (sc->conn) {
                sc->conn->close();
            }
        }
    }
};

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
        }
    }
    return data.size();
}

void server_connection::submit(frame& request) noexcept {
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
                if (reject->conn && !shard->submit(&server::impl::run_job, reject)) {
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
    if (!job->conn || !shard->submit(&server::impl::run_job, job)) {
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
    impl_->pending_listeners.clear();
}

bool server::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

server::statistics server::stats() const noexcept {
    statistics out;
    {
        std::lock_guard<std::mutex> lk(impl_->connections_mutex);
        out.active_connections = impl_->connections.size();
    }
    out.in_flight_calls = impl_->in_flight.load(std::memory_order_acquire);
    out.accepted_connections = impl_->accepted.load(std::memory_order_relaxed);
    out.requests_handled = impl_->handled.load(std::memory_order_relaxed);
    out.rejected_overload = impl_->rejected.load(std::memory_order_relaxed);
    out.handler_failures = impl_->failures.load(std::memory_order_relaxed);
    return out;
}

} // namespace crpc
