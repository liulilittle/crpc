// CRPC -- HTTP server facade implementation.
//
// Layout of this translation unit:
//
//   1. small byte/string helpers;
//   2. `server_state` -- shards, listeners, the TLS context and the session
//      registry (shared by every session);
//   3. `session` -- one accepted connection: protocol sniffing, the
//      HTTP/1.1 engine, the HTTP/2 engine, the serialized write queue and the
//      shard hand-off;
//   4. the `http_server` facade.
//
// Everything a session owns that is not cross-thread state is touched on the
// owning shard only. The only cross-thread inputs are the source callbacks
// (ingress bytes, EOF/error) and write completions; both are published under
// `session::mu_` and processed by a task posted to the shard.
#include <crpc/http/server.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <crpc/http2/connection.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>
#include <crpc/mvc/router.hpp>

namespace crpc {
namespace http {
namespace {

namespace h2 = crpc::http2;

// The HTTP/2 connection preface sent by a client before its first SETTINGS
// frame. The same bytes are used for h2c and for h2 over TLS (RFC 9113 3.4).
constexpr std::string_view h2_connection_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool ci_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

// True when a comma-separated header value contains `token` (case-insensitive).
// Used for `Connection: close` and `Connection: keep-alive`.
[[nodiscard]] bool header_has_token(const header_list& headers,
                                    std::string_view name,
                                    std::string_view token) noexcept {
    for (const header& field : headers) {
        if (!ci_equal(field.name, name)) {
            continue;
        }
        std::string_view value(field.value);
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = value.find(',', start);
            std::string_view item =
                comma == std::string_view::npos
                    ? value.substr(start)
                    : value.substr(start, comma - start);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
                item.remove_prefix(1);
            }
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
                item.remove_suffix(1);
            }
            if (ci_equal(item, token)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                return false;
            }
            start = comma + 1;
        }
    }
    return false;
}

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(ascii_lower(c));
    }
    return out;
}

[[nodiscard]] std::string_view local_reason(int status_code) noexcept {
    switch (status_code) {
    case 411:
        return "Length Required";
    case 431:
        return "Request Header Fields Too Large";
    default:
        return mvc::reason_phrase(status_code);
    }
}

[[nodiscard]] http::response make_problem(int status_code, std::string_view detail,
                                          std::string_view instance) {
    http::response response;
    response.status_code = status_code;
    response.reason = std::string(local_reason(status_code));
    response.headers.add("Content-Type", "application/problem+json");
    std::string body = "{\"status\":";
    body += std::to_string(status_code);
    body += ",\"detail\":\"";
    for (const char c : detail) {
        if (c == '"' || c == '\\') {
            body.push_back('\\');
        }
        body.push_back(c);
    }
    body += "\",\"instance\":\"";
    for (const char c : instance) {
        if (c == '"' || c == '\\') {
            body.push_back('\\');
        }
        body.push_back(c);
    }
    body += "\"}";
    response.body = std::move(body);
    return response;
}

// PUT and PATCH carry a representation and must be framed. A POST without a
// body is meaningful (an empty trigger), so it is routed rather than rejected;
// the 411 path exists for the methods that cannot infer an empty body.
[[nodiscard]] bool method_requires_framing(http::method value) noexcept {
    return value == http::method::put || value == http::method::patch;
}

[[nodiscard]] bool response_forbids_body(int status_code) noexcept {
    return (status_code >= 100 && status_code < 200) || status_code == 204 ||
           status_code == 304;
}

struct server_state;

// --- Session -----------------------------------------------------------------

class session;

// One transport source. It holds only a weak reference: the session may be
// destroyed while a backend still has the adapter installed, and the adapter
// must never touch a dead session. While a callback runs it holds a strong
// reference, so the session cannot be destroyed underneath it.
class reader_source final : public io::source {
public:
    explicit reader_source(std::weak_ptr<session> owner) noexcept
        : owner_(std::move(owner)) {}

    std::size_t on_data(span<const std::byte> data) noexcept override;
    void on_eof() noexcept override;
    void on_error(std::error_code ec) noexcept override;

private:
    std::weak_ptr<session> owner_;
};

// One outstanding write. When it completes it publishes the result to the
// session (a lock-protected queue) and schedules shard work; it never starts
// another push directly (R1/I3).
struct write_completion final : io::completion {
    std::shared_ptr<session> owner;

    void complete(io::io_result result) noexcept override;
};

struct pending_h2_stream {
    http::request request;
    bool headers_ready = false;
    // Absolute monotonic deadline for the request body (0 = none). Set when
    // HEADERS arrive; an open stream that never reaches END_STREAM is reset at
    // this deadline instead of being bounded only by the connection idle timer,
    // which every trickled byte refreshes (No-stall rule).
    u64 deadline_ns = 0;
};

// The request-processing engine of one connection.
class session final : public std::enable_shared_from_this<session> {
public:
    session(server_state& state, std::size_t shard_index,
            std::unique_ptr<connection> transport, bool tls_mode);
    ~session();

    session(const session&) = delete;
    session& operator=(const session&) = delete;

    // Installs the sources. Called once, after the owning shared_ptr exists.
    void start();

    // Called by `http_server::stop()` (any thread).
    void request_shutdown() noexcept;
    // Called by `http_server::stop()` after the drain deadline (any thread).
    void force_close() noexcept;

    // Source callbacks.
    std::size_t handle_data(span<const std::byte> data) noexcept;
    void handle_eof() noexcept;
    void handle_error(std::error_code ec) noexcept;

    // Write completion (any thread).
    void note_write_complete(bool ok) noexcept;

    [[nodiscard]] runtime* shard() const noexcept { return shard_; }

private:
    enum class phase : u8 { detect, http1, h2_preface, h2 };

    // Shard-thread processing.
    void process();
    void consume(std::string input);
    void process_http1(std::string input);
    void handle_http1_request(http::request request);
    void process_h2(std::string input);
    void after_h2_receive(const h2::frame_header& frame, std::string data);
    void build_h2_request(u32 stream_id, const header_list& fields);
    void dispatch_h2(u32 stream_id);
    void send_h2_response(u32 stream_id, http::response& response, bool head_only);
    void h2_flush(u32 stream_id);
    void h2_resume_parked();
    void h2_send_initial_settings();
    void h2_send_settings_ack();
    void h2_send_ping_ack();
    void h2_send_ping();
    void h2_send_goaway(h2::error_code code);
    void h2_send_rst(u32 stream_id, h2::error_code code);
    void h2_send_window_update(u32 stream_id, u32 increment);
    void h2_connection_error(h2::error_code code);

    // Output.
    void enqueue(std::string bytes);
    void flush_writes();
    void maybe_finish();
    void finish_now();
    void schedule() noexcept;
    // Arms the single session timer for the nearest of: an HTTP/2 parked
    // response probe, the idle deadline, and the request deadline. One timer
    // per session because `runtime::arm_timer` is keyed by the session address.
    void arm_probe();
    void on_probe();
    void note_activity() noexcept;
    void note_request_progress() noexcept;

    // HTTP/1.1 helpers.
    [[nodiscard]] bool keep_alive(const http::request& request) const noexcept;
    void send_http1_error(int status_code, std::string_view detail,
                          const http::request& request, bool keep);

    // Completion publishing helper (called under `mu_`).
    void queue_write_complete(bool ok) noexcept;

    static void process_trampoline(void* ctx) noexcept;
    static void probe_trampoline(void* ctx) noexcept;
    static void arm_trampoline(void* ctx) noexcept;

    server_state& state_;
    runtime* shard_ = nullptr;
    std::size_t shard_index_ = 0;
    bool tls_mode_ = false;

    std::unique_ptr<connection> transport_;
    std::unique_ptr<io::tls_stream> tls_;
    std::unique_ptr<reader_source> source_;
    io::sink* out_sink_ = nullptr;

    // Cross-thread state.
    std::mutex mu_;
    std::string ingress_;
    bool read_paused_ = false;
    bool input_closed_ = false;
    std::deque<bool> write_completions_;

    // Shard-only state.
    phase phase_ = phase::detect;
    std::string detect_buf_;
    http::http_parser parser_;
    h2::connection h2_;
    std::string h2_buf_;
    std::unordered_map<u32, pending_h2_stream> h2_streams_;
    std::unordered_set<u32> h2_end_pending_;
    u32 h2_last_peer_stream_ = 0;
    // Last RST_STREAM bookkeeping value we already accounted for. A locally
    // generated RST also updates the connection's `last_reset_stream`, so this
    // prevents the inbound path from re-processing our own reset.
    u32 h2_last_reset_seen_ = 0;
    bool h2_goaway_sent_ = false;
    bool h2_settings_sent_ = false;
    bool probe_armed_ = false;
    // Absolute monotonic deadlines in nanoseconds (0 = not armed). The idle
    // deadline bounds a connection that sends nothing; the request deadline
    // bounds an incomplete request (slowloris, No-stall rule).
    u64 idle_deadline_ns_ = 0;
    u64 request_deadline_ns_ = 0;

    std::deque<std::string> out_;
    std::size_t out_bytes_ = 0;
    bool write_in_flight_ = false;
    std::string in_flight_;
    bool write_failed_ = false;
    bool sent_100_ = false;
    bool stop_reading_ = false;
    bool close_when_drained_ = false;
    std::atomic<bool> shutdown_requested_{false};
    bool finished_ = false;
};

// --- Server state ------------------------------------------------------------

struct server_state {
    http_server_options options;
    const mvc::router* router = nullptr;

    std::vector<std::unique_ptr<runtime>> shards;
    std::atomic<std::size_t> next_shard{0};

    std::atomic<bool> accepting{false};

    // Statistics.
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> handled{0};
    std::atomic<std::size_t> responded{0};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> h2_connections{0};
    std::atomic<std::size_t> tls_connections{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> timed_out{0};

    struct pending_listener {
        endpoint ep;
        backend_factory* backend = nullptr;
        bool tls = false;
    };
    std::vector<pending_listener> pending_listeners;
    std::vector<std::unique_ptr<listener>> listeners;

    bool has_tls = false;
    io::tls_config tls_config{};
    std::unique_ptr<io::tls_context> tls_context;

    std::mutex sessions_mutex;
    std::vector<std::shared_ptr<session>> sessions;
    std::condition_variable drain_cv;

    // One acceptor per listener so the session knows whether its transport
    // speaks TLS. A single acceptor cannot tell which listener accepted.
    struct acceptor final : accept_sink {
        acceptor(server_state& owner, bool tls) noexcept : owner_(owner), tls_(tls) {}
        void on_accept(std::unique_ptr<connection> c) noexcept override {
            owner_.on_accept(std::move(c), tls_);
        }
        server_state& owner_;
        bool tls_;
    };
    std::vector<std::unique_ptr<acceptor>> acceptors;

    explicit server_state(http_server_options opt) : options(std::move(opt)) {}

    void on_accept(std::unique_ptr<connection> transport, bool use_tls) noexcept {
        if (transport == nullptr) {
            return;
        }
        if (!accepting.load(std::memory_order_acquire) || shards.empty() ||
            (use_tls && tls_context == nullptr)) {
            transport->close();
            return;
        }
        try {
            const std::size_t index = static_cast<std::size_t>(
                next_shard.fetch_add(1, std::memory_order_relaxed)) %
                shards.size();
            auto value = std::make_shared<session>(*this, index, std::move(transport),
                                                   use_tls);
            {
                std::lock_guard<std::mutex> lk(sessions_mutex);
                // The bound is checked and the session inserted under one lock,
                // so concurrent accepts cannot overshoot it.
                if (options.max_connections != 0 &&
                    sessions.size() >= options.max_connections) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                    value.reset();
                } else {
                    sessions.push_back(value);
                }
            }
            if (!value) {
                return;
            }
            accepted.fetch_add(1, std::memory_order_relaxed);
            if (use_tls) {
                tls_connections.fetch_add(1, std::memory_order_relaxed);
            }
            value->start();
        } catch (...) {
            // Allocation failure: the transport is closed by the destructor.
        }
    }

    void remove_session(const std::shared_ptr<session>& value) noexcept {
        std::lock_guard<std::mutex> lk(sessions_mutex);
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            if (it->get() == value.get()) {
                sessions.erase(it);
                break;
            }
        }
        drain_cv.notify_all();
    }

    [[nodiscard]] std::vector<std::shared_ptr<session>> snapshot() {
        std::lock_guard<std::mutex> lk(sessions_mutex);
        return sessions;
    }

    [[nodiscard]] std::size_t active_sessions() {
        std::lock_guard<std::mutex> lk(sessions_mutex);
        return sessions.size();
    }
};

// --- reader_source / write_completion ---------------------------------------

std::size_t reader_source::on_data(span<const std::byte> data) noexcept {
    std::shared_ptr<session> owner = owner_.lock();
    if (!owner) {
        return data.size();
    }
    return owner->handle_data(data);
}

void reader_source::on_eof() noexcept {
    std::shared_ptr<session> owner = owner_.lock();
    if (owner) {
        owner->handle_eof();
    }
}

void reader_source::on_error(std::error_code ec) noexcept {
    std::shared_ptr<session> owner = owner_.lock();
    if (owner) {
        owner->handle_error(ec);
    }
}

void write_completion::complete(io::io_result result) noexcept {
    if (owner) {
        owner->note_write_complete(result.ok());
    }
    delete this;
}

// --- session: construction and sources --------------------------------------

session::session(server_state& state, std::size_t shard_index,
                 std::unique_ptr<connection> transport, bool tls_mode)
    : state_(state),
      shard_(shard_index < state.shards.size() ? state.shards[shard_index].get()
                                               : nullptr),
      shard_index_(shard_index),
      tls_mode_(tls_mode),
      transport_(std::move(transport)),
      parser_(http::http_parser::kind::request, state.options.limits),
      h2_([&state] {
          h2::connection_options options;
          options.role = h2::connection_role::server;
          options.local_settings.max_concurrent_streams =
              state.options.h2_max_concurrent_streams;
          options.local_settings.initial_window_size =
              state.options.h2_initial_window_size;
          options.local_settings.max_frame_size = state.options.h2_max_frame_size;
          options.local_settings.max_header_list_size =
              state.options.h2_max_header_list_size;
          options.max_send_queue_bytes = state.options.h2_max_send_queue_bytes;
          return options;
      }()) {}

session::~session() {
    if (tls_) {
        tls_->close();
    }
    if (transport_) {
        transport_->bind_source(nullptr);
        transport_->close();
    }
}

void session::start() {
    source_ = std::make_unique<reader_source>(weak_from_this());
    if (shard_ == nullptr) {
        return;
    }
    if (tls_mode_) {
        if (state_.tls_context == nullptr) {
            return;
        }
        std::error_code ec;
        tls_ = io::tls_stream::create(transport_->sink(), *state_.tls_context,
                                      std::string(), ec);
        if (!tls_) {
            transport_->close();
            return;
        }
        out_sink_ = tls_.get();
        tls_->bind_source(source_.get());
        transport_->bind_source(tls_.get());
    } else {
        out_sink_ = &transport_->sink();
        transport_->bind_source(source_.get());
    }
    // The idle bound is armed on the owning shard: session deadlines are
    // shard-only state, and start() runs on the accept thread.
    std::shared_ptr<session> self = weak_from_this().lock();
    if (self) {
        auto* holder = new (std::nothrow) std::shared_ptr<session>(std::move(self));
        if (holder != nullptr && !shard_->post(&session::arm_trampoline, holder)) {
            delete holder;
        }
    }
}

void session::request_shutdown() noexcept {
    shutdown_requested_.store(true, std::memory_order_release);
    schedule();
}

void session::force_close() noexcept {
    // Thread-safe hard stop: close cancels delivery and any in-flight write.
    // Any armed probe timer is invalidated by the caller retaining the session
    // until the shards are joined (see http_server::stop).
    if (shard_ != nullptr) {
        (void)shard_->cancel_timer(this);
    }
    if (transport_) {
        transport_->close();
    }
}

std::size_t session::handle_data(span<const std::byte> data) noexcept {
    std::size_t accepted = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (input_closed_) {
            return 0;
        }
        if (read_paused_) {
            return 0;
        }
        const std::size_t cap = state_.options.ingress_cap_bytes;
        const std::size_t room = cap > ingress_.size() ? cap - ingress_.size() : 0;
        accepted = std::min(room, data.size());
        ingress_.append(reinterpret_cast<const char*>(data.data()), accepted);
        if (accepted < data.size()) {
            read_paused_ = true;
        }
    }
    if (accepted != 0) {
        schedule();
    }
    return accepted;
}

void session::handle_eof() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        input_closed_ = true;
    }
    schedule();
}

void session::handle_error(std::error_code ec) noexcept {
    (void)ec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        input_closed_ = true;
    }
    schedule();
}

void session::note_write_complete(bool ok) noexcept {
    queue_write_complete(ok);
    schedule();
}

void session::queue_write_complete(bool ok) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    write_completions_.push_back(ok);
}

// --- session: scheduling -----------------------------------------------------

void session::schedule() noexcept {
    std::shared_ptr<session> self = weak_from_this().lock();
    if (!self) {
        return;
    }
    if (shard_ == nullptr) {
        return;
    }
    auto* holder = new (std::nothrow) std::shared_ptr<session>(std::move(self));
    if (holder == nullptr) {
        return;
    }
    if (!shard_->post(&session::process_trampoline, holder)) {
        delete holder;
    }
}

void session::process_trampoline(void* ctx) noexcept {
    std::unique_ptr<std::shared_ptr<session>> holder(
        static_cast<std::shared_ptr<session>*>(ctx));
    (*holder)->process();
}

void session::probe_trampoline(void* ctx) noexcept {
    auto* self = static_cast<session*>(ctx);
    self->on_probe();
}

void session::arm_trampoline(void* ctx) noexcept {
    std::unique_ptr<std::shared_ptr<session>> holder(
        static_cast<std::shared_ptr<session>*>(ctx));
    session& self = **holder;
    self.note_activity();
    self.arm_probe();
}

void session::process() {
    if (finished_) {
        return;
    }
    for (;;) {
        std::string input;
        std::deque<bool> completions;
        {
            std::lock_guard<std::mutex> lk(mu_);
            input.swap(ingress_);
            completions.swap(write_completions_);
        }
        for (const bool ok : completions) {
            write_in_flight_ = false;
            if (!ok) {
                write_failed_ = true;
                close_when_drained_ = true;
                stop_reading_ = true;
            }
        }
        if (!input.empty()) {
            note_activity();
            consume(std::move(input));
            arm_probe();
        }
        const bool shutdown =
            shutdown_requested_.load(std::memory_order_acquire);
        if (shutdown) {
            stop_reading_ = true;
            if (phase_ == phase::h2) {
                h2_send_goaway(h2::error_code::no_error);
            }
            close_when_drained_ = true;
        }
        flush_writes();
        maybe_finish();
        if (finished_) {
            return;
        }
        bool resume = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (read_paused_ && ingress_.empty() && !close_when_drained_ &&
                !stop_reading_ &&
                out_bytes_ <= state_.options.egress_cap_bytes) {
                read_paused_ = false;
                resume = true;
            }
            if (ingress_.empty()) {
                break;
            }
        }
        if (resume) {
            if (tls_) {
                tls_->resume_read();
            }
            if (transport_) {
                transport_->resume_read();
            }
        }
    }
}

// --- session: protocol sniffing ---------------------------------------------

void session::consume(std::string input) {
    if (phase_ == phase::detect) {
        if (tls_mode_) {
            if (tls_ == nullptr) {
                close_when_drained_ = true;
                stop_reading_ = true;
                return;
            }
            if (!tls_->handshake_complete()) {
                // Plaintext cannot arrive before the handshake completes; keep
                // the bytes only to be safe against a decorator bug.
                detect_buf_.append(input);
                return;
            }
            const std::string alpn = tls_->alpn_protocol();
            if (alpn == "h2") {
                phase_ = phase::h2_preface;
            } else if (alpn.empty() || alpn == "http/1.1") {
                phase_ = phase::http1;
            } else {
                close_when_drained_ = true;
                stop_reading_ = true;
                return;
            }
            detect_buf_.append(input);
            input = std::move(detect_buf_);
            detect_buf_.clear();
        } else {
            detect_buf_.append(input);
            const std::size_t compare =
                std::min(detect_buf_.size(), h2_connection_preface.size());
            const bool prefix_ok =
                std::memcmp(detect_buf_.data(), h2_connection_preface.data(),
                            compare) == 0;
            if (!prefix_ok) {
                phase_ = phase::http1;
                input = std::move(detect_buf_);
                detect_buf_.clear();
            } else if (detect_buf_.size() >= h2_connection_preface.size()) {
                phase_ = phase::h2_preface;
                input = std::move(detect_buf_);
                detect_buf_.clear();
            } else {
                return;
            }
        }
    }

    if (phase_ == phase::http1) {
        process_http1(std::move(input));
        return;
    }

    if (phase_ == phase::h2_preface) {
        h2_buf_.append(input);
        if (h2_buf_.size() < h2_connection_preface.size()) {
            return;
        }
        if (std::memcmp(h2_buf_.data(), h2_connection_preface.data(),
                        h2_connection_preface.size()) != 0) {
            h2_connection_error(h2::error_code::protocol_error);
            return;
        }
        h2_buf_.erase(0, h2_connection_preface.size());
        phase_ = phase::h2;
        state_.h2_connections.fetch_add(1, std::memory_order_relaxed);
        std::string rest = std::move(h2_buf_);
        h2_buf_.clear();
        h2_send_initial_settings();
        if (!rest.empty()) {
            process_h2(std::move(rest));
        }
        return;
    }

    if (phase_ == phase::h2) {
        process_h2(std::move(input));
    }
}

// --- session: HTTP/1.1 -------------------------------------------------------

bool session::keep_alive(const http::request& request) const noexcept {
    const bool wants_close =
        header_has_token(request.headers, "connection", "close");
    if (request.version == http_version::http_1_0) {
        return !wants_close &&
               header_has_token(request.headers, "connection", "keep-alive");
    }
    return !wants_close;
}

void session::send_http1_error(int status_code, std::string_view detail,
                               const http::request& request, bool keep) {
    http::response response =
        make_problem(status_code, detail, request.path);
    response.version = request.version;
    if (!keep) {
        response.headers.add("Connection", "close");
    }
    std::string wire;
    if (!http::serialize(response, wire).ok()) {
        wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n";
        keep = false;
    }
    enqueue(std::move(wire));
    state_.responded.fetch_add(1, std::memory_order_relaxed);
    if (!keep) {
        close_when_drained_ = true;
        stop_reading_ = true;
    }
    flush_writes();
}

void session::process_http1(std::string input) {
    http::parse_status status =
        parser_.feed(as_bytes(input));
    for (;;) {
        if (status == http::parse_status::need_more) {
            // A request that has started but not completed is bounded by
            // `request_timeout` (slowloris, No-stall rule).
            note_request_progress();
            return;
        }
        if (status == http::parse_status::headers_complete) {
            note_request_progress();
            if (parser_.expect_100_continue() && !sent_100_) {
                sent_100_ = true;
                enqueue("HTTP/1.1 100 Continue\r\n\r\n");
                flush_writes();
            }
            return;
        }
        if (status == http::parse_status::error) {
            const rpc_error& error = parser_.error();
            int code = 400;
            if (error.code == rpc_code::resource_exhausted) {
                // The message layer names the limit in the error text; over a
                // header limit is 431, over the body limit is 413, and an over
                // long request line is a plain 400 (no 414 in this facade).
                if (error.message.find("header") != std::string::npos) {
                    code = 431;
                } else if (error.message.find("body") != std::string::npos) {
                    code = 413;
                }
            }
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_http1_error(code, error.message, parser_.get_request(), false);
            return;
        }

        // message_complete
        http::request request = parser_.get_request();
        const span<const std::byte> rest = parser_.pending();
        std::string pending(reinterpret_cast<const char*>(rest.data()),
                            rest.size());
        parser_.reset();
        sent_100_ = false;
        request_deadline_ns_ = 0;

        handle_http1_request(std::move(request));
        if (stop_reading_ || pending.empty()) {
            return;
        }
        // Hard egress bound (H-4): a batch of pipelined requests must not be
        // answered into an unbounded write queue. Once the queue is over the
        // cap, the unparsed bytes go back to the ingress buffer and the reader
        // stays paused until the queue drains. Nothing is dropped: the bytes
        // are bounded by `ingress_cap_bytes` and are parsed on a later turn.
        if (state_.options.egress_cap_bytes != 0 &&
            out_bytes_ > state_.options.egress_cap_bytes) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ingress_.insert(0, pending);
                read_paused_ = true;
            }
            return;
        }
        status = parser_.feed(as_bytes(pending));
    }
}

void session::handle_http1_request(http::request request) {
    const bool head_only = request.method == http::method::head;
    const bool keep = keep_alive(request);

    if (request.version == http_version::http_1_1 &&
        !request.headers.contains("host")) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_http1_error(400, "HTTP/1.1 requires a Host header", request, false);
        return;
    }

    const bool framed = request.headers.contains("content-length") ||
                        request.headers.contains("transfer-encoding");
    if (method_requires_framing(request.method) && !framed &&
        request.body.empty()) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_http1_error(411, "Length Required: send Content-Length or a chunked body",
                         request, keep);
        if (!keep) {
            return;
        }
    } else {
        http::request routed = std::move(request);
        if (head_only) {
            routed.method = http::method::get;
            routed.method_token = "GET";
        }
        http::response response;
        if (state_.router != nullptr) {
            (void)state_.router->handle(routed, response);
        } else {
            response = make_problem(404, "no router configured", routed.path);
        }
        response.version = routed.version;
        if (!keep) {
            response.headers.add("Connection", "close");
        } else if (routed.version == http_version::http_1_0) {
            response.headers.add("Connection", "keep-alive");
        }
        http::serialize_options options;
        options.include_body = !head_only;
        std::string wire;
        if (!http::serialize(response, wire, options).ok()) {
            wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n";
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        enqueue(std::move(wire));
        state_.handled.fetch_add(1, std::memory_order_relaxed);
        state_.responded.fetch_add(1, std::memory_order_relaxed);
        if (!keep) {
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        flush_writes();
        if (stop_reading_) {
            return;
        }
    }
}

// --- session: HTTP/2 ---------------------------------------------------------

void session::process_h2(std::string input) {
    h2_buf_.append(input);
    for (;;) {
        if (h2_buf_.size() < h2::frame_header_size) {
            return;
        }
        const span<const std::byte> head(
            reinterpret_cast<const std::byte*>(h2_buf_.data()),
            h2::frame_header_size);
        h2::frame_header frame;
        const status decoded = h2::decode_frame_header(
            head, frame, state_.options.h2_max_frame_size);
        if (!decoded.ok()) {
            h2_connection_error(h2::error_code::frame_size_error);
            return;
        }
        const std::size_t total = h2::frame_header_size + frame.length;
        if (h2_buf_.size() < total) {
            return;
        }
        const std::byte* payload =
            reinterpret_cast<const std::byte*>(h2_buf_.data()) + h2::frame_header_size;
        const h2::h2_error error = h2_.receive(
            frame, span<const std::byte>(payload, frame.length));

        // `last_data()` borrows the receive payload, which points into
        // `h2_buf_`. Copy it before erasing the buffer.
        std::string data;
        if (frame.type == h2::frame_type::data) {
            const span<const std::byte> body = h2_.last_data();
            data.assign(reinterpret_cast<const char*>(body.data()), body.size());
        }
        h2_buf_.erase(0, total);

        if (!error.ok()) {
            if (error.scope == h2::error_scope::connection) {
                h2_connection_error(error.code);
                return;
            }
            h2_send_rst(error.stream_id, error.code);
            h2_streams_.erase(error.stream_id);
            h2_end_pending_.erase(error.stream_id);
            continue;
        }

        if (frame.type == h2::frame_type::headers ||
            frame.type == h2::frame_type::continuation) {
            h2_last_peer_stream_ = frame.stream_id;
        }
        after_h2_receive(frame, std::move(data));
        if (stop_reading_) {
            return;
        }
    }
}

void session::after_h2_receive(const h2::frame_header& frame, std::string data) {
    if (h2_.pending_settings_ack()) {
        h2_send_settings_ack();
        h2_.settings_acknowledge();
    }
    if (h2_.pending_ping_ack()) {
        h2_send_ping_ack();
        h2_.ping_acknowledge();
    }

    if (h2_.last_headers_present()) {
        const u32 stream_id = h2_.last_headers_stream();
        const http::header_list fields = h2_.last_headers();
        h2_.clear_last_headers();
        build_h2_request(stream_id, fields);
        if (h2_.snapshot(stream_id).remote_end_stream) {
            dispatch_h2(stream_id);
        }
    }

    if (frame.type == h2::frame_type::data) {
        const u32 stream_id = frame.stream_id;
        auto it = h2_streams_.find(stream_id);
        if (it != h2_streams_.end() && it->second.headers_ready) {
            if (it->second.request.body.size() + data.size() >
                state_.options.limits.body_bytes) {
                h2_send_rst(stream_id, h2::error_code::enhance_your_calm);
                h2_streams_.erase(it);
                h2_end_pending_.erase(stream_id);
            } else if (!data.empty()) {
                it->second.request.body.append(data);
            }
        }
        // We consumed the data immediately; advertise the restored windows.
        const h2::window_credit credit =
            h2_.release_received(stream_id, frame.length);
        if (credit.connection != 0) {
            h2_send_window_update(0, credit.connection);
        }
        if (credit.stream != 0) {
            h2_send_window_update(stream_id, credit.stream);
        }
        if (h2_.snapshot(stream_id).remote_end_stream) {
            dispatch_h2(stream_id);
        }
    }

    if (h2_.last_reset_stream() != 0 &&
        h2_.last_reset_stream() != h2_last_reset_seen_) {
        const u32 stream_id = h2_.last_reset_stream();
        h2_last_reset_seen_ = stream_id;
        h2_streams_.erase(stream_id);
        h2_end_pending_.erase(stream_id);
    }

    if (h2_.goaway_received()) {
        if (!h2_goaway_sent_) {
            h2_send_goaway(h2::error_code::no_error);
        }
        close_when_drained_ = true;
    }

    if (h2_.has_parked()) {
        h2_resume_parked();
    }
}

void session::build_h2_request(u32 stream_id, const header_list& fields) {
    http::request request;
    request.version = http_version::http_1_1;
    bool have_method = false;
    bool have_path = false;
    bool have_scheme = false;
    bool bad_pseudo = false;

    for (const header& field : fields) {
        if (!field.name.empty() && field.name.front() == ':') {
            if (field.name == ":method") {
                request.method_token = field.value;
                request.method = http::method_from_token(field.value);
                have_method = true;
            } else if (field.name == ":path") {
                request.target = field.value;
                have_path = true;
            } else if (field.name == ":scheme") {
                have_scheme = true;
            } else if (field.name == ":authority") {
                request.headers.add("host", field.value);
            } else {
                bad_pseudo = true;
            }
        } else {
            request.headers.add(lowercase(field.name), field.value);
        }
    }

    http::target_form form = http::target_form::origin;
    const status split =
        have_path ? http::split_target(request.target, form, request.path,
                                       request.query)
                  : status{rpc_error(rpc_code::protocol_error)};
    if (bad_pseudo || !have_method || !have_path || !have_scheme || !split.ok()) {
        h2_send_rst(stream_id, h2::error_code::protocol_error);
        h2_streams_.erase(stream_id);
        return;
    }

    pending_h2_stream stream;
    stream.headers_ready = true;
    stream.request = std::move(request);
    if (shard_ != nullptr && state_.options.request_timeout.count() > 0) {
        stream.deadline_ns =
            shard_->now_ns() +
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 state_.options.request_timeout)
                                 .count());
    }
    h2_streams_[stream_id] = std::move(stream);
}

void session::dispatch_h2(u32 stream_id) {
    auto it = h2_streams_.find(stream_id);
    if (it == h2_streams_.end() || !it->second.headers_ready) {
        return;
    }
    const bool head_only = it->second.request.method == http::method::head;
    http::request routed = std::move(it->second.request);
    h2_streams_.erase(it);
    if (head_only) {
        routed.method = http::method::get;
        routed.method_token = "GET";
    }
    http::response response;
    if (state_.router != nullptr) {
        (void)state_.router->handle(routed, response);
    } else {
        response = make_problem(404, "no router configured", routed.path);
    }
    state_.handled.fetch_add(1, std::memory_order_relaxed);
    send_h2_response(stream_id, response, head_only);
}

void session::send_h2_response(u32 stream_id, http::response& response,
                               bool head_only) {
    if (h2_.state_of(stream_id) == h2::stream_state::closed) {
        return;
    }
    std::vector<h2::header_field> fields;
    fields.push_back({":status", std::to_string(response.status_code)});
    bool have_content_length = false;
    for (const header& field : response.headers) {
        const std::string name = lowercase(field.name);
        // Connection-specific fields are forbidden in HTTP/2 (RFC 9113 8.2.2).
        if (name == "connection" || name == "keep-alive" ||
            name == "transfer-encoding" || name == "upgrade" ||
            name == "proxy-connection") {
            continue;
        }
        if (name == "content-length") {
            have_content_length = true;
        }
        fields.push_back({name, field.value});
    }
    const bool body_forbidden = response_forbids_body(response.status_code);
    const bool send_body = !head_only && !body_forbidden && !response.body.empty();
    if (!body_forbidden && !have_content_length) {
        fields.push_back({"content-length",
                          std::to_string(response.body.size())});
    }

    std::string block;
    const status encoded = h2_.encoder().encode(
        span<const h2::header_field>(fields.data(), fields.size()), block);
    if (!encoded.ok()) {
        h2_send_rst(stream_id, h2::error_code::internal_error);
        return;
    }

    const std::size_t max_frame = h2_.remote_settings().max_frame_size;
    const bool end_stream = !send_body;
    std::size_t offset = 0;
    h2::frame_header header;
    header.type = h2::frame_type::headers;
    header.flags = static_cast<u8>(
        h2::flags::end_headers |
        (end_stream ? h2::flags::end_stream : static_cast<u8>(0)));
    header.stream_id = stream_id;
    const std::size_t first = std::min(block.size(), max_frame);
    header.length = static_cast<u32>(first);
    std::string header_frame(h2::frame_header_size + first, '\0');
    std::size_t written = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(header_frame.data()),
                        header_frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(block.data()), first),
        written);
    enqueue(std::move(header_frame));
    offset = first;
    while (offset < block.size()) {
        const std::size_t chunk = std::min(block.size() - offset, max_frame);
        const bool last = offset + chunk == block.size();
        h2::frame_header continuation;
        continuation.type = h2::frame_type::continuation;
        continuation.flags = last ? h2::flags::end_headers : static_cast<u8>(0);
        continuation.stream_id = stream_id;
        continuation.length = static_cast<u32>(chunk);
        std::string frame(h2::frame_header_size + chunk, '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
            continuation,
            span<const std::byte>(
                reinterpret_cast<const std::byte*>(block.data() + offset), chunk),
            count);
        enqueue(std::move(frame));
        offset += chunk;
    }

    if (!send_body) {
        (void)h2_.local_end_stream(stream_id);
        state_.responded.fetch_add(1, std::memory_order_relaxed);
        flush_writes();
        return;
    }

    std::size_t sent = 0;
    bool parked = false;
    const h2::h2_error error = h2_.send_data(
        stream_id, as_bytes(response.body), sent, parked);
    if (!error.ok()) {
        h2_send_rst(stream_id, error.code);
        return;
    }
    if (sent != 0) {
        h2::frame_header data_frame;
        data_frame.type = h2::frame_type::data;
        data_frame.flags = 0;
        data_frame.stream_id = stream_id;
        data_frame.length = static_cast<u32>(sent);
        std::string frame(h2::frame_header_size + sent, '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
            data_frame,
            span<const std::byte>(
                reinterpret_cast<const std::byte*>(response.body.data()), sent),
            count);
        enqueue(std::move(frame));
    }
    h2_end_pending_.insert(stream_id);
    state_.responded.fetch_add(1, std::memory_order_relaxed);
    h2_flush(stream_id);
    flush_writes();
}

void session::h2_flush(u32 stream_id) {
    for (;;) {
        std::vector<std::byte> out;
        bool flushed = false;
        const h2::h2_error error = h2_.flush_parked(stream_id, 0, out, flushed);
        if (!error.ok()) {
            h2_end_pending_.erase(stream_id);
            return;
        }
        if (!flushed || out.empty()) {
            break;
        }
        h2::frame_header header;
        header.type = h2::frame_type::data;
        header.flags = 0;
        header.stream_id = stream_id;
        header.length = static_cast<u32>(out.size());
        std::string frame(h2::frame_header_size + out.size(), '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
            header, span<const std::byte>(out.data(), out.size()), count);
        enqueue(std::move(frame));
    }

    if (h2_.parked_bytes(stream_id) != 0) {
        arm_probe();
        return;
    }
    if (h2_end_pending_.erase(stream_id) != 0) {
        const h2::h2_error ending = h2_.local_end_stream(stream_id);
        if (ending.ok()) {
            h2::frame_header header;
            header.type = h2::frame_type::data;
            header.flags = h2::flags::end_stream;
            header.stream_id = stream_id;
            header.length = 0;
            std::string frame(h2::frame_header_size, '\0');
            std::size_t count = 0;
            (void)h2::encode_frame(
                span<std::byte>(reinterpret_cast<std::byte*>(frame.data()),
                                frame.size()),
                header, span<const std::byte>(), count);
            enqueue(std::move(frame));
        }
    }
}

void session::h2_resume_parked() {
    const std::vector<u32> parked = h2_.parked_streams();
    for (const u32 stream_id : parked) {
        h2_flush(stream_id);
    }
    if (h2_.has_parked()) {
        arm_probe();
    }
    flush_writes();
}

void session::h2_send_initial_settings() {
    if (h2_settings_sent_) {
        return;
    }
    h2_settings_sent_ = true;
    h2::setting entries[5];
    entries[0].id = static_cast<u16>(h2::setting_id::max_concurrent_streams);
    entries[0].value = state_.options.h2_max_concurrent_streams;
    entries[1].id = static_cast<u16>(h2::setting_id::initial_window_size);
    entries[1].value = state_.options.h2_initial_window_size;
    entries[2].id = static_cast<u16>(h2::setting_id::max_frame_size);
    entries[2].value = state_.options.h2_max_frame_size;
    entries[3].id = static_cast<u16>(h2::setting_id::max_header_list_size);
    entries[3].value = state_.options.h2_max_header_list_size;
    entries[4].id = static_cast<u16>(h2::setting_id::enable_push);
    entries[4].value = 0;

    std::string payload(5 * 6, '\0');
    std::size_t written = 0;
    (void)h2::encode_settings(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        span<const h2::setting>(entries, 5), written);
    payload.resize(written);

    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    std::string frame(h2::frame_header_size + payload.size(), '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, as_bytes(payload), count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_settings_ack() {
    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = h2::flags::ack;
    header.stream_id = 0;
    header.length = 0;
    std::string frame(h2::frame_header_size, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, span<const std::byte>(), count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_ping_ack() {
    const h2::ping_opaque opaque = h2_.last_ping();
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_ping(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        opaque, written);
    h2::frame_header header;
    header.type = h2::frame_type::ping;
    header.flags = h2::flags::ack;
    header.stream_id = 0;
    header.length = 8;
    std::string frame(h2::frame_header_size + 8, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 8),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_ping() {
    h2::ping_opaque opaque{};
    const u64 now = shard_ != nullptr ? shard_->now_ns() : 0;
    for (std::size_t i = 0; i < 8; ++i) {
        opaque[i] = static_cast<std::byte>((now >> (i * 8)) & 0xFFu);
    }
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_ping(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        opaque, written);
    h2::frame_header header;
    header.type = h2::frame_type::ping;
    header.flags = 0;
    header.stream_id = 0;
    header.length = 8;
    std::string frame(h2::frame_header_size + 8, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 8),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_goaway(h2::error_code code) {
    if (h2_goaway_sent_) {
        return;
    }
    h2_goaway_sent_ = true;
    h2::goaway_payload value;
    value.last_stream_id = h2_last_peer_stream_;
    value.code = code;
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_goaway(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        value, written);
    h2::frame_header header;
    header.type = h2::frame_type::goaway;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    std::string frame(h2::frame_header_size + payload.size(), '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, as_bytes(payload), count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_rst(u32 stream_id, h2::error_code code) {
    if (h2_.stream_exists(stream_id) &&
        h2_.state_of(stream_id) != h2::stream_state::closed) {
        (void)h2_.reset_stream(stream_id, code);
        // Remember our own reset so the inbound bookkeeping path does not treat
        // it as a peer RST_STREAM on the next frame.
        h2_last_reset_seen_ = h2_.last_reset_stream();
    }
    std::string payload(4, '\0');
    std::size_t written = 0;
    (void)h2::encode_rst_stream(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        code, written);
    h2::frame_header header;
    header.type = h2::frame_type::rst_stream;
    header.flags = 0;
    header.stream_id = stream_id;
    header.length = 4;
    std::string frame(h2::frame_header_size + 4, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 4),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_window_update(u32 stream_id, u32 increment) {
    if (increment == 0) {
        return;
    }
    std::string payload(4, '\0');
    std::size_t written = 0;
    (void)h2::encode_window_update(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        increment, written);
    h2::frame_header header;
    header.type = h2::frame_type::window_update;
    header.flags = 0;
    header.stream_id = stream_id;
    header.length = 4;
    std::string frame(h2::frame_header_size + 4, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 4),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_connection_error(h2::error_code code) {
    h2_send_goaway(code);
    close_when_drained_ = true;
    stop_reading_ = true;
}

// --- session: output ---------------------------------------------------------

void session::enqueue(std::string bytes) {
    if (finished_) {
        return;
    }
    out_bytes_ += bytes.size();
    out_.push_back(std::move(bytes));
    if (out_bytes_ > state_.options.egress_cap_bytes) {
        // Backpressure: stop reading until the queue drains. The bytes already
        // parsed keep their response, so nothing is dropped (R18).
        std::lock_guard<std::mutex> lk(mu_);
        read_paused_ = true;
    }
}

void session::flush_writes() {
    if (finished_ || write_failed_ || write_in_flight_ || out_.empty()) {
        return;
    }
    in_flight_ = std::move(out_.front());
    out_.pop_front();
    out_bytes_ -= in_flight_.size();
    write_in_flight_ = true;

    auto* op = new (std::nothrow) write_completion();
    if (op == nullptr) {
        write_in_flight_ = false;
        write_failed_ = true;
        close_when_drained_ = true;
        return;
    }
    op->owner = shared_from_this();
    const span<const std::byte> bytes = as_bytes(in_flight_);
    if (out_sink_ == nullptr) {
        delete op;
        write_in_flight_ = false;
        write_failed_ = true;
        close_when_drained_ = true;
        return;
    }
    out_sink_->push(bytes, op);
}

void session::maybe_finish() {
    if (finished_) {
        return;
    }
    const bool drained = out_.empty() && !write_in_flight_;
    if (!drained) {
        return;
    }
    if (close_when_drained_ || input_closed_) {
        finish_now();
    }
}

void session::finish_now() {
    if (finished_) {
        return;
    }
    finished_ = true;
    close_when_drained_ = true;
    stop_reading_ = true;
    if (shard_ != nullptr) {
        (void)shard_->cancel_timer(this);
    }
    probe_armed_ = false;
    out_.clear();
    out_bytes_ = 0;
    in_flight_.clear();
    if (tls_) {
        tls_->close();
    }
    if (transport_) {
        transport_->bind_source(nullptr);
        transport_->close();
    }
    state_.remove_session(shared_from_this());
}

void session::note_activity() noexcept {
    if (shard_ == nullptr || state_.options.idle_timeout.count() <= 0) {
        return;
    }
    idle_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.idle_timeout)
                             .count());
}

void session::note_request_progress() noexcept {
    if (shard_ == nullptr || state_.options.request_timeout.count() <= 0 ||
        request_deadline_ns_ != 0) {
        return;
    }
    request_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.request_timeout)
                             .count());
}

void session::arm_probe() {
    if (probe_armed_ || finished_ || shard_ == nullptr) {
        return;
    }
    u64 deadline = 0;
    if (phase_ == phase::h2 && h2_.has_parked()) {
        deadline = shard_->now_ns() +
                   static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        state_.options.h2_probe_interval)
                                        .count());
    }
    if (idle_deadline_ns_ != 0 && (deadline == 0 || idle_deadline_ns_ < deadline)) {
        deadline = idle_deadline_ns_;
    }
    if (request_deadline_ns_ != 0 &&
        (deadline == 0 || request_deadline_ns_ < deadline)) {
        deadline = request_deadline_ns_;
    }
    // HTTP/2 bounds each open stream separately: one stream trickling bytes must
    // not keep another stream's request alive forever.
    for (const auto& item : h2_streams_) {
        const u64 stream_deadline = item.second.deadline_ns;
        if (stream_deadline != 0 && (deadline == 0 || stream_deadline < deadline)) {
            deadline = stream_deadline;
        }
    }
    if (deadline == 0) {
        return;
    }
    probe_armed_ = true;
    if (!shard_->arm_timer(this, deadline, &session::probe_trampoline, this)) {
        probe_armed_ = false;
    }
}

void session::on_probe() {
    probe_armed_ = false;
    if (finished_ || shutdown_requested_.load(std::memory_order_acquire) ||
        shard_ == nullptr) {
        return;
    }
    const u64 now = shard_->now_ns();
    if (request_deadline_ns_ != 0 && now >= request_deadline_ns_) {
        // A request that never completes must fail explicitly instead of
        // occupying the connection forever (No-stall rule).
        request_deadline_ns_ = 0;
        state_.timed_out.fetch_add(1, std::memory_order_relaxed);
        if (phase_ == phase::h2) {
            h2_send_goaway(h2::error_code::no_error);
        } else {
            http::request partial = parser_.get_request();
            send_http1_error(408, "request timeout", partial, false);
        }
        close_when_drained_ = true;
        stop_reading_ = true;
        flush_writes();
        return;
    }
    if (idle_deadline_ns_ != 0 && now >= idle_deadline_ns_) {
        idle_deadline_ns_ = 0;
        state_.timed_out.fetch_add(1, std::memory_order_relaxed);
        close_when_drained_ = true;
        stop_reading_ = true;
        flush_writes();
        maybe_finish();
        return;
    }
    if (phase_ == phase::h2 && h2_.has_parked()) {
        h2_resume_parked();
        if (h2_.has_parked()) {
            // A peer that never sends WINDOW_UPDATE must not stall the
            // connection silently: probe it so a dead peer is observed (R18).
            h2_send_ping();
        }
    }
    if (phase_ == phase::h2) {
        // Reset every stream whose request never completed. A stream-level
        // error is enough: the connection and the other streams stay usable.
        for (auto it = h2_streams_.begin(); it != h2_streams_.end();) {
            if (it->second.deadline_ns != 0 && now >= it->second.deadline_ns) {
                const u32 stream_id = it->first;
                it = h2_streams_.erase(it);
                h2_end_pending_.erase(stream_id);
                state_.timed_out.fetch_add(1, std::memory_order_relaxed);
                // h2_send_rst also closes the connection-level record and
                // records our own reset so it is not re-processed.
                h2_send_rst(stream_id, h2::error_code::cancel);
            } else {
                ++it;
            }
        }
    }
    arm_probe();
}

} // namespace

// --- http_server -------------------------------------------------------------

struct http_server::impl {
    server_state state;

    explicit impl(http_server_options options) : state(std::move(options)) {}
};

http_server::builder::builder() = default;
http_server::builder::~builder() = default;

http_server::builder& http_server::builder::add_listener(endpoint ep,
                                                         backend_factory& backend) {
    listener_config config;
    config.ep = std::move(ep);
    config.backend = &backend;
    listeners_.push_back(std::move(config));
    return *this;
}

http_server::builder& http_server::builder::router(const mvc::router& routes) {
    router_ = &routes;
    return *this;
}

http_server::builder& http_server::builder::shards(std::size_t count) {
    options_.shards = count == 0 ? 1 : count;
    return *this;
}

http_server::builder& http_server::builder::options(
    const http_server_options& value) {
    options_ = value;
    return *this;
}

http_server::builder& http_server::builder::set_parser_limits(
    const parser_limits& limits) {
    options_.limits = limits;
    return *this;
}

http_server::builder& http_server::builder::drain_timeout(
    std::chrono::milliseconds value) {
    options_.drain_timeout = value;
    return *this;
}

std::unique_ptr<http_server> http_server::builder::build() {
    auto result = std::unique_ptr<http_server>(new http_server(options_));
    result->impl_->state.router = router_;
    for (const listener_config& config : listeners_) {
        server_state::pending_listener pending;
        pending.ep = config.ep;
        pending.backend = config.backend;
        pending.tls = false;
        result->impl_->state.pending_listeners.push_back(std::move(pending));
    }
    return result;
}

http_server::http_server(http_server_options opt)
    : impl_(std::make_unique<impl>(std::move(opt))) {}

http_server::~http_server() {
    stop();
}

status http_server::listen_tls(const endpoint& ep, backend_factory& backend,
                               io::tls_config config) {
    if (impl_->state.accepting.load(std::memory_order_acquire)) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "listen_tls must be called before start()")};
    }
#if defined(CRPC_HAVE_OPENSSL)
    config.role = io::tls_role::server;
    config.require_alpn = true;
    if (config.alpn_protocols.empty()) {
        config.alpn_protocols = {"h2", "http/1.1"};
    } else {
        bool has_h2 = false;
        bool has_http1 = false;
        for (const std::string& item : config.alpn_protocols) {
            has_h2 = has_h2 || item == "h2";
            has_http1 = has_http1 || item == "http/1.1";
        }
        if (!has_h2) {
            config.alpn_protocols.insert(config.alpn_protocols.begin(), "h2");
        }
        if (!has_http1) {
            config.alpn_protocols.push_back("http/1.1");
        }
    }
    server_state::pending_listener pending;
    pending.ep = ep;
    pending.backend = &backend;
    pending.tls = true;
    impl_->state.pending_listeners.push_back(std::move(pending));
    impl_->state.has_tls = true;
    impl_->state.tls_config = std::move(config);
    return status{};
#else
    (void)ep;
    (void)backend;
    (void)config;
    return status{rpc_error(
        rpc_code::failed_precondition,
        "HTTPS/TLS is not available: the library was built without CRPC_HAVE_OPENSSL")};
#endif
}

status http_server::start() {
    server_state& state = impl_->state;
    if (state.accepting.load(std::memory_order_acquire)) {
        return status{};
    }
    if (state.router == nullptr) {
        state.router = &mvc::default_router();
    }
    if (!state.router->frozen()) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "the MVC router must be frozen before start()")};
    }

    if (state.has_tls) {
#if defined(CRPC_HAVE_OPENSSL)
        std::error_code ec;
        state.tls_context = io::tls_context::create(state.tls_config, ec);
        if (!state.tls_context) {
            return status{rpc_error(rpc_code::failed_precondition,
                                    "TLS context creation failed: " + ec.message())};
        }
#else
        return status{rpc_error(
            rpc_code::failed_precondition,
            "HTTPS/TLS is not available: the library was built without CRPC_HAVE_OPENSSL")};
#endif
    }

    try {
        const std::size_t count = state.options.shards == 0 ? 1 : state.options.shards;
        for (std::size_t i = 0; i < count; ++i) {
            runtime_options options = state.options.runtime;
            options.start_handler_pool = true;
            auto shard = std::make_unique<runtime>(options);
            shard->start_thread();
            state.shards.push_back(std::move(shard));
        }
        for (const server_state::pending_listener& config :
             state.pending_listeners) {
            if (config.backend == nullptr) {
                stop();
                return status{rpc_error(rpc_code::invalid_argument,
                                        "listener has no backend")};
            }
            std::unique_ptr<listener> bound =
                config.backend->listen(config.ep, listen_options{});
            if (!bound) {
                stop();
                return status{rpc_error(rpc_code::invalid_argument,
                                        "listener could not bind endpoint")};
            }
            state.listeners.push_back(std::move(bound));
            state.acceptors.push_back(
                std::make_unique<server_state::acceptor>(state, config.tls));
        }
        state.accepting.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < state.listeners.size(); ++i) {
            state.listeners[i]->start(state.acceptors[i].get());
        }
    } catch (...) {
        stop();
        return status{rpc_code::resource_exhausted};
    }
    return status{};
}

void http_server::stop() noexcept {
    server_state& state = impl_->state;
    const bool was_accepting = state.accepting.exchange(false, std::memory_order_acq_rel);
    (void)was_accepting;

    for (std::unique_ptr<listener>& listener : state.listeners) {
        if (listener) {
            listener->stop();
        }
    }
    state.listeners.clear();

    if (state.shards.empty()) {
        // Nothing was started (or start() failed early). Close anything the
        // backend already handed over.
        const std::vector<std::shared_ptr<session>> sessions = state.snapshot();
        for (const std::shared_ptr<session>& value : sessions) {
            value->force_close();
        }
        {
            std::lock_guard<std::mutex> lk(state.sessions_mutex);
            state.sessions.clear();
        }
        state.acceptors.clear();
        return;
    }

    std::vector<std::shared_ptr<session>> sessions = state.snapshot();
    for (const std::shared_ptr<session>& value : sessions) {
        value->request_shutdown();
    }

    {
        std::unique_lock<std::mutex> lk(state.sessions_mutex);
        state.drain_cv.wait_for(lk, state.options.drain_timeout, [&state] {
            return state.sessions.empty();
        });
    }

    sessions = state.snapshot();
    for (const std::shared_ptr<session>& value : sessions) {
        value->force_close();
    }
    // Sessions are retained until the shards are joined. A probe timer holds
    // only a raw pointer to its session, so a session must outlive the shard
    // that may still fire it; after the join no timer can run.
    for (std::unique_ptr<runtime>& shard : state.shards) {
        shard->stop();
    }
    for (std::unique_ptr<runtime>& shard : state.shards) {
        shard->join();
    }
    {
        std::lock_guard<std::mutex> lk(state.sessions_mutex);
        state.sessions.clear();
    }
    state.shards.clear();
    state.pending_listeners.clear();
    state.tls_context.reset();
    state.acceptors.clear();
}

bool http_server::running() const noexcept {
    return impl_->state.accepting.load(std::memory_order_acquire);
}

http_statistics http_server::stats() const noexcept {
    http_statistics out;
    {
        std::lock_guard<std::mutex> lk(impl_->state.sessions_mutex);
        out.active_connections = impl_->state.sessions.size();
    }
    out.accepted_connections =
        impl_->state.accepted.load(std::memory_order_relaxed);
    out.requests_handled = impl_->state.handled.load(std::memory_order_relaxed);
    out.responses_written = impl_->state.responded.load(std::memory_order_relaxed);
    out.bad_requests = impl_->state.bad.load(std::memory_order_relaxed);
    out.h2_connections = impl_->state.h2_connections.load(std::memory_order_relaxed);
    out.tls_connections = impl_->state.tls_connections.load(std::memory_order_relaxed);
    out.rejected_connections = impl_->state.rejected.load(std::memory_order_relaxed);
    out.timed_out_connections = impl_->state.timed_out.load(std::memory_order_relaxed);
    return out;
}

} // namespace http
} // namespace crpc
