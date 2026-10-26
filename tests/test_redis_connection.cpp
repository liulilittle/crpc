// CRPC Redis -- asynchronous connection management tests.
//
// These tests exercise the transport half of the Redis client against the
// in-process backend, so they need no network and are deterministic. They pin:
//
//   * write/read_exact/read_until over `io::source`/`io::sink`;
//   * backpressure (a full buffer returns a short credit and `resume_read()`
//     releases the retained tail);
//   * auto-reconnect after the peer drops the connection;
//   * the pool's acquire/release reuse, exhaustion and close;
//   * cooperative suspension of a coroutine on the shard.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <crpc/io.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/runtime.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

std::vector<std::byte> to_bytes(const char* text) {
    const std::size_t n = std::strlen(text);
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

bool equals(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// A no-op sink completion for the test server's own pushes.
class noop_completion final : public crpc::io::completion {
public:
    void complete(crpc::io::io_result) noexcept override {}
};

// The server side of an in-process pair. It records every byte the connection
// sends and can push replies. Pushed buffers are kept alive in `buffers_`
// because a deferred in-process push borrows the caller's span until it
// completes.
class test_server final : public crpc::io::source {
public:
    test_server() = default;

    void attach(crpc::connection* peer) noexcept { peer_ = peer; }

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            received_.insert(received_.end(), data.begin(), data.end());
        }
        cv_.notify_all();
        return data.size();
    }

    void on_eof() noexcept override {
        std::lock_guard<std::mutex> lk(mutex_);
        eof_ = true;
        cv_.notify_all();
    }

    void on_error(std::error_code) noexcept override {
        std::lock_guard<std::mutex> lk(mutex_);
        error_ = true;
        cv_.notify_all();
    }

    void push(const std::vector<std::byte>& bytes) {
        buffers_.push_back(bytes);
        const std::vector<std::byte>& stable = buffers_.back();
        peer_->sink().push(crpc::span<const std::byte>(stable.data(), stable.size()),
                           &completion_);
    }

    bool wait_received(std::size_t count, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return received_.size() >= count || eof_ || error_;
        });
    }

    std::size_t received_bytes() {
        std::lock_guard<std::mutex> lk(mutex_);
        return received_.size();
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mutex_);
        received_.clear();
        eof_ = false;
        error_ = false;
    }

private:
    crpc::connection* peer_;
    noop_completion completion_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::byte> received_;
    std::deque<std::vector<std::byte>> buffers_;
    bool eof_ = false;
    bool error_ = false;
};

// Accepts the server side of every connection the client establishes. The
// listener hands the connection over synchronously on the connecting thread.
class server_holder final : public crpc::accept_sink {
public:
    // Every accepted connection is kept alive. A pool acquires several
    // connections; destroying the earlier server side would deliver EOF to the
    // still-idle client connection and make the pool drop it.
    void on_accept(std::unique_ptr<crpc::connection> conn) noexcept override {
        conn->bind_source(&server_);
        server_.attach(conn.get());
        conns_.push_back(std::move(conn));
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ++accepts_;
        }
        cv_.notify_all();
    }

    bool wait_accepts(int count, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [&] { return accepts_ >= count; });
    }

    void push(const std::vector<std::byte>& bytes) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (conns_.empty()) {
            return;
        }
        server_.attach(conns_.back().get());
        server_.push(bytes);
    }

    bool wait_received(std::size_t count, int timeout_ms) {
        return server_.wait_received(count, timeout_ms);
    }

    std::size_t received_bytes() { return server_.received_bytes(); }

    void reset() { server_.reset(); }

    void close_peer() {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& conn : conns_) {
            if (conn != nullptr) {
                conn->close();
            }
        }
    }

    test_server& server() { return server_; }

private:
    test_server server_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<crpc::connection>> conns_;
    int accepts_ = 0;
};

// A listener plus its accept sink, so a `connection` can be built the same way
// a production client builds one.
struct harness {
    crpc::inproc_backend backend;
    crpc::endpoint ep{crpc::endpoint::inproc("crpc-redis-b")};
    std::unique_ptr<crpc::listener> listener;
    server_holder server;

    harness() {
        listener = backend.listen(ep, crpc::listen_options{});
        if (listener != nullptr) {
            listener->start(&server);
        }
    }

    crpc::redis::connection_config config() const {
        crpc::redis::connection_config cfg;
        cfg.ep = ep;
        return cfg;
    }
};

// Hands one already-created in-process client connection to the connection
// layer. Tests that need direct access to the peer (fault injection, a pausing
// source) build an `inproc_pair` and use this instead of a listener.
class pair_backend final : public crpc::backend_factory {
public:
    explicit pair_backend(std::unique_ptr<crpc::connection> client)
        : client_(std::move(client)) {}

    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return std::move(client_);
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                          const crpc::listen_options&) override {
        return nullptr;
    }

private:
    std::unique_ptr<crpc::connection> client_;
};

// Counts connect attempts while delegating to a real in-process backend. The
// backend has no listener, so every attempt fails; the counter is what proves a
// hard reconnect cap stops further attempts.
class counting_backend final : public crpc::backend_factory {
public:
    std::unique_ptr<crpc::connection> connect(
        const crpc::endpoint& ep, const crpc::connect_options& opt) override {
        connects_.fetch_add(1);
        return inner_.connect(ep, opt);
    }

    std::unique_ptr<crpc::listener> listen(
        const crpc::endpoint& ep, const crpc::listen_options& opt) override {
        return inner_.listen(ep, opt);
    }

    int connects() const noexcept { return connects_.load(); }

private:
    crpc::inproc_backend inner_;
    std::atomic<int> connects_{0};
};

// A peer that accepts nothing until released. Returning 0 from `on_data` is the
// io backpressure contract, so the in-process backend retains the tail and the
// writer's completion never runs while paused.
class pausing_server final : public crpc::io::source {
public:
    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        offered_.fetch_add(data.size());
        return consume_.load(std::memory_order_acquire) ? data.size() : 0;
    }

    void on_eof() noexcept override { eof_.store(true, std::memory_order_release); }
    void on_error(std::error_code) noexcept override {
        error_.store(true, std::memory_order_release);
    }

    void consume(bool value) noexcept {
        consume_.store(value, std::memory_order_release);
    }

    std::size_t offered() const noexcept {
        return offered_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> consume_{false};
    std::atomic<std::size_t> offered_{0};
    std::atomic<bool> eof_{false};
    std::atomic<bool> error_{false};
};

// Runs `warmup()` under a watchdog. A best-effort warmup must return even when
// the endpoint is unreachable (or already warm); closing the pool on timeout is
// the only way the old unbounded loop could ever be stopped.
bool warmup_with_timeout(crpc::redis::connection_pool& pool, int timeout_ms) {
    std::atomic<bool> done{false};
    std::thread worker([&pool, &done] {
        pool.warmup();
        done.store(true, std::memory_order_release);
    });
    const bool finished =
        crpc_test::wait_for([&] { return done.load(std::memory_order_acquire); },
                            timeout_ms);
    if (!finished) {
        pool.close();
    }
    worker.join();
    return finished;
}

} // namespace

CRPC_TEST(connection_write_read_exact) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);
    CRPC_CHECK(conn.is_open());

    const std::vector<std::byte> cmd = to_bytes("PING\r\n");
    const crpc::status wrote =
        conn.write(crpc::span<const std::byte>(cmd.data(), cmd.size()));
    CRPC_CHECK(wrote.ok());
    CRPC_CHECK(h.server.wait_received(cmd.size(), 2000));

    const std::vector<std::byte> reply = to_bytes("+PONG\r\n");
    h.server.push(reply);

    const crpc::result<std::vector<std::byte>> got = conn.read_exact(reply.size());
    CRPC_CHECK(got.ok());
    CRPC_CHECK(equals(reply, got.value()));
}

CRPC_TEST(connection_read_until_delimiter) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);

    const std::vector<std::byte> stream = to_bytes("$5\r\nhello\r\n$5\r\nworld\r\n");
    h.server.push(stream);

    const crpc::result<std::vector<std::byte>> first = conn.read_until(std::byte{'\n'});
    CRPC_CHECK(first.ok());
    CRPC_CHECK(equals(to_bytes("$5\r\n"), first.value()));

    const crpc::result<std::vector<std::byte>> second = conn.read_until(std::byte{'\n'});
    CRPC_CHECK(second.ok());
    CRPC_CHECK(equals(to_bytes("hello\r\n"), second.value()));

    const crpc::result<std::vector<std::byte>> third = conn.read_until(std::byte{'\n'});
    CRPC_CHECK(third.ok());
    CRPC_CHECK(equals(to_bytes("$5\r\n"), third.value()));

    const crpc::result<std::vector<std::byte>> fourth = conn.read_until(std::byte{'\n'});
    CRPC_CHECK(fourth.ok());
    CRPC_CHECK(equals(to_bytes("world\r\n"), fourth.value()));
}

CRPC_TEST(connection_read_available_drains_buffer) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);

    const std::vector<std::byte> payload = to_bytes("abc");
    h.server.push(payload);
    CRPC_CHECK(crpc_test::wait_for([&] { return conn.buffered() == payload.size(); },
                                   2000));

    const std::vector<std::byte> got = conn.read_available();
    CRPC_CHECK(equals(payload, got));
    CRPC_CHECK_EQ(std::size_t{0}, conn.buffered());
}

CRPC_TEST(connection_backpressure_pauses_and_resumes) {
    harness h;
    crpc::redis::connection_config cfg = h.config();
    cfg.max_buffered_bytes = 8;
    crpc::redis::connection conn(cfg, h.backend);

    const std::vector<std::byte> payload = to_bytes("0123456789abcdef");
    h.server.push(payload);

    // The connection consumes only up to the high-water mark and pauses; the
    // backend retains the tail until the application drains the buffer.
    CRPC_CHECK(crpc_test::wait_for([&] { return conn.buffered() == 8; }, 2000));
    CRPC_CHECK_EQ(std::size_t{8}, conn.buffered());

    const std::vector<std::byte> first = conn.read_available();
    CRPC_CHECK_EQ(std::size_t{8}, first.size());

    // Draining below the mark resumed the read, so the tail is delivered.
    CRPC_CHECK(crpc_test::wait_for([&] { return conn.buffered() == 8; }, 2000));
    const std::vector<std::byte> second = conn.read_available();
    CRPC_CHECK_EQ(std::size_t{8}, second.size());

    std::vector<std::byte> all;
    all.insert(all.end(), first.begin(), first.end());
    all.insert(all.end(), second.begin(), second.end());
    CRPC_CHECK(equals(payload, all));
}

CRPC_TEST(connection_auto_reconnects_after_peer_close) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);
    CRPC_CHECK(conn.is_open());

    const std::vector<std::byte> first_cmd = to_bytes("GET a\r\n");
    CRPC_CHECK(conn
                   .write(crpc::span<const std::byte>(first_cmd.data(),
                                                      first_cmd.size()))
                   .ok());
    CRPC_CHECK(h.server.wait_received(first_cmd.size(), 2000));
    h.server.reset();

    h.server.close_peer();
    CRPC_CHECK(crpc_test::wait_for([&] { return !conn.is_open(); }, 2000));
    CRPC_CHECK_EQ(std::uint64_t{0}, conn.reconnect_count());

    const std::vector<std::byte> second_cmd = to_bytes("GET b\r\n");
    CRPC_CHECK(conn
                   .write(crpc::span<const std::byte>(second_cmd.data(),
                                                      second_cmd.size()))
                   .ok());
    CRPC_CHECK_EQ(std::uint64_t{1}, conn.reconnect_count());
    CRPC_CHECK(h.server.wait_accepts(2, 2000));
    CRPC_CHECK(h.server.wait_received(second_cmd.size(), 2000));
}

CRPC_TEST(connection_close_is_idempotent) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);
    CRPC_CHECK(conn.is_open());
    conn.close();
    CRPC_CHECK_FALSE(conn.is_open());
    conn.close();
    CRPC_CHECK_FALSE(conn.is_open());
}

CRPC_TEST(pool_acquire_release_reuses_idle) {
    harness h;
    crpc::redis::pool_config pc;
    pc.connection = h.config();
    pc.max_size = 2;
    crpc::redis::connection_pool pool(pc, h.backend);

    const crpc::result<std::shared_ptr<crpc::redis::connection>> a = pool.acquire();
    CRPC_CHECK(a.ok());
    const crpc::result<std::shared_ptr<crpc::redis::connection>> b = pool.acquire();
    CRPC_CHECK(b.ok());
    CRPC_CHECK_EQ(std::size_t{2}, pool.total());
    CRPC_CHECK_EQ(std::size_t{2}, pool.in_use());

    // Keep the first connection alive so the pool hands it back out.
    std::shared_ptr<crpc::redis::connection> kept = a.value();
    pool.release(a.value());
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    const crpc::result<std::shared_ptr<crpc::redis::connection>> c = pool.acquire();
    CRPC_CHECK(c.ok());
    CRPC_CHECK(c.value().get() == kept.get());
    CRPC_CHECK_EQ(std::size_t{2}, pool.in_use());
}

CRPC_TEST(pool_exhaustion_fails_with_resource_exhausted) {
    harness h;
    crpc::redis::pool_config pc;
    pc.connection = h.config();
    pc.max_size = 1;
    pc.acquire_timeout = std::chrono::milliseconds(100);
    crpc::redis::connection_pool pool(pc, h.backend);

    const crpc::result<std::shared_ptr<crpc::redis::connection>> a = pool.acquire();
    CRPC_CHECK(a.ok());

    const crpc::result<std::shared_ptr<crpc::redis::connection>> b = pool.acquire();
    CRPC_CHECK_FALSE(b.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(b.code()));

    pool.release(a.value());
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());
}

CRPC_TEST(pool_close_rejects_acquire) {
    harness h;
    crpc::redis::pool_config pc;
    pc.connection = h.config();
    pc.max_size = 1;
    crpc::redis::connection_pool pool(pc, h.backend);

    const crpc::result<std::shared_ptr<crpc::redis::connection>> a = pool.acquire();
    CRPC_CHECK(a.ok());
    pool.release(a.value());
    CRPC_CHECK_EQ(std::size_t{1}, pool.idle());

    pool.close();
    const crpc::result<std::shared_ptr<crpc::redis::connection>> b = pool.acquire();
    CRPC_CHECK_FALSE(b.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  static_cast<int>(b.code()));
}

CRPC_TEST(connection_write_does_not_leak_completions) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);
    const std::vector<std::byte> payload(16, static_cast<std::byte>(0x5a));
    const crpc::span<const std::byte> bytes(payload.data(), payload.size());

    CRPC_CHECK(conn.write(bytes).ok());
    CRPC_CHECK(h.server.wait_received(payload.size(), 2000));
    h.server.reset();

#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtMemState before;
    _CrtMemState after;
    _CrtMemState diff;
    _CrtMemCheckpoint(&before);
#endif

    constexpr int iterations = 2000;
    for (int i = 0; i < iterations; ++i) {
        CRPC_CHECK(conn.write(bytes).ok());
        h.server.reset();
    }

#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtMemCheckpoint(&after);
    const int differs = _CrtMemDifference(&diff, &before, &after);
    CRPC_CHECK_EQ(0, differs);
#endif

    CRPC_CHECK_EQ(static_cast<std::uint64_t>(iterations + 1) * payload.size(),
                  conn.bytes_written());
}

CRPC_TEST(connection_write_timeout_is_not_reported_as_success) {
    crpc::inproc_options opts;
    opts.max_queued_bytes = 8;
    auto pair = crpc::inproc_pair::make_typed(opts);
    pausing_server server;
    pair.second->bind_source(&server);
    pair_backend backend(std::move(pair.first));

    crpc::redis::connection_config cfg;
    cfg.ep = crpc::endpoint::inproc("crpc-redis-b-paused");
    cfg.operation_timeout = std::chrono::milliseconds(100);
    crpc::redis::connection conn(cfg, backend);
    CRPC_CHECK(conn.is_open());

    const std::vector<std::byte> payload(64, static_cast<std::byte>(0x41));
    const crpc::status wrote = conn.write(
        crpc::span<const std::byte>(payload.data(), payload.size()));
    const bool open_after_timeout = conn.is_open();
    conn.close();

    CRPC_CHECK_FALSE(wrote.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(wrote.code()));
    CRPC_CHECK_EQ(std::uint64_t{0}, conn.bytes_written());
    CRPC_CHECK_FALSE(open_after_timeout);
    CRPC_CHECK_EQ(std::size_t{8}, server.offered());
}

CRPC_TEST(connection_max_reconnects_is_a_hard_cap) {
    counting_backend backend;
    crpc::redis::connection_config cfg;
    cfg.ep = crpc::endpoint::inproc("crpc-redis-b-missing");
    cfg.reconnect_backoff = std::chrono::nanoseconds{0};
    cfg.reconnect_max_backoff = std::chrono::nanoseconds{0};
    cfg.max_reconnects = 2;
    crpc::redis::connection conn(cfg, backend);

    CRPC_CHECK_FALSE(conn.is_open());
    const std::vector<std::byte> payload = to_bytes("x");
    const crpc::span<const std::byte> bytes(payload.data(), payload.size());

    const crpc::status first = conn.write(bytes);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(first.code()));

    const crpc::status second = conn.write(bytes);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                  static_cast<int>(second.code()));

    const crpc::status third = conn.write(bytes);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(third.code()));

    // One initial connect plus exactly max_reconnects retries; the cap stops
    // further attempts.
    CRPC_CHECK_EQ(3, backend.connects());
}

CRPC_TEST(connection_read_propagates_backend_error) {
    auto pair = crpc::inproc_pair::make_typed();
    pair_backend backend(std::move(pair.first));

    crpc::redis::connection_config cfg;
    cfg.ep = crpc::endpoint::inproc("crpc-redis-b-error");
    crpc::redis::connection conn(cfg, backend);
    CRPC_CHECK(conn.is_open());

    noop_completion done;
    const std::vector<std::byte> head = to_bytes("abc");
    pair.second->sink().push(
        crpc::span<const std::byte>(head.data(), head.size()), &done);
    CRPC_CHECK(crpc_test::wait_for([&] { return conn.buffered() == 3; }, 2000));

    pair.second->faults().inject_read_error =
        std::make_error_code(std::errc::connection_reset);
    const std::vector<std::byte> extra = to_bytes("x");
    pair.second->sink().push(
        crpc::span<const std::byte>(extra.data(), extra.size()), &done);
    CRPC_CHECK(crpc_test::wait_for([&] { return !conn.is_open(); }, 2000));

    const crpc::result<std::vector<std::byte>> got = conn.read_exact(8);
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                  static_cast<int>(got.code()));

    const crpc::result<std::vector<std::byte>> line = conn.read_until(std::byte{'\n'});
    CRPC_CHECK_FALSE(line.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                  static_cast<int>(line.code()));
}

CRPC_TEST(connection_read_reports_error_before_reconnecting) {
    auto pair = crpc::inproc_pair::make_typed();
    pair_backend backend(std::move(pair.first));

    crpc::redis::connection_config cfg;
    cfg.ep = crpc::endpoint::inproc("crpc-redis-b-error-empty");
    crpc::redis::connection conn(cfg, backend);
    CRPC_CHECK(conn.is_open());

    pair.second->faults().inject_read_error =
        std::make_error_code(std::errc::connection_reset);
    noop_completion done;
    const std::vector<std::byte> extra = to_bytes("x");
    pair.second->sink().push(
        crpc::span<const std::byte>(extra.data(), extra.size()), &done);
    CRPC_CHECK(crpc_test::wait_for([&] { return !conn.is_open(); }, 2000));
    CRPC_CHECK_EQ(std::size_t{0}, conn.buffered());

    const crpc::result<std::vector<std::byte>> got = conn.read_exact(1);
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                  static_cast<int>(got.code()));
}

CRPC_TEST(connection_close_is_terminal) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);
    CRPC_CHECK(conn.is_open());

    conn.close();
    CRPC_CHECK_FALSE(conn.is_open());

    const std::vector<std::byte> payload = to_bytes("x");
    const crpc::status wrote =
        conn.write(crpc::span<const std::byte>(payload.data(), payload.size()));
    CRPC_CHECK_FALSE(wrote.ok());
    CRPC_CHECK_EQ(std::uint64_t{0}, conn.reconnect_count());
    CRPC_CHECK_FALSE(h.server.wait_accepts(2, 200));

    const crpc::result<std::vector<std::byte>> got = conn.read_exact(1);
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK_EQ(std::uint64_t{0}, conn.reconnect_count());
    CRPC_CHECK_FALSE(conn.is_open());
}

CRPC_TEST(pool_acquire_discards_dead_idle_connection) {
    harness h;
    crpc::redis::pool_config pc;
    pc.connection = h.config();
    pc.min_size = 0;
    pc.max_size = 1;
    crpc::redis::connection_pool pool(pc, h.backend);

    const crpc::result<std::shared_ptr<crpc::redis::connection>> a = pool.acquire();
    CRPC_CHECK(a.ok());
    std::shared_ptr<crpc::redis::connection> kept = a.value();
    pool.release(a.value());
    CRPC_CHECK(crpc_test::wait_for([&] { return pool.idle() == 1; }, 2000));

    h.server.close_peer();
    CRPC_CHECK(crpc_test::wait_for([&] { return !kept->is_open(); }, 2000));

    const crpc::result<std::shared_ptr<crpc::redis::connection>> b = pool.acquire();
    CRPC_CHECK(b.ok());
    CRPC_CHECK(b.value().get() != kept.get());
    CRPC_CHECK(b.value()->is_open());
    CRPC_CHECK_EQ(std::size_t{1}, pool.total());
    CRPC_CHECK_EQ(std::size_t{0}, pool.idle());
    CRPC_CHECK_EQ(std::size_t{1}, pool.in_use());
    pool.release(b.value());
}

CRPC_TEST(connection_reconnect_forces_new_connection) {
    harness h;
    crpc::redis::connection conn(h.config(), h.backend);
    CRPC_CHECK(conn.is_open());
    CRPC_CHECK_EQ(std::uint64_t{0}, conn.reconnect_count());

    const crpc::status re = conn.reconnect();
    CRPC_CHECK(re.ok());
    CRPC_CHECK_EQ(std::uint64_t{1}, conn.reconnect_count());
    CRPC_CHECK(conn.is_open());
    CRPC_CHECK(h.server.wait_accepts(2, 2000));

    const std::vector<std::byte> payload = to_bytes("PING\r\n");
    CRPC_CHECK(conn
                   .write(crpc::span<const std::byte>(payload.data(),
                                                      payload.size()))
                   .ok());
    CRPC_CHECK(h.server.wait_received(payload.size(), 2000));
}

CRPC_TEST(pool_warmup_returns_when_endpoint_is_unreachable) {
    crpc::inproc_backend backend;
    crpc::redis::pool_config pc;
    pc.connection.ep = crpc::endpoint::inproc("crpc-redis-b-warmup-missing");
    pc.connection.reconnect_backoff = std::chrono::nanoseconds{0};
    pc.connection.reconnect_max_backoff = std::chrono::nanoseconds{0};
    pc.min_size = 2;
    pc.max_size = 4;
    pc.acquire_timeout = std::chrono::milliseconds(100);
    crpc::redis::connection_pool pool(pc, backend);

    const bool finished = warmup_with_timeout(pool, 1500);
    CRPC_CHECK(finished);
    CRPC_CHECK_EQ(std::size_t{0}, pool.total());
    CRPC_CHECK_EQ(std::size_t{0}, pool.idle());
}

CRPC_TEST(pool_warmup_reaches_min_size_without_spinning) {
    harness h;
    crpc::redis::pool_config pc;
    pc.connection = h.config();
    pc.min_size = 2;
    pc.max_size = 4;
    crpc::redis::connection_pool pool(pc, h.backend);

    const bool finished = warmup_with_timeout(pool, 1500);
    CRPC_CHECK(finished);
    CRPC_CHECK_EQ(std::size_t{2}, pool.total());
    CRPC_CHECK_EQ(std::size_t{2}, pool.idle());
}

CRPC_TEST(connection_concurrent_write_and_close_is_safe) {
    harness h;
    crpc::redis::connection_config cfg = h.config();
    cfg.operation_timeout = std::chrono::milliseconds(200);
    crpc::redis::connection conn(cfg, h.backend);

    std::atomic<bool> stop{false};
    std::atomic<int> ok_count{0};
    const std::vector<std::byte> payload = to_bytes("PING\r\n");
    const crpc::span<const std::byte> bytes(payload.data(), payload.size());

    std::thread writer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (conn.write(bytes).ok()) {
                ok_count.fetch_add(1);
            }
        }
    });

    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        conn.close();
        (void)conn.reconnect();
    }

    stop.store(true, std::memory_order_release);
    writer.join();
    conn.close();

    CRPC_CHECK(ok_count.load() > 0);
}

#if defined(CRPC_HAVE_COROCC)

namespace {

struct coro_job {
    crpc::redis::connection* conn = nullptr;
    harness* h = nullptr;
    std::atomic<int> phase{0};
    std::atomic<bool> done{false};
    std::vector<std::byte> reply;
    int code = 0;
};

void coro_entry(void* ctx) {
    auto* job = static_cast<coro_job*>(ctx);
    const std::vector<std::byte> cmd = to_bytes("ECHO hi\r\n");
    const crpc::status wrote =
        job->conn->write(crpc::span<const std::byte>(cmd.data(), cmd.size()));
    if (!wrote.ok()) {
        job->code = static_cast<int>(wrote.code());
        job->phase.store(2, std::memory_order_release);
        job->done.store(true, std::memory_order_release);
        return;
    }
    job->phase.store(1, std::memory_order_release);
    const crpc::result<std::vector<std::byte>> got = job->conn->read_exact(8);
    if (!got.ok()) {
        job->code = static_cast<int>(got.code());
    } else {
        job->reply = got.value();
    }
    job->phase.store(2, std::memory_order_release);
    job->done.store(true, std::memory_order_release);
}

} // namespace

CRPC_TEST(connection_coroutine_parks_and_resumes) {
    harness h;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    crpc::redis::connection conn(h.config(), h.backend, &shard);

    coro_job job;
    job.conn = &conn;
    job.h = &h;
    CRPC_CHECK(shard.spawn(&coro_entry, &job));

    // The coroutine writes, then reaches read_exact and parks.
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return h.server.received_bytes() >= std::strlen("ECHO hi\r\n"); },
        3000));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job.phase.load(std::memory_order_acquire) == 1; }, 3000));
    // Give the coroutine time to enter read_exact and park before answering:
    // the reply must arrive through the parked wait, not a ready buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CRPC_CHECK_FALSE(job.done.load(std::memory_order_acquire));

    const std::vector<std::byte> reply = to_bytes("hi there");
    h.server.push(reply);

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job.done.load(std::memory_order_acquire); }, 3000));
    CRPC_CHECK_EQ(2, job.phase.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(0, job.code);
    CRPC_CHECK(equals(reply, job.reply));

    conn.close();
    shard.stop();
    shard.join();
}

namespace {

struct posted_read_job {
    crpc::redis::connection* conn = nullptr;
    std::atomic<int> phase{0};
    std::atomic<bool> done{false};
    std::vector<std::byte> reply;
    int code = 0;
};

void posted_read_entry(void* ctx) noexcept {
    auto* job = static_cast<posted_read_job*>(ctx);
    job->phase.store(1, std::memory_order_release);
    try {
        const crpc::result<std::vector<std::byte>> got = job->conn->read_exact(8);
        if (!got.ok()) {
            job->code = static_cast<int>(got.code());
        } else {
            job->reply = got.value();
        }
    } catch (...) {
        job->code = static_cast<int>(crpc::rpc_code::internal);
    }
    job->phase.store(2, std::memory_order_release);
    job->done.store(true, std::memory_order_release);
}

} // namespace

CRPC_TEST(connection_shard_task_without_coroutine_falls_back) {
    harness h;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    crpc::redis::connection conn(h.config(), h.backend, &shard);

    posted_read_job job;
    job.conn = &conn;
    CRPC_CHECK(shard.post(&posted_read_entry, &job));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job.phase.load(std::memory_order_acquire) == 1; }, 2000));

    // A coroutine-less task on the shard thread must wait on the condition
    // variable instead of failing with failed_precondition.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CRPC_CHECK_FALSE(job.done.load(std::memory_order_acquire));

    const std::vector<std::byte> reply = to_bytes("fallback");
    h.server.push(reply);

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job.done.load(std::memory_order_acquire); }, 3000));
    CRPC_CHECK_EQ(0, job.code);
    CRPC_CHECK(equals(reply, job.reply));

    conn.close();
    shard.stop();
    shard.join();
}

#endif // CRPC_HAVE_COROCC

CRPC_TEST_MAIN()
