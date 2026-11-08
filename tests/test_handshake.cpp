// CRPC -- G08/P17 handshake pre-fixes (P1-P3).
//
// These cases pin the four defects the sealed plan's P1..P3 batch fixes:
//   * HS-36/HS-23 -- a GOAWAY's 8-byte payload (last_stream_id + rpc_code) must
//     be parsed; the old code threw the payload away and always reported
//     `shutting_down`.
//   * HS-55      -- the server must parse a peer GOAWAY (legal code stops
//     dispatch and tears down; a malformed/unknown code fails closed).
//   * HS-32      -- a SETTINGS header declaring a payload above 4 KiB is
//     rejected *before* the chunk is copied, including when the header is split
//     across chunks or the frame is preceded by another frame in the chunk.
//   * HS-24      -- a control frame whose staging allocation fails is an
//     explicit `resource_exhausted` close, not a silent drop; and there is no
//     fixed staging capacity that kills a batch of valid SETTINGS frames.
//
// Allocation-failure injection and the allocation byte counter replace this
// binary's global `operator new`, so the cases that use them are compiled out
// of a shared build (the library allocates inside the DLL and the replacement
// never sees those calls).
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/endian.hpp>
#include <crpc/frame.hpp>
#include <crpc/frame_compression.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

namespace {

std::atomic<std::size_t> g_alloc_bytes{0};
std::atomic<int> g_injected_alloc_failures{0};
thread_local bool g_fail_next_alloc = false;

} // namespace

void* operator new(std::size_t size) {
    g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    if (g_fail_next_alloc && size > 16) {
        g_fail_next_alloc = false;
        g_injected_alloc_failures.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace crpc {
inline std::ostream& operator<<(std::ostream& out, rpc_code code) {
    return out << to_string(code);
}
namespace rpc {
inline std::ostream& operator<<(std::ostream& out, call_state state) {
    return out << to_string(state);
}

// Test seam for the defensive re-entrant reader branch (M1). The production
// interface deliberately has no state-injection setter; `client` declares this
// type a friend (see include/crpc/rpc/client.hpp).
struct client_test_access {
    static void set_in_process(client& c, bool value) noexcept { c.in_process_ = value; }
    static bool reader_fatal(const client& c) noexcept { return c.reader_fatal_; }
    static std::size_t buffered(const client& c) noexcept { return c.decoder_.buffered(); }
};
} // namespace rpc
} // namespace crpc

namespace {

using namespace crpc;
using namespace crpc::rpc;
using namespace std::chrono_literals;

constexpr u32 kBigSettingsPayload = 16u * 1024u * 1024u;

struct noop_completion final : io::completion {
    void complete(io::io_result) noexcept override {}
};

// A connection whose sink captures writes (counting pushes) and whose inbound
// side is fed directly by the test through `client::source()`. It models a
// backend that delivers on a thread other than the writer, so the reader never
// re-enters the writer through a push. The push counter is shared so a test can
// still read it after the client has closed (and destroyed) the connection.
class manual_connection final : public connection {
public:
    manual_connection()
        : counter_(std::make_shared<std::atomic<std::size_t>>(0)),
          close_count_(std::make_shared<std::atomic<std::size_t>>(0)),
          sink_(*counter_) {}
    explicit manual_connection(std::shared_ptr<std::atomic<std::size_t>> counter)
        : counter_(std::move(counter)),
          close_count_(std::make_shared<std::atomic<std::size_t>>(0)),
          sink_(*counter_) {}
    manual_connection(std::shared_ptr<std::atomic<std::size_t>> counter,
                      std::shared_ptr<std::atomic<std::size_t>> close_count)
        : counter_(std::move(counter)), close_count_(std::move(close_count)), sink_(*counter_) {}

    io::source& source() noexcept override { return dummy_; }
    io::sink& sink() noexcept override { return sink_; }
    void bind_source(io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override {
        close_count_->fetch_add(1, std::memory_order_relaxed);
        open_ = false;
    }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] endpoint remote() const override { return endpoint::inproc("manual"); }

    [[nodiscard]] std::size_t pushes() const noexcept {
        return counter_->load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t close_calls() const noexcept {
        return close_count_->load(std::memory_order_relaxed);
    }

private:
    class dummy_source final : public io::source {
    public:
        std::size_t on_data(span<const std::byte> data) noexcept override { return data.size(); }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };
    class capture_sink final : public io::sink {
    public:
        explicit capture_sink(std::atomic<std::size_t>& counter) noexcept : counter_(counter) {}
        void push(span<const std::byte> data, io::completion* c) noexcept override {
            counter_.fetch_add(1, std::memory_order_relaxed);
            c->complete(io::io_result{{}, data.size()});
        }

    private:
        std::atomic<std::size_t>& counter_;
    };

    std::shared_ptr<std::atomic<std::size_t>> counter_;
    std::shared_ptr<std::atomic<std::size_t>> close_count_;
    dummy_source dummy_;
    capture_sink sink_;
    bool open_ = true;
};

span<const std::byte> bytes_of(const std::string& text) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::vector<std::byte> frame_bytes(frame_type type, u32 stream, span<const std::byte> payload) {
    frame value;
    value.header.type = type;
    value.header.stream_id = stream;
    value.payload = payload;
    std::vector<std::byte> out(encoded_frame_size(value));
    std::size_t written = 0;
    const status encoded = encode_frame(span<std::byte>(out.data(), out.size()), value, written);
    out.resize(encoded.ok() ? written : 0);
    return out;
}

std::vector<std::byte> goaway_payload(u32 last_stream_id, u32 code) {
    std::vector<std::byte> payload(8);
    endian::store_le32(payload.data(), last_stream_id);
    endian::store_le32(payload.data() + 4, code);
    return payload;
}

// The 16-byte fixed header of a frame whose declared payload length is
// `payload_len`. Only the header is needed by the early-rejection cases: the
// check under test happens before any payload byte is copied.
std::array<std::byte, frame_header_size> frame_header_bytes(frame_type type, u32 payload_len) {
    std::array<std::byte, frame_header_size> header{};
    endian::store_le32(header.data(), payload_len);
    header[4] = static_cast<std::byte>(static_cast<u8>(type));
    return header;
}

std::array<std::byte, frame_header_size> settings_header(u32 payload_len) {
    return frame_header_bytes(frame_type::settings, payload_len);
}

std::vector<std::byte> concat(std::initializer_list<span<const std::byte>> pieces) {
    std::size_t total = 0;
    for (const span<const std::byte> piece : pieces) {
        total += piece.size();
    }
    std::vector<std::byte> out;
    out.reserve(total);
    for (const span<const std::byte> piece : pieces) {
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

span<const std::byte> view(const std::vector<std::byte>& bytes) {
    return span<const std::byte>(bytes.data(), bytes.size());
}

// --- Hang watchdog -----------------------------------------------------------
//
// A regression of the transport-lifetime defect deadlocks the *test thread*
// (server::stop -> GOAWAY -> client::on_data -> self-wait). Without an external
// clock the whole suite would hang and report nothing. The watchdog runs on its
// own thread and turns that deadlock into a hard, fast failure (exit code 2 plus
// a stderr line), so a regression shows up as a failed test, not a wedged run.
struct watchdog_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool armed = true;
};

std::shared_ptr<watchdog_state> arm_watchdog(std::chrono::seconds budget, const char* label) {
    auto state = std::make_shared<watchdog_state>();
    std::thread([state, budget, label] {
        std::unique_lock<std::mutex> lk(state->mutex);
        if (state->cv.wait_for(lk, budget, [&state] { return !state->armed; })) {
            return;
        }
        std::fprintf(stderr,
                     "\nhandshake watchdog: %s did not finish within %lld s "
                     "(deadlock regression)\n",
                     label, static_cast<long long>(budget.count()));
        std::fflush(stderr);
        std::_Exit(2);
    }).detach();
    return state;
}

void disarm_watchdog(const std::shared_ptr<watchdog_state>& state) {
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        state->armed = false;
    }
    state->cv.notify_all();
}

struct watchdog_guard {
    std::shared_ptr<watchdog_state> state;
    ~watchdog_guard() { disarm_watchdog(state); }
};

bool wait_until_closed(client& c, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (c.is_open() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !c.is_open();
}

// Starts one reliable call that stays in flight because nothing ever answers.
call<std::string> start_inflight(client& c) {
    auto started = c.start_call<std::string>(1, 1, bytes_of("x"));
    return std::move(started).value();
}

std::vector<std::byte> success_response(u32 stream_id, const std::string& body) {
    buffer payload;
    (void)encode_response(rpc_code::ok, rpc_error{}, bytes_of(body), payload);
    return frame_bytes(frame_type::response, stream_id, payload.readable());
}

// --- Client GOAWAY parsing (HS-36 / HS-23) ----------------------------------

CRPC_TEST(handshake_hs36_client_goaway_code_mapping) {
    struct entry {
        const char* name;
        std::vector<std::byte> payload;
        rpc_code expected;
    };
    std::vector<entry> cases;
    cases.push_back({"ok", goaway_payload(3, static_cast<u32>(rpc_code::ok)),
                     rpc_code::shutting_down});
    cases.push_back({"permission_denied",
                     goaway_payload(0, static_cast<u32>(rpc_code::permission_denied)),
                     rpc_code::permission_denied});
    cases.push_back({"protocol_error",
                     goaway_payload(4, static_cast<u32>(rpc_code::protocol_error)),
                     rpc_code::protocol_error});
    cases.push_back({"shutting_down",
                     goaway_payload(0, static_cast<u32>(rpc_code::shutting_down)),
                     rpc_code::shutting_down});
    cases.push_back({"unknown_code", goaway_payload(0, 999u), rpc_code::protocol_error});
    cases.push_back({"short_payload", std::vector<std::byte>(7), rpc_code::protocol_error});
    cases.push_back({"long_payload", std::vector<std::byte>(9), rpc_code::protocol_error});

    for (entry& item : cases) {
        auto conn = std::make_unique<manual_connection>();
        client_options options;
        options.slot_capacity = 8;
        client c(std::move(conn), options);
        call<std::string> handle = start_inflight(c);
        CRPC_CHECK_FALSE(handle.ready());

        const std::vector<std::byte> wire = frame_bytes(frame_type::goaway, 0, view(item.payload));
        CRPC_CHECK(!wire.empty());
        (void)c.source().on_data(view(wire));

        CRPC_CHECK(handle.ready());
        if (handle.ready()) {
            CRPC_CHECK_EQ(item.expected, handle.get().code());
            CRPC_CHECK_EQ(0u, c.in_flight());
        }
        // A GOAWAY is terminal for the session: the connection must observe the
        // close and refuse new calls, not just fail the calls that were in
        // flight when it arrived.
        CRPC_CHECK_FALSE(c.is_open());
        auto rejected = c.start_call<std::string>(1, 1, bytes_of("y"));
        CRPC_CHECK_FALSE(rejected.ok());
    }
}

CRPC_TEST(handshake_hs23_goaway_protocol_error_is_not_shutting_down) {
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);

    // A peer that rejects the session (version/SETTINGS/protocol) reports it via
    // GOAWAY with rpc_code=protocol_error. The caller must see that code, not
    // the old fixed `shutting_down`.
    const std::vector<std::byte> payload =
        goaway_payload(0, static_cast<u32>(rpc_code::protocol_error));
    const std::vector<std::byte> wire = frame_bytes(frame_type::goaway, 0, view(payload));
    (void)c.source().on_data(view(wire));

    CRPC_CHECK(handle.ready());
    CRPC_CHECK_EQ(rpc_code::protocol_error, handle.get().code());
}

// D2: an EOF (or transport error) is terminal, but it does NOT hard-destroy the
// transport itself. A later user `close()` must still perform the deferred close
// instead of returning early just because `closed_` was already set.
CRPC_TEST(handshake_client_close_after_eof_still_closes_transport) {
    auto pushes = std::make_shared<std::atomic<std::size_t>>(0);
    auto closes = std::make_shared<std::atomic<std::size_t>>(0);
    auto conn = std::make_unique<manual_connection>(pushes, closes);
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);
    CRPC_CHECK_EQ(0u, closes->load(std::memory_order_relaxed));

    // Peer EOF: fails the in-flight call and sets `closed_`, but leaves the
    // transport object alive (the reader callback must not destroy it).
    c.source().on_eof();
    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::eof, handle.get().code());
    }
    CRPC_CHECK_FALSE(c.is_open());
    CRPC_CHECK_EQ(0u, closes->load(std::memory_order_relaxed));

    // A user close() must not be a no-op: it has to close the transport.
    // connection::close() is idempotent and may be entered more than once, so
    // the contract is "the connection is closed", not "close ran exactly once".
    // The connection object itself is released later by ~client, not here.
    c.close();
    CRPC_CHECK(closes->load(std::memory_order_relaxed) >= 1u);
    CRPC_CHECK_FALSE(c.is_open());
}

// --- Server GOAWAY parsing (HS-55) ------------------------------------------

struct counting_state {
    std::atomic<int> calls{0};
};

status counting_handler(void* self, const call_envelope&, span<const std::byte>, buffer&,
                        call_context&) {
    static_cast<counting_state*>(self)->calls.fetch_add(1, std::memory_order_relaxed);
    return status{};
}

class capture_source final : public io::source {
public:
    static constexpr std::size_t capture_capacity = 512;

    std::size_t on_data(span<const std::byte> data) noexcept override {
        const std::size_t have = size_.load(std::memory_order_relaxed);
        const std::size_t room = capture_capacity - have;
        const std::size_t take = data.size() < room ? data.size() : room;
        if (take != 0) {
            std::memcpy(bytes_.data() + have, data.data(), take);
            size_.store(have + take, std::memory_order_release);
        }
        got_data.store(true, std::memory_order_release);
        return data.size();
    }
    void on_eof() noexcept override { eof.store(true, std::memory_order_release); }
    void on_error(std::error_code) noexcept override { eof.store(true, std::memory_order_release); }

    span<const std::byte> captured() const noexcept {
        return span<const std::byte>(bytes_.data(), size_.load(std::memory_order_acquire));
    }

    std::atomic<bool> eof{false};
    std::atomic<bool> got_data{false};

private:
    std::array<std::byte, capture_capacity> bytes_{};
    std::atomic<std::size_t> size_{0};
};

// Scans `bytes` for a GOAWAY frame and reports its rpc_code. Used to prove that
// the server really emits GOAWAY(protocol_error) on the malformed path (the old
// test only proved "no dispatch", which a silently-dropped frame also satisfies).
bool find_goaway(span<const std::byte> bytes, rpc_code& code) {
    frame_decoder decoder;
    decoder.feed(bytes);
    frame value;
    for (;;) {
        const frame_decoder::result result = decoder.next(value);
        if (result != frame_decoder::result::frame_ready) {
            return false;
        }
        if (value.header.type == frame_type::goaway) {
            if (value.payload.size() != 8) {
                return false;
            }
            code = static_cast<rpc_code>(endian::load_le32(value.payload.data() + 4));
            return true;
        }
    }
}

struct running_server {
    inproc_backend backend;
    std::unique_ptr<server> srv;
    ~running_server() {
        if (srv) {
            srv->stop();
        }
    }
};

bool start_counting_server(running_server& rs, const std::string& name, counting_state& state) {
    server::builder builder;
    builder.add_listener(endpoint::inproc(name), rs.backend);
    builder.add_service(1, "svc");
    builder.add_raw_method(1, 1, "count", &counting_handler, &state);
    builder.shards(1);
    builder.handler_workers(2);
    rs.srv = builder.build();
    if (!rs.srv) {
        return false;
    }
    return rs.srv->start().ok();
}

std::vector<std::byte> request_frame() {
    call_envelope envelope;
    envelope.service_id = 1;
    envelope.method_id = 1;
    const std::size_t size = encoded_call_envelope_size(envelope);
    std::vector<std::byte> payload(size);
    std::size_t written = 0;
    (void)encode_call_envelope(span<std::byte>(payload.data(), payload.size()), envelope, written);
    payload.resize(written);
    return frame_bytes(frame_type::request, 1, view(payload));
}

bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return flag.load(std::memory_order_acquire);
}

bool wait_for_calls(const counting_state& state, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (state.calls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return state.calls.load(std::memory_order_acquire) != 0;
}

CRPC_TEST(handshake_hs55_server_legal_goaway_stops_dispatch) {
    running_server rs;
    counting_state state;
    const std::string name = "hs55-legal";
    CRPC_CHECK(start_counting_server(rs, name, state));

    auto conn = rs.backend.connect(endpoint::inproc(name), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    capture_source capture;
    conn->bind_source(&capture);

    const std::vector<std::byte> goaway =
        frame_bytes(frame_type::goaway, 0, view(goaway_payload(0, static_cast<u32>(rpc_code::ok))));
    const std::vector<std::byte> request = request_frame();
    const std::vector<std::byte> chunk = concat({view(goaway), view(request)});

    noop_completion done;
    conn->sink().push(view(chunk), &done);

    // A legal GOAWAY stops dispatch of the rest of the chunk. If the server
    // ignored it, the REQUEST that follows it would be executed.
    const bool dispatched = wait_for_calls(state, 500ms);
    CRPC_CHECK_FALSE(dispatched);
    CRPC_CHECK_EQ(0, state.calls.load(std::memory_order_acquire));
    CRPC_CHECK(wait_for(capture.eof, 2000ms));
    // A legal peer GOAWAY is not answered with our own GOAWAY: the teardown is
    // silent (only the close/EOF is observable).
    rpc_code echoed = rpc_code::ok;
    CRPC_CHECK_FALSE(find_goaway(capture.captured(), echoed));
}

CRPC_TEST(handshake_hs55_server_malformed_goaway_closes_without_dispatch) {
    running_server rs;
    counting_state state;
    const std::string name = "hs55-malformed";
    CRPC_CHECK(start_counting_server(rs, name, state));

    auto conn = rs.backend.connect(endpoint::inproc(name), connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    capture_source capture;
    conn->bind_source(&capture);

    watchdog_guard watchdog{arm_watchdog(std::chrono::seconds(10),
                                         "malformed goaway bounded close")};

    const std::vector<std::byte> goaway =
        frame_bytes(frame_type::goaway, 0, span<const std::byte>(goaway_payload(0, 0).data(), 7));
    const std::vector<std::byte> request = request_frame();
    const std::vector<std::byte> chunk = concat({view(goaway), view(request)});

    noop_completion done;
    conn->sink().push(view(chunk), &done);

    const bool dispatched = wait_for_calls(state, 500ms);
    CRPC_CHECK_FALSE(dispatched);
    CRPC_CHECK_EQ(0, state.calls.load(std::memory_order_acquire));
    // The malformed path must fail closed with an explicit GOAWAY carrying
    // `protocol_error`; the peer must be able to tell "peer rejected the
    // session" from a silent EOF. `capture_source` records the bytes the server
    // writes back to this connection.
    CRPC_CHECK(wait_for(capture.got_data, 2000ms));
    rpc_code code = rpc_code::ok;
    const bool saw_goaway = find_goaway(capture.captured(), code);
    CRPC_CHECK(saw_goaway);
    if (saw_goaway) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, code);
    }
    // Bounded teardown: the server queues GOAWAY(protocol_error), lets the
    // transport hand it over, then closes the transport itself — no stop() and
    // no peer action are needed. The 2 s budget is the bound; the watchdog
    // turns a hang into a failure instead of wedging the suite.
    CRPC_CHECK(wait_for(capture.eof, 2000ms));
}

// --- D1 transport lifetime over a real in-process transport ------------------
//
// server::stop() pushes GOAWAY synchronously through the inproc transport, so
// the client handles it *inside the server's push stack*. The pre-fix reader
// path destroyed the connection there: ~inproc_connection then waited for its
// own in-flight delivery count (inproc.cpp, delivery_cv) and self-deadlocked.
// The watchdog is essential: without it the whole suite would hang instead of
// failing.
CRPC_TEST(handshake_inproc_server_stop_observably_closes_client) {
    running_server rs;
    counting_state state;
    const std::string name = "hs-inproc-stop";
    CRPC_CHECK(start_counting_server(rs, name, state));

    auto physical = rs.backend.connect(endpoint::inproc(name), connect_options{});
    CRPC_CHECK(physical != nullptr);
    if (!physical) {
        return;
    }
    client_options options;
    options.slot_capacity = 8;
    options.default_timeout = std::chrono::nanoseconds{0};
    client c(std::move(physical), options);
    CRPC_CHECK(c.is_open());

    watchdog_guard watchdog{arm_watchdog(std::chrono::seconds(10), "inproc stop -> GOAWAY")};

    // Deadlocks on the broken implementation; the watchdog turns it into a
    // failure after 10 s instead of an infinite hang.
    rs.srv->stop();
    rs.srv.reset();

    // The close must be observable without waiting for ~client.
    CRPC_CHECK(wait_until_closed(c, std::chrono::milliseconds(2000)));
    CRPC_CHECK_FALSE(c.is_open());

    auto rejected = c.start_call<std::string>(1, 1, bytes_of("y"));
    CRPC_CHECK_FALSE(rejected.ok());
}

// --- 4 KiB pre-read (HS-32 (a)(b)(c)) ---------------------------------------

CRPC_TEST(handshake_hs32_split_header_big_settings_rejected_before_copy) {
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);

    const std::array<std::byte, frame_header_size> header = settings_header(kBigSettingsPayload);
    std::vector<std::byte> first(header.begin(), header.begin() + 15);
    std::vector<std::byte> second(1 + kBigSettingsPayload, std::byte{0});
    second[0] = header[15];

    (void)c.source().on_data(view(first));
    const std::size_t before = g_alloc_bytes.load(std::memory_order_relaxed);
    (void)c.source().on_data(view(second));
    [[maybe_unused]] const std::size_t delta = g_alloc_bytes.load(std::memory_order_relaxed) - before;

#if !defined(CRPC_SHARED)
    // The check runs on the completed header; the 16 MiB payload is never
    // copied. A regression that feeds the chunk first would allocate at least
    // the payload length here.
    CRPC_CHECK(delta < 1024u * 1024u);
#endif
    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, handle.get().code());
    }
}

CRPC_TEST(handshake_hs32_same_chunk_small_then_big_settings_rejected_before_copy) {
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);

    const std::array<std::byte, 8> ping_payload{};
    const std::vector<std::byte> small =
        frame_bytes(frame_type::ping, 0, span<const std::byte>(ping_payload.data(), ping_payload.size()));
    const std::array<std::byte, frame_header_size> header = settings_header(kBigSettingsPayload);
    // The per-frame check runs before the SETTINGS payload is consumed, so the
    // chunk only needs the header (any following bytes are irrelevant).
    const std::vector<std::byte> chunk = concat({view(small), span<const std::byte>(header.data(), header.size())});

    const std::size_t before = g_alloc_bytes.load(std::memory_order_relaxed);
    (void)c.source().on_data(view(chunk));
    [[maybe_unused]] const std::size_t delta = g_alloc_bytes.load(std::memory_order_relaxed) - before;

#if !defined(CRPC_SHARED)
    CRPC_CHECK(delta < 1024u * 1024u);
#endif
    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, handle.get().code());
    }
}

CRPC_TEST(handshake_hs32_residual_frame_then_big_settings_rejected_before_copy) {
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);

    // chunk1: only the 16-byte PING header. on_data leaves the frame incomplete
    // in the internal buffer (16 buffered, 8 payload bytes missing).
    const std::array<std::byte, frame_header_size> ping_header =
        frame_header_bytes(frame_type::ping, 8);
    (void)c.source().on_data(span<const std::byte>(ping_header.data(), ping_header.size()));

    // chunk2: the PING payload that completes the residual frame, immediately
    // followed by a 1 MiB SETTINGS frame. The pre-fix feed_external() fallback
    // copied this whole chunk into the internal buffer *before* the 4 KiB check
    // saw the SETTINGS header.
    const std::size_t big = 1024u * 1024u;
    const std::array<std::byte, frame_header_size> header =
        settings_header(static_cast<u32>(big));
    std::vector<std::byte> chunk;
    chunk.reserve(8 + header.size() + big);
    chunk.insert(chunk.end(), 8, std::byte{0}); // PING payload
    chunk.insert(chunk.end(), header.begin(), header.end());
    chunk.insert(chunk.end(), big, std::byte{0}); // SETTINGS payload

    const std::size_t before = g_alloc_bytes.load(std::memory_order_relaxed);
    (void)c.source().on_data(view(chunk));
    [[maybe_unused]] const std::size_t delta = g_alloc_bytes.load(std::memory_order_relaxed) - before;

#if !defined(CRPC_SHARED)
    // Only the 8 bytes that complete the PING frame may be copied. A regression
    // that falls back to feed(data) allocates the full 1 MiB payload here.
    CRPC_CHECK(delta < 1024u * 1024u);
#endif
    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, handle.get().code());
    }
}

CRPC_TEST(handshake_hs32_bytewise_header_big_settings_rejected_before_copy) {
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);

    const std::array<std::byte, frame_header_size> header = settings_header(kBigSettingsPayload);
    const std::size_t before = g_alloc_bytes.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < header.size(); ++i) {
        (void)c.source().on_data(span<const std::byte>(header.data() + i, 1));
        if (handle.ready()) {
            break;
        }
    }
    [[maybe_unused]] const std::size_t delta = g_alloc_bytes.load(std::memory_order_relaxed) - before;

#if !defined(CRPC_SHARED)
    CRPC_CHECK(delta < 1024u * 1024u);
#endif
    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, handle.get().code());
    }
}

// --- Control-frame staging (HS-24) ------------------------------------------

CRPC_TEST(handshake_hs24_control_staging_failure_is_explicit) {
#if !defined(CRPC_SHARED)
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);

    // A frame-layer-valid SETTINGS frame (24-byte payload). Staging it copies
    // the payload into `pending_frame::payload`; the injected allocation
    // failure hits exactly that copy.
    std::array<std::byte, 24> settings_payload{};
    settings_payload[0] = std::byte{2};
    const std::vector<std::byte> wire =
        frame_bytes(frame_type::settings, 0, span<const std::byte>(settings_payload.data(),
                                                                  settings_payload.size()));
    CRPC_CHECK(!wire.empty());

    g_fail_next_alloc = true;
    (void)c.source().on_data(view(wire));

    CRPC_CHECK(g_injected_alloc_failures.load(std::memory_order_acquire) >= 1);
    // A failed control frame must fail the connection explicitly; the old
    // `continue` silently dropped the frame and kept running.
    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, handle.get().code());
    }
    auto after_close = c.start_call<std::string>(1, 1, bytes_of("y"));
    CRPC_CHECK_FALSE(after_close.ok());
#endif
}

CRPC_TEST(handshake_hs24_many_settings_in_one_chunk_are_not_killed) {
    auto conn = std::make_unique<manual_connection>();
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);
    const u32 stream_id = handle.slot()->stream_id;

    std::array<std::byte, 24> settings_payload{};
    settings_payload[0] = std::byte{2};
    std::vector<std::byte> settings =
        frame_bytes(frame_type::settings, 0, span<const std::byte>(settings_payload.data(),
                                                                  settings_payload.size()));
    CRPC_CHECK(!settings.empty());
    std::vector<std::byte> chunk;
    for (int i = 0; i < 10; ++i) {
        chunk.insert(chunk.end(), settings.begin(), settings.end());
    }

    (void)c.source().on_data(view(chunk));

    // There is no fixed staging capacity: ten control frames in one chunk must
    // not fail the connection. A following response still completes the call.
    const std::vector<std::byte> response = success_response(stream_id, "ok");
    (void)c.source().on_data(view(response));

    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK(handle.get().ok());
    }
    CRPC_CHECK_EQ(0u, c.in_flight());
}

// --- Re-entrant reader fatal propagation (HS-32 (d)/(e), M1) ----------------
//
// After the dispatch moved out of the reader lock, the `in_process_` branch is
// a defensive path: the public API no longer re-enters it naturally. The test
// injects the state (the plan's allowed state-injection seam) and then drives
// the *real* outer drain so both halves of the contract are exercised:
//   (d) the inner call records `reader_fatal_`, aborts the decoder, and does not
//       report the failure itself;
//   (e) the outer drain sees the flag, drops the staged frame (no PONG), calls
//       protocol_failure exactly once, and leaves the internal buffer empty.
CRPC_TEST(handshake_hs32_reentrant_inner_fatal_propagates) {
    auto counter = std::make_shared<std::atomic<std::size_t>>(0);
    auto conn = std::make_unique<manual_connection>(counter);
    client_options options;
    options.slot_capacity = 8;
    client c(std::move(conn), options);
    call<std::string> handle = start_inflight(c);
    const std::size_t pushes_before = counter->load(std::memory_order_relaxed);

    // (d) Inner call while an outer drain is supposedly active.
    client_test_access::set_in_process(c, true);
    const std::array<std::byte, frame_header_size> header = settings_header(kBigSettingsPayload);
    std::vector<std::byte> inner(header.begin(), header.end());
    const std::size_t before = g_alloc_bytes.load(std::memory_order_relaxed);
    (void)c.source().on_data(view(inner));
    [[maybe_unused]] const std::size_t delta = g_alloc_bytes.load(std::memory_order_relaxed) - before;

#if !defined(CRPC_SHARED)
    CRPC_CHECK(delta < 1024u * 1024u);
#endif
    CRPC_CHECK(client_test_access::reader_fatal(c));
    CRPC_CHECK_EQ(0u, client_test_access::buffered(c));
    // The inner call must defer: closing under the outer drain's lock is the
    // bug this contract prevents.
    CRPC_CHECK_FALSE(handle.ready());

    // (e) Outer drain: a valid PING is now fed; it must be dropped, not handled.
    client_test_access::set_in_process(c, false);
    const std::array<std::byte, 8> ping_payload{};
    const std::vector<std::byte> ping =
        frame_bytes(frame_type::ping, 0,
                    span<const std::byte>(ping_payload.data(), ping_payload.size()));
    (void)c.source().on_data(view(ping));

    CRPC_CHECK(handle.ready());
    if (handle.ready()) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, handle.get().code());
    }
    // No PONG: the staged PING was abandoned with the rest of the chunk.
    CRPC_CHECK_EQ(pushes_before, counter->load(std::memory_order_relaxed));
    CRPC_CHECK_EQ(0u, client_test_access::buffered(c));
}

// --- frame_decoder::peek_header / abort_external contract -------------------

CRPC_TEST(handshake_peek_header_contract) {
    frame_decoder decoder;
    frame_header header{};
    CRPC_CHECK(peek_result::need_more == decoder.peek_header({}, header));

    const std::array<std::byte, frame_header_size> bytes = settings_header(24);
    decoder.feed(span<const std::byte>(bytes.data(), 15));
    CRPC_CHECK(peek_result::need_more == decoder.peek_header({}, header));
    // The new chunk's first byte completes the header without consuming it.
    CRPC_CHECK(peek_result::header_ready ==
               decoder.peek_header(span<const std::byte>(bytes.data() + 15, 1), header));
    CRPC_CHECK_EQ(static_cast<u32>(frame_type::settings), static_cast<u32>(header.type));
    CRPC_CHECK_EQ(24u, header.payload_len);

    frame value;
    CRPC_CHECK(frame_decoder::result::need_more == decoder.next(value));
    decoder.feed(span<const std::byte>(bytes.data() + 15, 1));
    // Header complete, payload still missing: peek says ready, next says more.
    CRPC_CHECK(peek_result::header_ready == decoder.peek_header({}, header));
    CRPC_CHECK(frame_decoder::result::need_more == decoder.next(value));

    decoder.abort_external();
    CRPC_CHECK_EQ(0u, decoder.buffered());

    // An active external window is dropped whole.
    frame_decoder external;
    external.feed_external(span<const std::byte>(bytes.data(), 8));
    CRPC_CHECK(peek_result::need_more == external.peek_header({}, header));
    CRPC_CHECK_EQ(8u, external.buffered());
    external.abort_external();
    CRPC_CHECK_EQ(0u, external.buffered());

    // An invalid header is reported, and the probe does not poison the decoder.
    std::array<std::byte, frame_header_size> bad{};
    bad[4] = static_cast<std::byte>(0x7F);
    CRPC_CHECK(peek_result::header_error ==
               decoder.peek_header(span<const std::byte>(bad.data(), bad.size()), header));
    CRPC_CHECK_FALSE(decoder.failed());
}

// --- P8a sender-side ratio self-check (D-B1 send side) ---------------------

namespace {
struct encode_outcome {
    status code;
    std::size_t written = 0;
    compression_algorithm chosen = compression_algorithm::none;
    compression_fallback_reason reason = compression_fallback_reason::none;
    std::vector<std::byte> bytes;
};

encode_outcome encode_with(frame value, compression_algorithm preferred,
                           const compression_limits& limits) {
    encode_outcome result;
    const std::size_t reserve = encoded_frame_size_compressed(value, preferred, limits);
    result.bytes.resize(reserve);
    result.code = encode_frame_compressed_ex(span<std::byte>(result.bytes.data(), result.bytes.size()),
                                             value, preferred, limits, result.written, &result.chosen,
                                             nullptr, &result.reason);
    result.bytes.resize(result.written);
    return result;
}
} // namespace

CRPC_TEST(handshake_p8_sender_ratio_self_check) {
    compression_limits limits;
    limits.max_compression_ratio = 300;
    limits.min_payload = 64;
    limits.max_payload = max_frame_default;

    // Compressible and within the ratio guard: lz4 emits COMPRESSED.
    {
        std::vector<std::byte> zeros(1024u * 1024u, std::byte{0});
        frame value;
        value.header.type = frame_type::request;
        value.header.stream_id = 1;
        value.payload = span<const std::byte>(zeros.data(), zeros.size());
        const encode_outcome result = encode_with(value, compression_algorithm::lz4, limits);
        CRPC_CHECK(result.code.ok());
        CRPC_CHECK_EQ(static_cast<int>(compression_algorithm::lz4),
                      static_cast<int>(result.chosen));
        CRPC_CHECK_EQ(static_cast<int>(compression_fallback_reason::none),
                      static_cast<int>(result.reason));
        frame_header header{};
        CRPC_CHECK(decode_frame_header(span<const std::byte>(result.bytes.data(), result.bytes.size()),
                                       header)
                       .ok());
        CRPC_CHECK((header.flags & frame_flag_compressed) != 0);
    }

    // Incompressible: the encoder never inflates; reason not_smaller.
    {
        std::vector<std::byte> body(1024u * 1024u);
        std::uint32_t lcg = 987654321u;
        for (std::size_t i = 0; i < body.size(); ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            body[i] = static_cast<std::byte>(static_cast<unsigned>(lcg >> 24) & 0xFFu);
        }
        frame value;
        value.header.type = frame_type::request;
        value.header.stream_id = 1;
        value.payload = span<const std::byte>(body.data(), body.size());
        const encode_outcome result = encode_with(value, compression_algorithm::lz4, limits);
        CRPC_CHECK(result.code.ok());
        CRPC_CHECK_EQ(static_cast<int>(compression_algorithm::none),
                      static_cast<int>(result.chosen));
        CRPC_CHECK_EQ(static_cast<int>(compression_fallback_reason::not_smaller),
                      static_cast<int>(result.reason));
    }

    // Highly compressible but ratio > 300: fall back to plaintext (a regression
    // of the sender ratio check would emit COMPRESSED and be rejected by the
    // receiver's identical guard).
    {
        std::vector<std::byte> zeros(1024u * 1024u, std::byte{0});
        frame value;
        value.header.type = frame_type::request;
        value.header.stream_id = 1;
        value.payload = span<const std::byte>(zeros.data(), zeros.size());
        const encode_outcome result = encode_with(value, compression_algorithm::gzip, limits);
        CRPC_CHECK(result.code.ok());
        CRPC_CHECK_EQ(static_cast<int>(compression_algorithm::none),
                      static_cast<int>(result.chosen));
        CRPC_CHECK_EQ(static_cast<int>(compression_fallback_reason::ratio_exceeded),
                      static_cast<int>(result.reason));
        frame decoded;
        const status decode_status =
            decode_frame(span<const std::byte>(result.bytes.data(), result.bytes.size()), decoded);
        CRPC_CHECK(decode_status.ok());
        CRPC_CHECK_EQ(zeros.size(), decoded.payload.size());
        CRPC_CHECK_EQ(0, static_cast<int>(decoded.header.flags & frame_flag_compressed));
    }
}

} // namespace

CRPC_TEST_MAIN()
