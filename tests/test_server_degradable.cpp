// CRPC -- the `crpc-degradable` advertisement on server rejection responses.
//
// A client can only tell "the server refused before running anything" from
// "the handler ran and then failed with the same code" if the first case
// carries the reserved `crpc-degradable` response meta key. These tests pin
// both halves of that contract:
//
//   * the four pre-execution admission refusals advertise it (breaker,
//     per-service rate, per-service bulkhead, per-service in-flight cap);
//   * every other error path does not, including a handler that returns
//     `overload` / `unavailable` itself, the connection-level flood bucket,
//     the per-connection cap, `not_found`, `no_such_method` and `internal`.
//
// The byte-level case reads the RESPONSE payload the client actually received
// (a tee on the client side of the in-process transport), never a re-encoding
// of the decoded error. The golden formula replaces the two-byte
// `meta_count = 0x0000` with `0x0001` plus one TLV entry, so the payload grows
// by exactly 23 bytes.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/admission.hpp>
#include <crpc/resilience/bulkhead.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/fallback.hpp>
#include <crpc/resilience/rate_limiter.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

constexpr crpc::u64 kService = 100;
constexpr crpc::u64 kEcho = 1;
constexpr crpc::u64 kSlow = 2;
constexpr crpc::u64 kFail = 4;
constexpr crpc::u64 kReturnCode = 5;
constexpr crpc::u64 kBlocking = 7;

// The frozen clock origin. It is far from zero on purpose: a test that
// accidentally reads the real steady clock instead of the injected one produces
// a huge value, not a value near zero, so the mistake is visible in the
// assertion instead of silently passing.
constexpr crpc::u64 kT0 = 1'000'000'000ull;

// The low-level client's clock type returns a `time_point`, not nanoseconds.
std::chrono::steady_clock::time_point manual_now(void* context) noexcept {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(
            static_cast<crpc::resilience::manual_clock*>(context)->now_ns()));
}

// A deadline expressed on the frozen client clock. The client encodes
// `deadline - now`, so with both frozen the wire value is exactly `left`.
std::chrono::steady_clock::time_point frozen_deadline(
    const crpc::resilience::manual_clock& clock, std::chrono::nanoseconds left) {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(clock.now_ns()) + left);
}

// Diagnostic override for the server clock advance between two admissions. It
// exists only to prove the timing tests still discriminate: with
// `e >= h` (the gap between the two admissions larger than the rate hint) the
// rate branch flips to the deadline exit and the assertions must fail. It is
// never set in a normal run.
crpc::u64 server_advance_override() {
    long parsed = 0;
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "CRPC_S4_SERVER_ADVANCE_NS") == 0 &&
        value != nullptr) {
        parsed = std::strtol(value, nullptr, 10);
        std::free(value);
    }
#else
    const char* value = std::getenv("CRPC_S4_SERVER_ADVANCE_NS");
    if (value != nullptr) {
        parsed = std::strtol(value, nullptr, 10);
    }
#endif
    return parsed > 0 ? static_cast<crpc::u64>(parsed) : 0;
}

// The reserved metadata entry, byte for byte: key length 15, key
// "crpc-degradable", value length 4, value "true".
constexpr const char* kDegradableTlvHex =
    "0100"
    "0f00"
    "637270632d64656772616461626c65"
    "0400"
    "74727565";

std::string hex_of(crpc::span<const std::byte> bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const unsigned value = static_cast<unsigned>(b);
        out.push_back(digits[(value >> 4) & 0x0Fu]);
        out.push_back(digits[value & 0x0Fu]);
    }
    return out;
}

std::string hex_of(const std::vector<std::byte>& bytes) {
    return hex_of(crpc::span<const std::byte>(bytes.data(), bytes.size()));
}

// --- Service under test ------------------------------------------------------

struct service_state {
    std::atomic<int> echo_calls{0};
    std::atomic<int> blocking_calls{0};
    std::atomic<int> blocking_gate{0};

    int echo(const int& value) {
        echo_calls.fetch_add(1, std::memory_order_relaxed);
        return value + 1;
    }

    int slow(const int& value) {
        std::this_thread::sleep_for(200ms);
        return value + 1;
    }

    // Holds the handler inside dispatch until the test raises the gate.
    int blocking(const int& value) {
        blocking_calls.fetch_add(1, std::memory_order_relaxed);
        while (blocking_gate.load(std::memory_order_acquire) == 0) {
            std::this_thread::sleep_for(1ms);
        }
        return value + 1;
    }

    static crpc::status raw_fail(void*, const crpc::call_envelope&,
                                 crpc::span<const std::byte>, crpc::buffer&,
                                 crpc::rpc::call_context&) {
        return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "always")};
    }

    static crpc::status raw_return_code(void*, const crpc::call_envelope&,
                                        crpc::span<const std::byte> body,
                                        crpc::buffer&,
                                        crpc::rpc::call_context&) {
        int code = 0;
        const crpc::status decoded = crpc::rpc::call_codec<int>::decode(body, code);
        if (!decoded.ok() || code == 0) {
            return decoded;
        }
        return crpc::status{
            crpc::rpc_error(static_cast<crpc::rpc_code>(code), "scripted outcome")};
    }
};

struct setup {
    std::size_t shards = 1;
    std::size_t per_service = 65536;
    std::size_t per_connection = 1024;
    std::chrono::milliseconds overload_retry{10};
    bool resilience_enabled = true;
    // Frozen clock: the server reads only `server_clock`, so the interval
    // between two admissions is exactly the amount this test advances it by
    // (zero unless it deliberately advances). Tests that leave this false keep
    // the real monotonic clock.
    bool frozen_clock = false;
    std::optional<crpc::resilience::rate_limit_config> connection_rate;
    std::optional<crpc::resilience::rate_limit_config> service_rate;
    std::optional<crpc::resilience::bulkhead_config> service_bulkhead;
    std::optional<crpc::resilience::breaker_config> breaker;
};

struct server_under_test {
    crpc::inproc_backend backend;
    service_state service;
    std::unique_ptr<crpc::server> srv;
    // The injected clock. Two separate clocks are deliberate: a real deployment
    // has one clock per process, and the timing experiments advance exactly one
    // side.
    crpc::resilience::manual_clock server_clock{kT0};
    crpc::resilience::manual_clock client_clock{kT0};
    std::string name = "crpc-degradable";

    ~server_under_test() {
        if (srv) {
            srv->stop();
        }
    }
};

bool start(server_under_test& f, const setup& cfg) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(f.name), f.backend);
    sb.add_service(kService, "svc");
    sb.template add_method<service_state, &service_state::echo>(kService, kEcho,
                                                                "echo", f.service);
    sb.template add_method<service_state, &service_state::slow>(kService, kSlow,
                                                                "slow", f.service);
    sb.add_raw_method(kService, kFail, "fail", &service_state::raw_fail,
                      &f.service);
    sb.add_raw_method(kService, kReturnCode, "return_code",
                      &service_state::raw_return_code, &f.service);
    sb.template add_method<service_state, &service_state::blocking>(
        kService, kBlocking, "blocking", f.service);
    sb.shards(cfg.shards);
    sb.handler_workers(8);
    sb.max_inflight_per_connection(cfg.per_connection);
    sb.per_service_inflight(cfg.per_service);
    sb.overload_retry_after(cfg.overload_retry);
    sb.drain_timeout(2000ms);
    sb.resilience_enabled(cfg.resilience_enabled);
    if (cfg.connection_rate) {
        sb.per_connection_rate(*cfg.connection_rate);
    }
    if (cfg.service_rate) {
        sb.per_service_rate(*cfg.service_rate);
    }
    if (cfg.service_bulkhead) {
        sb.per_service_bulkhead(*cfg.service_bulkhead);
    }
    if (cfg.breaker) {
        sb.breaker(*cfg.breaker);
    }
    if (cfg.frozen_clock) {
        sb.clock(f.server_clock.source());
    }
    f.srv = sb.build();
    return f.srv != nullptr && f.srv->start().ok();
}

// --- Client-side tee ---------------------------------------------------------

// Records every byte the client receives and forwards it unchanged, so a test
// can read the RESPONSE payload the server actually produced.
class tee_source final : public crpc::io::source {
public:
    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        crpc::io::source* target = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            bytes_.insert(bytes_.end(), data.begin(), data.end());
            target = inner_;
        }
        return target != nullptr ? target->on_data(data) : data.size();
    }

    void on_eof() noexcept override {
        crpc::io::source* target = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            target = inner_;
        }
        if (target != nullptr) {
            target->on_eof();
        }
    }

    void on_error(std::error_code ec) noexcept override {
        crpc::io::source* target = nullptr;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            target = inner_;
        }
        if (target != nullptr) {
            target->on_error(ec);
        }
    }

    void bind(crpc::io::source* inner) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        inner_ = inner;
    }

    std::vector<std::byte> bytes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return bytes_;
    }

private:
    mutable std::mutex mutex_;
    crpc::io::source* inner_ = nullptr;
    std::vector<std::byte> bytes_;
};

class tee_connection final : public crpc::connection {
public:
    explicit tee_connection(std::unique_ptr<crpc::connection> inner)
        : inner_(std::move(inner)) {}

    ~tee_connection() override {
        if (bound_) {
            inner_->bind_source(nullptr);
            tee_.bind(nullptr);
        }
    }

    crpc::io::source& source() noexcept override { return inner_->source(); }
    crpc::io::sink& sink() noexcept override { return inner_->sink(); }

    void bind_source(crpc::io::source* source) noexcept override {
        if (source == nullptr) {
            // Drain first: after this returns no delivery can reach the tee.
            inner_->bind_source(nullptr);
            tee_.bind(nullptr);
            bound_ = false;
            return;
        }
        tee_.bind(source);
        inner_->bind_source(&tee_);
        bound_ = true;
    }

    void resume_read() noexcept override { inner_->resume_read(); }
    void close() noexcept override { inner_->close(); }
    [[nodiscard]] bool is_open() const noexcept override {
        return inner_->is_open();
    }
    [[nodiscard]] crpc::endpoint remote() const override {
        return inner_->remote();
    }

    std::vector<std::byte> received_bytes() const { return tee_.bytes(); }

private:
    std::unique_ptr<crpc::connection> inner_;
    tee_source tee_;
    bool bound_ = false;
};

struct tee_client {
    std::unique_ptr<crpc::rpc::client> client;
    tee_connection* tee = nullptr;
    // Set when the client clock is frozen; `call_int` then derives its deadline
    // from the same clock, so the encoded wire deadline is deterministic.
    crpc::resilience::manual_clock* clock = nullptr;
};

tee_client make_client(server_under_test& f) {
    tee_client out;
    auto inner =
        f.backend.connect(crpc::endpoint::inproc(f.name), crpc::connect_options{});
    if (!inner) {
        return out;
    }
    auto wrapped = std::make_unique<tee_connection>(std::move(inner));
    out.tee = wrapped.get();
    crpc::rpc::client_options options;
    options.slot_capacity = 8;
    // No default deadline: the wire carries no deadline unless the call sets
    // one, which keeps the golden payloads free of that field.
    options.default_timeout = std::chrono::nanoseconds{0};
    out.client = std::make_unique<crpc::rpc::client>(std::move(wrapped), options);
    return out;
}

// Same connection, but the low-level client reads the frozen clock. The
// deadline has to be expressed on that same clock (see `call_int`): the client
// encodes `deadline - now`, so a real-clock deadline against a frozen clock
// would put a huge relative value on the wire.
tee_client make_client(server_under_test& f, crpc::resilience::manual_clock& clock) {
    tee_client out;
    auto inner =
        f.backend.connect(crpc::endpoint::inproc(f.name), crpc::connect_options{});
    if (!inner) {
        return out;
    }
    auto wrapped = std::make_unique<tee_connection>(std::move(inner));
    out.tee = wrapped.get();
    crpc::rpc::client_options options;
    options.slot_capacity = 8;
    options.default_timeout = std::chrono::nanoseconds{0};
    options.clock = &manual_now;
    options.clock_ctx = &clock;
    out.client = std::make_unique<crpc::rpc::client>(std::move(wrapped), options);
    out.clock = &clock;
    return out;
}

// Every RESPONSE payload the client received, in arrival order.
std::vector<std::vector<std::byte>> response_payloads(tee_connection& tee) {
    const std::vector<std::byte> raw = tee.received_bytes();
    crpc::frame_decoder decoder(crpc::max_frame_default);
    decoder.feed(crpc::span<const std::byte>(raw.data(), raw.size()));
    std::vector<std::vector<std::byte>> out;
    for (;;) {
        crpc::frame value;
        const crpc::frame_decoder::result result = decoder.next(value);
        if (result == crpc::frame_decoder::result::need_more) {
            break;
        }
        if (result != crpc::frame_decoder::result::frame_ready) {
            break;
        }
        if (value.header.type == crpc::frame_type::response) {
            out.emplace_back(value.payload.begin(), value.payload.end());
        }
    }
    return out;
}

struct call_outcome {
    bool started = false;
    crpc::rpc_error error;
    // The payload of the last RESPONSE this call produced (empty when the call
    // produced none, e.g. a ONE_WAY).
    std::vector<std::byte> payload;
};

call_outcome call_int(tee_client& c, crpc::u64 method, int argument,
                      std::optional<std::chrono::nanoseconds> deadline =
                          std::nullopt,
                      bool one_way = false) {
    call_outcome out;
    crpc::buffer body;
    if (!crpc::rpc::call_codec<int>::encode(argument, body).ok()) {
        return out;
    }
    crpc::rpc::call_context ctx;
    if (deadline) {
        ctx.has_deadline = true;
        // The deadline must be expressed on the same clock the client encodes
        // with, or the wire value becomes meaningless (see make_client).
        ctx.deadline = c.clock != nullptr
                           ? frozen_deadline(*c.clock, *deadline)
                           : std::chrono::steady_clock::now() + *deadline;
    }
    if (one_way) {
        ctx.mode = crpc::rpc::delivery_mode::unreliable;
    }
    const std::size_t before = response_payloads(*c.tee).size();
    auto started = c.client->start_call<int>(kService, method, body.readable(), ctx);
    if (!started.ok()) {
        out.started = true;
        out.error = started.error();
        return out;
    }
    out.started = true;
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        out.error = result.error();
    }
    std::vector<std::vector<std::byte>> payloads = response_payloads(*c.tee);
    if (payloads.size() > before) {
        out.payload = std::move(payloads.back());
    }
    return out;
}

bool has_degradable_marker(const crpc::rpc_error& error) {
    return crpc::resilience::degradable(error);
}

// The payload `encode_response` produces for the same code / message / hint
// without the marker. This is the public encoder, not the server path, so it
// is an independent oracle for the golden formula.
std::vector<std::byte> plain_payload(crpc::rpc_code code, const char* message,
                                     std::chrono::milliseconds retry_after) {
    crpc::rpc_error error(code, message);
    error.retry_after = retry_after;
    crpc::buffer out;
    (void)crpc::rpc::encode_response(code, error, crpc::span<const std::byte>(),
                                     out);
    return std::vector<std::byte>(out.readable().begin(), out.readable().end());
}

// Asserts the byte-level contract for one marked payload:
//   * the plain payload ends with meta_count == 0x0000;
//   * the marked payload is the plain payload with those two bytes replaced by
//     0x0001 and the one TLV entry appended;
//   * the growth is exactly 23 bytes.
void check_golden(const std::vector<std::byte>& marked,
                  const std::vector<std::byte>& plain, const char* label) {
    const std::string plain_hex = hex_of(plain);
    const std::string marked_hex = hex_of(marked);
    CRPC_CHECK_EQ(static_cast<std::size_t>(23),
                  marked.size() - plain.size());
    CRPC_CHECK(plain.size() >= 2);
    if (plain.size() < 2) {
        return;
    }
    CRPC_CHECK_EQ(std::string("0000"), plain_hex.substr(plain_hex.size() - 4));
    const std::string expected =
        plain_hex.substr(0, plain_hex.size() - 4) + kDegradableTlvHex;
    if (marked_hex != expected) {
        std::fprintf(stderr, "%s plain=%s\n%s marked=%s\n%s expected=%s\n",
                     label, plain_hex.c_str(), label, marked_hex.c_str(), label,
                     expected.c_str());
    }
    CRPC_CHECK_EQ(expected, marked_hex);
}

} // namespace

// --- T1: breaker -------------------------------------------------------------

CRPC_TEST(degradable_breaker_refusal_is_marked) {
    // Given: a server whose breaker opens after one dependency failure, with a
    // short open duration so the retry hint stays in the one-byte varint range.
    server_under_test f;
    setup cfg;
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.0;
    breaker.slow_call_threshold = 10s;
    breaker.open_duration = 5s;
    cfg.breaker = breaker;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: the handler fails once, then the breaker refuses the next call.
    const call_outcome first = call_int(c, kFail, 1);
    const call_outcome second = call_int(c, kFail, 2);

    // Then: the first failure is the handler's own (no marker), the second is a
    // pre-execution refusal and advertises it.
    CRPC_CHECK(first.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(first.error.code));
    CRPC_CHECK_FALSE(has_degradable_marker(first.error));
    CRPC_CHECK(second.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(second.error.code));
    CRPC_CHECK_EQ(std::string("service circuit breaker is open"),
                  second.error.message);
    CRPC_CHECK(second.error.retryable);
    CRPC_CHECK(has_degradable_marker(second.error));
    const std::string* value = second.error.metadata.find(
        crpc::resilience::degradable_metadata_key);
    CRPC_CHECK(value != nullptr);
    if (value != nullptr) {
        CRPC_CHECK_EQ(std::string("true"), *value);
    }
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1), f.srv->stats().breaker_open);

    // The response body is empty: a rejection carries no value.
    crpc::rpc_code decoded_code = crpc::rpc_code::ok;
    crpc::rpc_error decoded;
    crpc::span<const std::byte> body;
    CRPC_CHECK(crpc::rpc::decode_response(
                   crpc::span<const std::byte>(second.payload.data(),
                                               second.payload.size()),
                   decoded_code, decoded, body)
                   .ok());
    CRPC_CHECK(body.empty());

    // Golden: only the formula and the 23-byte delta. The breaker hint is
    // ceil(remaining open duration), so its varint width varies with time and
    // must never be pinned to an absolute payload length.
    check_golden(second.payload,
                 plain_payload(crpc::rpc_code::unavailable,
                               "service circuit breaker is open",
                               second.error.retry_after),
                 "breaker");
}

// --- T2: per-service bulkhead ------------------------------------------------

CRPC_TEST(degradable_bulkhead_refusal_is_marked) {
    // Given: a bulkhead of one slot and a handler that holds it. Two shards and
    // two connections let the two handlers truly overlap.
    server_under_test f;
    setup cfg;
    cfg.shards = 2;
    crpc::resilience::bulkhead_config bulkhead;
    bulkhead.capacity = 1;
    bulkhead.retry_after = 10ms; // fixed hint: one-byte varint
    cfg.service_bulkhead = bulkhead;
    CRPC_CHECK(start(f, cfg));
    tee_client first = make_client(f);
    tee_client second = make_client(f);
    CRPC_CHECK(first.client != nullptr);
    CRPC_CHECK(second.client != nullptr);
    if (!first.client || !second.client) {
        return;
    }

    // When: the first call holds the only slot and the second is refused.
    f.service.blocking_gate.store(0, std::memory_order_release);
    crpc::buffer first_body;
    crpc::buffer second_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, first_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(2, second_body).ok());
    auto first_started = first.client->start_call<int>(kService, kBlocking,
                                                       first_body.readable());
    CRPC_CHECK(first_started.ok());
    if (!first_started.ok()) {
        return;
    }
    auto first_handle = std::move(first_started).value();
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.blocking_calls.load() >= 1; }, 2000));

    const std::size_t second_before = response_payloads(*second.tee).size();
    auto second_started = second.client->start_call<int>(kService, kBlocking,
                                                         second_body.readable());
    CRPC_CHECK(second_started.ok());
    crpc::rpc_error second_error;
    if (second_started.ok()) {
        auto handle = std::move(second_started).value();
        auto result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            second_error = result.error();
        }
    }
    f.service.blocking_gate.store(1, std::memory_order_release);
    (void)first_handle.get();
    const std::vector<std::vector<std::byte>> payloads =
        response_payloads(*second.tee);
    CRPC_CHECK(payloads.size() > second_before);

    // Then: `overload` from the bulkhead is marked, and the golden holds.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(second_error.code));
    CRPC_CHECK_EQ(std::string("service bulkhead is full"),
                  second_error.message);
    CRPC_CHECK_EQ(10, static_cast<int>(second_error.retry_after.count()));
    CRPC_CHECK(has_degradable_marker(second_error));
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().bulkhead_rejected);

    const std::vector<std::byte>& marked = payloads.back();
    const std::vector<std::byte> plain = plain_payload(
        crpc::rpc_code::overload, "service bulkhead is full", 10ms);
    // Fixed hint (10 ms, one-byte varint) => stable payload length.
    CRPC_CHECK_EQ(static_cast<std::size_t>(32), plain.size());
    check_golden(marked, plain, "bulkhead");
}

// --- T3: per-service rate ----------------------------------------------------

CRPC_TEST(degradable_service_rate_refusal_is_marked) {
    // Given: an empty service bucket with no refill. The hint is therefore the
    // limiter's fixed ceiling (one hour = 3'600'000 ms), independent of how
    // much wall-clock time passed between two calls.
    server_under_test f;
    setup cfg;
    crpc::resilience::rate_limit_config rate;
    rate.rate_per_second = 0.0;
    rate.burst = 1.0;
    rate.initial_tokens = 0.0;
    cfg.service_rate = rate;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: a call without a deadline reaches the empty bucket.
    const call_outcome out = call_int(c, kEcho, 1);

    // Then: `resource_exhausted` is marked; the hint is the fixed ceiling.
    CRPC_CHECK(out.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(out.error.code));
    CRPC_CHECK_EQ(std::string("service rate limit reached"), out.error.message);
    CRPC_CHECK_EQ(3600000, static_cast<int>(out.error.retry_after.count()));
    CRPC_CHECK(has_degradable_marker(out.error));
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().rate_limited_service);
    CRPC_CHECK_EQ(0, f.service.echo_calls.load());

    const std::vector<std::byte> plain =
        plain_payload(crpc::rpc_code::resource_exhausted,
                      "service rate limit reached", 3600000ms);
    // No refill => the hint is the ceiling (a four-byte varint).
    CRPC_CHECK_EQ(static_cast<std::size_t>(37), plain.size());
    check_golden(out.payload, plain, "service_rate");
}

// --- T4: per-service in-flight cap -------------------------------------------

CRPC_TEST(degradable_service_capacity_refusal_is_marked) {
    // Given: a per-service cap of one and a handler that holds it.
    server_under_test f;
    setup cfg;
    cfg.shards = 2;
    cfg.per_service = 1;
    cfg.overload_retry = 10ms; // fixed hint
    CRPC_CHECK(start(f, cfg));
    tee_client first = make_client(f);
    tee_client second = make_client(f);
    CRPC_CHECK(first.client != nullptr);
    CRPC_CHECK(second.client != nullptr);
    if (!first.client || !second.client) {
        return;
    }

    f.service.blocking_gate.store(0, std::memory_order_release);
    crpc::buffer first_body;
    crpc::buffer second_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, first_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(2, second_body).ok());
    auto first_started = first.client->start_call<int>(kService, kBlocking,
                                                       first_body.readable());
    CRPC_CHECK(first_started.ok());
    if (!first_started.ok()) {
        return;
    }
    auto first_handle = std::move(first_started).value();
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.blocking_calls.load() >= 1; }, 2000));

    const std::size_t second_before = response_payloads(*second.tee).size();
    auto second_started = second.client->start_call<int>(kService, kBlocking,
                                                         second_body.readable());
    CRPC_CHECK(second_started.ok());
    crpc::rpc_error second_error;
    if (second_started.ok()) {
        auto handle = std::move(second_started).value();
        auto result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            second_error = result.error();
        }
    }
    f.service.blocking_gate.store(1, std::memory_order_release);
    (void)first_handle.get();
    const std::vector<std::vector<std::byte>> payloads =
        response_payloads(*second.tee);
    CRPC_CHECK(payloads.size() > second_before);

    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(second_error.code));
    CRPC_CHECK_EQ(std::string("service concurrency limit reached"),
                  second_error.message);
    CRPC_CHECK_EQ(10, static_cast<int>(second_error.retry_after.count()));
    CRPC_CHECK(has_degradable_marker(second_error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                  f.srv->stats().rejected_overload);

    const std::vector<std::byte> plain = plain_payload(
        crpc::rpc_code::overload, "service concurrency limit reached", 10ms);
    CRPC_CHECK_EQ(static_cast<std::size_t>(41), plain.size());
    check_golden(payloads.back(), plain, "service_capacity");
}

// --- T4b: the cap with an expired wire deadline ------------------------------
//
// The cap branch shares the `!wire_deadline_expired` guard with the rate
// branch, but the rate case cannot discriminate the cap's copy. A cap refusal
// for a request whose wire deadline is already below the 1 ms floor must not
// advertise degradability: the client could not deliver a fallback value in
// time either. Without this case, deleting the guard from the cap branch would
// leave every test green.
//
// Same shape as the marked cap case: two shards and two connections, never a
// single-drain in-process reentry (that deadlocks; recorded defect).

CRPC_TEST(degradable_service_capacity_refusal_with_expired_wire_deadline_is_not_marked) {
    // Given: a per-service cap of one and nothing else configured, so the cap
    // is the first refusal the second request meets. Both clocks are frozen, so
    // the encoded wire deadline is the constant 990 us: the marker decision no
    // longer depends on how long the two admissions took.
    server_under_test f;
    setup cfg;
    cfg.shards = 2;
    cfg.per_service = 1;
    cfg.overload_retry = 10ms;
    cfg.frozen_clock = true;
    CRPC_CHECK(start(f, cfg));
    tee_client first = make_client(f, f.client_clock);
    tee_client second = make_client(f, f.client_clock);
    CRPC_CHECK(first.client != nullptr);
    CRPC_CHECK(second.client != nullptr);
    if (!first.client || !second.client) {
        return;
    }

    f.service.blocking_gate.store(0, std::memory_order_release);
    crpc::buffer first_body;
    crpc::buffer second_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, first_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(2, second_body).ok());
    auto first_started = first.client->start_call<int>(kService, kBlocking,
                                                       first_body.readable());
    CRPC_CHECK(first_started.ok());
    if (!first_started.ok()) {
        return;
    }
    auto first_handle = std::move(first_started).value();
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.blocking_calls.load() >= 1; }, 2000));

    // When: the second request arrives with 990 us of wire deadline left, below
    // the 1 ms floor. `frozen_deadline` uses the clock the client encodes with,
    // so the wire carries exactly 990000 ns.
    crpc::rpc::call_context ctx;
    ctx.has_deadline = true;
    ctx.deadline = frozen_deadline(f.client_clock, std::chrono::microseconds(990));
    auto second_started =
        second.client->start_call<int>(kService, kBlocking, second_body.readable(), ctx);
    CRPC_CHECK(second_started.ok());
    crpc::rpc_error second_error;
    if (second_started.ok()) {
        auto handle = std::move(second_started).value();
        auto result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            second_error = result.error();
        }
    }
    f.service.blocking_gate.store(1, std::memory_order_release);
    (void)first_handle.get();

    // Then: the cap still refuses, but the refusal is not marked.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(second_error.code));
    CRPC_CHECK_EQ(std::string("service concurrency limit reached"),
                  second_error.message);
    CRPC_CHECK_FALSE(has_degradable_marker(second_error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), second_error.metadata.size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                  f.srv->stats().rejected_overload);
    // The handler ran exactly once: only the holder got through.
    CRPC_CHECK_EQ(1, f.service.blocking_calls.load());
}

// --- T5: the rate branch that answers deadline_exceeded ----------------------

CRPC_TEST(degradable_rate_deadline_exit_is_not_marked) {
    // Given: an empty bucket and a call whose remaining time is too short for
    // the hint. The rate branch answers `deadline_exceeded`, not
    // `resource_exhausted`.
    server_under_test f;
    setup cfg;
    crpc::resilience::rate_limit_config rate;
    rate.rate_per_second = 1.0;
    rate.burst = 1.0;
    rate.initial_tokens = 0.0;
    cfg.service_rate = rate;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: the deadline cannot be met by waiting for the bucket.
    const call_outcome out = call_int(c, kEcho, 1, 5ms);

    // Then: a deadline miss is never degradable, so no marker is written.
    CRPC_CHECK(out.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(out.error.code));
    CRPC_CHECK_FALSE(has_degradable_marker(out.error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), out.error.metadata.size());
    CRPC_CHECK_EQ(0, f.service.echo_calls.load());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().deadline_rejected);
}

// --- T5b: the rate branch answers resource_exhausted with an expired wire
// deadline ---------------------------------------------------------------
//
// `rate_limiter::attempt_locked` decides `deadline_met` only by "would waiting
// the hint still meet the deadline"; it never compares the remaining time with
// the dispatcher's 1 ms floor. A request with ~990 us left and a ~909 us hint
// therefore takes the success exit (`resource_exhausted`), not the deadline
// exit. The server must still withhold the marker because the wire deadline has
// already expired: a fallback value could not be delivered in time either.
//
// Recipe (fixed by measurement, not by the constructor's initial fill): start
// with one token, spend it on an untimed call, and only then send the timed
// call. `initial_tokens = 0` is not stable, because the bucket refills from the
// wall clock and may already hold a token by the time the first call arrives.
//
// Both clocks are frozen and the burn is awaited before the timed call, so the
// interval between the two admissions is exactly zero (no refill) and the wire
// deadline is exactly 990 us. The recipe no longer depends on how long a round
// trip takes: the bucket cannot refill because the injected clock does not
// advance. `CRPC_S4_SERVER_ADVANCE_NS` advances the injected clock between the
// two admissions; setting it to the hint (909 us) or more must flip the branch
// to the deadline exit and fail this case (the discrimination check).

CRPC_TEST(degradable_rate_refusal_with_expired_wire_deadline_is_not_marked) {
    // Given: a bucket that needs ~909 us to refill one token, starting full,
    // and a server whose clock is frozen at kT0.
    server_under_test f;
    setup cfg;
    crpc::resilience::rate_limit_config rate;
    rate.rate_per_second = 1100.0;
    rate.burst = 1.0;
    rate.initial_tokens = 1.0;
    cfg.service_rate = rate;
    cfg.frozen_clock = true;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f, f.client_clock);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // Both request bodies are encoded before the first call, so the gap between
    // the two admissions contains no encoding work.
    crpc::buffer burn_body;
    crpc::buffer timed_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(0, burn_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, timed_body).ok());

    // Spend the only token so the next call is certain to be rate limited.
    auto burn_started = c.client->start_call<int>(kService, kEcho,
                                                  burn_body.readable(),
                                                  crpc::rpc::call_context{});
    CRPC_CHECK(burn_started.ok());
    if (!burn_started.ok()) {
        return;
    }
    crpc::rpc::call<int> burned = std::move(burn_started).value();

    // Wait until the burn was admitted (its handler ran), then optionally
    // advance the injected server clock. Advancing BEFORE the burn's admission
    // would let the burn itself absorb the delta, so `e` would stay zero and the
    // discrimination check below would be a no-op.
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.echo_calls.load() >= 1; }, 2000));
    if (const crpc::u64 advance = server_advance_override(); advance != 0) {
        f.server_clock.advance(std::chrono::nanoseconds(advance));
    }

    // When: the wire deadline has 990 us left, below the 1 ms floor, while the
    // hint (one token at 1100/s = 909 us) still fits inside it.
    crpc::rpc::call_context timed_ctx;
    timed_ctx.has_deadline = true;
    timed_ctx.deadline =
        frozen_deadline(f.client_clock, std::chrono::microseconds(990));
    auto timed_started = c.client->start_call<int>(kService, kEcho,
                                                   timed_body.readable(),
                                                   timed_ctx);
    CRPC_CHECK(timed_started.ok());
    if (!timed_started.ok()) {
        return;
    }
    crpc::rpc::call<int> out = std::move(timed_started).value();

    // Then: the burn is admitted and the answer is `resource_exhausted` (the
    // success exit), but it is not marked: a deadline that cannot be met is
    // never degradable.
    const crpc::rpc::rpc_result<int> burn_result = burned.get();
    CRPC_CHECK(burn_result.ok());
    const crpc::rpc::rpc_result<int> out_result = out.get();
    CRPC_CHECK(!out_result.ok());
    const crpc::rpc_error& error = out_result.error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(error.code));
    CRPC_CHECK_EQ(std::string("service rate limit reached"), error.message);
    CRPC_CHECK_FALSE(has_degradable_marker(error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), error.metadata.size());
    // This proves the deadline exit (`:1054`) was NOT taken.
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0),
                  f.srv->stats().deadline_rejected);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().rate_limited_service);
    // Only the burn reached the handler.
    CRPC_CHECK_EQ(1, f.service.echo_calls.load());
}

// --- T6: ONE_WAY never carries a response ------------------------------------

CRPC_TEST(degradable_one_way_breaker_refusal_produces_no_frame) {
    // Given: an open breaker.
    server_under_test f;
    setup cfg;
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.0;
    breaker.slow_call_threshold = 10s;
    breaker.open_duration = 5s;
    cfg.breaker = breaker;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }
    const call_outcome opened = call_int(c, kFail, 1);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(opened.error.code));

    // When: a ONE_WAY request reaches the open breaker.
    const std::size_t before = response_payloads(*c.tee).size();
    const call_outcome one_way = call_int(c, kFail, 2, std::nullopt, true);
    std::this_thread::sleep_for(150ms);

    // Then: the refusal happened (counter) but no frame was written, so there
    // is nothing to mark.
    CRPC_CHECK(one_way.started);
    CRPC_CHECK(one_way.payload.empty());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1), f.srv->stats().breaker_open);
    CRPC_CHECK_EQ(before, response_payloads(*c.tee).size());
}

// --- T7: ordinary dispatcher errors ------------------------------------------

CRPC_TEST(degradable_dispatcher_errors_are_not_marked) {
    // Given: a healthy server.
    server_under_test f;
    setup cfg;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When/Then: unknown service, unknown method and a handler-produced
    // `internal` are all ordinary failures, never pre-execution admissions.
    crpc::buffer body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, body).ok());
    auto unknown_service =
        c.client->start_call<int>(9999, kEcho, body.readable());
    CRPC_CHECK(unknown_service.ok());
    if (unknown_service.ok()) {
        auto handle = std::move(unknown_service).value();
        auto result = handle.get();
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(result.code()));
        CRPC_CHECK_FALSE(has_degradable_marker(result.error()));
    }

    auto unknown_method =
        c.client->start_call<int>(kService, 9999, body.readable());
    CRPC_CHECK(unknown_method.ok());
    if (unknown_method.ok()) {
        auto handle = std::move(unknown_method).value();
        auto result = handle.get();
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::no_such_method),
                      static_cast<int>(result.code()));
        CRPC_CHECK_FALSE(has_degradable_marker(result.error()));
    }

    const call_outcome internal = call_int(c, kReturnCode, 19);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                  static_cast<int>(internal.error.code));
    CRPC_CHECK_FALSE(has_degradable_marker(internal.error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), internal.error.metadata.size());
}

// --- T8: connection-level load shedding --------------------------------------

CRPC_TEST(degradable_connection_flood_bucket_is_not_marked) {
    // Given: a connection bucket with one token and no refill.
    server_under_test f;
    setup cfg;
    crpc::resilience::rate_limit_config rate;
    rate.rate_per_second = 0.0;
    rate.burst = 1.0;
    rate.initial_tokens = 1.0;
    cfg.connection_rate = rate;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: the second request on the connection is shed before any job.
    const call_outcome first = call_int(c, kEcho, 1);
    const call_outcome second = call_int(c, kEcho, 2);

    // Then: `resource_exhausted` from the flood bucket carries no marker, even
    // though a per-service rate refusal with the same code would.
    CRPC_CHECK(first.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(first.error.code));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(second.error.code));
    CRPC_CHECK_EQ(std::string("rate limit exceeded"), second.error.message);
    CRPC_CHECK_FALSE(has_degradable_marker(second.error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), second.error.metadata.size());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().rate_limited_connection);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0),
                  f.srv->stats().rate_limited_service);
}

CRPC_TEST(degradable_per_connection_cap_is_not_marked) {
    // Given: a per-connection in-flight cap of one.
    server_under_test f;
    setup cfg;
    cfg.per_connection = 1;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: a second request is pipelined while the first is still running.
    crpc::buffer first_body;
    crpc::buffer second_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, first_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(2, second_body).ok());
    auto first = c.client->start_call<int>(kService, kSlow, first_body.readable());
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    auto first_handle = std::move(first).value();
    auto second = c.client->start_call<int>(kService, kSlow, second_body.readable());
    CRPC_CHECK(second.ok());
    crpc::rpc_error second_error;
    if (second.ok()) {
        auto handle = std::move(second).value();
        auto result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            second_error = result.error();
        }
    }
    auto first_result = first_handle.get();
    CRPC_CHECK(first_result.ok());

    // Then: the per-connection cap is load shedding, not a degradable
    // admission decision.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(second_error.code));
    CRPC_CHECK_EQ(std::string("overloaded"), second_error.message);
    CRPC_CHECK_FALSE(has_degradable_marker(second_error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), second_error.metadata.size());
    CRPC_CHECK(f.srv->stats().rejected_overload >= 1);
}

// --- T9: handler outcomes are not pre-execution refusals ---------------------

CRPC_TEST(degradable_handler_outcomes_are_not_marked) {
    // Given: a healthy server.
    server_under_test f;
    setup cfg;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: the handler itself returns `overload` and `unavailable`.
    const call_outcome overload = call_int(c, kReturnCode, 8);
    const call_outcome unavailable = call_int(c, kReturnCode, 3);

    // Then: neither is marked. The marker is not derivable from the code: the
    // same code from an admission refusal is marked (T2/T4, T1).
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(overload.error.code));
    CRPC_CHECK(overload.error.retryable);
    CRPC_CHECK_FALSE(has_degradable_marker(overload.error));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(unavailable.error.code));
    CRPC_CHECK(unavailable.error.retryable);
    CRPC_CHECK_FALSE(has_degradable_marker(unavailable.error));

    // Code and retryable are identical to the admission refusals: only the
    // marker tells the two apart.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(crpc::resilience::reject_code(
                      crpc::resilience::stage_kind::service_bulkhead)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(crpc::resilience::reject_code(
                      crpc::resilience::stage_kind::breaker)));
}

// --- T11a: resilience disabled => rate and bulkhead are not built -----------

CRPC_TEST(degradable_resilience_disabled_skips_rate_and_bulkhead) {
    // Given: rate and bulkhead configured but resilience disabled.
    server_under_test f;
    setup cfg;
    cfg.resilience_enabled = false;
    crpc::resilience::rate_limit_config rate;
    rate.rate_per_second = 0.0;
    rate.burst = 1.0;
    rate.initial_tokens = 0.0;
    cfg.service_rate = rate;
    crpc::resilience::bulkhead_config bulkhead;
    bulkhead.capacity = 1;
    cfg.service_bulkhead = bulkhead;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: two calls would both be refused if those stages existed.
    const call_outcome first = call_int(c, kEcho, 1);
    const call_outcome second = call_int(c, kEcho, 2);

    // Then: both are served.
    CRPC_CHECK(first.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(first.error.code));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(second.error.code));
    CRPC_CHECK_EQ(2, f.service.echo_calls.load());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0),
                  f.srv->stats().rate_limited_service);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0),
                  f.srv->stats().bulkhead_rejected);
}

// --- T11b: resilience disabled still keeps the breaker AND the cap ----------

CRPC_TEST(degradable_resilience_disabled_keeps_breaker_marker) {
    // Given: resilience disabled; the breaker is built regardless.
    server_under_test f;
    setup cfg;
    cfg.resilience_enabled = false;
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.0;
    breaker.slow_call_threshold = 10s;
    breaker.open_duration = 5s;
    cfg.breaker = breaker;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // When: the breaker opens and then refuses.
    const call_outcome first = call_int(c, kFail, 1);
    const call_outcome second = call_int(c, kFail, 2);

    // Then: the refusal still happens and still advertises degradability.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(first.error.code));
    CRPC_CHECK_FALSE(has_degradable_marker(first.error));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(second.error.code));
    CRPC_CHECK_EQ(std::string("service circuit breaker is open"),
                  second.error.message);
    CRPC_CHECK(has_degradable_marker(second.error));
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1), f.srv->stats().breaker_open);
}

CRPC_TEST(degradable_resilience_disabled_keeps_service_cap_marker) {
    // Given: resilience disabled and a per-service cap of one. The cap is
    // checked outside the `resilience_enabled` guard. Two shards and two
    // connections keep the holder and the refused call on different shards:
    // a single-drain in-process reentry deadlocks here (recorded defect).
    server_under_test f;
    setup cfg;
    cfg.resilience_enabled = false;
    cfg.shards = 2;
    cfg.per_service = 1;
    CRPC_CHECK(start(f, cfg));
    tee_client first = make_client(f);
    tee_client second = make_client(f);
    CRPC_CHECK(first.client != nullptr);
    CRPC_CHECK(second.client != nullptr);
    if (!first.client || !second.client) {
        return;
    }

    f.service.blocking_gate.store(0, std::memory_order_release);
    crpc::buffer first_body;
    crpc::buffer second_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, first_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(2, second_body).ok());
    auto first_started = first.client->start_call<int>(kService, kBlocking,
                                                       first_body.readable());
    CRPC_CHECK(first_started.ok());
    if (!first_started.ok()) {
        return;
    }
    auto first_handle = std::move(first_started).value();
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.blocking_calls.load() >= 1; }, 2000));

    auto second_started = second.client->start_call<int>(kService, kBlocking,
                                                         second_body.readable());
    CRPC_CHECK(second_started.ok());
    crpc::rpc_error second_error;
    if (second_started.ok()) {
        auto handle = std::move(second_started).value();
        auto result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            second_error = result.error();
        }
    }
    f.service.blocking_gate.store(1, std::memory_order_release);
    (void)first_handle.get();

    // Then: the cap still refuses and the refusal is still marked.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(second_error.code));
    CRPC_CHECK_EQ(std::string("service concurrency limit reached"),
                  second_error.message);
    CRPC_CHECK(has_degradable_marker(second_error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                  f.srv->stats().rejected_overload);
}

CRPC_TEST(degradable_breaker_refusal_with_expired_wire_deadline_is_not_marked) {
    // Given: a breaker that opens after one dependency failure, on a frozen
    // clock. With `now` pinned to kT0 the 5 s open window cannot expire while
    // the test runs, so the second call deterministically meets an open breaker
    // instead of racing the open duration.
    server_under_test f;
    setup cfg;
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.0;
    breaker.slow_call_threshold = 10s;
    breaker.open_duration = 5s;
    cfg.breaker = breaker;
    cfg.frozen_clock = true;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f, f.client_clock);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // Open the breaker with a plain call.
    const call_outcome first = call_int(c, kFail, 1);
    CRPC_CHECK(first.started);

    // When: the breaker refusal is asked with 990 us of wire deadline left,
    // below the 1 ms floor.
    const call_outcome out =
        call_int(c, kFail, 2, std::chrono::microseconds(990));

    // Then: it is still a breaker refusal, but it is not marked: a fallback
    // value could not reach the caller in time.
    CRPC_CHECK(out.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(out.error.code));
    CRPC_CHECK_EQ(std::string("service circuit breaker is open"),
                  out.error.message);
    CRPC_CHECK_FALSE(has_degradable_marker(out.error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), out.error.metadata.size());
}

CRPC_TEST(degradable_bulkhead_refusal_with_expired_wire_deadline_is_not_marked) {
    // Given: a per-service bulkhead of one slot, two shards and two connections
    // so the two handlers truly overlap. Both clocks are frozen so the wire
    // deadline is the constant 990 us.
    server_under_test f;
    setup cfg;
    cfg.shards = 2;
    crpc::resilience::bulkhead_config bulkhead;
    bulkhead.capacity = 1;
    bulkhead.retry_after = 10ms;
    cfg.service_bulkhead = bulkhead;
    cfg.frozen_clock = true;
    CRPC_CHECK(start(f, cfg));
    tee_client first = make_client(f, f.client_clock);
    tee_client second = make_client(f, f.client_clock);
    CRPC_CHECK(first.client != nullptr);
    CRPC_CHECK(second.client != nullptr);
    if (!first.client || !second.client) {
        return;
    }

    f.service.blocking_gate.store(0, std::memory_order_release);
    crpc::buffer first_body;
    crpc::buffer second_body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(1, first_body).ok());
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(2, second_body).ok());
    auto first_started = first.client->start_call<int>(kService, kBlocking,
                                                       first_body.readable());
    CRPC_CHECK(first_started.ok());
    if (!first_started.ok()) {
        return;
    }
    auto first_handle = std::move(first_started).value();
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.blocking_calls.load() >= 1; }, 2000));

    // When: the second request arrives with 990 us of wire deadline left, below
    // the 1 ms floor. `frozen_deadline` uses the clock the client encodes with,
    // so the wire carries exactly 990000 ns.
    crpc::rpc::call_context ctx;
    ctx.has_deadline = true;
    ctx.deadline = frozen_deadline(f.client_clock, std::chrono::microseconds(990));
    auto second_started =
        second.client->start_call<int>(kService, kBlocking, second_body.readable(), ctx);
    CRPC_CHECK(second_started.ok());
    crpc::rpc_error second_error;
    if (second_started.ok()) {
        auto handle = std::move(second_started).value();
        auto result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            second_error = result.error();
        }
    }
    f.service.blocking_gate.store(1, std::memory_order_release);
    (void)first_handle.get();

    // Then: the bulkhead still refuses, but the refusal is not marked.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload),
                  static_cast<int>(second_error.code));
    CRPC_CHECK_EQ(std::string("service bulkhead is full"),
                  second_error.message);
    CRPC_CHECK_FALSE(has_degradable_marker(second_error));
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), second_error.metadata.size());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().bulkhead_rejected);
}

// --- T9/T10: the injected clock reaches every consumer ----------------------
//
// "One clock, four consumers" is a contract, not a comment. These two cases can
// only be written with an injected clock: advancing it is the only way to
// observe that the breaker and the bucket read the injected source instead of
// the real one. If either consumer kept `std::chrono::steady_clock`, the
// advance would do nothing and the case would fail.

CRPC_TEST(degradable_breaker_probe_follows_the_injected_clock) {
    // Given: a breaker with a 1 ms open duration on a frozen clock.
    server_under_test f;
    setup cfg;
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.0;
    breaker.slow_call_threshold = 10s;
    breaker.open_duration = 1ms;
    cfg.breaker = breaker;
    cfg.frozen_clock = true;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f, f.client_clock);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // Open the breaker: one dependency failure is enough with min_requests=1.
    const call_outcome opened = call_int(c, kFail, 1);
    CRPC_CHECK(opened.started);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(opened.error.code));
    const call_outcome refused = call_int(c, kFail, 2);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(refused.error.code));
    CRPC_CHECK_EQ(std::string("service circuit breaker is open"),
                  refused.error.message);

    // When: the injected clock advances past the open duration.
    f.server_clock.advance(2ms);

    // Then: the next call is admitted as a half-open probe and reaches the
    // handler. A real clock would still be inside the 1 ms window only for a
    // moment, so this assertion also proves the breaker is not reading it.
    const call_outcome probe = call_int(c, kFail, 3);
    CRPC_CHECK_EQ(std::string("always"), probe.error.message);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1),
                  f.srv->stats().probe_admitted);
}

CRPC_TEST(degradable_rate_bucket_follows_the_injected_clock) {
    // Given: a bucket that refills one token per second, frozen after one burn.
    server_under_test f;
    setup cfg;
    crpc::resilience::rate_limit_config rate;
    rate.rate_per_second = 1.0;
    rate.burst = 1.0;
    rate.initial_tokens = 1.0;
    cfg.service_rate = rate;
    cfg.frozen_clock = true;
    CRPC_CHECK(start(f, cfg));
    tee_client c = make_client(f, f.client_clock);
    CRPC_CHECK(c.client != nullptr);
    if (!c.client) {
        return;
    }

    // Burn the only token; with the clock frozen the bucket cannot refill.
    const call_outcome burn = call_int(c, kEcho, 1);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(burn.error.code));
    const call_outcome refused = call_int(c, kEcho, 2);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(refused.error.code));
    CRPC_CHECK_EQ(1, f.service.echo_calls.load());

    // When: the injected clock advances by a full refill interval.
    f.server_clock.advance(1s);

    // Then: the bucket has a token again and the call reaches the handler.
    const call_outcome admitted = call_int(c, kEcho, 3);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(admitted.error.code));
    CRPC_CHECK_EQ(2, f.service.echo_calls.load());
}

CRPC_TEST_MAIN()
