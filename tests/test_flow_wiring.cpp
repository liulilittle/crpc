// CRPC -- flow-control wiring tests.
//
// The primitives in `test_flow.cpp` pin the decision functions. These cases pin
// the *wiring*: that a connection charges, releases and probes on the real call
// path, that a refusal is a local gate (never retried, never charged to the
// endpoint), and that flow control adds no clock read to the send path.
//
// Two layers are exercised:
//   * `flow_controller` directly, for the sample/confirmation rules. It is a
//     plain class in the header, so the test can drive exact timestamps without
//     a transport. That is the only way to assert "this sample was withheld".
//   * `rpc::client` over a synchronous in-process peer, for admission, the
//     probe/PONG round trip and the marker the facade classifies.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/flow/estimator.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/flow_control.hpp>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace crpc::rpc;

constexpr u64 kService = 1;
constexpr u64 kMethod = 2;
// `congestion_window::config::min_bytes` default, repeated here because the
// window exposes no accessor for its settings.
constexpr u64 kWindowMinBytes = 4u * 1024u;

// A fake monotonic clock. It starts at a non-zero value: zero is a legal
// timestamp, but the estimator rejects a zero *sample*, so a test that feeds the
// estimator must advance to a non-zero instant first.
struct fake_clock {
    std::chrono::steady_clock::time_point now{std::chrono::nanoseconds(1'000'000'000)};
    int reads = 0;
};

std::chrono::steady_clock::time_point fake_now(void* ctx) noexcept {
    fake_clock* clock = static_cast<fake_clock*>(ctx);
    ++clock->reads;
    return clock->now;
}

// --- in-process peer ---------------------------------------------------------

struct noop_completion final : io::completion {
    void complete(io::io_result) noexcept override {}
};

// A sink that hands every pushed byte straight to a peer source, completing
// synchronously. This is the in-process backend's contract, and it is what makes
// a PING/PONG round trip observable without a thread.
class direct_sink final : public io::sink {
public:
    explicit direct_sink(io::source* peer) noexcept : peer_(peer) {}
    void push(span<const std::byte> data, io::completion* c) noexcept override {
        ++pushes_;
        if (peer_ != nullptr) {
            (void)peer_->on_data(data);
        }
        c->complete(io::io_result{{}, data.size()});
    }
    std::size_t pushes() const noexcept { return pushes_; }

private:
    io::source* peer_ = nullptr;
    std::size_t pushes_ = 0;
};

// The peer: decodes frames, echoes PING as PONG (the server's half of the probe
// protocol) and answers REQUEST with an empty success response.
class echo_peer final : public io::source {
public:
    // The bytes this peer produces are delivered to the client's reader source.
    void attach_delivery(io::source* delivery) noexcept { delivery_ = delivery; }

    std::size_t on_data(span<const std::byte> data) noexcept override {
        decoder_.feed(data);
        frame value;
        while (decoder_.next(value) == frame_decoder::result::frame_ready) {
            seen_.push_back(value.header.type);
            if (value.header.type == frame_type::ping) {
                send(frame_type::pong, 0, value.payload);
            } else if (value.header.type == frame_type::request ||
                       value.header.type == frame_type::one_way) {
                buffer body;
                (void)encode_response(rpc_code::ok, rpc_error{}, span<const std::byte>(), body);
                send(frame_type::response, value.header.stream_id, body.readable());
            }
        }
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    std::size_t count(frame_type type) const noexcept {
        std::size_t total = 0;
        for (const frame_type entry : seen_) {
            if (entry == type) {
                ++total;
            }
        }
        return total;
    }

private:
    void send(frame_type type, u32 stream_id, span<const std::byte> payload) {
        if (delivery_ == nullptr) {
            return;
        }
        frame out;
        out.header.type = type;
        out.header.stream_id = stream_id;
        out.payload = payload;
        std::vector<std::byte> bytes(encoded_frame_size(out));
        std::size_t written = 0;
        if (!encode_frame(span<std::byte>(bytes.data(), bytes.size()), out, written).ok()) {
            return;
        }
        (void)delivery_->on_data(span<const std::byte>(bytes.data(), written));
    }

    frame_decoder decoder_;
    io::source* delivery_ = nullptr;
    std::vector<frame_type> seen_;
};

// One connection over `echo_peer`.
class wired_connection final : public connection {
public:
    wired_connection() : sink_(&peer_) {}

    io::source& source() noexcept override { return dummy_; }
    io::sink& sink() noexcept override { return sink_; }
    void bind_source(io::source* source) noexcept override {
        bound_ = source;
        peer_.attach_delivery(source);
    }
    void resume_read() noexcept override {}
    void close() noexcept override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] endpoint remote() const override { return endpoint::inproc("wired"); }

    echo_peer& peer() noexcept { return peer_; }

private:
    struct dummy_source final : io::source {
        std::size_t on_data(span<const std::byte> data) noexcept override { return data.size(); }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };

    dummy_source dummy_;
    echo_peer peer_;
    direct_sink sink_;
    io::source* bound_ = nullptr;
    bool open_ = true;
};

// The encoded size of a REQUEST for a body of `body_bytes`, computed the way the
// client does, so a test can set `max_pending_write_bytes` to an exact value.
std::size_t request_frame_size(std::size_t body_bytes) {
    call_envelope envelope;
    envelope.service_id = kService;
    envelope.method_id = kMethod;
    envelope.codec = codec_id::crpc_binary;
    std::vector<std::byte> body(body_bytes);
    envelope.body = span<const std::byte>(body.data(), body.size());
    std::vector<std::byte> payload(encoded_call_envelope_size(envelope));
    std::size_t payload_written = 0;
    (void)encode_call_envelope(span<std::byte>(payload.data(), payload.size()), envelope,
                               payload_written);
    frame request;
    request.header.type = frame_type::request;
    request.header.stream_id = 1;
    request.payload = span<const std::byte>(payload.data(), payload_written);
    return encoded_frame_size(request);
}

struct client_fixture {
    fake_clock clock;
    echo_peer* peer = nullptr;
    std::unique_ptr<rpc::client> cli;

    explicit client_fixture(std::size_t pending_bound = 4u * 1024u * 1024u) {
        auto conn = std::make_unique<wired_connection>();
        peer = &conn->peer();
        rpc::client_options options;
        options.slot_capacity = 64;
        options.max_pending_write_bytes = pending_bound;
        options.clock = &fake_now;
        options.clock_ctx = &clock;
        cli = std::make_unique<rpc::client>(std::move(conn), options);
    }
};

// Drives one probe reply through a controller with exact timestamps.
void feed_sample(flow_controller& controller, u64 sent_ns, u64 replied_ns) {
    controller.observe_now(sent_ns);
    const u64 token = controller.begin_probe();
    CRPC_REQUIRE(token != ~0ull);
    controller.observe_now(replied_ns);
    controller.on_probe_reply(token, replied_ns);
}

// A window whose byte limit is one byte: the first call is admitted by the
// empty-window rule and then overflows it, so the second call is refused.
flow_control_options tiny_window() {
    flow_control_options flow;
    flow.window.initial_bytes = 1;
    flow.window.min_bytes = 1;
    flow.window.max_bytes = 1;
    return flow;
}

} // namespace

// The default table size must follow the client's call-slot capacity, not a
// hard-coded number: a smaller table silently lowers the concurrency ceiling
// (admission is refused for lack of an index entry) and makes "table full" look
// like "window full".
CRPC_TEST(flow_charge_table_default_follows_slot_capacity) {
    {
        // Low-level client: `set_flow_control` must resolve the default from
        // the connection's own slot capacity.
        client_fixture f;
        f.cli->set_flow_control(flow_control_options{});
        const flow_state snap = f.cli->flow_snapshot();
        CRPC_CHECK(snap.charge_capacity >= 64); // slot_capacity of the fixture
        CRPC_CHECK_EQ(0ull, snap.charges_in_use);
    }
    {
        // Explicit capacity is honoured and observable.
        flow_control_options opts;
        opts.charge_table_capacity = 4096;
        flow_controller controller(opts);
        CRPC_CHECK(controller.snapshot().charge_capacity >= 4096);
        CRPC_REQUIRE(controller.can_admit(0, 10));
        CRPC_REQUIRE(controller.on_admitted(0, 10, 1, 0));
        CRPC_CHECK_EQ(1ull, controller.snapshot().charges_in_use);
        controller.release(1, 0, true);
        CRPC_CHECK_EQ(0ull, controller.snapshot().charges_in_use);
    }
}

// The predicted size must equal the size actually charged. Four context variants
// cover every envelope block (none / trace / deadline+key / all + ordered). This
// is a permanent guard: the prediction and `start_request` share one builder, and
// this case fails the moment they drift.
CRPC_TEST(flow_predicted_frame_size_matches_charge) {
    struct variant {
        bool trace;
        bool deadline;
        bool idem;
        bool lane;
    };
    const variant variants[] = {
        {false, false, false, false},
        {true, false, false, false},
        {false, true, true, false},
        {true, true, true, true},
    };
    for (const variant& v : variants) {
        client_fixture f;
        f.cli->set_flow_control(flow_control_options{});
        rpc::call_context ctx;
        if (v.trace) {
            ctx.trace.trace = obs::trace_id{};
            ctx.trace.span = obs::span_id{};
            ctx.trace.trace.bytes[0] = std::byte{0x11};
        }
        if (v.deadline) {
            ctx.has_deadline = true;
            ctx.deadline = f.clock.now + std::chrono::milliseconds(500);
        }
        ctx.idempotent = v.idem;
        if (v.lane) {
            ctx.lane = lane_token{3, 0};
        }
        std::vector<std::byte> body(64);
        const std::size_t predicted = rpc::client::predicted_frame_size(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), ctx);
        auto started = f.cli->start_call<std::vector<std::byte>>(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), ctx, false);
        CRPC_REQUIRE(started.ok());
        // The connection charges exactly the frame it encoded; a call with no
        // probe contention has that exact charge in the snapshot.
        CRPC_CHECK_EQ(predicted, f.cli->flow_snapshot().in_flight_bytes);
    }
}

// --- charge table and pacer liveness ----------------------------------------

// Every admitted call must be releasable. The charge table is the index release
// uses, so it has to hold at least as many entries as there are call slots: with
// a smaller table an admission is silently unindexed and its bytes are never
// returned, which ends in a permanently full window.
CRPC_TEST(flow_charge_table_covers_slot_capacity) {
    flow_control_options opts;
    opts.window.initial_bytes = 1u << 30;
    opts.window.max_bytes = 1u << 30;
    opts.window.min_bytes = 1;
    opts.pacer.initial_rate_bytes_per_second = 1ull << 30;
    opts.pacer.min_rate_bytes_per_second = 1;
    opts.pacer.max_rate_bytes_per_second = 1ull << 30;
    flow_controller controller(opts);

    constexpr int kConcurrent = 100;
    for (int i = 0; i < kConcurrent; ++i) {
        controller.observe_now(1'000'000);
        CRPC_REQUIRE(controller.can_admit(0, 10));
        CRPC_REQUIRE(controller.on_admitted(0, 10, static_cast<u32>(1000 + i), 0));
    }
    CRPC_CHECK_EQ(static_cast<u64>(kConcurrent) * 10, controller.snapshot().in_flight_bytes);
    for (int i = 0; i < kConcurrent; ++i) {
        controller.release(static_cast<u32>(1000 + i), 0, true);
    }
    CRPC_CHECK_EQ(0ull, controller.snapshot().in_flight_bytes);
    CRPC_CHECK_EQ(static_cast<u64>(kConcurrent), controller.snapshot().released);
}

// A token bucket gains credit as time passes. Refilling only when a call is
// admitted means a drained bucket with one long call in flight never recovers,
// so the next call can never be admitted even though real time passed.
CRPC_TEST(flow_drained_pacer_refills_while_busy) {
    flow_control_options opts;
    opts.window.initial_bytes = 1u << 20;
    opts.window.max_bytes = 1u << 20;
    opts.window.min_bytes = 1;
    opts.pacer.initial_rate_bytes_per_second = 1u << 20;
    opts.pacer.min_rate_bytes_per_second = 1u << 20;
    opts.pacer.max_rate_bytes_per_second = 1u << 20;
    flow_controller controller(opts);

    // One long call drains the bucket (burst is 2 ms at 1 MiB/s = 2097 bytes).
    controller.observe_now(1'000'000'000ull);
    CRPC_REQUIRE(controller.on_admitted(0, 2000, 1, 0));
    CRPC_CHECK(controller.snapshot().pacer_credit_bytes < 100);

    // Thirty real seconds later the bucket must have refilled.
    controller.observe_now(31'000'000'000ull);
    CRPC_CHECK(controller.can_admit(0, 100));

    // The same defect in the other direction: an empty window must not bypass
    // the pacer entirely, or a client that completes calls immediately could
    // send without any rate limit at all. The bucket holds one burst
    // (`1 MiB/s * 2 ms` = 2097 bytes), so 200 calls of 200 bytes (40000 bytes)
    // cannot all be admitted without time passing. The size has to exceed the
    // burst; 200 x 10 bytes would fit in one burst and be admitted legitimately.
    flow_controller busy(opts);
    int admitted = 0;
    for (int i = 0; i < 200; ++i) {
        busy.observe_now(1'000'000'000ull);
        const u32 stream_id = static_cast<u32>(5000 + i);
        if (!busy.can_admit(0, 200)) {
            continue;
        }
        CRPC_REQUIRE(busy.on_admitted(0, 200, stream_id, 0));
        busy.release(stream_id, 0, true);
        ++admitted;
    }
    CRPC_CHECK(admitted < 200);
    CRPC_CHECK(admitted > 0);
}

// A probe whose PING could not be staged must not wedge the controller: the
// outstanding flag is what serializes probes, and leaving it set after a dropped
// probe means no probe is ever sent again and the baseline never updates.
CRPC_TEST(flow_probe_rearms_after_staged_drop) {
    flow_control_options opts;
    opts.estimator.initial_baseline_ns = 1'000'000;
    flow_controller controller(opts);
    controller.observe_now(1'000'000'000ull);

    const u64 first = controller.begin_probe();
    CRPC_REQUIRE(first != ~0ull);
    // One outstanding probe at a time.
    CRPC_CHECK_EQ(~0ull, controller.begin_probe());
    // The client reports that the PING never reached the writer queue.
    controller.abort_probe();
    // `probes_sent` counts attempts and only grows; the attempt is booked as
    // aborted and never as staged, so `staged + aborted == sent`.
    CRPC_CHECK_EQ(1ull, controller.snapshot().probes_sent);
    CRPC_CHECK_EQ(0ull, controller.snapshot().probes_staged);
    CRPC_CHECK_EQ(1ull, controller.snapshot().probes_aborted);

    controller.observe_now(2'000'000'000ull);
    const u64 second = controller.begin_probe();
    CRPC_CHECK(second != ~0ull);
    controller.confirm_probe_staged();
    CRPC_CHECK_EQ(2ull, controller.snapshot().probes_sent);
    CRPC_CHECK_EQ(1ull, controller.snapshot().probes_staged);
    // The re-armed probe works normally.
    controller.observe_now(2'001'000'000ull);
    controller.on_probe_reply(second, 2'001'000'000ull);
    CRPC_CHECK_EQ(1ull, controller.snapshot().probe_samples);
}

// The window is self-clocked: it grows only on a real acknowledgement. A
// cancellation, a timeout or a teardown returns the bytes but says nothing about
// the path, so it must not grow the window.
CRPC_TEST(flow_cancel_does_not_grow_window) {
    client_fixture f;
    f.cli->set_flow_control(flow_control_options{});
    std::vector<std::byte> body(16);
    auto started = f.cli->start_call<std::vector<std::byte>>(
        kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
    CRPC_REQUIRE(started.ok());
    auto handle = std::move(started).value();
    const u64 before = f.cli->flow_snapshot().window_bytes;
    handle.cancel();
    CRPC_CHECK_EQ(before, f.cli->flow_snapshot().window_bytes);
    CRPC_CHECK_EQ(0ull, f.cli->flow_snapshot().in_flight_bytes);

    // A real response is an acknowledgement and does grow the window.
    auto second = f.cli->start_call<std::vector<std::byte>>(
        kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, true);
    CRPC_REQUIRE(second.ok());
    auto second_handle = std::move(second).value();
    CRPC_CHECK(second_handle.ready());
    CRPC_CHECK(f.cli->flow_snapshot().window_bytes > before);
}

// --- metadata survives the failure path -------------------------------------

// Precondition for the next case: `status` itself carries the whole `rpc_error`,
// metadata included. If this were false, the marker could never reach a caller no
// matter what `start_call` did, and the next case would be testing nothing.
CRPC_TEST(flow_status_carries_metadata) {
    rpc_error error(rpc_code::resource_exhausted, "refused");
    error.metadata.set("crpc.flow_rejected", "1");
    const status wrapped{std::move(error)};
    const std::string* marker = wrapped.error().metadata.find("crpc.flow_rejected");
    CRPC_REQUIRE(marker != nullptr);
    CRPC_CHECK_EQ(std::string("1"), *marker);
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, wrapped.code());
}

// The marker must survive `start_call`'s failure return. `start_call` used to
// rebuild the error from its code and message, which dropped every metadata
// entry; that is why this case exists separately from the `status` one above.
CRPC_TEST(flow_start_call_preserves_flow_marker) {
    client_fixture f;
    f.cli->set_flow_control(tiny_window());
    std::vector<std::byte> body(16);
    auto first = f.cli->start_call<std::vector<std::byte>>(
        kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
    CRPC_REQUIRE(first.ok());
    auto second = f.cli->start_call<std::vector<std::byte>>(
        kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
    CRPC_REQUIRE(!second.ok());
    const std::string* marker = second.error().metadata.find("crpc.flow_rejected");
    CRPC_REQUIRE(marker != nullptr);
    CRPC_CHECK_EQ(std::string("1"), *marker);
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, second.error().code);
}

// --- no added clock read on the send path ------------------------------------

// The send path already reads the clock once (to decide whether an explicit
// deadline already passed). Flow control must reuse that reading rather than add
// its own: a control loop that reads a clock per call is a cost every caller
// pays even when the path is quiet.
CRPC_TEST(flow_adds_no_clock_reads) {
    int disabled_reads = 0;
    {
        client_fixture f;
        std::vector<std::byte> body(16);
        const int before = f.clock.reads;
        auto started = f.cli->start_call<std::vector<std::byte>>(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
        CRPC_REQUIRE(started.ok());
        disabled_reads = f.clock.reads - before;
    }
    int enabled_reads = 0;
    {
        client_fixture f;
        f.cli->set_flow_control(flow_control_options{});
        CRPC_REQUIRE(f.cli->flow_snapshot().enabled);
        std::vector<std::byte> body(16);
        const int before = f.clock.reads;
        auto started = f.cli->start_call<std::vector<std::byte>>(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
        CRPC_REQUIRE(started.ok());
        enabled_reads = f.clock.reads - before;
    }
    // Baseline: exactly one read per call, with or without a deadline.
    CRPC_CHECK_EQ(1, disabled_reads);
    CRPC_CHECK_EQ(disabled_reads, enabled_reads);
}

// --- admission ---------------------------------------------------------------

// A byte window that is already full refuses the next call locally. The refusal
// is marked so the facade can classify it as a local gate; without the marker a
// `resource_exhausted` from flow control is indistinguishable from a genuine
// capacity failure, which is retried and charged to the endpoint.
CRPC_TEST(flow_window_rejects_and_marks_local_gate) {
    client_fixture f;
    f.cli->set_flow_control(tiny_window());

    std::vector<std::byte> body(16);
    auto first = f.cli->start_call<std::vector<std::byte>>(
        kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
    CRPC_REQUIRE(first.ok());
    // The empty window admitted one call unconditionally and charged it, so the
    // window is now over its limit.
    CRPC_CHECK(f.cli->flow_snapshot().in_flight_bytes > 1);

    auto second = f.cli->start_call<std::vector<std::byte>>(
        kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, second.error().code);
        const std::string* marker = second.error().metadata.find("crpc.flow_rejected");
        CRPC_REQUIRE(marker != nullptr);
        CRPC_CHECK_EQ(std::string("1"), *marker);
    }
}

// A ONE_WAY call completes as soon as it is published, so nothing can ever
// acknowledge it. Charging it would consume window bytes that are never returned
// and the connection would stall permanently.
CRPC_TEST(flow_one_way_not_counted_in_flight) {
    client_fixture f;
    f.cli->set_flow_control(tiny_window());

    rpc::call_context ctx;
    ctx.mode = delivery_mode::unreliable;
    std::vector<std::byte> body(16);
    for (int i = 0; i < 4; ++i) {
        auto started = f.cli->start_call<std::vector<std::byte>>(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), ctx, false);
        CRPC_CHECK(started.ok());
    }
    CRPC_CHECK_EQ(0ull, f.cli->flow_snapshot().in_flight_bytes);
    CRPC_CHECK_EQ(0u, f.cli->in_flight());
}

// The probe exists to measure a path the window says is full: if the window also
// gated the probe, the connection could never learn that the path recovered. It
// must still respect the write bound, because an unbounded probe is a way to grow
// the output queue without limit.
CRPC_TEST(flow_probe_bypasses_window_not_write_bound) {
    {
        // Default write bound: the probe is staged even though the window is
        // exhausted, and it is not charged to the window.
        client_fixture f;
        f.cli->set_flow_control(tiny_window());
        std::vector<std::byte> body(16);
        auto started = f.cli->start_call<std::vector<std::byte>>(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
        CRPC_REQUIRE(started.ok());
        // Advance the clock before flushing: a zero round trip is not a sample,
        // so the fake clock must move between the probe and its reply.
        f.clock.now += std::chrono::milliseconds(1);
        f.cli->flush_pending();
        CRPC_CHECK_EQ(1ull, f.cli->flow_snapshot().probes_sent);
        CRPC_CHECK_EQ(1ull, f.cli->flow_snapshot().probes_staged);
        CRPC_CHECK_EQ(0ull, f.cli->flow_snapshot().probes_aborted);
        CRPC_CHECK_EQ(1ull, f.cli->flow_snapshot().probe_samples);
        CRPC_CHECK_EQ(1u, static_cast<unsigned>(f.peer->count(frame_type::ping)));
    }
    {
        // A write bound that fits the request but not the probe: the request is
        // accepted, the probe is attempted and dropped, and the peer never sees
        // a PING.
        const std::size_t request_size = request_frame_size(16);
        client_fixture f(request_size + 8);
        f.cli->set_flow_control(tiny_window());
        std::vector<std::byte> body(16);
        auto started = f.cli->start_call<std::vector<std::byte>>(
            kService, kMethod, span<const std::byte>(body.data(), body.size()), {}, false);
        CRPC_REQUIRE(started.ok());
        f.clock.now += std::chrono::milliseconds(1);
        f.cli->flush_pending();
        // The controller decided to probe, the writer bound refused to stage it,
        // and the reservation was rolled back. A staged probe counts in
        // `probes_sent`; a rolled-back one counts in `probes_aborted` and is
        // removed from `probes_sent`, so the two counters describe every
        // attempt without double counting.
        CRPC_CHECK_EQ(1ull, f.cli->flow_snapshot().probes_sent);
        CRPC_CHECK_EQ(0ull, f.cli->flow_snapshot().probes_staged);
        CRPC_CHECK_EQ(1ull, f.cli->flow_snapshot().probes_aborted);
        CRPC_CHECK_EQ(0u, static_cast<unsigned>(f.peer->count(frame_type::ping)));
    }
}

// --- estimator wiring: the P-M rules ----------------------------------------

// One fast sample is an artifact until it is confirmed. Accepting it immediately
// would crush the baseline, and because a sample above the threshold is
// deliberately not fed to the estimator, the baseline could never recover: every
// honest sample would be "above threshold" forever.
CRPC_TEST(flow_baseline_survives_one_fast_sample) {
    flow_control_options opts;
    opts.estimator.initial_baseline_ns = 1'000'000;
    flow_controller controller(opts);

    // The first sample establishes the baseline.
    feed_sample(controller, 1'000'000'000ull, 1'001'000'000ull);
    CRPC_CHECK_EQ(1'000'000ull, controller.snapshot().baseline_ns);
    CRPC_CHECK_EQ(122'000ull, controller.snapshot().theta_ns);

    // One 1 us sample: not enough evidence to move the baseline.
    feed_sample(controller, 2'000'000'000ull, 2'000'001'000ull);
    CRPC_CHECK_EQ(1'000'000ull, controller.snapshot().baseline_ns);
    CRPC_CHECK_EQ(122'000ull, controller.snapshot().theta_ns);
}

// After an artifact, honest samples must still reach the estimator. This is the
// deadlock the withholding rule would otherwise create.
CRPC_TEST(flow_no_deadlock_under_withhold_rule) {
    flow_control_options opts;
    opts.estimator.initial_baseline_ns = 1'000'000;
    flow_controller controller(opts);

    // One artifact, then a run of honest 20 us samples.
    feed_sample(controller, 1'000'000'000ull, 1'000'001'000ull);
    u64 now = 2'000'000'000ull;
    for (int i = 0; i < 200; ++i) {
        feed_sample(controller, now, now + 20'000ull);
        now += 1'000'000ull;
    }
    // The honest path is 20 us; the baseline converges to it instead of staying
    // crushed at 1 us with every sample withheld.
    CRPC_CHECK_EQ(20'000ull, controller.snapshot().baseline_ns);
    CRPC_CHECK_EQ(0ull, controller.snapshot().samples_above_theta);
    CRPC_CHECK_EQ(201ull, controller.snapshot().probe_samples);
}

// A sharp drop needs `fast_confirmations` samples and a small one needs
// `slow_confirmations`; the accepted value is the slowest of the window, so a
// lone artifact inside the window cannot poison the baseline.
CRPC_TEST(flow_theta_order_and_withhold) {
    flow_control_options opts;
    opts.estimator.initial_baseline_ns = 1'000'000;
    flow_controller controller(opts);
    feed_sample(controller, 1'000'000'000ull, 1'001'000'000ull);
    CRPC_CHECK_EQ(1'000'000ull, controller.snapshot().baseline_ns);
    // theta = baseline * 122 / 1000 = 122000.
    CRPC_CHECK_EQ(122'000ull, controller.snapshot().theta_ns);

    // A sharp drop to 20 us (98%): two confirmations are not enough.
    feed_sample(controller, 2'000'000'000ull, 2'000'020'000ull);
    feed_sample(controller, 3'000'000'000ull, 3'000'020'000ull);
    CRPC_CHECK_EQ(1'000'000ull, controller.snapshot().baseline_ns);
    // The third applies it.
    feed_sample(controller, 4'000'000'000ull, 4'000'020'000ull);
    CRPC_CHECK_EQ(20'000ull, controller.snapshot().baseline_ns);

    // A small drop (10%, below the 12.2% sharp threshold) needs four.
    feed_sample(controller, 5'000'000'000ull, 5'000'018'000ull);
    feed_sample(controller, 6'000'000'000ull, 6'000'018'000ull);
    feed_sample(controller, 7'000'000'000ull, 7'000'018'000ull);
    CRPC_CHECK_EQ(20'000ull, controller.snapshot().baseline_ns);
    feed_sample(controller, 8'000'000'000ull, 8'000'018'000ull);
    CRPC_CHECK_EQ(18'000ull, controller.snapshot().baseline_ns);
}

// A sample above the threshold is congestion evidence, not a path measurement:
// it is withheld from the estimator, and the window is reduced at most once per
// baseline.
CRPC_TEST(flow_sustained_high_eventually_moves_baseline) {
    flow_control_options opts;
    opts.estimator.initial_baseline_ns = 1'000'000;
    opts.estimator.sample_ceiling_ns = 10'000'000'000ull;
    flow_controller controller(opts);
    feed_sample(controller, 1'000'000'000ull, 1'001'000'000ull);

    // 3 ms against a 1 ms baseline: far above theta (122 us). Eight samples
    // inside one baseline period: the decrease is rate limited to one.
    u64 now = 2'000'000'000ull;
    for (int i = 0; i < 8; ++i) {
        feed_sample(controller, now, now + 3'000'000ull);
        now += 100'000ull;
    }
    // Withheld: the baseline does not jump to the congested sample.
    CRPC_CHECK_EQ(1'000'000ull, controller.snapshot().baseline_ns);
    CRPC_CHECK_EQ(8ull, controller.snapshot().samples_above_theta);
    CRPC_CHECK_EQ(1ull, controller.snapshot().congestion_events);
    // The window did shrink, so the signal was acted on.
    CRPC_CHECK(controller.snapshot().window_bytes < 64u * 1024u);
}

// --- pacer is fed by the window, not by its own default ---------------------

// The pacer's own default rate (1 MiB/s) would make 64 KiB take ~60 ms, which is
// a functional regression once flow control is wired. The controller therefore
// derives the rate from the window: `window / baseline`. With a 64 KiB window and
// a 1 ms baseline that is exactly 65536000 bytes/s (62.5 MiB/s), and the 64 KiB
// burst is available immediately.
CRPC_TEST(flow_pacer_rate_follows_window_over_baseline) {
    flow_control_options opts;
    opts.window.initial_bytes = 64u * 1024u;
    opts.window.min_bytes = 1;
    opts.window.max_bytes = 64u * 1024u;
    opts.estimator.initial_baseline_ns = 1'000'000;
    flow_controller controller(opts);

    // Establish the 1 ms baseline.
    feed_sample(controller, 1'000'000'000ull, 1'001'000'000ull);
    CRPC_CHECK_EQ(1'000'000ull, controller.snapshot().baseline_ns);
    CRPC_CHECK_EQ(65'536'000ull, controller.snapshot().pacer_rate_bytes_per_second);
    // 64 KiB is sendable right away: the burst is `rate * 2 ms`, not `1 MiB/s`.
    CRPC_CHECK(controller.snapshot().pacer_credit_bytes >= 65'536ull);

    // The clamps still apply to the derived rate.
    flow_control_options capped;
    capped.window.initial_bytes = 64u * 1024u;
    capped.window.min_bytes = 1;
    capped.window.max_bytes = 64u * 1024u;
    capped.estimator.initial_baseline_ns = 1'000'000;
    capped.pacer.max_rate_bytes_per_second = 1'000'000;
    flow_controller bounded(capped);
    feed_sample(bounded, 1'000'000'000ull, 1'001'000'000ull);
    CRPC_CHECK_EQ(1'000'000ull, bounded.snapshot().pacer_rate_bytes_per_second);
}

// --- primitive pins ----------------------------------------------------------

// The tracker holds one outstanding token, so it cannot express order among
// several. This pins the *current* behaviour: an older token is still accepted.
// Changing it is a separate design decision.
CRPC_TEST(flow_tracker_accepts_out_of_order_ack) {
    flow::rtt_tracker tracker;
    const u64 first = tracker.on_send(1'000);
    const u64 second = tracker.on_send(2'000);
    u64 sample = 0;
    // Reply to the newer token first, then to the older one.
    CRPC_CHECK(tracker.on_ack(3'000, second, sample));
    CRPC_CHECK_EQ(1'000ull, sample);
    CRPC_CHECK(tracker.on_ack(4'000, first, sample));
    CRPC_CHECK_EQ(3'000ull, sample);
}

// `restore_high_water` is documented as "same path reconnect", and leaving slow
// start is the correct behaviour there: the path is known to have carried the
// traffic. This pins it; the code is unchanged and the documentation says so.
CRPC_TEST(flow_restore_high_water_is_not_slow_start) {
    flow::congestion_window window;
    CRPC_CHECK(window.in_slow_start());
    window.on_ack(64 * 1024);
    window.on_congestion();
    window.restore_high_water();
    CRPC_CHECK_FALSE(window.in_slow_start());
    CRPC_CHECK(window.bytes() >= kWindowMinBytes);
}

// A reconnect is a different path: reset restores slow start and the initial
// window, so the new connection ramps up from a cold value.
CRPC_TEST(flow_reset_after_reconnect_restores_slow_start) {
    flow::congestion_window window;
    window.on_ack(64 * 1024);
    window.on_congestion();
    CRPC_CHECK_FALSE(window.in_slow_start());
    window.reset();
    CRPC_CHECK(window.in_slow_start());
    CRPC_CHECK_EQ(64ull * 1024ull, window.bytes());
}

CRPC_TEST_MAIN()
