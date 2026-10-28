// CRPC -- AMQP 0-9-1 framing and field-table codec.
//
// These cases exercise the parts a real broker also exercises but with values a
// broker will not produce on demand: a body that must be split at frame_max, a
// frame-end octet that is wrong, a field table with every type this codec
// implements. The integration test covers the method field numbers against a
// live RabbitMQ; this file covers the byte layout in isolation.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/mq/amqp/connection.hpp>
#include <crpc/mq/amqp/protocol.hpp>
#include <crpc/mq/amqp/types.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::mq::amqp::field_table;
using crpc::mq::amqp::field_value;
using crpc::mq::amqp::frame;
using crpc::mq::amqp::frame_type;
using crpc::mq::amqp::message_properties;
using crpc::mq::amqp::reader;
using crpc::mq::amqp::writer;

[[nodiscard]] crpc::span<const std::byte> bytes_of(const std::vector<std::byte>& data) {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] crpc::span<const std::byte> bytes_of(std::string_view text) {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

} // namespace

CRPC_TEST(amqp_protocol_header_is_the_0_9_1_magic) {
    // The header is not a frame: it is the eight bytes a client writes before
    // any frame, and RabbitMQ rejects an AMQP 0-9 client with the frame "AMQP"
    // layout. Spelling it out here is what keeps a typo from looking like a
    // server problem.
    const std::vector<std::byte> expected = {
        std::byte{0x41}, std::byte{0x4D}, std::byte{0x51}, std::byte{0x50},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x09}, std::byte{0x01}};
    CRPC_CHECK_EQ(expected.size(), crpc::mq::amqp::protocol_header.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(expected[i]),
                      static_cast<int>(crpc::mq::amqp::protocol_header[i]));
    }
}

CRPC_TEST(amqp_frame_roundtrip_keeps_type_channel_and_payload) {
    const std::vector<std::byte> payload = {std::byte{0x00}, std::byte{0x0A},
                                            std::byte{0x00}, std::byte{0x0A}};
    const std::vector<std::byte> encoded =
        crpc::mq::amqp::encode_frame(frame_type::method, 7, bytes_of(payload));
    // type(1) + channel(2) + size(4) + payload + end(1)
    CRPC_CHECK_EQ(std::size_t{12}, encoded.size());
    CRPC_CHECK_EQ(static_cast<int>(frame_type::method),
                  static_cast<int>(encoded[0]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[1]));
    CRPC_CHECK_EQ(7, static_cast<int>(encoded[2]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[3]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[4]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[5]));
    CRPC_CHECK_EQ(4, static_cast<int>(encoded[6]));
    CRPC_CHECK_EQ(static_cast<int>(crpc::mq::amqp::frame_end_octet),
                  static_cast<int>(encoded[encoded.size() - 1]));

    frame decoded;
    std::size_t consumed = 0;
    const crpc::status parsed =
        crpc::mq::amqp::decode_frame(bytes_of(encoded), decoded, consumed);
    CRPC_CHECK(parsed.ok());
    CRPC_CHECK_EQ(encoded.size(), consumed);
    CRPC_CHECK_EQ(static_cast<int>(frame_type::method),
                  static_cast<int>(decoded.type));
    CRPC_CHECK_EQ(7, static_cast<int>(decoded.channel));
    CRPC_CHECK_EQ(payload.size(), decoded.payload.size());
}

CRPC_TEST(amqp_heartbeat_frame_has_an_empty_payload) {
    const std::vector<std::byte> encoded =
        crpc::mq::amqp::encode_frame(frame_type::heartbeat, 0, crpc::span<const std::byte>());
    CRPC_CHECK_EQ(std::size_t{8}, encoded.size());
    frame decoded;
    std::size_t consumed = 0;
    CRPC_CHECK(crpc::mq::amqp::decode_frame(bytes_of(encoded), decoded, consumed).ok());
    CRPC_CHECK_EQ(static_cast<int>(frame_type::heartbeat), static_cast<int>(decoded.type));
    CRPC_CHECK_EQ(0, static_cast<int>(decoded.channel));
    CRPC_CHECK_EQ(std::size_t{0}, decoded.payload.size());
}

CRPC_TEST(amqp_frame_with_a_wrong_end_octet_is_rejected) {
    std::vector<std::byte> encoded =
        crpc::mq::amqp::encode_frame(frame_type::method, 1, bytes_of(std::string_view("x")));
    encoded.back() = std::byte{0x00};
    frame decoded;
    std::size_t consumed = 0;
    const crpc::status parsed =
        crpc::mq::amqp::decode_frame(bytes_of(encoded), decoded, consumed);
    CRPC_CHECK_FALSE(parsed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(parsed.code()));
}

CRPC_TEST(amqp_truncated_frame_is_rejected) {
    std::vector<std::byte> encoded =
        crpc::mq::amqp::encode_frame(frame_type::body, 2, bytes_of(std::string_view("abcd")));
    encoded.resize(encoded.size() - 1);
    frame decoded;
    std::size_t consumed = 0;
    const crpc::status parsed =
        crpc::mq::amqp::decode_frame(bytes_of(encoded), decoded, consumed);
    CRPC_CHECK_FALSE(parsed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(parsed.code()));
}

CRPC_TEST(amqp_frame_with_an_unknown_type_is_rejected) {
    std::vector<std::byte> encoded =
        crpc::mq::amqp::encode_frame(frame_type::method, 1, crpc::span<const std::byte>());
    encoded[0] = std::byte{0x07};
    frame decoded;
    std::size_t consumed = 0;
    const crpc::status parsed =
        crpc::mq::amqp::decode_frame(bytes_of(encoded), decoded, consumed);
    CRPC_CHECK_FALSE(parsed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(parsed.code()));
}

CRPC_TEST(amqp_field_table_roundtrips_every_supported_type) {
    field_table inner;
    inner.set("answer", field_value::make_int32(-7));
    inner.set("nested_flag", field_value::make_bool(false));

    field_table table;
    table.set("text", field_value::make_string("hello world"));
    table.set("short", field_value::make_short_string("tiny"));
    table.set("flag", field_value::make_bool(true));
    table.set("signed", field_value::make_int32(-123456));
    table.set("unsigned", field_value::make_uint32(4000000000u));
    table.set("wide", field_value::make_int64(-9000000000LL));
    table.set("real", field_value::make_double(2.5));
    table.set("stamp", field_value::make_timestamp(1700000000));
    table.set("inner", field_value::make_table(inner));
    table.set("list", field_value::make_array({field_value::make_int32(1),
                                               field_value::make_string("two"),
                                               field_value::make_bool(false)}));

    writer out;
    out.table(table);
    CRPC_CHECK(out.ok());
    CRPC_CHECK(!out.data().empty());

    reader in(bytes_of(out.data()));
    field_table back;
    CRPC_CHECK(in.read_table(back));
    CRPC_CHECK(in.ok());
    CRPC_CHECK(in.remaining() == 0);
    CRPC_CHECK_EQ(table.size(), back.size());
    CRPC_CHECK(table == back);

    // A specific value is checked separately so a failure names the field that
    // did not survive instead of only "tables differ".
    const field_value* count = back.find("signed");
    CRPC_CHECK(count != nullptr);
    if (count != nullptr) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::mq::amqp::field_type::int32),
                      static_cast<int>(count->type));
        CRPC_CHECK_EQ(std::int64_t{-123456}, count->signed_integer);
    }
    const field_value* nested = back.find("inner");
    CRPC_CHECK(nested != nullptr);
    if (nested != nullptr && nested->table != nullptr) {
        const field_value* answer = nested->table->find("answer");
        CRPC_CHECK(answer != nullptr);
        if (answer != nullptr) {
            CRPC_CHECK_EQ(std::int64_t{-7}, answer->signed_integer);
        }
    } else {
        CRPC_CHECK(false);
    }
    const field_value* list = back.find("list");
    CRPC_CHECK(list != nullptr);
    if (list != nullptr) {
        CRPC_CHECK_EQ(std::size_t{3}, list->array.size());
    }
}

CRPC_TEST(amqp_field_table_with_a_bad_length_is_rejected) {
    // One entry is a u32 table length followed by its content. Claiming four
    // bytes more than the buffer holds must be a decode error, not a read past
    // the end.
    const std::vector<std::byte> table = {
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x09},
        std::byte{0x01}, std::byte{0x61}, std::byte{0x49},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
    reader in(bytes_of(table));
    field_table decoded;
    CRPC_CHECK_FALSE(in.read_table(decoded));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(in.error()));
}

CRPC_TEST(amqp_unknown_field_type_is_rejected) {
    const std::vector<std::byte> table = {
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x07},
        std::byte{0x01}, std::byte{0x61}, std::byte{0x5A},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
    reader in(bytes_of(table));
    field_table decoded;
    CRPC_CHECK_FALSE(in.read_table(decoded));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(in.error()));
}

CRPC_TEST(amqp_short_string_longer_than_255_is_an_encode_error) {
    const std::string too_long(300, 'x');
    writer out;
    out.short_string(too_long);
    CRPC_CHECK_FALSE(out.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::encode_error),
                  static_cast<int>(out.error()));
}

CRPC_TEST(amqp_basic_properties_roundtrip) {
    message_properties properties;
    properties.content_type = "application/json";
    properties.content_encoding = "utf-8";
    properties.delivery_mode = std::uint8_t{2};
    properties.priority = std::uint8_t{3};
    properties.correlation_id = "corr-1";
    properties.reply_to = "reply.queue";
    properties.expiration = "60000";
    properties.message_id = "msg-1";
    properties.timestamp = std::uint64_t{1700000123};
    properties.type = "test.event";
    properties.user_id = "guest";
    properties.app_id = "crpc-test";
    properties.cluster_id = "cluster-1";
    properties.headers.set("trace", field_value::make_string("abc"));
    properties.headers.set("retry", field_value::make_int32(2));

    writer out;
    out.properties(properties);
    CRPC_CHECK(out.ok());

    reader in(bytes_of(out.data()));
    message_properties back;
    CRPC_CHECK(in.read_properties(back));
    CRPC_CHECK(in.ok());
    CRPC_CHECK(in.remaining() == 0);
    CRPC_CHECK(properties == back);
}

CRPC_TEST(amqp_properties_with_reserved_flag_bits_are_rejected) {
    // Bit 0 and bit 1 are reserved. A peer that sets them is malformed, and
    // silently accepting the frame would shift every following property.
    const std::vector<std::byte> encoded = {std::byte{0x00}, std::byte{0x03}};
    reader in(bytes_of(encoded));
    message_properties back;
    CRPC_CHECK_FALSE(in.read_properties(back));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(in.error()));
}

CRPC_TEST(amqp_content_is_split_at_frame_max_minus_eight) {
    std::vector<std::byte> body(1500);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = std::byte(static_cast<unsigned int>(i * 31u) & 0xFFu);
    }
    message_properties properties;
    properties.content_type = "application/octet-stream";
    properties.delivery_mode = std::uint8_t{1};

    std::vector<std::byte> encoded;
    const crpc::status appended = crpc::mq::amqp::append_content_frames(
        encoded, 5, crpc::mq::amqp::class_id::basic, properties, bytes_of(body), 512);
    CRPC_CHECK(appended.ok());

    std::size_t offset = 0;
    std::size_t header_frames = 0;
    std::size_t body_frames = 0;
    std::vector<std::byte> reassembled;
    std::uint64_t announced = 0;
    while (offset < encoded.size()) {
        frame decoded;
        std::size_t consumed = 0;
        const crpc::status parsed = crpc::mq::amqp::decode_frame(
            crpc::span<const std::byte>(encoded.data() + offset, encoded.size() - offset),
            decoded, consumed);
        CRPC_CHECK(parsed.ok());
        if (!parsed.ok()) {
            break;
        }
        CRPC_CHECK_EQ(5, static_cast<int>(decoded.channel));
        if (decoded.type == frame_type::header) {
            ++header_frames;
            reader in(bytes_of(decoded.payload));
            const unsigned int header_class = in.u16();
            const unsigned int weight = in.u16();
            announced = in.u64();
            message_properties decoded_properties;
            CRPC_CHECK(in.read_properties(decoded_properties));
            CRPC_CHECK_EQ(static_cast<unsigned int>(crpc::mq::amqp::class_id::basic),
                          header_class);
            CRPC_CHECK_EQ(0u, weight);
            CRPC_CHECK(decoded_properties == properties);
        } else if (decoded.type == frame_type::body) {
            ++body_frames;
            // frame_max counts the whole frame; eight bytes are header and end.
            CRPC_CHECK(decoded.payload.size() <= std::size_t{512 - 8});
            reassembled.insert(reassembled.end(), decoded.payload.begin(),
                               decoded.payload.end());
        } else {
            CRPC_CHECK(false);
        }
        offset += consumed;
    }
    CRPC_CHECK_EQ(std::size_t{1}, header_frames);
    CRPC_CHECK_EQ(std::size_t{3}, body_frames);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(body.size()), announced);
    CRPC_CHECK_EQ(body.size(), reassembled.size());
    CRPC_CHECK(body == reassembled);
}

CRPC_TEST(amqp_empty_body_produces_only_a_header_frame) {
    message_properties properties;
    std::vector<std::byte> encoded;
    const crpc::status appended = crpc::mq::amqp::append_content_frames(
        encoded, 1, crpc::mq::amqp::class_id::basic, properties,
        crpc::span<const std::byte>(), 4096);
    CRPC_CHECK(appended.ok());
    frame decoded;
    std::size_t consumed = 0;
    CRPC_CHECK(crpc::mq::amqp::decode_frame(bytes_of(encoded), decoded, consumed).ok());
    CRPC_CHECK(consumed == encoded.size());
    CRPC_CHECK_EQ(static_cast<int>(frame_type::header), static_cast<int>(decoded.type));
}

CRPC_TEST(amqp_frame_max_smaller_than_the_frame_overhead_is_rejected) {
    std::vector<std::byte> encoded;
    const crpc::status appended = crpc::mq::amqp::append_content_frames(
        encoded, 1, crpc::mq::amqp::class_id::basic, message_properties(),
        bytes_of(std::string_view("x")), 8);
    CRPC_CHECK_FALSE(appended.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::encode_error),
                  static_cast<int>(appended.code()));
}

CRPC_TEST(amqp_node_visit_order_rotates_from_last_success) {
    namespace order = crpc::mq::amqp;
    // The node that succeeded last is probed first; the rest follow in index
    // order. This is what keeps a healthy broker the primary and a recovering
    // one the first fallback without reordering the configured list.
    const std::vector<std::size_t> from_one = order::node_visit_order(3, 1);
    CRPC_CHECK_EQ(std::size_t{3}, from_one.size());
    CRPC_CHECK_EQ(std::size_t{1}, from_one[0]);
    CRPC_CHECK_EQ(std::size_t{0}, from_one[1]);
    CRPC_CHECK_EQ(std::size_t{2}, from_one[2]);

    const std::vector<std::size_t> from_zero = order::node_visit_order(3, 0);
    CRPC_CHECK_EQ(std::size_t{0}, from_zero[0]);
    CRPC_CHECK_EQ(std::size_t{1}, from_zero[1]);
    CRPC_CHECK_EQ(std::size_t{2}, from_zero[2]);

    // Two nodes: the dead primary first, the healthy standby second. This is
    // the exact shape a failover reconnect visits.
    const std::vector<std::size_t> pair = order::node_visit_order(2, 0);
    CRPC_CHECK_EQ(std::size_t{2}, pair.size());
    CRPC_CHECK_EQ(std::size_t{0}, pair[0]);
    CRPC_CHECK_EQ(std::size_t{1}, pair[1]);

    // A single node visits only itself, and the rotation never repeats an
    // index.
    const std::vector<std::size_t> lone = order::node_visit_order(1, 0);
    CRPC_CHECK_EQ(std::size_t{1}, lone.size());
    CRPC_CHECK_EQ(std::size_t{0}, lone[0]);

    const std::vector<std::size_t> empty = order::node_visit_order(0, 0);
    CRPC_CHECK(empty.empty());
}

CRPC_TEST(amqp_node_backoff_grows_exponentially_up_to_the_cap) {
    namespace order = crpc::mq::amqp;
    using crpc::mq::amqp::node_backoff_delay;
    using std::chrono::milliseconds;
    using std::chrono::duration_cast;

    const milliseconds base(100);
    const milliseconds cap(2000);
    // Zero jitter isolates the exponential part. The framework compares with
    // operator<<, which chrono durations do not provide, so compare counts.
    const auto d0 = node_backoff_delay(0, base, cap, 0).count();
    const auto d1 = node_backoff_delay(1, base, cap, 0).count();
    const auto d2 = node_backoff_delay(2, base, cap, 0).count();
    const auto d3 = node_backoff_delay(3, base, cap, 0).count();
    const auto d4 = node_backoff_delay(4, base, cap, 0).count();
    CRPC_CHECK_EQ(static_cast<std::int64_t>(100), d0);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(200), d1);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(400), d2);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(800), d3);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1600), d4);
    // Attempt 5 would be 3200ms; the cap pins the delay at 2000ms.
    const auto d5 = node_backoff_delay(5, base, cap, 0).count();
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2000), d5);
    const auto d100 = node_backoff_delay(100, base, cap, 0).count();
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2000), d100);
}

CRPC_TEST(amqp_node_backoff_jitter_stays_within_the_base) {
    namespace order = crpc::mq::amqp;
    using crpc::mq::amqp::node_backoff_delay;
    using std::chrono::milliseconds;

    const milliseconds base(100);
    const milliseconds cap(1000);
    // Jitter is deterministic from the basis, so the schedule is testable and
    // a fleet does not stampede: every agent draws a slightly different delay.
    const auto j42 = node_backoff_delay(2, base, cap, 42).count();
    CRPC_CHECK_EQ(static_cast<std::int64_t>(442), j42);
    const auto j99 = node_backoff_delay(2, base, cap, 99).count();
    CRPC_CHECK_EQ(static_cast<std::int64_t>(499), j99);
    // A jitter basis of exactly base wraps to zero.
    const auto j100 = node_backoff_delay(2, base, cap, 100).count();
    CRPC_CHECK_EQ(static_cast<std::int64_t>(400), j100);
    // The jittered delay never exceeds the capped backoff plus base - 1.
    for (std::uint64_t basis = 0; basis < 400; basis += 17) {
        const auto delay = node_backoff_delay(10, base, cap, basis).count();
        CRPC_CHECK(delay <= static_cast<std::int64_t>(2000 + 99));
    }
}

CRPC_TEST_MAIN()