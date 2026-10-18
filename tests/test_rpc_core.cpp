// CRPC -- RPC core tests: state machine, slot pool, client, dispatcher.
//
// The race tests exercise the single-terminal-transition guarantee directly on
// a `call_slot`; the client tests wire a client to a dispatcher through the
// in-process backend so a whole request/response round trip runs without any
// background thread.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>

#include "test_framework.hpp"

// Stream adapters so CRPC_CHECK_EQ can print the enum-class values it compares.
// They live in the enums' own namespaces so argument-dependent lookup finds
// them inside the harness's check_equal template.
namespace crpc {
inline std::ostream& operator<<(std::ostream& out, rpc_code code) {
    return out << to_string(code);
}
namespace rpc {
inline std::ostream& operator<<(std::ostream& out, call_state state) {
    return out << to_string(state);
}
} // namespace rpc
} // namespace crpc

namespace {

using namespace crpc;
using namespace crpc::rpc;

span<const std::byte> bytes_of(const std::string& text) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

std::string text_of(span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// --- Handler fixtures --------------------------------------------------------

status echo_body(void*, const call_envelope&, span<const std::byte> body, buffer& out,
                 call_context&) {
    if (!body.empty()) {
        (void)out.append(body);
    }
    return status{};
}

status throwing_body(void*, const call_envelope&, span<const std::byte>, buffer&, call_context&) {
    throw std::runtime_error("boom");
}

std::atomic<bool> g_flag_ran{false};

status flag_body(void*, const call_envelope&, span<const std::byte>, buffer&, call_context&) {
    g_flag_ran.store(true, std::memory_order_relaxed);
    return status{};
}

// --- Transport fixtures ------------------------------------------------------

// A completion that does nothing; the in-process sink completes synchronously.
struct noop_completion final : io::completion {
    void complete(io::io_result) noexcept override {}
};

class dummy_source final : public io::source {
public:
    std::size_t on_data(span<const std::byte> data) noexcept override { return data.size(); }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

// Server side: decodes frames, dispatches REQUEST/ONE_WAY, replies, records the
// frame types it saw.
class test_server final : public io::source {
public:
    test_server(dispatcher& d, io::sink& sink) : dispatcher_(d), sink_(sink) {}

    std::size_t on_data(span<const std::byte> data) noexcept override {
        decoder_.feed(data);
        frame value;
        frame_decoder::result result = frame_decoder::result::need_more;
        while ((result = decoder_.next(value)) == frame_decoder::result::frame_ready) {
            seen_.push_back(value.header.type);
            if (value.header.type == frame_type::request ||
                value.header.type == frame_type::one_way) {
                buffer payload;
                bool has_response = false;
                (void)dispatcher_.dispatch(value, payload, has_response,
                                           std::chrono::steady_clock::now());
                if (has_response) {
                    send(value.header.stream_id, payload.readable());
                }
            }
        }
        if (result == frame_decoder::result::error) {
            failed_ = true;
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    [[nodiscard]] bool saw(frame_type type) const noexcept {
        for (const frame_type entry : seen_) {
            if (entry == type) {
                return true;
            }
        }
        return false;
    }

    bool failed_ = false;
    std::vector<frame_type> seen_;

private:
    void send(u32 stream_id, span<const std::byte> payload) {
        frame response;
        response.header.type = frame_type::response;
        response.header.stream_id = stream_id;
        response.payload = payload;
        const std::size_t size = encoded_frame_size(response);
        buffer encoded;
        if (!encoded.reserve(size)) {
            return;
        }
        std::size_t written = 0;
        if (!encode_frame(span<std::byte>(encoded.data(), size), response, written).ok()) {
            return;
        }
        noop_completion done;
        sink_.push(span<const std::byte>(encoded.data(), written), &done);
    }

    frame_decoder decoder_;
    dispatcher& dispatcher_;
    io::sink& sink_;
};

// Server that consumes but never replies, recording the frames it saw. Used to
// keep a call in flight and to observe RST_STREAM.
class silent_server final : public io::source {
public:
    std::size_t on_data(span<const std::byte> data) noexcept override {
        decoder_.feed(data);
        frame value;
        while (decoder_.next(value) == frame_decoder::result::frame_ready) {
            seen_.push_back(value.header.type);
        }
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    [[nodiscard]] bool saw(frame_type type) const noexcept {
        for (const frame_type entry : seen_) {
            if (entry == type) {
                return true;
            }
        }
        return false;
    }

    std::vector<frame_type> seen_;

private:
    frame_decoder decoder_;
};

// A sink that never completes a push: models a suspended backend.
class stalling_sink final : public io::sink {
public:
    void push(span<const std::byte>, io::completion* c) noexcept override {
        last_ = c;
        ++pushes_;
    }
    io::completion* last_ = nullptr;
    int pushes_ = 0;
};

class fake_connection final : public connection {
public:
    io::source& source() noexcept override { return source_; }
    io::sink& sink() noexcept override { return sink_; }
    void bind_source(io::source* source) noexcept override { bound_ = source; }
    void resume_read() noexcept override {}
    void close() noexcept override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] endpoint remote() const override { return endpoint::inproc("fake"); }

private:
    dummy_source source_;
    stalling_sink sink_;
    io::source* bound_ = nullptr;
    bool open_ = true;
};

// A monotonic clock a test can advance.
struct fake_clock {
    std::chrono::steady_clock::time_point now{};
};

std::chrono::steady_clock::time_point fake_now(void* ctx) noexcept {
    return static_cast<fake_clock*>(ctx)->now;
}

// Builds an encoded REQUEST frame with a call envelope.
struct built_request {
    buffer payload;
    frame value;
};

built_request make_call(u64 service_id, u64 method_id, span<const std::byte> body,
                        bool has_deadline, u64 deadline_ns) {
    call_envelope envelope;
    envelope.service_id = service_id;
    envelope.method_id = method_id;
    envelope.codec = codec_id::binary;
    envelope.body = body;
    if (has_deadline) {
        envelope.call_flags = call_flag_has_extensions;
        envelope.extensions.flags = trace_extension::has_deadline;
        envelope.extensions.deadline_ns = deadline_ns;
    }
    built_request request;
    const std::size_t size = encoded_call_envelope_size(envelope);
    if (!request.payload.reserve(size)) {
        return request;
    }
    std::size_t written = 0;
    (void)encode_call_envelope(span<std::byte>(request.payload.data(), size), envelope, written);
    request.value.header.type = frame_type::request;
    request.value.header.stream_id = 1;
    request.value.payload = span<const std::byte>(request.payload.data(), written);
    return request;
}

// Runs a dispatch and returns the response's rpc_code (ok when there is no
// response, i.e. a ONE_WAY).
rpc_code dispatch_code(dispatcher& d, const frame& request,
                       std::chrono::steady_clock::time_point now) {
    buffer response;
    bool has_response = false;
    (void)d.dispatch(request, response, has_response, now);
    if (!has_response) {
        return rpc_code::ok;
    }
    rpc_code code = rpc_code::ok;
    rpc_error error;
    span<const std::byte> body;
    if (!decode_response(response.readable(), code, error, body).ok()) {
        return rpc_code::protocol_error;
    }
    return code;
}

void register_echo(service_registry& reg, u64 service_id = 1, u64 method_id = 1) {
    (void)reg.register_service(service_id, "test.svc");
    (void)reg.register_method(service_id, method_id, "echo", &echo_body, nullptr);
    (void)reg.freeze();
}

// --- State machine -----------------------------------------------------------

CRPC_TEST(state_machine_states) {
    CRPC_CHECK_EQ(std::string("idle"), std::string(to_string(call_state::idle)));
    CRPC_CHECK_EQ(std::string("sent"), std::string(to_string(call_state::sent)));
    CRPC_CHECK_EQ(std::string("timed_out"), std::string(to_string(call_state::timed_out)));
    CRPC_CHECK_FALSE(is_terminal(call_state::idle));
    CRPC_CHECK_FALSE(is_terminal(call_state::queued));
    CRPC_CHECK_FALSE(is_terminal(call_state::sent));
    CRPC_CHECK(is_terminal(call_state::completed));
    CRPC_CHECK(is_terminal(call_state::timed_out));
    CRPC_CHECK(is_terminal(call_state::canceled));
    CRPC_CHECK(is_terminal(call_state::failed));
    CRPC_CHECK_EQ(rpc_code::ok, default_terminal_code(call_state::completed));
    CRPC_CHECK_EQ(rpc_code::deadline_exceeded, default_terminal_code(call_state::timed_out));
    CRPC_CHECK_EQ(rpc_code::canceled, default_terminal_code(call_state::canceled));
    CRPC_CHECK_EQ(rpc_code::internal, default_terminal_code(call_state::failed));

    call_slot slot;
    CRPC_CHECK_EQ(call_state::idle, load_state(slot));
    set_state(slot, call_state::queued);
    CRPC_CHECK_EQ(call_state::queued, load_state(slot));
    set_state(slot, call_state::sent);
    CRPC_CHECK_EQ(call_state::sent, load_state(slot));
    CRPC_CHECK(try_claim_terminal(slot, call_state::completed));
    CRPC_CHECK_FALSE(try_claim_terminal(slot, call_state::canceled));
    CRPC_CHECK_FALSE(try_claim_terminal(slot, call_state::timed_out));
}

// Runs one two-way terminal race 1000 times and checks exactly one winner.
void run_pair_race(call_state first, call_state second) {
    for (int iteration = 0; iteration < 1000; ++iteration) {
        call_slot slot;
        std::atomic<int> winners{0};
        const auto attempt = [&slot, &winners](call_state terminal) {
            const rpc_code code = default_terminal_code(terminal);
            if (finish_slot(slot, terminal, code, rpc_error(code), span<const std::byte>())) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread a(attempt, first);
        std::thread b(attempt, second);
        a.join();
        b.join();
        CRPC_CHECK_EQ(1, winners.load());
        CRPC_CHECK(is_terminal(load_state(slot)));
        CRPC_CHECK(result_ready(slot));
    }
}

CRPC_TEST(state_machine_response_vs_timeout) {
    run_pair_race(call_state::completed, call_state::timed_out);
}
CRPC_TEST(state_machine_cancel_vs_response) {
    run_pair_race(call_state::canceled, call_state::completed);
}
CRPC_TEST(state_machine_loss_vs_response) {
    run_pair_race(call_state::failed, call_state::completed);
}
CRPC_TEST(state_machine_timeout_vs_cancel) {
    run_pair_race(call_state::timed_out, call_state::canceled);
}
CRPC_TEST(state_machine_loss_vs_timeout) {
    run_pair_race(call_state::failed, call_state::timed_out);
}
CRPC_TEST(state_machine_loss_vs_cancel) {
    run_pair_race(call_state::failed, call_state::canceled);
}

CRPC_TEST(state_machine_four_way_race) {
    const call_state states[4] = {call_state::completed, call_state::timed_out,
                                  call_state::canceled, call_state::failed};
    for (int iteration = 0; iteration < 300; ++iteration) {
        call_slot slot;
        std::atomic<int> winners{0};
        const auto attempt = [&slot, &winners](call_state terminal) {
            const rpc_code code = default_terminal_code(terminal);
            if (finish_slot(slot, terminal, code, rpc_error(code), span<const std::byte>())) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread threads[4];
        for (int i = 0; i < 4; ++i) {
            threads[i] = std::thread(attempt, states[i]);
        }
        for (auto& thread : threads) {
            thread.join();
        }
        CRPC_CHECK_EQ(1, winners.load());
        CRPC_CHECK(is_terminal(load_state(slot)));
    }
}

// --- Slot pool ---------------------------------------------------------------

CRPC_TEST(slot_pool_exhaustion_and_recycle) {
    call_slot_pool pool(4);
    CRPC_CHECK_EQ(4u, pool.capacity());
    CRPC_CHECK_EQ(4u, pool.available());

    call_slot* slots[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < 4; ++i) {
        slots[i] = pool.try_acquire();
        CRPC_CHECK(slots[i] != nullptr);
    }
    CRPC_CHECK_EQ(4u, pool.in_use());
    CRPC_CHECK_EQ(0u, pool.available());
    CRPC_CHECK(pool.try_acquire() == nullptr);
    CRPC_CHECK_EQ(4u, pool.in_use());

    const u32 generation = slots[0]->generation;
    pool.release(slots[0]);
    slots[0] = nullptr;
    CRPC_CHECK_EQ(3u, pool.in_use());
    CRPC_CHECK_EQ(1u, pool.available());

    slots[4] = pool.try_acquire();
    CRPC_CHECK(slots[4] != nullptr);
    CRPC_CHECK_EQ(generation + 1, slots[4]->generation);
    CRPC_CHECK_EQ(4u, pool.in_use());

    for (int i = 1; i < 5; ++i) {
        pool.release(slots[i]);
    }
    CRPC_CHECK_EQ(0u, pool.in_use());
    CRPC_CHECK_EQ(4u, pool.available());
}

CRPC_TEST(slot_pool_zero_capacity) {
    call_slot_pool pool(0);
    CRPC_CHECK_EQ(0u, pool.capacity());
    CRPC_CHECK(pool.try_acquire() == nullptr);
}

// --- Stream id allocation ----------------------------------------------------

struct in_flight_set {
    u32 ids[4] = {0, 0, 0, 0};
    std::size_t count = 0;
};

bool in_flight_probe_test(void* ctx, u32 id) noexcept {
    const auto* set = static_cast<const in_flight_set*>(ctx);
    for (std::size_t i = 0; i < set->count; ++i) {
        if (set->ids[i] == id) {
            return true;
        }
    }
    return false;
}

CRPC_TEST(stream_id_wrap_skips_in_flight_and_zero) {
    u32 counter = max_stream_id - 1; // 0x7FFFFFFE
    in_flight_set set;
    set.ids[0] = max_stream_id;
    set.ids[1] = 1;
    set.ids[2] = 3;
    set.count = 3;
    const u32 first = allocate_stream_id(counter, &in_flight_probe_test, &set);
    CRPC_CHECK_EQ(2u, first);
    const u32 second = allocate_stream_id(counter, &in_flight_probe_test, &set);
    CRPC_CHECK_EQ(4u, second);
    CRPC_CHECK(first != 0);
    CRPC_CHECK(second != 0);
    CRPC_CHECK(counter <= max_stream_id);
}

// --- Client round trips ------------------------------------------------------

CRPC_TEST(client_round_trip) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    service_registry reg;
    register_echo(reg);
    dispatcher d(reg);
    test_server server(d, server_conn->sink());
    server_conn->bind_source(&server);

    client_options options;
    options.slot_capacity = 16;
    client c(std::move(client_conn), options);

    std::string body = "hello";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body));
    CRPC_CHECK(started.ok());
    auto handle = std::move(started).value();
    CRPC_CHECK(handle.valid());

    const auto result_value = handle.get();
    CRPC_CHECK(result_value.ok());
    CRPC_CHECK_EQ(std::string("hello"), result_value.value());
    CRPC_CHECK_EQ(0u, c.in_flight());
}

CRPC_TEST(client_thousand_concurrent_calls) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    service_registry reg;
    register_echo(reg);
    dispatcher d(reg);
    test_server server(d, server_conn->sink());
    server_conn->bind_source(&server);

    client_options options;
    options.slot_capacity = 2048;
    client c(std::move(client_conn), options);

    const std::string body = "payload";
    std::vector<call<std::string>> handles;
    handles.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        auto started = c.start_call<std::string>(1, 1, bytes_of(body));
        CRPC_CHECK(started.ok());
        handles.push_back(std::move(started).value());
    }
    CRPC_CHECK_EQ(1000u, c.slots_in_use());

    for (auto& handle : handles) {
        const auto result_value = handle.get();
        CRPC_CHECK(result_value.ok());
        CRPC_CHECK_EQ(std::string("payload"), result_value.value());
    }
    handles.clear();
    CRPC_CHECK_EQ(0u, c.slots_in_use());
    CRPC_CHECK_EQ(0u, c.in_flight());
}

CRPC_TEST(client_timeout_is_terminal_deadline_exceeded) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    silent_server server;
    server_conn->bind_source(&server);

    fake_clock clock;
    clock.now = std::chrono::steady_clock::now();

    client_options options;
    options.slot_capacity = 8;
    options.clock = &fake_now;
    options.clock_ctx = &clock;
    client c(std::move(client_conn), options);

    call_context ctx;
    ctx.has_deadline = true;
    ctx.deadline = clock.now + std::chrono::seconds(1);

    std::string body = "x";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body), ctx);
    CRPC_CHECK(started.ok());
    auto handle = std::move(started).value();
    CRPC_CHECK_FALSE(handle.ready());

    clock.now += std::chrono::seconds(2);
    c.poll();
    CRPC_CHECK(handle.ready());
    CRPC_CHECK_EQ(call_state::timed_out, handle.state());

    const auto result_value = handle.get();
    CRPC_CHECK_EQ(rpc_code::deadline_exceeded, result_value.code());
    // Non-idempotent and already written: the side effect is unknown.
    CRPC_CHECK(handle.slot()->error.metadata.find("unknown_side_effect") != nullptr);
    CRPC_CHECK_EQ(0u, c.in_flight());
}

CRPC_TEST(client_idempotent_timeout_has_no_unknown_side_effect) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    silent_server server;
    server_conn->bind_source(&server);

    fake_clock clock;
    clock.now = std::chrono::steady_clock::now();

    client_options options;
    options.slot_capacity = 8;
    options.clock = &fake_now;
    options.clock_ctx = &clock;
    client c(std::move(client_conn), options);

    call_context ctx;
    ctx.has_deadline = true;
    ctx.deadline = clock.now + std::chrono::seconds(1);
    ctx.idempotent = true;

    std::string body = "x";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body), ctx);
    CRPC_CHECK(started.ok());
    auto handle = std::move(started).value();

    clock.now += std::chrono::seconds(2);
    c.poll();
    const auto result_value = handle.get();
    CRPC_CHECK_EQ(rpc_code::deadline_exceeded, result_value.code());
    CRPC_CHECK(handle.slot()->error.metadata.find("unknown_side_effect") == nullptr);
}

CRPC_TEST(client_cancel_sends_rst_stream) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    // A server that answers would complete the call; use one that records but
    // never replies so the call stays in flight until cancel.
    silent_server server;
    server_conn->bind_source(&server);

    client_options options;
    options.slot_capacity = 8;
    client c(std::move(client_conn), options);

    std::string body = "x";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body));
    CRPC_CHECK(started.ok());
    auto handle = std::move(started).value();
    CRPC_CHECK_FALSE(handle.ready());

    handle.cancel();
    CRPC_CHECK(handle.ready());
    CRPC_CHECK_EQ(call_state::canceled, handle.state());
    const auto result_value = handle.get();
    CRPC_CHECK_EQ(rpc_code::canceled, result_value.code());
    CRPC_CHECK(server.saw(frame_type::request));
    CRPC_CHECK(server.saw(frame_type::rst_stream));
    CRPC_CHECK_EQ(0u, c.in_flight());
}

CRPC_TEST(client_peer_close_fails_in_flight) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    silent_server server;
    server_conn->bind_source(&server);

    client_options options;
    options.slot_capacity = 8;
    client c(std::move(client_conn), options);

    std::string body = "x";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body));
    CRPC_CHECK(started.ok());
    auto handle = std::move(started).value();
    CRPC_CHECK_FALSE(handle.ready());

    server_conn->close();
    CRPC_CHECK(handle.ready());
    CRPC_CHECK_EQ(call_state::failed, handle.state());
    const rpc_code code = handle.get().code();
    CRPC_CHECK(code == rpc_code::eof || code == rpc_code::connection_reset);
    CRPC_CHECK_EQ(0u, c.in_flight());
}

CRPC_TEST(client_unreliable_one_way) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    service_registry reg;
    register_echo(reg);
    dispatcher d(reg);
    test_server server(d, server_conn->sink());
    server_conn->bind_source(&server);

    client_options options;
    options.slot_capacity = 8;
    client c(std::move(client_conn), options);

    call_context ctx;
    ctx.mode = delivery_mode::unreliable;

    std::string body = "fire";
    auto started = c.start_call<std::string>(1, 1, bytes_of(body), ctx);
    CRPC_CHECK(started.ok());
    auto handle = std::move(started).value();
    CRPC_CHECK(handle.ready());
    CRPC_CHECK_EQ(call_state::completed, handle.state());
    CRPC_CHECK(handle.get().ok());
    CRPC_CHECK(server.saw(frame_type::one_way));
}

// --- Dispatcher --------------------------------------------------------------

CRPC_TEST(dispatcher_unknown_service_and_method) {
    service_registry reg;
    register_echo(reg, 1, 1);
    dispatcher d(reg);
    const auto now = std::chrono::steady_clock::now();

    std::string body = "x";
    built_request unknown_service = make_call(2, 1, bytes_of(body), false, 0);
    CRPC_CHECK_EQ(rpc_code::not_found, dispatch_code(d, unknown_service.value, now));

    built_request unknown_method = make_call(1, 9, bytes_of(body), false, 0);
    CRPC_CHECK_EQ(rpc_code::no_such_method, dispatch_code(d, unknown_method.value, now));
}

CRPC_TEST(dispatcher_duplicate_registration) {
    service_registry reg;
    CRPC_CHECK(reg.register_service(1, "svc").ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, reg.register_service(1, "svc2").code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, reg.register_service(0, "bad").code());
    CRPC_CHECK(reg.register_method(1, 1, "echo", &echo_body, nullptr).ok());
    CRPC_CHECK_EQ(rpc_code::failed_precondition,
                  reg.register_method(1, 1, "echo2", &echo_body, nullptr).code());
    CRPC_CHECK_EQ(rpc_code::not_found,
                  reg.register_method(2, 1, "echo", &echo_body, nullptr).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  reg.register_method(1, 0, "bad", &echo_body, nullptr).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  reg.register_method(1, 2, "null", nullptr, nullptr).code());
    CRPC_CHECK_EQ(1u, reg.service_count());
    CRPC_CHECK_EQ(1u, reg.method_count());

    CRPC_CHECK(reg.freeze().ok());
    CRPC_CHECK(reg.frozen());
    CRPC_CHECK_EQ(rpc_code::failed_precondition, reg.register_service(3, "late").code());
    CRPC_CHECK_EQ(rpc_code::failed_precondition,
                  reg.register_method(1, 5, "late", &echo_body, nullptr).code());
    CRPC_CHECK(reg.find_service(1) != nullptr);
    CRPC_CHECK(reg.find_method(1, 1) != nullptr);
    CRPC_CHECK(reg.find_method(1, 5) == nullptr);
    CRPC_CHECK(reg.find_service(4) == nullptr);
}

CRPC_TEST(dispatcher_decode_error) {
    service_registry reg;
    register_echo(reg);
    dispatcher d(reg);
    const auto now = std::chrono::steady_clock::now();

    std::byte junk[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
    frame request;
    request.header.type = frame_type::request;
    request.header.stream_id = 1;
    request.payload = span<const std::byte>(junk, sizeof(junk));
    CRPC_CHECK_EQ(rpc_code::decode_error, dispatch_code(d, request, now));
}

CRPC_TEST(dispatcher_handler_exception_maps_to_internal) {
    service_registry reg;
    (void)reg.register_service(1, "svc");
    (void)reg.register_method(1, 1, "throw", &throwing_body, nullptr);
    (void)reg.freeze();
    dispatcher d(reg);
    const auto now = std::chrono::steady_clock::now();

    std::string body = "x";
    built_request request = make_call(1, 1, bytes_of(body), false, 0);
    CRPC_CHECK_EQ(rpc_code::internal, dispatch_code(d, request.value, now));
}

CRPC_TEST(dispatcher_expired_deadline_skips_handler) {
    g_flag_ran.store(false, std::memory_order_relaxed);
    service_registry reg;
    (void)reg.register_service(1, "svc");
    (void)reg.register_method(1, 1, "flag", &flag_body, nullptr);
    (void)reg.freeze();
    dispatcher d(reg);

    const auto now = std::chrono::steady_clock::now();
    const auto past =
        static_cast<u64>(now.time_since_epoch().count()) - 1'000'000u; // 1 ms in the past
    std::string body = "x";
    built_request request = make_call(1, 1, bytes_of(body), true, past);
    CRPC_CHECK_EQ(rpc_code::deadline_exceeded, dispatch_code(d, request.value, now));
    CRPC_CHECK_FALSE(g_flag_ran.load(std::memory_order_relaxed));
}

CRPC_TEST(dispatcher_typed_trampoline_round_trip) {
    struct echo_controller {
        std::string echo(const std::string& in) { return in; }
    };

    service_registry reg;
    echo_controller controller;
    (void)reg.register_service(1, "svc");
    const status registered = register_typed_method<echo_controller, &echo_controller::echo>(
        reg, 1, 1, "echo", controller);
    CRPC_CHECK(registered.ok());
    (void)reg.freeze();
    dispatcher d(reg);

    const auto now = std::chrono::steady_clock::now();
    std::string body = "typed";
    built_request request = make_call(1, 1, bytes_of(body), false, 0);

    buffer response;
    bool has_response = false;
    (void)d.dispatch(request.value, response, has_response, now);
    CRPC_CHECK(has_response);
    rpc_code code = rpc_code::ok;
    rpc_error error;
    span<const std::byte> decoded_body;
    CRPC_CHECK(decode_response(response.readable(), code, error, decoded_body).ok());
    CRPC_CHECK_EQ(rpc_code::ok, code);
    CRPC_CHECK_EQ(std::string("typed"), text_of(decoded_body));
}

// --- No-stall policy ---------------------------------------------------------

CRPC_TEST(no_stall_writer_queue_rejects_bounded) {
    client_options options;
    options.slot_capacity = 64;
    options.max_pending_write_bytes = 256;
    client c(std::make_unique<fake_connection>(), options);

    const std::string body(32, 'x');
    std::vector<call<std::string>> handles;
    int rejected = 0;
    for (int i = 0; i < 64; ++i) {
        auto started = c.start_call<std::string>(1, 1, bytes_of(body));
        if (!started.ok()) {
            CRPC_CHECK_EQ(rpc_code::resource_exhausted, started.error().code);
            ++rejected;
            break;
        }
        handles.push_back(std::move(started).value());
    }
    CRPC_CHECK(rejected >= 1);
    handles.clear();
    CRPC_CHECK_EQ(0u, c.slots_in_use());
}

CRPC_TEST(client_slots_released_and_no_leak) {
    auto pair = inproc_pair::make();
    auto client_conn = std::move(pair.first);
    auto server_conn = std::move(pair.second);

    service_registry reg;
    register_echo(reg);
    dispatcher d(reg);
    test_server server(d, server_conn->sink());
    server_conn->bind_source(&server);

    client_options options;
    options.slot_capacity = 32;
    client c(std::move(client_conn), options);

    std::string body = "y";
    {
        std::vector<call<std::string>> handles;
        for (int i = 0; i < 32; ++i) {
            auto started = c.start_call<std::string>(1, 1, bytes_of(body));
            CRPC_CHECK(started.ok());
            handles.push_back(std::move(started).value());
        }
        // Pool is full.
        auto overflow = c.start_call<std::string>(1, 1, bytes_of(body));
        CRPC_CHECK_FALSE(overflow.ok());
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, overflow.error().code);
        for (auto& handle : handles) {
            CRPC_CHECK(handle.get().ok());
        }
    }
    CRPC_CHECK_EQ(0u, c.slots_in_use());
}

} // namespace

CRPC_TEST_MAIN()
