// The shared harness reads environment variables with std::getenv; MSVC's
// secure-CRT deprecation warning is noise for a test binary.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

// CRPC -- epoll transport backend tests.
//
// On Windows (and any non-Linux platform) the whole file collapses to a
// skipped test that prints an explicit message and exits 0, so the Windows
// build still passes while the epoll backend is Linux-only.
#if defined(__linux__)

#include <crpc/io/epoll_backend.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "test_framework.hpp"

namespace {

using namespace crpc;

span<const std::byte> bytes_of(const std::string& s) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()),
                                 s.size());
}

span<const std::byte> bytes_of(const std::vector<std::byte>& v) {
    return span<const std::byte>(v.data(), v.size());
}

std::string text_of(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

template <typename F>
bool wait_until(F pred, int timeout_ms = 3000) {
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return pred();
}

// Listener ports are drawn from below the kernel's ephemeral range.
//
// The old range (20000 + pid % 20000, plus up to 4000) reached ~44000, which
// sits inside Linux's default `net.ipv4.ip_local_port_range` of 32768-60999.
// Every outgoing connection this test makes takes an ephemeral port from that
// same range, so a later listener could be asked to bind a port the kernel had
// just handed to one of our own client sockets -- EADDRINUSE, and the fixture
// failed. It reproduced on 15 of 30 consecutive runs on an idle machine
// (verified against /proc/net/tcp: the TIME_WAIT entries were in 35552-35762,
// inside both ranges).
//
// Staying below 32768 keeps the two spaces disjoint. The range is still
// per-process so parallel test binaries rarely meet, and the fixture retries
// a few candidates so a genuine clash costs one attempt instead of a case.
constexpr int kPortFloor = 11000;
constexpr int kPortSpan = 8000;   // 11000..18999, entirely below 32768

std::uint16_t next_port() {
    static std::atomic<int> counter{0};
    // Both terms are offsets *within* the span, and the result is wrapped once
    // into [kPortFloor, kPortFloor + kPortSpan). Adding the pid offset first and
    // then wrapping would let the sum exceed the span and land outside the
    // intended window.
    const int pid_offset = static_cast<int>(::getpid()) % kPortSpan;
    const int step = static_cast<int>(counter.fetch_add(7) % 1000);
    return static_cast<std::uint16_t>(kPortFloor +
                                      (pid_offset + step) % kPortSpan);
}

struct recording_source final : io::source {
    mutable std::mutex m;
    std::vector<std::byte> received;
    std::size_t credit = static_cast<std::size_t>(-1);

    std::atomic<int> data_calls{0};
    std::atomic<int> eof_calls{0};
    std::atomic<int> error_calls{0};
    std::atomic<int> last_error{0};

    std::size_t on_data(span<const std::byte> data) noexcept override {
        data_calls.fetch_add(1);
        const std::size_t n = credit < data.size() ? credit : data.size();
        std::lock_guard<std::mutex> lk(m);
        received.insert(received.end(), data.begin(),
                        data.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    void on_eof() noexcept override { eof_calls.fetch_add(1); }
    void on_error(std::error_code ec) noexcept override {
        last_error.store(ec.value());
        error_calls.fetch_add(1);
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m);
        return received.size();
    }
    std::vector<std::byte> snapshot() const {
        std::lock_guard<std::mutex> lk(m);
        return received;
    }
};

struct recording_completion final : io::completion {
    std::atomic<int> calls{0};
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> ok{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> broken{false};

    void complete(io::io_result result) noexcept override {
        bytes.store(result.bytes);
        ok.store(!result.ec);
        canceled.store(result.ec == std::errc::operation_canceled);
        broken.store(result.ec == std::errc::broken_pipe);
        calls.fetch_add(1);
    }
};

struct null_completion final : io::completion {
    void complete(io::io_result) noexcept override {}
};

struct counting_completion final : io::completion {
    std::atomic<int> calls{0};
    std::atomic<bool> all_ok{true};
    void complete(io::io_result result) noexcept override {
        if (result.ec) {
            all_ok.store(false);
        }
        calls.fetch_add(1);
    }
};

// Echoes every received byte back on the same connection.  It queues into an
// owned buffer and pushes one borrowed chunk at a time, so it never violates
// the one-in-flight-push rule and it exercises the reentrant-completion path.
class echo_source final : public io::source {
public:
    explicit echo_source(connection* c) noexcept : conn_(c), done_(*this) {}

    std::size_t on_data(span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lk(m_);
            pending_.insert(pending_.end(), data.begin(), data.end());
        }
        pump();
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

private:
    struct done_t final : io::completion {
        explicit done_t(echo_source& o) noexcept : o_(o) {}
        void complete(io::io_result r) noexcept override { o_.finish(r); }
        echo_source& o_;
    };

    void finish(io::io_result) noexcept {
        {
            std::lock_guard<std::mutex> lk(m_);
            in_flight_ = false;
            out_.clear();
        }
        pump();
    }

    void pump() noexcept {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (in_flight_ || pending_.empty()) {
                return;
            }
            in_flight_ = true;
            out_ = std::move(pending_);
            pending_.clear();
        }
        conn_->sink().push(bytes_of(out_), &done_);
    }

    connection* conn_;
    done_t done_;
    std::mutex m_;
    std::vector<std::byte> pending_;
    std::vector<std::byte> out_;
    bool in_flight_ = false;
};

struct collecting_acceptor final : accept_sink {
    std::mutex m;
    std::vector<std::unique_ptr<connection>> conns;
    std::atomic<int> calls{0};

    void on_accept(std::unique_ptr<connection> c) noexcept override {
        {
            std::lock_guard<std::mutex> lk(m);
            conns.push_back(std::move(c));
        }
        calls.fetch_add(1);
    }

    std::unique_ptr<connection> take() {
        std::lock_guard<std::mutex> lk(m);
        if (conns.empty()) {
            return nullptr;
        }
        std::unique_ptr<connection> c = std::move(conns.front());
        conns.erase(conns.begin());
        return c;
    }
};

std::vector<std::byte> make_pattern(std::size_t n) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>('0' + (i % 10));
    }
    return v;
}

bool matches_pattern(const std::vector<std::byte>& v) {
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != static_cast<std::byte>('0' + (i % 10))) {
            return false;
        }
    }
    return true;
}

// Creates a listener + one connected client/server pair on a per-test port.
// The backend is owned outside so it can outlive every connection.
struct fixture {
    std::unique_ptr<listener> ln;
    collecting_acceptor acceptor;
    std::unique_ptr<connection> client;
    std::unique_ptr<connection> server;
    bool ok = false;

    // The connections are closed before the sources in the test body are
    // destroyed (the fixture is declared before them), so the loop thread can
    // never deliver to a dead source.
    ~fixture() { shutdown(); }

    void shutdown() {
        if (server != nullptr) {
            server->close();
        }
        if (client != nullptr) {
            client->close();
        }
    }

    fixture(io::epoll_backend& b, listen_options lo = listen_options{},
            connect_options co = connect_options{}) {
        // A port collision with another process (or a leftover listener) is not
        // a defect in the code under test, so it must cost another attempt, not
        // a failed case. Only a bind that fails this many times is reported.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::uint16_t port = next_port();
            ln = b.listen(endpoint::tcp("127.0.0.1", port), lo);
            if (ln == nullptr) {
                continue;
            }
            ln->start(&acceptor);
            client = b.connect(endpoint::tcp("127.0.0.1", port), co);
            if (client == nullptr) {
                ln->stop();
                ln.reset();
                continue;
            }
            if (!wait_until([&] { return acceptor.calls.load() == 1; })) {
                client->close();
                client.reset();
                ln->stop();
                ln.reset();
                continue;
            }
            server = acceptor.take();
            ok = server != nullptr;
            return;
        }
    }
};

// --- Tests ------------------------------------------------------------------

CRPC_TEST(epoll_create_and_fail_closed_on_bad_endpoints) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    auto& b = *be.value();
    CRPC_CHECK(b.connect(endpoint::inproc("nope"), connect_options{}) == nullptr);
    CRPC_CHECK(b.listen(endpoint::inproc("nope"), listen_options{}) == nullptr);
    CRPC_CHECK_EQ(0u, b.connection_count());
}

CRPC_TEST(epoll_listen_connect_and_echo) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    echo_source echo(f.server.get());
    recording_source client_rx;
    f.server->bind_source(&echo);
    f.client->bind_source(&client_rx);

    const std::string payload = "hello-epoll";
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);

    CRPC_CHECK(wait_until([&] { return client_rx.size() == payload.size(); }));
    CRPC_CHECK_EQ(payload, text_of(client_rx.snapshot()));
    CRPC_CHECK_EQ(2u, be.value()->connection_count());
    f.shutdown();
}

CRPC_TEST(epoll_one_mib_echo_small_queue_bounded_wakes) {
    io::epoll_options opt;
    opt.max_queued_bytes = 8192;
    opt.write_budget_bytes = 1024;
    auto be = io::epoll_backend::create(opt);
    CRPC_REQUIRE(be.ok());
    auto& b = *be.value();
    fixture f(b);
    CRPC_REQUIRE(f.ok);

    echo_source echo(f.server.get());
    recording_source client_rx;
    f.server->bind_source(&echo);
    f.client->bind_source(&client_rx);

    const std::vector<std::byte> payload = make_pattern(1024 * 1024);
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);

    CRPC_CHECK(wait_until([&] { return client_rx.size() == payload.size(); }, 20000));
    CRPC_CHECK_EQ(payload.size(), client_rx.size());
    CRPC_CHECK(matches_pattern(client_rx.snapshot()));

    // The budget path must self-post, but the wake count must stay bounded:
    // roughly bytes/budget, never one per loop iteration or per byte.
    const std::uint64_t wakes = b.r4_self_post_count();
    CRPC_CHECK(wakes > 0);
    CRPC_CHECK(wakes <= 8192);
    f.shutdown();
}

CRPC_TEST(epoll_reader_pause_and_resume) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    recording_source server_rx;
    server_rx.credit = 0;
    f.server->bind_source(&server_rx);

    const std::string payload = "paused-data";
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);

    CRPC_CHECK(wait_until([&] { return server_rx.data_calls.load() >= 1; }));
    CRPC_CHECK_EQ(0u, server_rx.size());

    server_rx.credit = static_cast<std::size_t>(-1);
    f.server->resume_read();
    CRPC_CHECK(wait_until([&] { return server_rx.size() == payload.size(); }));
    CRPC_CHECK_EQ(payload, text_of(server_rx.snapshot()));
    f.shutdown();
}

// Consumes one byte per callback and -- on the first callback -- resumes the
// reader from *inside* on_data, i.e. while the loop thread is still executing
// the callback and has not yet recorded the pause it is about to take for the
// unconsumed remainder. A resume-wake that lands in that window and is then
// overwritten by the pause would leave the remainder unread forever (no new
// edge arrives for bytes already sitting in the socket buffer), so this test
// is the red line for the lost-resume-wake window: it issues exactly one
// resume and then waits, without ever resuming again.
struct resume_mid_callback_source final : io::source {
    crpc::connection* owner = nullptr;
    mutable std::mutex m;
    std::vector<std::byte> received;
    std::atomic<int> data_calls{0};

    std::size_t on_data(span<const std::byte> data) noexcept override {
        data_calls.fetch_add(1);
        if (data_calls.load() == 1 && owner != nullptr) {
            owner->resume_read();
        }
        std::lock_guard<std::mutex> lk(m);
        if (received.empty() && !data.empty()) {
            received.push_back(data[0]);
            return 1;
        }
        received.insert(received.end(), data.begin(), data.end());
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m);
        return received.size();
    }
    std::vector<std::byte> snapshot() const {
        std::lock_guard<std::mutex> lk(m);
        return received;
    }
};

CRPC_TEST(epoll_resume_during_on_data_is_not_lost) {
// Red-line evidence for the lost-wake window: a resume_read() that lands
// *inside* on_data -- before the loop stashes the unconsumed remainder into
// rx_ and records the pause -- must queue a delivery demand. While a delivery
// attempt is in flight neither `read_paused_` nor `rx_tail_` holds yet, so
// the backend tracks the attempt with `delivering_` and resume_read() queues
// on it (src/io/epoll_backend.cpp, resume_read). Without that, the short
// consumption below takes the pause with pending_wakes_ == 0 and the retained
// bytes are never delivered: this test failed red until the window was
// closed, and now runs unconditionally.
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    resume_mid_callback_source server_rx;
    server_rx.owner = f.server.get();
    f.server->bind_source(&server_rx);

    const std::string payload = "resume-during-callback";
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);

    CRPC_CHECK(wait_until([&] { return server_rx.size() == payload.size(); }, 5000));
    CRPC_CHECK_EQ(payload, text_of(server_rx.snapshot()));
    f.shutdown();
}

CRPC_TEST(epoll_peer_close_delivers_eof_once) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    recording_source server_rx;
    f.server->bind_source(&server_rx);

    f.client->close();
    CRPC_CHECK(wait_until([&] { return server_rx.eof_calls.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CRPC_CHECK_EQ(1, server_rx.eof_calls.load());
    CRPC_CHECK_EQ(0, server_rx.error_calls.load());
    CRPC_CHECK_EQ(0, server_rx.data_calls.load());
    f.shutdown();
}

CRPC_TEST(epoll_abrupt_reset_delivers_error_once) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    listen_options lo;
    lo.recv_buffer_bytes = 8192;
    connect_options co;
    co.recv_buffer_bytes = 8192;
    co.send_buffer_bytes = 8192;
    fixture f(*be.value(), lo, co);
    CRPC_REQUIRE(f.ok);

    recording_source server_rx;
    server_rx.credit = 0;
    f.server->bind_source(&server_rx);
    recording_source client_rx;
    f.client->bind_source(&client_rx);

    const std::vector<std::byte> payload = make_pattern(1024 * 1024);
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);
    CRPC_CHECK(wait_until([&] { return server_rx.data_calls.load() >= 1; }));
    // Let the client fill the server's (small) receive queue, so the server
    // closes with unread bytes and the kernel answers with RST.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    f.server->close();
    CRPC_CHECK(wait_until([&] { return client_rx.error_calls.load() == 1; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CRPC_CHECK_EQ(1, client_rx.error_calls.load());
    CRPC_CHECK_EQ(0, client_rx.eof_calls.load());
    f.shutdown();
}

CRPC_TEST(epoll_close_cancels_inflight_write_once) {
    io::epoll_options opt;
    opt.max_queued_bytes = 4096;
    opt.write_budget_bytes = 4096;
    auto be = io::epoll_backend::create(opt);
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    // A reader that never consumes bytes keeps the peer's kernel buffers full,
    // so the large push cannot drain and its completion must stay deferred.
    recording_source server_rx;
    server_rx.credit = 0;
    f.server->bind_source(&server_rx);

    const std::vector<std::byte> payload = make_pattern(16 * 1024 * 1024);
    recording_completion big;
    f.client->sink().push(bytes_of(payload), &big);
    CRPC_CHECK_EQ(0, big.calls.load());

    f.client->close();
    CRPC_CHECK(wait_until([&] { return big.calls.load() == 1; }));
    CRPC_CHECK(big.canceled.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CRPC_CHECK_EQ(1, big.calls.load());
    CRPC_CHECK_FALSE(f.client->is_open());

    recording_completion after;
    f.client->sink().push(bytes_of(std::string("x")), &after);
    CRPC_CHECK_EQ(1, after.calls.load());
    CRPC_CHECK(after.broken.load());
    f.shutdown();
}

CRPC_TEST(epoll_hundred_concurrent_connections) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    auto& b = *be.value();

    const std::uint16_t port = next_port();
    collecting_acceptor acceptor;
    auto listener =
        b.listen(endpoint::tcp("127.0.0.1", port), listen_options{});
    // listener->start below dereferences it, so a bind failure must abort the
    // case instead of crashing on it: a crash would destroy the evidence of which
    // assertion failed.
    CRPC_REQUIRE(listener != nullptr);
    listener->start(&acceptor);

    constexpr int kN = 100;
    std::vector<std::unique_ptr<connection>> clients;
    for (int i = 0; i < kN; ++i) {
        auto c = b.connect(endpoint::tcp("127.0.0.1", port), connect_options{});
        CRPC_CHECK(c != nullptr);
        clients.push_back(std::move(c));
    }
    CRPC_CHECK(wait_until([&] { return acceptor.calls.load() == kN; }, 10000));
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2 * kN), b.connection_count());

    std::vector<std::unique_ptr<connection>> servers;
    {
        std::lock_guard<std::mutex> lk(acceptor.m);
        for (auto& c : acceptor.conns) {
            servers.push_back(std::move(c));
        }
    }

    std::vector<std::unique_ptr<echo_source>> echoes;
    std::vector<std::unique_ptr<recording_source>> recvs;
    for (int i = 0; i < kN; ++i) {
        echoes.emplace_back(new echo_source(servers[static_cast<std::size_t>(i)].get()));
        recvs.emplace_back(new recording_source());
        servers[static_cast<std::size_t>(i)]->bind_source(echoes.back().get());
        clients[static_cast<std::size_t>(i)]->bind_source(recvs.back().get());
    }

    std::vector<std::string> messages;
    std::vector<null_completion> completions(static_cast<std::size_t>(kN));
    for (int i = 0; i < kN; ++i) {
        messages.push_back("connection-" + std::to_string(i));
    }
    for (int i = 0; i < kN; ++i) {
        clients[static_cast<std::size_t>(i)]->sink().push(
            bytes_of(messages[static_cast<std::size_t>(i)]),
            &completions[static_cast<std::size_t>(i)]);
    }
    for (int i = 0; i < kN; ++i) {
        const std::size_t idx = static_cast<std::size_t>(i);
        CRPC_CHECK(wait_until([&] {
            return recvs[idx]->size() == messages[idx].size();
        }));
        CRPC_CHECK_EQ(messages[idx], text_of(recvs[idx]->snapshot()));
    }

    // Close every connection before the echo/recording sources go away.
    for (auto& c : clients) {
        c->close();
    }
    for (auto& s : servers) {
        s->close();
    }
}

CRPC_TEST(epoll_ten_thousand_messages_without_loss) {
    io::epoll_options opt;
    opt.max_queued_bytes = 64 * 1024;
    auto be = io::epoll_backend::create(opt);
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    recording_source server_rx;
    f.server->bind_source(&server_rx);

    const std::string unit = "0123456789ABCDEF"; // 16 bytes
    constexpr int kCount = 10000;
    counting_completion cc;
    int expected = 0;
    for (int i = 0; i < kCount; ++i) {
        const int before = cc.calls.load();
        f.client->sink().push(bytes_of(unit), &cc);
        if (!wait_until([&] { return cc.calls.load() > before; }, 5000)) {
            CRPC_CHECK(false);
            break;
        }
        expected += 1;
    }
    CRPC_CHECK_EQ(kCount, expected);
    CRPC_CHECK(cc.all_ok.load());

    const std::size_t total = unit.size() * static_cast<std::size_t>(kCount);
    CRPC_CHECK(wait_until([&] { return server_rx.size() == total; }, 20000));
    CRPC_CHECK_EQ(total, server_rx.size());

    const std::vector<std::byte> got = server_rx.snapshot();
    bool intact = true;
    for (std::size_t off = 0; off + unit.size() <= got.size();
         off += unit.size()) {
        if (std::memcmp(got.data() + off, unit.data(), unit.size()) != 0) {
            intact = false;
            break;
        }
    }
    CRPC_CHECK(intact);
    f.shutdown();
}

namespace {
std::atomic<int> g_timer_fired{0};
void timer_probe(void* arg) noexcept {
    static_cast<std::atomic<int>*>(arg)->fetch_add(1);
}
} // namespace

CRPC_TEST(epoll_bind_after_consumed_edge_still_delivers) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    recording_source server_rx;
    const std::string payload = "early-bytes";
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);
    CRPC_CHECK(wait_until([&] { return done.calls.load() == 1; }, 3000));

    // Give the loop time to consume the EPOLLIN edge while no source is bound.
    // The bytes stay buffered and no further edge will arrive in ET mode.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CRPC_CHECK_EQ(0, server_rx.data_calls.load());

    f.server->bind_source(&server_rx);
    // No further write: only bind_source's unconditional schedule() can
    // deliver the already-buffered bytes.
    CRPC_CHECK(wait_until([&] { return server_rx.size() == payload.size(); },
                          3000));
    CRPC_CHECK_EQ(payload.size(), server_rx.size());

    f.shutdown();
}

// A source that blocks inside on_data until the test releases it. It lets the
// test observe whether bind_source(nullptr) waits for the call to return.
struct blocking_source final : io::source {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::atomic<bool> exited{false};

    std::size_t on_data(span<const std::byte> data) noexcept override {
        {
            std::unique_lock<std::mutex> lk(m);
            entered = true;
            cv.notify_all();
            cv.wait(lk, [this] { return release; });
        }
        exited.store(true, std::memory_order_release);
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    bool wait_entered(int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [this] { return entered; });
    }
    void unblock() {
        {
            std::lock_guard<std::mutex> lk(m);
            release = true;
        }
        cv.notify_all();
    }
};

CRPC_TEST(epoll_bind_source_null_waits_for_in_flight_delivery) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    blocking_source rx;
    f.server->bind_source(&rx);

    const std::string payload = "in-flight";
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);
    CRPC_CHECK(rx.wait_entered());

    std::atomic<bool> unbind_returned{false};
    std::atomic<bool> exited_at_return{false};
    std::thread unbinder([&] {
        f.server->bind_source(nullptr);
        exited_at_return.store(rx.exited.load(std::memory_order_acquire));
        unbind_returned.store(true, std::memory_order_release);
    });

    // The callback is still blocked, so the unbind must not have returned.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CRPC_CHECK_FALSE(unbind_returned.load(std::memory_order_acquire));

    rx.unblock();
    unbinder.join();
    CRPC_CHECK(unbind_returned.load(std::memory_order_acquire));
    CRPC_CHECK(exited_at_return.load(std::memory_order_acquire));

    f.shutdown();
}

CRPC_TEST(epoll_push_completes_only_after_kernel_accepted) {
// Red-line evidence for the push-completion contract: when the bytes fit the
// send ring, push used to complete immediately -- while the bytes were still
// only in the user-space ring. A caller that closes right after the
// completion (the `Connection: close` pattern) then hits the abortive close,
// which discards undelivered queued bytes, and the peer loses the response.
// The loop thread is parked inside another connection's on_data, so no write
// pass can run while the test pushes: pre-fix the completion had already
// fired at that point (red), post-fix it stays pending until the bytes have
// been handed to the kernel (green). The second half proves the completed
// push survives an immediate close: the peer must observe every byte.
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    fixture f(*be.value());
    CRPC_REQUIRE(f.ok);

    recording_source server_rx;
    f.server->bind_source(&server_rx);

    // Park the loop thread inside the client connection's on_data.
    blocking_source park;
    f.client->bind_source(&park);
    const std::string probe = "park";
    recording_completion parked;
    f.server->sink().push(bytes_of(probe), &parked);
    CRPC_CHECK(park.wait_entered());

    // Exactly ring capacity: the push takes the "fully fits the ring" path.
    const std::string payload(64 * 1024, 'x');
    recording_completion done;
    f.client->sink().push(bytes_of(payload), &done);
    // The loop is parked inside on_data, so nothing can have reached the
    // kernel yet: the completion must still be pending.
    CRPC_CHECK_EQ(0, done.calls.load());

    park.unblock();
    CRPC_CHECK(wait_until([&] { return done.calls.load() == 1; }, 5000));

    // The completion now means kernel-accepted, so the abortive close that
    // follows must not discard the payload.
    f.client->close();
    CRPC_CHECK(wait_until([&] { return server_rx.size() == payload.size(); },
                          5000));
    CRPC_CHECK_EQ(payload.size(), server_rx.size());
    f.shutdown();
}

CRPC_TEST(epoll_submillisecond_timer_fires) {
    auto be = io::epoll_backend::create();
    CRPC_REQUIRE(be.ok());
    auto& b = *be.value();

    g_timer_fired.store(0);
    const io::timer_id id =
        b.arm_timer(std::chrono::microseconds(200), &timer_probe, &g_timer_fired);
    CRPC_CHECK(id != io::invalid_timer);
    CRPC_CHECK(wait_until([&] { return g_timer_fired.load() == 1; }, 2000));
    CRPC_CHECK_EQ(1, g_timer_fired.load());
    CRPC_CHECK_FALSE(b.cancel_timer(id)); // already fired
}

} // namespace

CRPC_TEST_MAIN()

#else // !defined(__linux__)

#include <cstdio>

int main() {
    std::printf("SKIP crpc_test_io_epoll: linux only; this binary executes nothing\n");
    return 0;
}

#endif // __linux__
