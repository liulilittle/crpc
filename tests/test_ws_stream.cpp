// CRPC -- tests for the WebSocket session state machine (crpc/ws/stream.hpp).
//
// The channel is scripted: each test preloads the peer's bytes and inspects
// every frame the session writes, so the control-frame semantics (ping -> pong,
// the two-way close handshake) and the error reactions (close 1002/1007/1009)
// are verified without a socket. The end-to-end path over a real transport is
// exercised by test_ws_e2e.cpp.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/endian.hpp>
#include <crpc/ws/stream.hpp>

#include "test_framework.hpp"
#include <ostream>
#include <type_traits>

// check_equal() streams both sides of a failed comparison; this library's
// enum-classes have no operator<<, so the integer form is provided for ADL.
namespace crpc {
template <typename E,
          typename std::enable_if<std::is_enum<E>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& os, E value) {
    return os << static_cast<long long>(value);
}
} // namespace crpc

using namespace crpc;
using namespace crpc::ws;

namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

// A channel that hands back a scripted byte stream and records every write.
class fake_channel final : public db::channel {
public:
    std::string from_peer; // bytes read_some returns, in order
    std::string to_peer;   // bytes written by the session
    bool closed = false;

    explicit fake_channel(std::string scripted = std::string()) noexcept
        : from_peer(std::move(scripted)) {}

    result<std::vector<std::byte>> read_exact(std::size_t count) override {
        std::vector<std::byte> out;
        out.reserve(count);
        while (out.size() < count) {
            if (from_peer.empty()) {
                return rpc_error{rpc_code::eof, "scripted channel exhausted"};
            }
            const std::size_t take = std::min(count - out.size(), from_peer.size());
            const char* base = from_peer.data();
            out.insert(out.end(), reinterpret_cast<const std::byte*>(base),
                       reinterpret_cast<const std::byte*>(base) + take);
            from_peer.erase(0, take);
        }
        return out;
    }

    result<std::size_t> read_some(span<std::byte> buffer) override {
        if (buffer.empty()) {
            return std::size_t{0};
        }
        if (from_peer.empty()) {
            return rpc_error{rpc_code::eof, "scripted channel exhausted"};
        }
        const std::size_t take = std::min(buffer.size(), from_peer.size());
        std::memcpy(buffer.data(), reinterpret_cast<const std::byte*>(from_peer.data()),
                    take);
        from_peer.erase(0, take);
        return take;
    }

    status write(span<const std::byte> data) override {
        to_peer.append(reinterpret_cast<const char*>(data.data()), data.size());
        return status{};
    }

    void close() noexcept override { closed = true; }
};

// Builds a masked frame (the shape a real client sends) for server-side tests.
std::string masked_frame(opcode op, std::string_view payload, bool fin = true) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<unsigned>(op) | (fin ? 0x80u : 0u)));
    const std::size_t n = payload.size();
    if (n <= 125) {
        out.push_back(static_cast<char>(0x80u | n));
    } else if (n <= 0xffff) {
        out.push_back(static_cast<char>(0x80u | 0x7eu));
        const std::uint16_t len = static_cast<std::uint16_t>(n);
        char raw[2];
        crpc::endian::store_be16(raw, len);
        out.append(raw, 2);
    } else {
        out.push_back(static_cast<char>(0x80u | 0x7fu));
        char raw[8];
        crpc::endian::store_be64(raw, static_cast<std::uint64_t>(n));
        out.append(raw, 8);
    }
    const char mask[4] = {'\x5a', '\xa5', '\x0f', '\xf0'};
    out.append(mask, 4);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    }
    return out;
}

// Builds an unmasked frame (the shape a real server sends) for client-side
// tests.
std::string unmasked_frame(opcode op, std::string_view payload, bool fin = true) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<unsigned>(op) | (fin ? 0x80u : 0u)));
    const std::size_t n = payload.size();
    if (n <= 125) {
        out.push_back(static_cast<char>(n));
    } else if (n <= 0xffff) {
        out.push_back(static_cast<char>(0x7eu));
        const std::uint16_t len = static_cast<std::uint16_t>(n);
        char raw[2];
        crpc::endian::store_be16(raw, len);
        out.append(raw, 2);
    } else {
        out.push_back(static_cast<char>(0x7fu));
        char raw[8];
        crpc::endian::store_be64(raw, static_cast<std::uint64_t>(n));
        out.append(raw, 8);
    }
    out.append(payload.data(), payload.size());
    return out;
}

std::vector<decoded_frame> parse_written(const std::string& bytes, bool masked) {
    std::vector<decoded_frame> out;
    frame_decoder decoder(frame_decoder::options{masked, 1u << 20});
    decoded_frame frame;
    if (decoder.feed(as_bytes(bytes), frame) == frame_status::frame) {
        out.push_back(std::move(frame));
    }
    while (decoder.feed(span<const std::byte>(), frame) == frame_status::frame) {
        out.push_back(std::move(frame));
    }
    return out;
}

struct session {
    explicit session(std::string scripted = std::string(),
                     bool is_client = false,
                     stream_options opt = stream_options{}) {
        opt.is_client = is_client;
        auto link = std::make_unique<fake_channel>(std::move(scripted));
        chan = link.get();
        s = std::make_unique<stream>(std::move(link), opt);
    }

    fake_channel* chan = nullptr;
    std::unique_ptr<stream> s;
};

CRPC_TEST(ws_stream_server_receives_text_and_binary) {
    session env(masked_frame(opcode::text, "hello") +
                masked_frame(opcode::binary, "bytes"));
    result<event> first = env.s->recv();
    CRPC_CHECK(first.ok());
    CRPC_CHECK_EQ(event::kind::message, first.value().type);
    CRPC_CHECK_EQ(opcode::text, first.value().op);
    CRPC_CHECK_EQ("hello", first.value().data);
    result<event> second = env.s->recv();
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(opcode::binary, second.value().op);
    CRPC_CHECK_EQ("bytes", second.value().data);
}

CRPC_TEST(ws_stream_client_receives_unmasked_frames) {
    session env(unmasked_frame(opcode::text, "from server"), true);
    result<event> ev = env.s->recv();
    CRPC_CHECK(ev.ok());
    CRPC_CHECK_EQ(event::kind::message, ev.value().type);
    CRPC_CHECK_EQ("from server", ev.value().data);
}

CRPC_TEST(ws_stream_reassembles_fragmented_message) {
    session env(masked_frame(opcode::text, "Hel", false) +
                masked_frame(opcode::continuation, "lo ", false) +
                masked_frame(opcode::continuation, "World"));
    result<event> ev = env.s->recv();
    CRPC_CHECK(ev.ok());
    CRPC_CHECK_EQ(event::kind::message, ev.value().type);
    CRPC_CHECK_EQ(opcode::text, ev.value().op);
    CRPC_CHECK_EQ("Hello World", ev.value().data);
}

CRPC_TEST(ws_stream_control_frames_interleave_fragments) {
    session env(masked_frame(opcode::text, "He", false) +
                masked_frame(opcode::ping, "P") +
                masked_frame(opcode::continuation, "llo"));
    result<event> ping = env.s->recv();
    CRPC_CHECK(ping.ok());
    CRPC_CHECK_EQ(event::kind::ping, ping.value().type);
    CRPC_CHECK_EQ("P", ping.value().data);
    result<event> message = env.s->recv();
    CRPC_CHECK(message.ok());
    CRPC_CHECK_EQ(event::kind::message, message.value().type);
    CRPC_CHECK_EQ("Hello", message.value().data);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::pong, written[0].op);
    CRPC_CHECK_EQ("P", written[0].payload);
}

CRPC_TEST(ws_stream_pong_is_surfaced) {
    session env(masked_frame(opcode::pong, "reply"));
    result<event> ev = env.s->recv();
    CRPC_CHECK(ev.ok());
    CRPC_CHECK_EQ(event::kind::pong, ev.value().type);
    CRPC_CHECK_EQ("reply", ev.value().data);
}

CRPC_TEST(ws_stream_continuation_without_start_is_error) {
    session env(masked_frame(opcode::continuation, "orphan"));
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::close, written[0].op);
    CRPC_CHECK_EQ(1002u,
                  static_cast<std::uint16_t>(
                      (static_cast<unsigned char>(written[0].payload[0]) << 8) |
                      static_cast<unsigned char>(written[0].payload[1])));
}

CRPC_TEST(ws_stream_data_frame_mid_fragment_is_error) {
    session env(masked_frame(opcode::text, "He", false) +
                masked_frame(opcode::binary, "nope"));
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
}

CRPC_TEST(ws_stream_mask_direction_enforced) {
    {
        // Server side receives an unmasked frame: protocol error.
        session env(unmasked_frame(opcode::text, "nope"));
        result<event> ev = env.s->recv();
        CRPC_CHECK_FALSE(ev.ok());
        CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    }
    {
        // Client side receives a masked frame: protocol error.
        session env(masked_frame(opcode::text, "nope"), true);
        result<event> ev = env.s->recv();
        CRPC_CHECK_FALSE(ev.ok());
        CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    }
}

CRPC_TEST(ws_stream_text_utf8_enforced) {
    session env(masked_frame(opcode::text, std::string("caf\xc3", 4)));
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::close, written[0].op);
    CRPC_CHECK_EQ(1007u,
                  static_cast<std::uint16_t>(
                      (static_cast<unsigned char>(written[0].payload[0]) << 8) |
                      static_cast<unsigned char>(written[0].payload[1])));
}

CRPC_TEST(ws_stream_fragmented_utf8_checked_after_reassembly) {
    // A UTF-8 sequence split across a fragment boundary is valid once the
    // message is reassembled; a genuinely invalid sequence is still rejected.
    {
        const std::string euro = "\xe2\x82\xac";
        session ok(masked_frame(opcode::text, euro.substr(0, 1), false) +
                   masked_frame(opcode::continuation, euro.substr(1)));
        result<event> ev = ok.s->recv();
        CRPC_CHECK(ev.ok());
        CRPC_CHECK_EQ(euro, ev.value().data);
    }
    {
        std::string first = "\xe2\x82\xac";
        session bad(masked_frame(opcode::text, "ok", false) +
                    masked_frame(opcode::continuation, std::string("\xff", 1)));
        result<event> ev = bad.s->recv();
        CRPC_CHECK_FALSE(ev.ok());
        CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    }
}

CRPC_TEST(ws_stream_close_handshake_exchanges_codes) {
session env(masked_frame(opcode::close, std::string("\x03\xe8", 2))); // code 1000
    result<event> ev = env.s->recv();
    CRPC_CHECK(ev.ok());
    CRPC_CHECK_EQ(event::kind::closed, ev.value().type);
    CRPC_CHECK(ev.value().had_code);
    CRPC_CHECK_EQ(1000u, ev.value().code);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::close, written[0].op);
    CRPC_CHECK_EQ(2u, written[0].payload.size());
    CRPC_CHECK_EQ(1000u,
                  static_cast<std::uint16_t>(
                      (static_cast<unsigned char>(written[0].payload[0]) << 8) |
                      static_cast<unsigned char>(written[0].payload[1])));
    // No data frames after the close handshake.
    CRPC_CHECK_FALSE(env.s->send_text("late").ok());
    // The channel stays readable until it is gone; then eof.
    result<event> after = env.s->recv();
    CRPC_CHECK_FALSE(after.ok());
    CRPC_CHECK_EQ(rpc_code::eof, after.error().code);
}

CRPC_TEST(ws_stream_close_without_code) {
    session env(masked_frame(opcode::close, ""));
    result<event> ev = env.s->recv();
    CRPC_CHECK(ev.ok());
    CRPC_CHECK_EQ(event::kind::closed, ev.value().type);
    CRPC_CHECK_FALSE(ev.value().had_code);
    CRPC_CHECK_EQ(1005u, ev.value().code); // close_code::no_status_received
}

CRPC_TEST(ws_stream_close_reason_echoed) {
    session env(masked_frame(opcode::close, std::string("\x03\xe8" "done", 6)));
    result<event> ev = env.s->recv();
    CRPC_CHECK(ev.ok());
    CRPC_CHECK_EQ(1000u, ev.value().code);
    CRPC_CHECK_EQ("done", ev.value().data);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ("done", written[0].payload.substr(2));
}

CRPC_TEST(ws_stream_invalid_close_code_is_protocol_error) {
    {
        session env(masked_frame(opcode::close, std::string("\x03\xec", 2))); // 1004: reserved
        result<event> ev = env.s->recv();
        CRPC_CHECK_FALSE(ev.ok());
        CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    }
    {
        session env(masked_frame(opcode::close, std::string("\x03\xf8", 2))); // 1016: reserved range
        result<event> ev = env.s->recv();
        CRPC_CHECK_FALSE(ev.ok());
        CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    }
    {
        session env(masked_frame(opcode::close, std::string("\x03\xe5", 2))); // 1005: reserved
        result<event> ev = env.s->recv();
        CRPC_CHECK_FALSE(ev.ok());
        CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
    }
    {
        // 1012 (IANA-registered) is acceptable on the wire.
        session env(masked_frame(opcode::close, std::string("\x03\xf4", 2)));
        result<event> ev = env.s->recv();
        CRPC_CHECK(ev.ok());
        CRPC_CHECK_EQ(event::kind::closed, ev.value().type);
        CRPC_CHECK_EQ(1012u, ev.value().code);
    }
}

CRPC_TEST(ws_stream_close_with_one_byte_payload_is_error) {
    session env(masked_frame(opcode::close, std::string("\x03", 1)));
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
}

CRPC_TEST(ws_stream_close_with_invalid_reason_utf8_is_error) {
    session env(masked_frame(opcode::close, std::string("\x03\xe8\xff", 3)));
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
}

CRPC_TEST(ws_stream_message_over_limit_closes_1009) {
    stream_options opt;
    opt.max_message_bytes = 8;
    session env(masked_frame(opcode::binary, "0123456789abcde"), false, opt);
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, ev.error().code);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::close, written[0].op);
    CRPC_CHECK_EQ(1009u,
                  static_cast<std::uint16_t>(
                      (static_cast<unsigned char>(written[0].payload[0]) << 8) |
                      static_cast<unsigned char>(written[0].payload[1])));
}

CRPC_TEST(ws_stream_fragmented_message_over_limit_closes_1009) {
    stream_options opt;
    opt.max_message_bytes = 8;
    session env(masked_frame(opcode::text, "aaaab", false) +
                masked_frame(opcode::continuation, "bbbb"),
                false, opt);
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, ev.error().code);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(1009u,
                  static_cast<std::uint16_t>(
                      (static_cast<unsigned char>(written[0].payload[0]) << 8) |
                      static_cast<unsigned char>(written[0].payload[1])));
}

CRPC_TEST(ws_stream_data_after_close_sent_is_protocol_error) {
    session env(masked_frame(opcode::text, "late"));
    CRPC_CHECK(env.s->send_close().ok());
    result<event> ev = env.s->recv();
    CRPC_CHECK_FALSE(ev.ok());
    CRPC_CHECK_EQ(rpc_code::protocol_error, ev.error().code);
}

CRPC_TEST(ws_stream_send_text_after_close_fails) {
    session env;
    CRPC_CHECK(env.s->send_close().ok());
    CRPC_CHECK_FALSE(env.s->send_text("nope").ok());
    CRPC_CHECK_FALSE(env.s->send_binary(as_bytes("nope")).ok());
}

CRPC_TEST(ws_stream_send_text_invalid_utf8_fails) {
    session env;
    CRPC_CHECK_FALSE(env.s->send_text(std::string("\xff", 1)).ok());
    CRPC_CHECK(env.chan->to_peer.empty());
}

CRPC_TEST(ws_stream_send_ping_payload_bound) {
    session env;
    CRPC_CHECK(env.s->send_ping(as_bytes("ok")).ok());
    CRPC_CHECK_FALSE(env.s->send_ping(as_bytes(std::string(126, 'x'))).ok());
    CRPC_CHECK_FALSE(env.s->send_pong(as_bytes(std::string(126, 'x'))).ok());
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, false);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::ping, written[0].op);
    CRPC_CHECK_EQ("ok", written[0].payload);
}

CRPC_TEST(ws_stream_outgoing_fragmentation) {
    stream_options opt;
    opt.max_fragment_bytes = 8;
    opt.is_client = true; // mask the output, so the inspect decode wants masks
    session env("", true, opt);
    const std::string payload(20, 'z');
    CRPC_CHECK(env.s->send_binary(as_bytes(payload)).ok());
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, true);
    CRPC_CHECK_EQ(3u, written.size());
    CRPC_CHECK_FALSE(written[0].fin);
    CRPC_CHECK_EQ(opcode::binary, written[0].op);
    CRPC_CHECK_EQ(8u, written[0].payload.size());
    CRPC_CHECK_FALSE(written[1].fin);
    CRPC_CHECK_EQ(opcode::continuation, written[1].op);
    CRPC_CHECK_EQ(8u, written[1].payload.size());
    CRPC_CHECK(written[2].fin);
    CRPC_CHECK_EQ(opcode::continuation, written[2].op);
    CRPC_CHECK_EQ(4u, written[2].payload.size());
    CRPC_CHECK_EQ(payload, written[0].payload + written[1].payload + written[2].payload);
}

CRPC_TEST(ws_stream_client_masks_outbound) {
    session env("", true);
CRPC_CHECK(env.s->send_text("hi").ok());
    // 2 header + 4 mask + 2 payload, with the client mask bit set.
    CRPC_CHECK_EQ(8u, env.chan->to_peer.size());
    CRPC_CHECK((static_cast<unsigned char>(env.chan->to_peer[1]) & 0x80u) != 0u);
    const std::vector<decoded_frame> written = parse_written(env.chan->to_peer, true);
    CRPC_CHECK_EQ(1u, written.size());
    CRPC_CHECK_EQ(opcode::text, written[0].op);
    CRPC_CHECK_EQ("hi", written[0].payload);
}

CRPC_TEST(ws_stream_abort_closes_channel) {
    session env;
    CRPC_CHECK(env.s->is_open());
    env.s->abort();
    CRPC_CHECK_FALSE(env.s->is_open());
    CRPC_CHECK(env.chan->closed);
}

} // namespace

CRPC_TEST_MAIN()
