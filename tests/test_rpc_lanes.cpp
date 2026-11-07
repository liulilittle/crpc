// CRPC -- ordered-lane end-to-end tests (requirement 11).
//
// These cases drive the real server request path (inproc transport, shard
// runtimes, dispatcher) and pin the two core invariants the scheduler is for,
// plus the edge semantics the unit tests cannot see from the outside:
//
//   * same lane, concurrent submission -> execution order is strictly the
//     per-lane `seq` order and the observed handler-concurrency peak is 1;
//   * a blocked lane does not block another lane (no head-of-line blocking),
//     proven with a gate and a time lower bound rather than sleeps;
//   * out-of-order arrival (frames staged so seq 1 reaches the server before
//     seq 0) still executes 0 then 1;
//   * a missing sequence is skipped after the bounded wait, deterministically
//     late, and never hangs;
//   * a duplicate sequence without an idempotency key is refused, while the
//     same sequence replayed with its key rides the dedup table to the
//     original outcome;
//   * plain calls (no lane token) are untouched;
//   * connection close / shutdown fails every queued call with an explicit
//     error and returns the server's in-flight accounting to zero;
//   * a long run across many lanes leaves no accounting behind.
//
// The scheduler's own state machine is pinned in test_rpc_lane_queue.cpp.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

constexpr crpc::u64 kService = 200;
constexpr crpc::u64 kProbe = 10;
constexpr crpc::u64 kBlock = 11;
constexpr crpc::u64 kEcho = 12;

// One service whose handlers record what a lane-ordered consumer needs:
// the execution order as `(lane, seq, saw-token)`, the concurrency peak and a
// gate that parks a handler deterministically.
struct lane_service {
    struct lane_record {
        crpc::u64 lane = 0;
        crpc::u64 seq = 0;
        bool has_lane = false;
    };

    std::mutex order_mutex;
    std::vector<lane_record> order;
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<int> started{0};
    // 0 = the blocking handler parks; 1 = released. `gate_open_ns` records
    // when it flipped, so a test can assert a time lower bound.
    std::atomic<int> gate{0};
    std::atomic<long long> gate_open_ns{0};
    std::atomic<long long> other_done_ns{0};

    void record(const crpc::rpc::call_context& ctx, crpc::u64 fallback_seq) {
        const int prior = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = max_in_flight.load(std::memory_order_acquire);
        while (prior > observed &&
               !max_in_flight.compare_exchange_weak(observed, prior,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
        }
        std::lock_guard<std::mutex> lk(order_mutex);
        order.push_back(lane_record{ctx.lane.has_value() ? ctx.lane->lane_id : 0,
                               ctx.lane.has_value() ? ctx.lane->seq : fallback_seq,
                               ctx.lane.has_value()});
    }

    void leave() noexcept { in_flight.fetch_sub(1, std::memory_order_acq_rel); }

    static crpc::status probe(void* self, const crpc::call_envelope&,
                              crpc::span<const std::byte> body, crpc::buffer& out,
                              crpc::rpc::call_context& ctx) {
        auto* state = static_cast<lane_service*>(self);
        crpc::u64 value = 0;
        const crpc::status decoded = crpc::rpc::call_codec<crpc::u64>::decode(body, value);
        if (!decoded.ok()) {
            return decoded;
        }
        state->record(ctx, value);
        const crpc::status encoded = crpc::rpc::call_codec<crpc::u64>::encode(value, out);
        state->leave();
        return encoded;
    }

    static crpc::status block(void* self, const crpc::call_envelope& envelope,
                              crpc::span<const std::byte> body, crpc::buffer& out,
                              crpc::rpc::call_context& ctx) {
        auto* state = static_cast<lane_service*>(self);
        state->started.fetch_add(1, std::memory_order_acq_rel);
        // Deterministic park: the handler stays inside dispatch until the test
        // opens the gate, which is what makes "the other lane finished first"
        // a proof rather than a timing bet.
        while (state->gate.load(std::memory_order_acquire) == 0) {
            std::this_thread::sleep_for(1ms);
        }
        return probe(self, envelope, body, out, ctx);
    }

    // Echoes an arbitrary-length body without decoding it: the byte-cap
    // end-to-end cases drive the cap with multi-KiB frames, which no
    // fixed-size codec can decode.
    static crpc::status echo_blob(void* self, const crpc::call_envelope&,
                                  crpc::span<const std::byte> body, crpc::buffer& out,
                                  crpc::rpc::call_context& ctx) {
        auto* state = static_cast<lane_service*>(self);
        state->record(ctx, 0);
        const crpc::status encoded = out.append(body) ? crpc::status{}
                                                      : crpc::status{crpc::rpc_code::encode_error};
        state->leave();
        return encoded;
    }
};

long long steady_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Opens the gate on every exit path: the blocked handler holds its shard
// thread, so the fixture's server stop/join needs the gate open even when an
// assertion failed earlier in the body.
struct gate_opener {
    lane_service* state = nullptr;
    ~gate_opener() {
        if (state != nullptr) {
            const int expected = 0;
            int desired = 1;
            (void)expected;
            state->gate.store(desired, std::memory_order_release);
        }
    }
};

struct env_guard {
    explicit env_guard(const char* value, const char* name = "CRPC_LANE_GAP_TIMEOUT_MS")
        : name_(name) {
#if defined(_MSC_VER)
        char* previous = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&previous, &size, name_) == 0 && previous != nullptr) {
            previous_.assign(previous, size > 0 ? size - 1 : 0);
            std::free(previous);
            had_ = true;
        }
        (void)_putenv_s(name_, value);
#else
        const char* previous = std::getenv(name_);
        if (previous != nullptr) {
            previous_.assign(previous);
            had_ = true;
        }
        (void)setenv(name_, value, 1);
#endif
    }
    ~env_guard() {
#if defined(_MSC_VER)
        (void)_putenv_s(name_, had_ ? previous_.c_str() : "");
#else
        if (had_) {
            (void)setenv(name_, previous_.c_str(), 1);
        } else {
            (void)unsetenv(name_);
        }
#endif
    }
    env_guard(const env_guard&) = delete;
    env_guard& operator=(const env_guard&) = delete;

    const char* name_;
    std::string previous_;
    bool had_ = false;
};

struct lane_fixture {
    crpc::inproc_backend backend;
    lane_service service;
    std::unique_ptr<crpc::runtime> shard;
    std::unique_ptr<crpc::client> cli;
    std::unique_ptr<crpc::server> srv;
    std::string name = "lane-e2e";

    lane_fixture() = default;
    ~lane_fixture() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
        if (shard) {
            shard->stop();
            shard->join();
        }
    }

    bool start(std::size_t shards = 1) {
        crpc::server::builder sb;
        sb.add_listener(crpc::endpoint::inproc(name), backend);
        sb.add_service(kService, "lane_svc");
        sb.add_raw_method(kService, kProbe, "probe", &lane_service::probe, &service);
        sb.add_raw_method(kService, kBlock, "block", &lane_service::block, &service);
        sb.add_raw_method(kService, kEcho, "echo_blob", &lane_service::echo_blob, &service);
        sb.shards(shards);
        // Shutdown must not sit out a long drain when a handler is parked on
        // the gate; the tests below hold that bound explicitly.
        sb.drain_timeout(500ms);
        srv = sb.build();
        if (!srv || !srv->start().ok()) {
            return false;
        }
        return true;
    }

    // Facade with one pooled connection: a lane caller on this client gets
    // the end-to-end per-connection ordering guarantee. The plain-thread wait
    // bound and the deadline are deliberately generous: the shared CI box
    // routinely saturates every core while other suites build, and a wait
    // bound of 5 s then turns scheduling latency into a local timeout that
    // has nothing to do with lane ordering.
    bool start_facade() {
        crpc::runtime_options ro;
        ro.start_handler_pool = false;
        shard = std::make_unique<crpc::runtime>(ro);
        shard->start_thread();
        crpc::client::builder cb;
        cb.add_endpoint(crpc::endpoint::inproc(name), backend);
        cb.pool_size(1);
        cb.default_deadline(30s);
        cb.plain_thread_timeout(25s);
        cb.on_shard(*shard);
        // A lane call may queue behind its predecessors; the default breaker's
        // 1 ms slow threshold would open on ambient latency (see the fixture
        // comment in test_server_client.cpp), so pin it high.
        crpc::resilience::breaker_config relaxed;
        relaxed.slow_call_threshold = 25s;
        cb.breaker(relaxed);
        cli = cb.build();
        return cli != nullptr;
    }
};

std::unique_ptr<crpc::rpc::client> raw_client(lane_fixture& f) {
    auto conn = f.backend.connect(crpc::endpoint::inproc(f.name), crpc::connect_options{});
    if (!conn) {
        return nullptr;
    }
    crpc::rpc::client_options opts;
    opts.slot_capacity = 64;
    opts.default_timeout = std::chrono::nanoseconds{0};
    return std::make_unique<crpc::rpc::client>(std::move(conn), opts);
}

bool encode_u64(crpc::u64 value, crpc::buffer& out) {
    return crpc::rpc::call_codec<crpc::u64>::encode(value, out).ok();
}

crpc::rpc::call_context lane_ctx(crpc::u64 lane, crpc::u64 seq) {
    crpc::rpc::call_context ctx;
    ctx.lane = crpc::rpc::lane_token{lane, seq};
    return ctx;
}

} // namespace

// Core invariant (a): 24 calls on one lane submitted from three threads run
// strictly in seq order and never overlap (observed concurrency peak == 1).
// The threads make the client-side frame order genuinely race: a higher seq
// can reach the server first, which is exactly the out-of-order case the lane
// machinery buffers.
CRPC_TEST(lanes_e2e_same_lane_strict_order_max_concurrency_one) {
    lane_fixture f;
    f.name = "lane-e2e-order";
    CRPC_CHECK(f.start());
    CRPC_CHECK(f.start_facade());
    if (!f.srv || !f.cli) {
        return;
    }

    constexpr int kThreads = 3;
    constexpr int kPerThread = 8;
    constexpr crpc::u64 kLane = 7;
    std::atomic<int> failures{0};
    {
        auto lane = f.cli->lane(kLane);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                auto stub = lane.service<crpc::u64, crpc::u64>(kService, kProbe);
                for (int i = 0; i < kPerThread; ++i) {
                    const crpc::u64 value =
                        static_cast<crpc::u64>(t * kPerThread + i);
                    auto started = stub.call(value);
                    if (!started.ok()) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    auto handle = std::move(started).value();
                    auto result = handle.get();
                    if (!result.ok() || result.value() != value) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (std::thread& th : threads) {
            th.join();
        }
    }
    CRPC_CHECK_EQ(0, failures.load());
    CRPC_CHECK_EQ(kThreads * kPerThread, static_cast<int>(f.service.order.size()));
    // The execution log is exactly the seq order: position k holds seq k.
    if (f.service.order.size() == static_cast<std::size_t>(kThreads * kPerThread)) {
        for (std::size_t k = 0; k < f.service.order.size(); ++k) {
            CRPC_CHECK_EQ(static_cast<crpc::u64>(k), f.service.order[k].seq);
            CRPC_CHECK_EQ(kLane, f.service.order[k].lane);
            CRPC_CHECK(f.service.order[k].has_lane);
        }
    }
    // No two handlers of the lane ever overlapped.
    CRPC_CHECK_EQ(1, f.service.max_in_flight.load());
}

// Core invariant (b): a lane whose handler is parked on a gate does not stop
// another lane from completing. The proof is a time lower bound, not a sleep
// guess: B finishes strictly before the gate opens, and A's handler is
// observably inside the gate the whole time.
CRPC_TEST(lanes_e2e_blocked_lane_does_not_block_other_lanes) {
    lane_fixture f;
    f.name = "lane-e2e-hol";
    f.service.gate.store(0, std::memory_order_release);
    CRPC_CHECK(f.start(2)); // two shards, one per raw connection (round-robin)
    if (!f.srv) {
        return;
    }
    gate_opener opener{&f.service};
    auto a = raw_client(f);
    auto b = raw_client(f);
    CRPC_CHECK(a && b);
    if (!a || !b) {
        return;
    }

    crpc::buffer body;
    CRPC_CHECK(encode_u64(1, body));
    auto started_a = a->start_call<crpc::u64>(kService, kBlock, body.readable(),
                                              lane_ctx(1, 0));
    CRPC_CHECK(started_a.ok());
    if (!started_a.ok()) {
        return;
    }
    auto handle_a = std::move(started_a).value();
    // A's handler is provably parked (started, gate still closed).
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.service.started.load(std::memory_order_acquire) >= 1; }, 10000));
    CRPC_CHECK_EQ(0, f.service.gate.load(std::memory_order_acquire));

    auto started_b = b->start_call<crpc::u64>(kService, kProbe, body.readable(),
                                              lane_ctx(2, 0));
    CRPC_CHECK(started_b.ok());
    if (!started_b.ok()) {
        return;
    }
    auto handle_b = std::move(started_b).value();
    auto result_b = handle_b.get();
    f.service.other_done_ns.store(steady_ms(), std::memory_order_release);
    CRPC_CHECK(result_b.ok());
    if (result_b.ok()) {
        CRPC_CHECK_EQ(1u, result_b.value());
    }
    // A is still inside the blocked handler: it cannot have finished while
    // the gate is closed.
    CRPC_CHECK_FALSE(handle_a.ready());

    // Lower bound: the gate (and therefore A's completion) opens no earlier
    // than B's response was already in hand. The structural proof above --
    // A started, A not finished, B finished, gate still closed -- carries the
    // invariant; the timestamps are the measurable form of the same fact.
    f.service.gate.store(1, std::memory_order_release);
    f.service.gate_open_ns.store(steady_ms(), std::memory_order_release);
    auto result_a = handle_a.get();
    CRPC_CHECK(result_a.ok());
    CRPC_CHECK(f.service.other_done_ns.load(std::memory_order_acquire) <=
               f.service.gate_open_ns.load(std::memory_order_acquire));
}

// Out-of-order arrival: the frames are staged so seq 1 reaches the server
// before seq 0 (the writer queue is FIFO, so staging order is wire order).
// The lane buffers 1 behind the missing 0 and executes 0 then 1.
CRPC_TEST(lanes_e2e_out_of_order_arrival_runs_in_seq_order) {
    lane_fixture f;
    f.name = "lane-e2e-ooo";
    CRPC_CHECK(f.start());
    if (!f.srv) {
        return;
    }
    auto c = raw_client(f);
    CRPC_CHECK(c != nullptr);
    if (!c) {
        return;
    }

    crpc::buffer body1;
    crpc::buffer body0;
    CRPC_CHECK(encode_u64(11, body1));
    CRPC_CHECK(encode_u64(10, body0));
    // flush = false stages the frame; staging seq 1 first puts it first on
    // the wire when the pending bytes are flushed together.
    auto started1 = c->start_call<crpc::u64>(kService, kProbe, body1.readable(),
                                             lane_ctx(9, 1), false);
    CRPC_CHECK(started1.ok());
    auto started0 = c->start_call<crpc::u64>(kService, kProbe, body0.readable(),
                                             lane_ctx(9, 0), false);
    CRPC_CHECK(started0.ok());
    if (!started1.ok() || !started0.ok()) {
        return;
    }
    c->flush_pending();
    auto handle1 = std::move(started1).value();
    auto handle0 = std::move(started0).value();

    auto result0 = handle0.get();
    auto result1 = handle1.get();
    CRPC_CHECK(result0.ok());
    CRPC_CHECK(result1.ok());
    if (result0.ok() && result1.ok()) {
        CRPC_CHECK_EQ(10u, result0.value());
        CRPC_CHECK_EQ(11u, result1.value());
    }
    std::lock_guard<std::mutex> lk(f.service.order_mutex);
    CRPC_CHECK_EQ(2u, f.service.order.size());
    if (f.service.order.size() == 2) {
        CRPC_CHECK_EQ(9u, f.service.order[0].lane);
        CRPC_CHECK_EQ(0u, f.service.order[0].seq);
        CRPC_CHECK_EQ(9u, f.service.order[1].lane);
        CRPC_CHECK_EQ(1u, f.service.order[1].seq);
    }
    CRPC_CHECK_EQ(1, f.service.max_in_flight.load());
}

// A missing sequence is skipped only after the bounded wait: the buffered
// call does not run early, and it runs without hanging once the gap expires.
// The skip deadline is driven by the server's gap timer (3 s here), so the
// negative window is checked against a condition with a generous margin.
CRPC_TEST(lanes_e2e_missing_seq_skipped_after_bounded_wait) {
    env_guard gap_ms("3000");
    lane_fixture f;
    f.name = "lane-e2e-gap";
    CRPC_CHECK(f.start());
    if (!f.srv) {
        return;
    }
    auto c = raw_client(f);
    CRPC_CHECK(c != nullptr);
    if (!c) {
        return;
    }

    crpc::buffer body;
    CRPC_CHECK(encode_u64(2, body));
    // seq 0: runs immediately.
    auto s0 = c->start_call<crpc::u64>(kService, kProbe, body.readable(), lane_ctx(5, 0));
    CRPC_CHECK(s0.ok());
    if (!s0.ok()) {
        return;
    }
    auto h0 = std::move(s0).value();
    CRPC_CHECK(h0.get().ok());
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            std::lock_guard<std::mutex> lk(f.service.order_mutex);
            return f.service.order.size() >= 1;
        },
        2000));

    // seq 2 with seq 1 never sent: parked behind the gap.
    auto s2 = c->start_call<crpc::u64>(kService, kProbe, body.readable(), lane_ctx(5, 2));
    CRPC_CHECK(s2.ok());
    if (!s2.ok()) {
        return;
    }
    auto h2 = std::move(s2).value();

    // The gap wait is 3 s and starts at seq 0's drain; a full second later
    // the buffered call must still not have run. A condition wait with a
    // hard budget (never a sleep-and-hope) guards this upper bound.
    const bool early = crpc_test::wait_for(
        [&] {
            std::lock_guard<std::mutex> lk(f.service.order_mutex);
            return f.service.order.size() >= 2;
        },
        1000);
    CRPC_CHECK_FALSE(early);

    // After the skip the call completes normally.
    auto result2 = h2.get();
    CRPC_CHECK(result2.ok());
    std::lock_guard<std::mutex> lk(f.service.order_mutex);
    CRPC_CHECK_EQ(2u, f.service.order.size());
    if (f.service.order.size() == 2) {
        CRPC_CHECK_EQ(0u, f.service.order[0].seq);
        CRPC_CHECK_EQ(2u, f.service.order[1].seq);
    }
}

// Duplicate sequence handling. Without an idempotency key a replayed seq is
// refused (its position was consumed; running it would double-apply the side
// effect). With the call's own key the replay rides the dedup table to the
// original outcome instead of a refusal.
CRPC_TEST(lanes_e2e_duplicate_rejected_and_keyed_replay_dedups) {
    lane_fixture f;
    f.name = "lane-e2e-dup";
    CRPC_CHECK(f.start());
    if (!f.srv) {
        return;
    }
    auto c = raw_client(f);
    CRPC_CHECK(c != nullptr);
    if (!c) {
        return;
    }

    crpc::buffer body;
    CRPC_CHECK(encode_u64(31, body));

    // Unkeyed: seq 0 runs; replaying seq 0 is refused without executing.
    auto first = c->start_call<crpc::u64>(kService, kProbe, body.readable(), lane_ctx(11, 0));
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    auto first_handle = std::move(first).value();
    auto first_result = first_handle.get();
    CRPC_CHECK(first_result.ok());

    auto replay = c->start_call<crpc::u64>(kService, kProbe, body.readable(), lane_ctx(11, 0));
    CRPC_CHECK(replay.ok());
    if (replay.ok()) {
        auto replay_result = std::move(replay).value().get();
        CRPC_CHECK_FALSE(replay_result.ok());
        CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition, replay_result.code());
    }

    // Keyed: the same key and the next lane position replay the original
    // outcome. The key is the R6 triple; both attempts send the same one.
    crpc::rpc::call_id key;
    key.client_id[0] = std::byte{0x5A};
    key.client_epoch = 1;
    key.seq = 77;
    crpc::buffer body2;
    CRPC_CHECK(encode_u64(32, body2));
    crpc::rpc::call_context keyed = lane_ctx(11, 1);
    keyed.idempotent = true;
    keyed.has_idempotency_key = true;
    keyed.idempotency = key;

    auto leader = c->start_call<crpc::u64>(kService, kProbe, body2.readable(), keyed);
    CRPC_CHECK(leader.ok());
    if (!leader.ok()) {
        return;
    }
    auto leader_result = std::move(leader).value().get();
    CRPC_CHECK(leader_result.ok());
    if (leader_result.ok()) {
        CRPC_CHECK_EQ(32u, leader_result.value());
    }

    crpc::rpc::call_context replay_keyed = lane_ctx(11, 1);
    replay_keyed.idempotent = true;
    replay_keyed.has_idempotency_key = true;
    replay_keyed.idempotency = key;
    auto replayed = c->start_call<crpc::u64>(kService, kProbe, body2.readable(), replay_keyed);
    CRPC_CHECK(replayed.ok());
    if (replayed.ok()) {
        auto replayed_result = std::move(replayed).value().get();
        CRPC_CHECK(replayed_result.ok());
        if (replayed_result.ok()) {
            CRPC_CHECK_EQ(32u, replayed_result.value());
        }
    }
    // The unkeyed replay never executed: exactly three executions happened
    // (seq 0, keyed seq 1 leader, keyed replay served from the table).
    std::lock_guard<std::mutex> lk(f.service.order_mutex);
    CRPC_CHECK_EQ(2u, f.service.order.size());
}

// Regression: a call without a lane token behaves exactly as before, and a
// lane call's handler observes the token that ordered it.
CRPC_TEST(lanes_e2e_no_lane_token_unchanged_handler_sees_context) {
    lane_fixture f;
    f.name = "lane-e2e-plain";
    CRPC_CHECK(f.start());
    CRPC_CHECK(f.start_facade());
    if (!f.srv || !f.cli) {
        return;
    }

    // Plain call: no token, value echoed.
    {
        auto stub = f.cli->service<crpc::u64, crpc::u64>(kService, kProbe);
        auto started = stub.call(41);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            auto result = std::move(started).value().get();
            CRPC_CHECK(result.ok());
            if (result.ok()) {
                CRPC_CHECK_EQ(41u, result.value());
            }
        }
    }
    // Lane call: the handler sees the token.
    {
        auto stub = f.cli->lane(3).service<crpc::u64, crpc::u64>(kService, kProbe);
        auto started = stub.call(42);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            auto result = std::move(started).value().get();
            CRPC_CHECK(result.ok());
            if (result.ok()) {
                CRPC_CHECK_EQ(42u, result.value());
            }
        }
    }
    std::lock_guard<std::mutex> lk(f.service.order_mutex);
    CRPC_CHECK_EQ(2u, f.service.order.size());
    if (f.service.order.size() == 2) {
        CRPC_CHECK_FALSE(f.service.order[0].has_lane);
        CRPC_CHECK(f.service.order[1].has_lane);
        CRPC_CHECK_EQ(3u, f.service.order[1].lane);
        CRPC_CHECK_EQ(0u, f.service.order[1].seq);
    }
}

// Shutdown with parked lane calls: every queued call is answered with an
// explicit error (not a deadline), the blocked head finishes without
// wedging the shard, and the server's in-flight accounting returns to zero.
CRPC_TEST(lanes_e2e_shutdown_fails_queued_calls_and_releases_accounting) {
    lane_fixture f;
    f.name = "lane-e2e-close";
    f.service.gate.store(0, std::memory_order_release);
    CRPC_CHECK(f.start());
    if (!f.srv) {
        return;
    }
    gate_opener opener{&f.service};
    auto c = raw_client(f);
    CRPC_CHECK(c != nullptr);
    if (!c) {
        return;
    }

    crpc::buffer body;
    CRPC_CHECK(encode_u64(9, body));
    // seq 0 runs and parks on the gate; seq 1 and 2 queue behind it.
    auto s0 = c->start_call<crpc::u64>(kService, kBlock, body.readable(), lane_ctx(4, 0));
    CRPC_CHECK(s0.ok());
    if (!s0.ok()) {
        return;
    }
    auto h0 = std::move(s0).value();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.service.started.load(std::memory_order_acquire) >= 1; }, 10000));
    auto s1 = c->start_call<crpc::u64>(kService, kProbe, body.readable(), lane_ctx(4, 1));
    auto s2 = c->start_call<crpc::u64>(kService, kProbe, body.readable(), lane_ctx(4, 2));
    CRPC_CHECK(s1.ok());
    CRPC_CHECK(s2.ok());
    if (!s1.ok() || !s2.ok()) {
        return;
    }
    auto h1 = std::move(s1).value();
    auto h2 = std::move(s2).value();

    // stop() drops the queued calls with explicit `shutting_down` responses
    // while the transport is still alive; the drain and the shard join wait
    // for the parked head, so stop() runs on a helper thread and the gate
    // opens once the queued calls have been failed.
    std::atomic<bool> stopped{false};
    std::thread stopper([&f, &stopped] {
        f.srv->stop();
        stopped.store(true, std::memory_order_release);
    });
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return h1.ready() && h2.ready(); }, 8000));

    const auto r1 = h1.get();
    const auto r2 = h2.get();
    CRPC_CHECK_FALSE(r1.ok());
    CRPC_CHECK_FALSE(r2.ok());
    if (!r1.ok()) {
        CRPC_CHECK_EQ(crpc::rpc_code::shutting_down, r1.code());
    }
    if (!r2.ok()) {
        CRPC_CHECK_EQ(crpc::rpc_code::shutting_down, r2.code());
    }

    // The blocked head cannot finish until the gate opens; open it and let
    // stop() complete (its shard join waits for the parked handler), then the
    // accounting must be fully released.
    f.service.gate.store(1, std::memory_order_release);
    stopper.join();
    CRPC_CHECK_EQ(1, static_cast<int>(stopped.load()));
    const crpc::server::statistics stats = f.srv->stats();
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), stats.in_flight_calls);
}

// Long run across many lanes: every call completes and the server's in-flight
// accounting returns to zero -- no parked work, no accounting leak. The
// scheduler's own entry release is pinned by active_lanes() in the unit suite.
CRPC_TEST(lanes_e2e_many_lanes_leave_no_accounting) {
    lane_fixture f;
    f.name = "lane-e2e-many";
    CRPC_CHECK(f.start());
    CRPC_CHECK(f.start_facade());
    if (!f.srv || !f.cli) {
        return;
    }

    constexpr int kLanes = 40;
    constexpr int kPerLane = 5;
    for (int lane = 0; lane < kLanes; ++lane) {
        auto stub = f.cli->lane(static_cast<crpc::u64>(lane + 1))
                        .service<crpc::u64, crpc::u64>(kService, kProbe);
        for (int i = 0; i < kPerLane; ++i) {
            auto started = stub.call(static_cast<crpc::u64>(i));
            CRPC_CHECK(started.ok());
            if (started.ok()) {
                auto result = std::move(started).value().get();
                CRPC_CHECK(result.ok());
                if (result.ok()) {
                    CRPC_CHECK_EQ(static_cast<crpc::u64>(i), result.value());
                }
            }
        }
    }
    std::lock_guard<std::mutex> lk(f.service.order_mutex);
    CRPC_CHECK_EQ(kLanes * kPerLane, static_cast<int>(f.service.order.size()));
    CRPC_CHECK_EQ(1, f.service.max_in_flight.load());
    f.cli->close();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.srv->stats().in_flight_calls == 0; }, 5000));
}

// The production path for `admission::rejected`: a lane whose buffered frame
// bytes would exceed the 16 KiB per-lane budget refuses the new call with
// `rpc_code::overload` (a real server response, not a drop), nothing already
// buffered is evicted, and the lane keeps running for the calls that fit.
// Driven through the real inproc server. The frames arrive out of order --
// seq 1 first (buffered behind the gap for seq 0), seq 2 next (two ~9 KiB
// buffered frames would exceed the default 16 KiB cap: refused), seq 0 last
// (runs immediately and drains seq 1 in order). No handler ever parks, so
// the shard thread stays live for every admission.
CRPC_TEST(lanes_e2e_lane_byte_cap_answers_overload) {
    lane_fixture f;
    f.name = "lane-e2e-bytecap";
    CRPC_CHECK(f.start());
    if (!f.srv) {
        return;
    }
    auto c = raw_client(f);
    CRPC_CHECK(c != nullptr);
    if (!c) {
        return;
    }

    const std::string big(9 * 1024, 'x');
    const crpc::span<const std::byte> big_body(
        reinterpret_cast<const std::byte*>(big.data()), big.size());
    // seq 1 arrives first: buffered behind the gap (seq 0 has not arrived).
    auto first = c->start_call<std::vector<std::byte>>(kService, kEcho, big_body,
                                                       lane_ctx(21, 1));
    CRPC_CHECK(first.ok());
    // seq 2 arrives next: the lane would buffer ~18 KiB > the 16 KiB default,
    // so it is refused with overload. Nothing already queued is evicted.
    auto refused = c->start_call<std::vector<std::byte>>(kService, kEcho, big_body,
                                                         lane_ctx(21, 2));
    CRPC_CHECK(refused.ok());
    if (!first.ok() || !refused.ok()) {
        return;
    }
    auto first_handle = std::move(first).value();
    auto refused_handle = std::move(refused).value();

    const auto refused_result = refused_handle.get();
    CRPC_CHECK_FALSE(refused_result.ok());
    if (!refused_result.ok()) {
        CRPC_CHECK_EQ(crpc::rpc_code::overload, refused_result.code());
    }

    // seq 0 arrives last: it is next, the lane is idle and everything
    // buffered sits above it, so it runs now and drains seq 1 in order.
    crpc::buffer head;
    CRPC_CHECK(encode_u64(0, head));
    auto running = c->start_call<crpc::u64>(kService, kProbe, head.readable(),
                                            lane_ctx(21, 0));
    CRPC_CHECK(running.ok());
    if (!running.ok()) {
        return;
    }
    auto running_handle = std::move(running).value();
    const auto first_result = first_handle.get();
    CRPC_CHECK(first_result.ok());
    if (first_result.ok()) {
        CRPC_CHECK_EQ(big.size(), first_result.value().size());
    }
    const auto running_result = running_handle.get();
    CRPC_CHECK(running_result.ok());
    if (running_result.ok()) {
        CRPC_CHECK_EQ(static_cast<crpc::u64>(0), running_result.value());
    }

    // The refusal consumed no position: seq 2 is admitted fresh.
    auto again = c->start_call<std::vector<std::byte>>(kService, kEcho, big_body,
                                                       lane_ctx(21, 2));
    CRPC_CHECK(again.ok());
    if (again.ok()) {
        auto again_handle = std::move(again).value();
        const auto again_result = again_handle.get();
        CRPC_CHECK(again_result.ok());
        if (again_result.ok()) {
            CRPC_CHECK_EQ(big.size(), again_result.value().size());
        }
    }
    // The call handles above must outlive no pool: the client (and its slot
    // pool) is destroyed by reverse declaration order after every handle is
    // gone, so no explicit reset here.
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.srv->stats().in_flight_calls == 0; }, 8000));
    std::lock_guard<std::mutex> lk(f.service.order_mutex);
    // The refusal must not have reordered or dropped the lane's work: the
    // head ran first, then seq 1, then the retried seq 2.
    CRPC_CHECK_EQ(3, static_cast<int>(f.service.order.size()));
    if (f.service.order.size() == 3) {
        CRPC_CHECK_EQ(21u, f.service.order[0].lane);
        CRPC_CHECK_EQ(0u, f.service.order[0].seq);
        CRPC_CHECK_EQ(1u, f.service.order[1].seq);
        CRPC_CHECK_EQ(2u, f.service.order[2].seq);
    }
}

// A malformed `CRPC_LANE_MAX_BUFFERED_BYTES` falls back to the default
// instead of parsing as zero (which would refuse every buffered frame): the
// server behaves exactly as with the variable unset.
CRPC_TEST(lanes_e2e_lane_byte_cap_env_malformed_falls_back_to_default) {
    env_guard bad("not-a-number", "CRPC_LANE_MAX_BUFFERED_BYTES");
    lane_fixture f;
    f.name = "lane-e2e-bytecap-env";
    CRPC_CHECK(f.start());
    if (!f.srv) {
        return;
    }
    auto c = raw_client(f);
    CRPC_CHECK(c != nullptr);
    if (!c) {
        return;
    }

    const std::string big(9 * 1024, 'y');
    const crpc::span<const std::byte> big_body(
        reinterpret_cast<const std::byte*>(big.data()), big.size());
    // The same out-of-order arrival: seq 1 buffered, seq 2 refused (the
    // default 16 KiB budget is in force), seq 0 runs and drains.
    auto first = c->start_call<std::vector<std::byte>>(kService, kEcho, big_body,
                                                       lane_ctx(22, 1));
    CRPC_CHECK(first.ok());
    auto refused = c->start_call<std::vector<std::byte>>(kService, kEcho, big_body,
                                                         lane_ctx(22, 2));
    CRPC_CHECK(refused.ok());
    if (!first.ok() || !refused.ok()) {
        return;
    }
    auto first_handle = std::move(first).value();
    auto refused_handle = std::move(refused).value();

    const auto refused_result = refused_handle.get();
    CRPC_CHECK_FALSE(refused_result.ok());
    if (!refused_result.ok()) {
        CRPC_CHECK_EQ(crpc::rpc_code::overload, refused_result.code());
    }

    crpc::buffer head;
    CRPC_CHECK(encode_u64(0, head));
    auto running = c->start_call<crpc::u64>(kService, kProbe, head.readable(),
                                            lane_ctx(22, 0));
    CRPC_CHECK(running.ok());
    if (!running.ok()) {
        return;
    }
    auto running_handle = std::move(running).value();
    const auto first_result = first_handle.get();
    CRPC_CHECK(first_result.ok());
    if (first_result.ok()) {
        CRPC_CHECK_EQ(big.size(), first_result.value().size());
    }
    const auto running_result = running_handle.get();
    CRPC_CHECK(running_result.ok());
    // The call handles above must outlive no pool: the client (and its slot
    // pool) is destroyed by reverse declaration order after every handle is
    // gone, so no explicit reset here.
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return f.srv->stats().in_flight_calls == 0; }, 8000));
}

CRPC_TEST_MAIN()
