// CRPC -- HTTP/1.1 keep-alive and connection-pool tests.
//
// The client used to be one request per connection (`Connection: close`). These
// tests pin the replacement contract:
//
//   * one connection carries N sequential requests -- proven by the *server's*
//     accept counter (`http_statistics::accepted_connections`), never by the
//     client's own claim;
//   * a response with bytes beyond its framing poisons the connection: the
//     response itself is delivered, the next request on that connection fails
//     with a clear protocol error and never returns the leftover bytes as
//     content;
//   * a connection the peer closed is never handed out again (borrow-time
//     health check), and a death that slips past the check costs one
//     transparent retry -- for idempotent verbs only;
//   * a POST/PUT/DELETE/PATCH is never replayed automatically: a request that
//     may have been executed reports `unknown_side_effect`;
//   * `Connection: close` responses and HTTP/1.0 servers (no keep-alive token)
//     retire the connection; the next exchange dials a new one;
//   * HEAD responses are framing headers only -- the client must not wait for
//     a body the response declares;
//   * the pool mirrors `db::pool`'s contract: max_size / max_idle /
//     idle_timeout / max_lifetime / acquire timeout, borrow-time validation,
//     statistics, and RAII leases;
//   * HTTPS reuses the connection the same way.
//
// Real loopback TCP wherever a connection is actually made (IOCP on Windows,
// epoll elsewhere). The scripted server speaks raw HTTP/1.1 bytes so a test can
// produce responses the library server never would: framing violations,
// HTTP/1.0, mid-life closes. Pool edge cases run on scripted `db::channel`
// fakes, which is what makes "the factory was called twice" and "the POST was
// not replayed" exact rather than timing-dependent.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/db/hbase/rest.hpp>
#include <crpc/db/stream_channel.hpp>
#include <crpc/http/client.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#include "test_tcp_backend.hpp"
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_framework.hpp"
#include "tls_test_certificate.hpp"

using namespace crpc;

namespace {


[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

crpc::u16 next_port() {
#if defined(_WIN32)
    static const unsigned base = 12000u + (::GetCurrentProcessId() % 15000u);
#else
    static const unsigned base = 12000u + (static_cast<unsigned>(::getpid()) % 15000u);
#endif
    static std::atomic<unsigned> counter{0};
    const unsigned value = base + counter.fetch_add(11, std::memory_order_relaxed);
    // Stay clear of the OS ephemeral range (49152+ on Windows): a port in it
    // can be claimed by an outgoing connection between the bind attempts, so
    // the range is capped below it.
    return static_cast<crpc::u16>(value % 36000u + 1024u);
}

[[nodiscard]] std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// True when a comma-separated header value carries the token.
[[nodiscard]] bool has_token(const std::string& value, std::string_view token) {
    std::string lower = to_lower(value);
    std::size_t start = 0;
    while (start <= lower.size()) {
        const std::size_t comma = lower.find(',', start);
        std::string item = lower.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
            item.erase(item.begin());
        }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
            item.pop_back();
        }
        if (item == token) {
            return true;
        }
        if (comma == std::string::npos) {
            return false;
        }
        start = comma + 1;
    }
    return false;
}

// The value of the first header whose lower-cased name matches, or "".
[[nodiscard]] std::string header_value(const std::string& message,
                                       std::string_view lower_name) {
    const std::size_t block_end = message.find("\r\n\r\n");
    const std::size_t limit =
        block_end == std::string::npos ? message.size() : block_end;
    std::size_t line_start = 0;
    while (line_start < limit) {
        const std::size_t line_end = message.find("\r\n", line_start);
        const std::size_t end =
            line_end == std::string::npos || line_end > limit ? limit : line_end;
        const std::string line = message.substr(line_start, end - line_start);
        line_start = end + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (to_lower(line.substr(0, colon)) != lower_name) {
            continue;
        }
        return line.substr(colon + 1);
    }
    return {};
}

// --- Scripted TCP server -----------------------------------------------------

struct script_step {
    // Raw response bytes, exactly as they go on the wire.
    std::string response;
    // Close the connection after writing.
    bool close_after = false;
};

struct logged_request {
    std::size_t connection = 0;
    std::string start_line;
    bool connection_close = false;
    bool connection_keep_alive = false;
};

// Accepts loopback connections and serves each with a per-connection script.
// `per_connection[i]` drives connection i; the last script repeats for every
// further connection. Every connection restarts its script from the first step,
// so a test that forces a re-dial gets a fresh answer on the second connection.
class scripted_server {
public:
    explicit scripted_server(std::vector<std::vector<script_step>> per_connection)
        : per_connection_(std::move(per_connection)) {
        // A port a recent run released can still be in a binding state for a
        // moment; the loopback range is wide, so a failed bind is retried on
        // fresh ports rather than failing the test.
        for (int attempt = 0; attempt < 8 && listener_ == nullptr; ++attempt) {
            port_ = next_port();
            listener_ = backend_.listen(endpoint::tcp("127.0.0.1", port_),
                                        listen_options{});
        }
        CRPC_CHECK(listener_ != nullptr);
        if (listener_ != nullptr) {
            listener_->start(&pump_);
            dispatcher_ = std::thread([this] { dispatch(); });
        }
    }

    ~scripted_server() {
        if (listener_) {
            listener_->stop();
        }
        pump_.close();
        if (dispatcher_.joinable()) {
            dispatcher_.join();
        }
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    scripted_server(const scripted_server&) = delete;
    scripted_server& operator=(const scripted_server&) = delete;

    [[nodiscard]] crpc::u16 port() const noexcept { return port_; }

    // How many connections the server accepted.
    [[nodiscard]] std::size_t accepted() const {
        return accepted_.load(std::memory_order_acquire);
    }

    // Every request the server read, in arrival order.
    [[nodiscard]] std::vector<logged_request> requests() const {
        std::lock_guard<std::mutex> lk(requests_mu_);
        return requests_;
    }

private:
    // Bridges the backend's accept callback to the dispatcher thread.
    class pump final : public accept_sink {
    public:
        void on_accept(std::unique_ptr<connection> accepted) noexcept override {
            if (!accepted) {
                return;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                pending_.push_back(std::move(accepted));
            }
            cv_.notify_one();
        }

        std::unique_ptr<connection> take(bool& closed) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !pending_.empty() || closed_; });
            closed = pending_.empty();
            if (closed) {
                return nullptr;
            }
            std::unique_ptr<connection> link = std::move(pending_.front());
            pending_.pop_front();
            return link;
        }

        void close() noexcept {
            {
                std::lock_guard<std::mutex> lk(mu_);
                closed_ = true;
            }
            cv_.notify_all();
        }

    private:
        std::mutex mu_;
        std::condition_variable cv_;
        std::deque<std::unique_ptr<connection>> pending_;
        bool closed_ = false;
    };

    void dispatch() {
        for (;;) {
            bool closed = false;
            std::unique_ptr<connection> link = pump_.take(closed);
            if (closed) {
                return;
            }
            const std::size_t index =
                accepted_.fetch_add(1, std::memory_order_acq_rel);
            if (workers_.size() <= index) {
                workers_.resize(index + 1);
            }
            workers_[index] = std::thread(
                [this, index, raw = std::move(link)]() mutable { serve(index, std::move(raw)); });
        }
    }

    void serve(std::size_t index, std::unique_ptr<connection> raw) {
        const std::size_t which =
            index < per_connection_.size() ? index : per_connection_.size() - 1;
        db::stream_channel link(std::move(raw), std::chrono::milliseconds(10000));
        for (const script_step& step : per_connection_[which]) {
            std::string request;
            if (!read_one_request(link, request)) {
                return;
            }
            log_request(index, request);
            const status written = link.write(as_bytes(step.response));
            if (!written) {
                return;
            }
            if (step.close_after) {
                // connection::close is abortive (io.hpp): it discards bytes
                // still queued in the backend's send ring. The response is
                // flushed to the kernel asynchronously by the loop thread, so
                // hold the connection briefly before closing -- what a real
                // server's graceful teardown does -- or the peer may observe
                // nothing but the EOF.
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                return;
            }
        }
        // A real keep-alive server does not slam the socket shut the instant a
        // response is written: it leaves the connection open for the next
        // request. Holding briefly after the script is what lets a test
        // return a live connection to the pool before the server retires it.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    void log_request(std::size_t index, const std::string& request) {
        logged_request entry;
        entry.connection = index;
        const std::size_t line_end = request.find("\r\n");
        entry.start_line =
            line_end == std::string::npos ? request : request.substr(0, line_end);
        const std::string connection = header_value(request, "connection");
        entry.connection_close = has_token(connection, "close");
        entry.connection_keep_alive = has_token(connection, "keep-alive");
        std::lock_guard<std::mutex> lk(requests_mu_);
        requests_.push_back(entry);
    }

    // Reads one request: the header block, then the body its Content-Length
    // declares. False when the peer went away first.
    [[nodiscard]] static bool read_one_request(db::channel& link, std::string& out) {
        out.clear();
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            if (out.size() > 64u * 1024u) {
                return false;
            }
            std::byte buffer[4096];
            result<std::size_t> got = link.read_some(span<std::byte>(buffer, sizeof buffer));
            if (!got) {
                return false;
            }
            if (got.value() == 0) {
                continue;
            }
            out.append(reinterpret_cast<const char*>(buffer), got.value());
            header_end = out.find("\r\n\r\n");
        }
        const std::string length_text =
            header_value(out.substr(0, header_end), "content-length");
        std::size_t content_length = 0;
        for (const char c : length_text) {
            if (c < '0' || c > '9') {
                break;
            }
            content_length = content_length * 10 + static_cast<std::size_t>(c - '0');
        }
        const std::size_t body_start = header_end + 4;
        while (out.size() - body_start < content_length) {
            std::byte buffer[4096];
            result<std::size_t> got = link.read_some(span<std::byte>(buffer, sizeof buffer));
            if (!got) {
                return false;
            }
            if (got.value() == 0) {
                continue;
            }
            out.append(reinterpret_cast<const char*>(buffer), got.value());
        }
        return true;
    }

    tcp_backend backend_;
    std::unique_ptr<listener> listener_;
    crpc::u16 port_ = 0;
    pump pump_;
    std::thread dispatcher_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> accepted_{0};
    mutable std::mutex requests_mu_;
    std::vector<logged_request> requests_;
    std::vector<std::vector<script_step>> per_connection_;
};

// --- Library HTTP server fixture ---------------------------------------------

status handle_hello(void*, const http::request&, const mvc::route_params&,
                    const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = "hello";
    return status{};
}

status handle_echo(void*, const http::request& req, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = req.body;
    return status{};
}

void add_route(mvc::router& router, http::method method, const char* path,
               mvc::invoke_fn fn) {
    mvc::route_entry entry;
    entry.method = method;
    entry.path = path;
    entry.name = path;
    entry.invoke = fn;
    const status added = router.add(std::move(entry));
    CRPC_CHECK(added.ok());
}

struct library_server {
    tcp_backend backend;
    mvc::router router;
    std::unique_ptr<http::http_server> server;
    crpc::u16 port = 0;

    // `forced_port != 0` binds that port (a test that needs a second listener
    // on the same server picks the TLS port itself); 0 picks a free port.
    explicit library_server(crpc::u16 forced_port = 0) {
        add_route(router, http::method::get, "/hello", &handle_hello);
        add_route(router, http::method::post, "/echo", &handle_echo);
        const status frozen = router.freeze();
        CRPC_CHECK(frozen.ok());
        // A port a recent run released can still be in a binding state for a
        // moment; retry on fresh ports instead of failing the test.
        for (int attempt = 0; attempt < 8 && server == nullptr; ++attempt) {
            port = forced_port != 0 ? forced_port : next_port();
            http::http_server::builder builder;
            builder.add_listener(endpoint::tcp("127.0.0.1", port), backend);
            builder.router(router);
            std::unique_ptr<http::http_server> candidate = builder.build();
            if (!candidate) {
                continue;
            }
            if (candidate->start().ok()) {
                server = std::move(candidate);
            }
        }
        CRPC_CHECK(server != nullptr);
    }
};

[[nodiscard]] http::client_options dial_options(crpc::u16 port) {
    http::client_options options;
    options.host = "localhost";
    options.port = port;
    options.timeout_seconds = 10;
    return options;
}

// --- Scripted channel fakes (no sockets; exact factory accounting) -----------

// The response every healthy fake serves: one framed body, keep-alive.
constexpr std::string_view k_ok_response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "hi";

class fake_channel final : public db::channel {
public:
    // A healthy connection that answers reads with `bytes`, then EOF.
    static std::unique_ptr<fake_channel> serving(std::string bytes) {
        std::unique_ptr<fake_channel> link(new fake_channel());
        link->script_ = std::move(bytes);
        return link;
    }

    // The stale-connection signature: writes are accepted, reads report the
    // peer's clean close.
    static std::unique_ptr<fake_channel> stale() {
        return std::unique_ptr<fake_channel>(new fake_channel());
    }

    // The write refused: the kernel never accepted the request bytes.
    static std::unique_ptr<fake_channel> broken_write() {
        std::unique_ptr<fake_channel> link(new fake_channel());
        link->write_fails_ = true;
        return link;
    }

    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count) override {
        std::vector<std::byte> out;
        out.reserve(count);
        while (out.size() < count) {
            std::byte one[1];
            result<std::size_t> got = read_some(span<std::byte>(one, sizeof one));
            if (!got) {
                return got.error();
            }
            if (got.value() == 0) {
                return rpc_error{rpc_code::eof, "the fake channel ended"};
            }
            out.push_back(one[0]);
        }
        return out;
    }

    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override {
        if (closed_) {
            return rpc_error{rpc_code::shutting_down, "the fake channel is closed"};
        }
        if (read_position_ >= script_.size()) {
            // Everything scripted was delivered; the peer has closed.
            return rpc_error{rpc_code::eof, "the fake peer closed cleanly"};
        }
        const std::size_t take =
            std::min(buffer.size(), script_.size() - read_position_);
        for (std::size_t i = 0; i < take; ++i) {
            buffer[i] = static_cast<std::byte>(script_[read_position_ + i]);
        }
        read_position_ += take;
        return take;
    }

    [[nodiscard]] status write(span<const std::byte> data) override {
        if (closed_) {
            return status{rpc_code::shutting_down, "the fake channel is closed"};
        }
        if (write_fails_) {
            return status{rpc_code::broken_pipe, "the fake write was refused"};
        }
        written_.append(reinterpret_cast<const char*>(data.data()), data.size());
        return status{};
    }

    void close() noexcept override { closed_ = true; }

    [[nodiscard]] bool is_open() const noexcept override { return !closed_; }

    [[nodiscard]] const std::string& written() const noexcept { return written_; }

private:
    fake_channel() = default;

    std::string script_;
    std::size_t read_position_ = 0;
    std::string written_;
    bool write_fails_ = false;
    bool closed_ = false;
};

class fake_factory final : public http::client_factory {
public:
    // What the next open() hands out. An empty slot serves one healthy fake.
    std::vector<std::unique_ptr<db::channel>> queued;

    [[nodiscard]] result<std::unique_ptr<http::client>> open() override {
        const int call = calls_.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::unique_ptr<db::channel> link;
        if (!queued.empty()) {
            link = std::move(queued.front());
            queued.erase(queued.begin());
        } else {
            link = fake_channel::serving(std::string(k_ok_response));
        }
        if (!link) {
            return rpc_error{rpc_code::unavailable, "the fake factory is exhausted"};
        }
        http::client_options options;
        options.host = "fake";
        options.keep_alive = true;
        if (log_writes_) {
            std::lock_guard<std::mutex> lk(log_mu_);
            opened_.push_back(call);
        }
        return std::unique_ptr<http::client>(
            new http::client(std::move(options), std::move(link)));
    }

    [[nodiscard]] int calls() const noexcept {
        return calls_.load(std::memory_order_acquire);
    }

private:
    std::atomic<int> calls_{0};
    bool log_writes_ = false;
    std::mutex log_mu_;
    std::vector<int> opened_;
};

} // namespace

// --- Batch 1: connection reuse, leftover poisoning, HEAD framing -------------

// The server-side accept count is the proof, not the client's own claim: five
// requests through one client must arrive over exactly one accepted connection.
CRPC_TEST(keepalive_reuses_one_connection_for_n_requests) {
    library_server fixture;
    if (!fixture.server || !fixture.server->running()) {
        return;
    }
    const crpc::u16 port = fixture.port;
    tcp_backend client_backend;
    std::unique_ptr<http::client> link = http::make_client(
        dial_options(port), client_backend, endpoint::tcp("127.0.0.1", port));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    for (int round = 0; round < 5; ++round) {
        const result<http::response> reply = link->send(http::method::get, "/hello", {});
        CRPC_CHECK(reply.ok());
        if (!reply) {
            std::fprintf(stderr, "  request %d failed: %s (%d)\n", round,
                         reply.error().message.c_str(),
                         static_cast<int>(reply.error().code));
            return;
        }
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }
    const http::http_statistics stats = fixture.server->stats();
    CRPC_CHECK_EQ(std::size_t{1}, stats.accepted_connections);
    CRPC_CHECK_EQ(std::size_t{5}, stats.requests_handled);
    CRPC_CHECK_EQ(std::size_t{1}, stats.active_connections);
}

// The first response is framed as five bytes but the server writes ten. The
// response itself is delivered intact; the connection is poisoned, so the next
// request fails with a clear protocol error instead of reading " XXXXX" as the
// start of a status line -- and the garbage is never returned as content.
CRPC_TEST(leftover_bytes_poison_the_connection) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "AAAAA XXXXX"),
                      false}}});
    tcp_backend backend;
    std::unique_ptr<http::client> link =
        http::make_client(dial_options(server.port()), backend,
                          endpoint::tcp("127.0.0.1", server.port()));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    const result<http::response> first = link->send(http::method::get, "/one", {});
    CRPC_CHECK(first.ok());
    if (!first) {
        return;
    }
    CRPC_CHECK_EQ(200, first.value().status_code);
    CRPC_CHECK_EQ(std::string("AAAAA"), first.value().body);

    // The extra bytes are a framing violation: the connection is unusable.
    CRPC_CHECK_FALSE(link->is_open());
    const result<http::response> second = link->send(http::method::get, "/two", {});
    CRPC_CHECK_FALSE(second.ok());
    if (!second) {
        std::fprintf(stderr, "  poison: %s (%d)\n", second.error().message.c_str(),
                     static_cast<int>(second.error().code));
        CRPC_CHECK_EQ(rpc_code::protocol_error, second.error().code);
        CRPC_CHECK(second.error().message.find("beyond") != std::string::npos);
    }
}

// A HEAD response declares `Content-Length: 5` and sends no body: the framing
// headers describe the body a GET would have carried. The client must read the
// headers and nothing else, then keep using the same connection.
CRPC_TEST(head_response_does_not_wait_for_a_body) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"),
                      false},
          script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 2\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "ok"),
                      false}}});
    tcp_backend backend;
    std::unique_ptr<http::client> link =
        http::make_client(dial_options(server.port()), backend,
                          endpoint::tcp("127.0.0.1", server.port()));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    const result<http::response> head = link->send(http::method::head, "/x", {});
    CRPC_CHECK(head.ok());
    if (!head) {
        std::fprintf(stderr, "  head failed: %s (%d)\n", head.error().message.c_str(),
                     static_cast<int>(head.error().code));
        return;
    }
    CRPC_CHECK_EQ(200, head.value().status_code);
    CRPC_CHECK(head.value().body.empty());
    // The follow-up request proves no head-body bytes were consumed or awaited.
    const result<http::response> next = link->send(http::method::get, "/x", {});
    CRPC_CHECK(next.ok());
    if (next) {
        CRPC_CHECK_EQ(200, next.value().status_code);
        CRPC_CHECK_EQ(std::string("ok"), next.value().body);
    }
    CRPC_CHECK_EQ(std::size_t{1}, server.accepted());
}

// --- Batch 2: dead connections, non-idempotent replay, server retire ---------

// The server answers one exchange and closes its side. The pool must not hand
// that dead connection out again: the second borrow dials a fresh one, and
// the response it carries is the second script's, never garbage from the
// closed socket. (A death that lands after the health check is the retry
// test's job; this one proves the check itself.)
CRPC_TEST(closed_connection_is_not_reused) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "AAAAA"),
                      false}},
         {script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "BBBBB"),
                      false}}});
    tcp_backend backend;
    http::client_pool_options options;
    http::dial_factory factory(dial_options(server.port()), backend,
                               endpoint::tcp("127.0.0.1", server.port()));
    http::client_pool pool(options, factory);

    result<http::response> first{rpc_error{rpc_code::canceled, "not run"}};
    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        first = borrowed.value().send(http::method::get, "/one", {});
        CRPC_CHECK(first.ok());
        if (first) {
            CRPC_CHECK_EQ(200, first.value().status_code);
            CRPC_CHECK_EQ(std::string("AAAAA"), first.value().body);
        }
        CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
        (void)borrowed.value().release();
    }
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    // The scripted handler closes once its script is exhausted; give the
    // backend thread a moment to observe the FIN before the next borrow.
    //
    // Tried replacing this with a wait on an observed-close counter: the
    // scripted handler finishes its loop *before* the socket is closed, so that
    // counter fired early and the case then reused the still-open connection
    // (it went red at :794/:802/:803). The fixture has no cheap way to observe
    // the close itself, so the fixed wait stays.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    result<http::response> second{rpc_error{rpc_code::canceled, "not run"}};
    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        second = borrowed.value().send(http::method::get, "/two", {});
        CRPC_CHECK(second.ok());
        if (second) {
            CRPC_CHECK_EQ(200, second.value().status_code);
            CRPC_CHECK_EQ(std::string("BBBBB"), second.value().body);
        }
        (void)borrowed.value().release();
    }
    // The dead connection was replaced, not reused.
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
    CRPC_CHECK_EQ(std::size_t{2}, server.accepted());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
}

// A POST whose connection dies after the request was written is never
// replayed: the pool opens no replacement (the factory is called exactly
// once), and the failure names the unknown side effect so a caller can
// reconcile instead of guessing.
CRPC_TEST(post_is_never_replayed) {
    fake_factory factory;
    factory.queued.push_back(fake_channel::stale());
    http::client_pool_options options;
    options.validate_on_borrow = false; // the death must reach the send, not the borrow
    http::client_pool pool(options, factory);

    result<http::client_pool::lease> borrowed = pool.acquire();
    CRPC_CHECK(borrowed.ok());
    if (!borrowed) {
        return;
    }
    const std::string body = "{\"must\":\"not run twice\"}";
    const result<http::response> reply =
        borrowed.value().send(http::method::post, "/write", as_bytes(body));
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        std::fprintf(stderr, "  post on a stale connection: %s (%d)\n",
                     reply.error().message.c_str(),
                     static_cast<int>(reply.error().code));
        CRPC_CHECK_EQ(rpc_code::unknown_side_effect, reply.error().code);
    }
    // Exactly one connection was opened; the request was not replayed.
    CRPC_CHECK_EQ(1, factory.calls());
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
    // The lease still releases sanely: the dead connection is discarded.
    (void)borrowed.value().release();
    CRPC_CHECK_EQ(std::size_t{0}, pool.total());
    CRPC_CHECK_EQ(std::size_t{0}, pool.in_use());
}

// A `Connection: close` response retires the connection even when the wire
// stays usable: the pool replaces it, and a bare client refuses a second
// exchange instead of writing into a socket the server said it would close.
CRPC_TEST(connection_close_header_retires_the_connection) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "AAAAA"),
                      true}},
         {script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "BBBBB"),
                      false}}});
    tcp_backend backend;
    http::client_pool_options options;
    http::dial_factory factory(dial_options(server.port()), backend,
                               endpoint::tcp("127.0.0.1", server.port()));
    http::client_pool pool(options, factory);

    for (int round = 0; round < 2; ++round) {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        const result<http::response> reply =
            borrowed.value().send(http::method::get, "/x", {});
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(200, reply.value().status_code);
            CRPC_CHECK_EQ(std::string(round == 0 ? "AAAAA" : "BBBBB"), reply.value().body);
        }
        (void)borrowed.value().release();
    }
    CRPC_CHECK_EQ(std::size_t{2}, server.accepted());
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());

    // The bare-client form, against its own close-scripted server: the close
    // is honored, and a second send on the same client fails rather than
    // reusing a retired connection.
    scripted_server one_shot(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "AAAAA"),
                      true}}});
    tcp_backend bare_backend;
    std::unique_ptr<http::client> link =
        http::make_client(dial_options(one_shot.port()), bare_backend,
                          endpoint::tcp("127.0.0.1", one_shot.port()));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    const result<http::response> one = link->send(http::method::get, "/x", {});
    CRPC_CHECK(one.ok());
    if (!one) {
        return;
    }
    CRPC_CHECK_EQ(std::string("AAAAA"), one.value().body);
    CRPC_CHECK_FALSE(link->is_open());
    const result<http::response> two = link->send(http::method::get, "/x", {});
    CRPC_CHECK_FALSE(two.ok());
    if (!two) {
        std::fprintf(stderr, "  retired connection: %s (%d)\n", two.error().message.c_str(),
                     static_cast<int>(two.error().code));
        CRPC_CHECK_EQ(rpc_code::failed_precondition, two.error().code);
    }
}

// --- Batch 3: pool contract (deterministic fakes, no sockets) ----------------

// The retry that backs up the borrow-time health check: a connection that
// dies after the check but before the response costs exactly one replacement
// for an idempotent verb, and the exchange still succeeds.
CRPC_TEST(stale_connection_costs_one_idempotent_retry) {
    fake_factory factory;
    factory.queued.push_back(fake_channel::stale());
    http::client_pool_options options;
    options.validate_on_borrow = false; // the death must reach the send, not the borrow
    http::client_pool pool(options, factory);

    result<http::client_pool::lease> borrowed = pool.acquire();
    CRPC_CHECK(borrowed.ok());
    if (!borrowed) {
        return;
    }
    const result<http::response> reply =
        borrowed.value().send(http::method::get, "/read", {});
    CRPC_CHECK(reply.ok());
    if (!reply) {
        std::fprintf(stderr, "  stale GET: %s (%d)\n", reply.error().message.c_str(),
                     static_cast<int>(reply.error().code));
    }
    if (reply) {
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hi"), reply.value().body);
    }
    // The stale connection was replaced through the factory, once.
    CRPC_CHECK_EQ(2, factory.calls());
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
    CRPC_CHECK_EQ(std::size_t{1}, pool.in_use());
    (void)borrowed.value().release();
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
}

// The hard ceiling: at `max_size` an acquire waits for a release and then
// fails with a retryable error; releasing makes room again.
CRPC_TEST(pool_enforces_max_size_and_acquire_timeout) {
    fake_factory factory;
    http::client_pool_options options;
    options.max_size = 1;
    options.acquire_timeout = std::chrono::milliseconds(150);
    http::client_pool pool(options, factory);

    result<http::client_pool::lease> holder = pool.acquire();
    CRPC_CHECK(holder.ok());
    if (!holder) {
        return;
    }
    const auto began = std::chrono::steady_clock::now();
    const result<http::client_pool::lease> denied = pool.acquire();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - began)
                            .count();
    CRPC_CHECK_FALSE(denied.ok());
    if (!denied) {
        std::fprintf(stderr, "  exhausted: %s (%d), waited %lld ms\n",
                     denied.error().message.c_str(),
                     static_cast<int>(denied.error().code),
                     static_cast<long long>(waited));
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, denied.error().code);
    }
    CRPC_CHECK(waited >= 100);
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());

    // Releasing opens the slot again.
    (void)holder.value().release();
    result<http::client_pool::lease> again = pool.acquire();
    CRPC_CHECK(again.ok());
    if (again) {
        (void)again.value().release();
    }
}

// The borrow-time health check: a connection the peer closed while idle is
// discarded, never handed out, and replaced.
CRPC_TEST(pool_borrow_health_check_discards_dead_idles) {
    fake_factory factory;
    std::unique_ptr<fake_channel> doomed = fake_channel::serving(std::string(k_ok_response));
    fake_channel* spy = doomed.get();
    factory.queued.push_back(std::move(doomed));
    http::client_pool_options options;
    http::client_pool pool(options, factory);

    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        const result<http::response> reply =
            borrowed.value().send(http::method::get, "/x", {});
        CRPC_CHECK(reply.ok());
        (void)borrowed.value().release();
    }
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    // The peer's close arrives (the transport observed it; the channel
    // reports it) while the connection sits in the idle set.
    spy->close();

    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        const result<http::response> reply =
            borrowed.value().send(http::method::get, "/x", {});
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(std::string("hi"), reply.value().body);
        }
        (void)borrowed.value().release();
    }
    // The dead one was discarded; a fresh one was dialed.
    CRPC_CHECK_EQ(2, factory.calls());
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
}

// Beyond `max_idle` a release closes the connection instead of pooling it.
CRPC_TEST(pool_max_idle_closes_surplus) {
    fake_factory factory;
    http::client_pool_options options;
    options.max_idle = 1;
    options.max_size = 4;
    http::client_pool pool(options, factory);

    result<http::client_pool::lease> first = pool.acquire();
    result<http::client_pool::lease> second = pool.acquire();
    CRPC_CHECK(first.ok() && second.ok());
    if (!first || !second) {
        return;
    }
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
    (void)first.value().release();
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
    (void)second.value().release();
    // The surplus connection was closed, not pooled.
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());

    // The next acquire reuses the survivor; no new connection is dialed.
    result<http::client_pool::lease> third = pool.acquire();
    CRPC_CHECK(third.ok());
    if (third) {
        CRPC_CHECK_EQ(2, factory.calls());
        (void)third.value().release();
    }
}

// An idle connection older than `idle_timeout` is closed instead of reused.
CRPC_TEST(pool_idle_timeout_retires_idle_connections) {
    fake_factory factory;
    http::client_pool_options options;
    options.idle_timeout = std::chrono::milliseconds(30);
    http::client_pool pool(options, factory);

    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (borrowed) {
            (void)borrowed.value().release();
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (borrowed) {
            const result<http::response> reply =
                borrowed.value().send(http::method::get, "/x", {});
            CRPC_CHECK(reply.ok());
            (void)borrowed.value().release();
        }
    }
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
}

// A connection older than `max_lifetime` is closed instead of reused, however
// recently it was released -- measured from open, not from the last release.
CRPC_TEST(pool_max_lifetime_retires_old_connections) {
    fake_factory factory;
    http::client_pool_options options;
    options.max_lifetime = std::chrono::milliseconds(30);
    http::client_pool pool(options, factory);

    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (borrowed) {
            (void)borrowed.value().release();
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (borrowed) {
            const result<http::response> reply =
                borrowed.value().send(http::method::get, "/x", {});
            CRPC_CHECK(reply.ok());
            (void)borrowed.value().release();
        }
    }
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
}

// `close()` rejects further acquires, closes the idle set, and turns a later
// release into a close -- a lease held across the close never returns its
// connection to a dead pool. Calling close() again changes nothing.
CRPC_TEST(pool_close_rejects_and_closes_idle) {
    fake_factory factory;
    http::client_pool_options options;
    options.max_idle = 4;
    http::client_pool pool(options, factory);

    result<http::client_pool::lease> held = pool.acquire();
    CRPC_CHECK(held.ok());
    if (!held) {
        return;
    }
    pool.close();
    CRPC_CHECK(pool.is_closed());
    const result<http::client_pool::lease> denied = pool.acquire();
    CRPC_CHECK_FALSE(denied.ok());
    if (!denied) {
        CRPC_CHECK_EQ(rpc_code::shutting_down, denied.error().code);
    }
    (void)held.value().release();
    // The held connection was closed, never returned to the idle set.
    CRPC_CHECK_EQ(std::size_t{0}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{0}, pool.total());
    pool.close(); // idempotent (P15)
    CRPC_CHECK(pool.is_closed());
}

// One lease is one borrowed connection: moves hand the connection over,
// a second release is a no-op, and the destructor releases what is left.
CRPC_TEST(pool_lease_is_move_safe_and_release_is_idempotent) {
    fake_factory factory;
    http::client_pool_options options;
    http::client_pool pool(options, factory);

    result<http::client_pool::lease> first = pool.acquire();
    CRPC_CHECK(first.ok());
    if (!first) {
        return;
    }
    http::client_pool::lease moved = std::move(first.value());
    CRPC_CHECK_FALSE(first.value().valid());
    CRPC_CHECK(moved.valid());
    CRPC_CHECK(moved.release().ok());
    CRPC_CHECK(moved.release().ok()); // P2: the second release is a no-op
    CRPC_CHECK_EQ(std::size_t{0}, pool.in_use());

    {
        result<http::client_pool::lease> scoped = pool.acquire();
        CRPC_CHECK(scoped.ok());
        // The destructor releases.
    }
    CRPC_CHECK_EQ(std::size_t{0}, pool.in_use());
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
}

// A lease outliving its pool is safe: releasing into a destroyed pool closes
// the connection instead of touching freed state (P10).
CRPC_TEST(pool_lease_outliving_the_pool_closes_the_connection) {
    fake_factory factory;
    http::client_pool::lease orphan;
    {
        http::client_pool_options options;
        http::client_pool pool(options, factory);
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        orphan = std::move(borrowed.value());
        CRPC_CHECK(orphan.valid());
        // The pool is destroyed here while the lease still holds the
        // connection; the lease's shared state keeps the pool's bookkeeping
        // alive until the release.
    }
    CRPC_CHECK(orphan.valid());
    // Releasing into a destroyed pool closes the connection and must not
    // dereference freed pool state.
    CRPC_CHECK(orphan.release().ok());
    CRPC_CHECK_FALSE(orphan.valid());
}

// --- Batch 4: framing corners, one-shot compatibility, HTTPS, HBase ----------

// An HTTP/1.0 reply without a keep-alive token is one-shot by definition: the
// connection is retired even though the wire is fine, and the next exchange
// dials again.
CRPC_TEST(http_1_0_without_keep_alive_token_is_not_reused) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.0 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "\r\n"
                                 "AAAAA"),
                      false}},
         {script_step{std::string("HTTP/1.0 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "\r\n"
                                 "BBBBB"),
                      false}}});
    tcp_backend backend;
    http::client_pool_options options;
    http::dial_factory factory(dial_options(server.port()), backend,
                               endpoint::tcp("127.0.0.1", server.port()));
    http::client_pool pool(options, factory);
    for (int round = 0; round < 2; ++round) {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        const result<http::response> reply = borrowed.value().send(http::method::get, "/", {});
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(200, reply.value().status_code);
            CRPC_CHECK_EQ(std::string(round == 0 ? "AAAAA" : "BBBBB"), reply.value().body);
        }
        (void)borrowed.value().release();
    }
    CRPC_CHECK_EQ(std::size_t{2}, server.accepted());
    CRPC_CHECK_EQ(std::uint64_t{2}, pool.opened());
}

// An HTTP/1.0 server that says `Connection: keep-alive` explicitly keeps the
// connection: the explicit token is the only thing that makes a 1.0 reply
// persistent.
CRPC_TEST(http_1_0_with_keep_alive_token_is_reused) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.0 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "AAAAA"),
                      false},
          script_step{std::string("HTTP/1.0 200 OK\r\n"
                                 "Content-Length: 5\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "BBBBB"),
                      false}}});
    tcp_backend backend;
    http::client_pool_options options;
    http::dial_factory factory(dial_options(server.port()), backend,
                               endpoint::tcp("127.0.0.1", server.port()));
    http::client_pool pool(options, factory);
    for (int round = 0; round < 2; ++round) {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        const result<http::response> reply = borrowed.value().send(http::method::get, "/", {});
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(200, reply.value().status_code);
            CRPC_CHECK_EQ(std::string(round == 0 ? "AAAAA" : "BBBBB"), reply.value().body);
        }
        (void)borrowed.value().release();
    }
    CRPC_CHECK_EQ(std::size_t{1}, server.accepted());
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
}

// A zero-length body and a bodyless 204 leave the connection exactly where it
// was: nothing was consumed, so the next exchange reuses it.
CRPC_TEST(zero_length_and_204_keep_the_connection_usable) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 0\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"),
                      false},
          script_step{std::string("HTTP/1.1 204 No Content\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"),
                      false},
          script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 2\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "ok"),
                      false}}});
    tcp_backend backend;
    std::unique_ptr<http::client> link =
        http::make_client(dial_options(server.port()), backend,
                          endpoint::tcp("127.0.0.1", server.port()));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    const result<http::response> empty = link->send(http::method::get, "/a", {});
    CRPC_CHECK(empty.ok());
    if (empty) {
        CRPC_CHECK(empty.value().body.empty());
    }
    const result<http::response> no_content = link->send(http::method::get, "/b", {});
    CRPC_CHECK(no_content.ok());
    if (no_content) {
        CRPC_CHECK_EQ(204, no_content.value().status_code);
        CRPC_CHECK(no_content.value().body.empty());
    }
    const result<http::response> third = link->send(http::method::get, "/c", {});
    CRPC_CHECK(third.ok());
    if (third) {
        CRPC_CHECK_EQ(std::string("ok"), third.value().body);
    }
    CRPC_CHECK_EQ(std::size_t{1}, server.accepted());
}

// A chunked response on a keep-alive connection: the terminating chunk (and
// its trailers) are consumed to the last byte, so the next response starts
// clean.
CRPC_TEST(chunked_response_on_keep_alive_connection) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Transfer-Encoding: chunked\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "3\r\n"
                                 "abc\r\n"
                                 "0\r\n"
                                 "\r\n"),
                      false},
          script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 2\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "ok"),
                      false}}});
    tcp_backend backend;
    std::unique_ptr<http::client> link =
        http::make_client(dial_options(server.port()), backend,
                          endpoint::tcp("127.0.0.1", server.port()));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    const result<http::response> chunked = link->send(http::method::get, "/a", {});
    CRPC_CHECK(chunked.ok());
    if (chunked) {
        CRPC_CHECK_EQ(std::string("abc"), chunked.value().body);
    }
    const result<http::response> next = link->send(http::method::get, "/b", {});
    CRPC_CHECK(next.ok());
    if (next) {
        CRPC_CHECK_EQ(std::string("ok"), next.value().body);
    }
    CRPC_CHECK_EQ(std::size_t{1}, server.accepted());
}

// `keep_alive = false` is the historical contract: the request asks for a
// close, the client closes after the response, and a second send is refused.
CRPC_TEST(keep_alive_false_preserves_one_shot_behavior) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 2\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "ok"),
                      false}}});
    tcp_backend backend;
    http::client_options options = dial_options(server.port());
    options.keep_alive = false;
    std::unique_ptr<http::client> link =
        http::make_client(options, backend, endpoint::tcp("127.0.0.1", server.port()));
    CRPC_CHECK(link != nullptr);
    if (!link) {
        return;
    }
    const result<http::response> reply = link->send(http::method::get, "/", {});
    CRPC_CHECK(reply.ok());
    if (!reply) {
        return;
    }
    CRPC_CHECK_EQ(std::string("ok"), reply.value().body);
    // The request asked for a close...
    const std::vector<logged_request> seen = server.requests();
    CRPC_CHECK_EQ(std::size_t{1}, seen.size());
    if (seen.size() == 1) {
        CRPC_CHECK(seen[0].connection_close);
    }
    // ...and the client retired its side.
    CRPC_CHECK_FALSE(link->is_open());
    const result<http::response> second = link->send(http::method::get, "/", {});
    CRPC_CHECK_FALSE(second.ok());
    if (!second) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, second.error().code);
    }
}

#if !defined(CRPC_HAVE_OPENSSL)

CRPC_TEST(https_pool_reuses_one_tls_connection) {
    crpc_test::skip("the library was built without CRPC_WITH_OPENSSL");
}

#else

// The self-signed test certificate, written to the temporary directory (the
// public configuration takes file paths).
struct cert_files {
    std::string certificate;
    std::string private_key;

    cert_files() {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
#if defined(_WIN32)
        const unsigned pid = ::GetCurrentProcessId();
#else
        const unsigned pid = static_cast<unsigned>(::getpid());
#endif
        certificate =
            (dir / ("crpc_keepalive_certificate_" + std::to_string(pid) + ".pem")).string();
        private_key =
            (dir / ("crpc_keepalive_private_key_" + std::to_string(pid) + ".pem")).string();
        write(certificate, crpc_test_tls::k_certificate_pem);
        write(private_key, crpc_test_tls::k_private_key_pem);
    }

private:
    static void write(const std::string& path, const char* data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data, static_cast<std::streamsize>(std::strlen(data)));
    }
};

const cert_files& test_certificates() {
    static const cert_files files;
    return files;
}

// HTTPS reuses the connection the same way: three exchanges, one accept, one
// TLS handshake -- counted by the server, not the client. The pool's dial
// factory negotiates http/1.1 only, so the reuse is HTTP/1.1 keep-alive over
// one TLS session.
CRPC_TEST(https_pool_reuses_one_tls_connection) {
    const cert_files& certificates = test_certificates();
    tcp_backend server_backend;
    mvc::router router;
    add_route(router, http::method::get, "/hello", &handle_hello);
    CRPC_CHECK(router.freeze().ok());

    const crpc::u16 tls_port = next_port();
    // listen_tls registers before start(): that is the facade's contract.
    http::http_server::builder builder;
    builder.router(router);
    std::unique_ptr<http::http_server> server = builder.build();
    CRPC_CHECK(server != nullptr);
    if (!server) {
        return;
    }
    io::tls_config config;
    config.certificate_chain = certificates.certificate;
    config.private_key = certificates.private_key;
    const status registered =
        server->listen_tls(endpoint::tcp("127.0.0.1", tls_port), server_backend, config);
    CRPC_CHECK(registered.ok());
    if (!registered) {
        std::fprintf(stderr, "  listen_tls: %s\n", registered.message().c_str());
        return;
    }
    const status started = server->start();
    CRPC_CHECK(started.ok());
    if (!started) {
        return;
    }

    tcp_backend client_backend;
    http::client_options options = http::client_options::https("localhost");
    options.port = tls_port;
    options.tls_ca_bundle = certificates.certificate;
    options.timeout_seconds = 10;
    options.tls_alpn = {"http/1.1"};
    http::dial_factory factory(options, client_backend,
                               endpoint::tcp("127.0.0.1", tls_port));
    http::client_pool_options pool_options;
    http::client_pool pool(pool_options, factory);

    for (int round = 0; round < 3; ++round) {
        result<http::client_pool::lease> borrowed = pool.acquire();
        CRPC_CHECK(borrowed.ok());
        if (!borrowed) {
            return;
        }
        const result<http::response> reply =
            borrowed.value().send(http::method::get, "/hello", {});
        if (!reply) {
            std::fprintf(stderr, "  https request %d failed: %s (%d)\n", round,
                         reply.error().message.c_str(),
                         static_cast<int>(reply.error().code));
        }
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(200, reply.value().status_code);
            CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
        }
        (void)borrowed.value().release();
    }
    // The server's counters are the proof: one accept, one TLS connection,
    // three handled requests -- and the pool dialed exactly once.
    const http::http_statistics stats = server->stats();
    CRPC_CHECK_EQ(std::size_t{1}, stats.tls_connections);
    CRPC_CHECK_EQ(std::size_t{1}, stats.accepted_connections);
    CRPC_CHECK_EQ(std::size_t{3}, stats.requests_handled);
    CRPC_CHECK_EQ(std::uint64_t{1}, pool.opened());
    server->stop();
}

#endif // CRPC_HAVE_OPENSSL

// --- The consumer: HBase REST over the gateway -------------------------------

// With `reuse_connection` opted in, consecutive calls to the gateway ride one
// keep-alive connection -- the server's accept count is the proof.
CRPC_TEST(hbase_rest_reuses_the_gateway_connection_when_opted_in) {
    const std::string version_reply =
        std::string("HTTP/1.1 200 OK\r\n"
                    "Content-Length: 9\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "gateway-1");
    scripted_server server(
        {{script_step{version_reply, false},
          script_step{version_reply, false}}});
    tcp_backend backend;
    crpc::db::hbase::client_options options;
    options.host = "localhost";
    options.port = server.port();
    options.reuse_connection = true;
    crpc::db::hbase::client gateway(options, [&] {
        return http::make_client(dial_options(server.port()), backend,
                                 endpoint::tcp("127.0.0.1", server.port()));
    });
    const result<std::string> first = gateway.version();
    const result<std::string> second = gateway.version();
    CRPC_CHECK(first.ok() && second.ok());
    if (first.ok() && second.ok()) {
        CRPC_CHECK_EQ(std::string("gateway-1"), first.value());
        CRPC_CHECK_EQ(std::string("gateway-1"), second.value());
    }
    CRPC_CHECK_EQ(std::size_t{1}, server.accepted());
}

// The default keeps the failover design's one-exchange-per-connection
// contract: every call dials.
CRPC_TEST(hbase_rest_default_dials_every_call) {
    const std::string version_reply =
        std::string("HTTP/1.1 200 OK\r\n"
                    "Content-Length: 9\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "gateway-1");
    scripted_server server(
        {{script_step{version_reply, false},
          script_step{version_reply, false}}});
    tcp_backend backend;
    crpc::db::hbase::client_options options;
    options.host = "localhost";
    options.port = server.port();
    crpc::db::hbase::client gateway(options, [&] {
        return http::make_client(dial_options(server.port()), backend,
                                 endpoint::tcp("127.0.0.1", server.port()));
    });
    const result<std::string> first = gateway.version();
    const result<std::string> second = gateway.version();
    CRPC_CHECK(first.ok() && second.ok());
    CRPC_CHECK_EQ(std::size_t{2}, server.accepted());
}

// A connection the gateway retired (`Connection: close`) is not parked: the
// next call dials a fresh one and still succeeds.
CRPC_TEST(hbase_rest_redials_after_the_gateway_retires_the_connection) {
    scripted_server server(
        {{script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 9\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "gateway-1"),
                      true}},
         {script_step{std::string("HTTP/1.1 200 OK\r\n"
                                 "Content-Length: 9\r\n"
                                 "Connection: keep-alive\r\n"
                                 "\r\n"
                                 "gateway-1"),
                      false}}});
    tcp_backend backend;
    crpc::db::hbase::client_options options;
    options.host = "localhost";
    options.port = server.port();
    options.reuse_connection = true;
    crpc::db::hbase::client gateway(options, [&] {
        return http::make_client(dial_options(server.port()), backend,
                                 endpoint::tcp("127.0.0.1", server.port()));
    });
    const result<std::string> first = gateway.version();
    const result<std::string> second = gateway.version();
    CRPC_CHECK(first.ok() && second.ok());
    if (first.ok() && second.ok()) {
        CRPC_CHECK_EQ(std::string("gateway-1"), first.value());
        CRPC_CHECK_EQ(std::string("gateway-1"), second.value());
    }
    CRPC_CHECK_EQ(std::size_t{2}, server.accepted());
}

CRPC_TEST_MAIN()
