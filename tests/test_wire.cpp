// CRPC -- tests for the wire reader and writer.
//
// Coverage targets: every primitive round trip, varint boundaries (0, 127, 128,
// 2^32-1, 2^63-1, 2^64-1), zigzag sign handling, malformed varints (11 bytes,
// over-long final byte), truncated input, and the "no partial write on failure"
// contract.
#include <crpc/wire.hpp>

#include "test_framework.hpp"

using namespace crpc;

CRPC_TEST(wire_u8_u16_u32_u64_round_trip) {
    std::byte storage[15] = {};
    byte_writer writer(span<std::byte>(storage, sizeof(storage)));
    CRPC_CHECK(writer.put_u8(0x7Fu));
    CRPC_CHECK(writer.put_u16(0x1234u));
    CRPC_CHECK(writer.put_u32(0xDEADBEEFu));
    CRPC_CHECK(writer.put_u64(0x0123456789ABCDEFull));
    CRPC_CHECK(writer.ok());
    CRPC_CHECK_EQ(writer.written(), static_cast<std::size_t>(15));

    byte_reader reader(span<const std::byte>(storage, sizeof(storage)));
    u8 v8 = 0;
    u16 v16 = 0;
    u32 v32 = 0;
    u64 v64 = 0;
    CRPC_CHECK(reader.get_u8(v8));
    CRPC_CHECK(reader.get_u16(v16));
    CRPC_CHECK(reader.get_u32(v32));
    CRPC_CHECK(reader.get_u64(v64));
    CRPC_CHECK_EQ(static_cast<int>(v8), 0x7F);
    CRPC_CHECK_EQ(v16, 0x1234u);
    CRPC_CHECK_EQ(v32, 0xDEADBEEFu);
    CRPC_CHECK_EQ(v64, 0x0123456789ABCDEFull);
    CRPC_CHECK_EQ(reader.remaining(), static_cast<std::size_t>(0));
}

CRPC_TEST(wire_float_round_trip) {
    std::byte storage[12] = {};
    byte_writer writer(span<std::byte>(storage, sizeof(storage)));
    CRPC_CHECK(writer.put_f32(1.25f));
    CRPC_CHECK(writer.put_f64(-3.5));
    CRPC_CHECK(writer.ok());

    byte_reader reader(span<const std::byte>(storage, sizeof(storage)));
    float f = 0.0f;
    double d = 0.0;
    CRPC_CHECK(reader.get_f32(f));
    CRPC_CHECK(reader.get_f64(d));
    CRPC_CHECK_EQ(f, 1.25f);
    CRPC_CHECK_EQ(d, -3.5);
}

CRPC_TEST(wire_varint_boundaries) {
    const u64 values[] = {0ull,
                          1ull,
                          127ull,
                          128ull,
                          255ull,
                          256ull,
                          16383ull,
                          16384ull,
                          0xFFFFFFFFull,
                          0x100000000ull,
                          0x7FFFFFFFFFFFFFFFull,
                          0xFFFFFFFFFFFFFFFFull};
    for (u64 value : values) {
        std::byte storage[10] = {};
        byte_writer writer(span<std::byte>(storage, sizeof(storage)));
        CRPC_CHECK(writer.put_varint(value));
        CRPC_CHECK(writer.ok());

        byte_reader reader(span<const std::byte>(storage, writer.written()));
        u64 decoded = 0;
        CRPC_CHECK(reader.get_varint(decoded));
        CRPC_CHECK_EQ(decoded, value);
        CRPC_CHECK_EQ(reader.remaining(), static_cast<std::size_t>(0));
    }
}

CRPC_TEST(wire_varint_encoding_sizes) {
    struct expected_size {
        u64 value;
        std::size_t bytes;
    };
    const expected_size cases[] = {{0ull, 1},   {127ull, 1},  {128ull, 2},
                                   {16383ull, 2}, {16384ull, 3},
                                   {0xFFFFFFFFull, 5}, {0xFFFFFFFFFFFFFFFFull, 10}};
    for (const expected_size& c : cases) {
        std::byte storage[10] = {};
        byte_writer writer(span<std::byte>(storage, sizeof(storage)));
        CRPC_CHECK(writer.put_varint(c.value));
        CRPC_CHECK_EQ(writer.written(), c.bytes);
    }
}

CRPC_TEST(wire_varint_zigzag_signs) {
    const i64 values[] = {0, -1, 1, -2, 2, -1000, 1000, INT64_MIN, INT64_MAX};
    for (i64 value : values) {
        std::byte storage[10] = {};
        byte_writer writer(span<std::byte>(storage, sizeof(storage)));
        CRPC_CHECK(writer.put_varint_i64(value));
        CRPC_CHECK(writer.ok());

        byte_reader reader(span<const std::byte>(storage, writer.written()));
        i64 decoded = 0;
        CRPC_CHECK(reader.get_varint_i64(decoded));
        CRPC_CHECK_EQ(decoded, value);
    }
}

CRPC_TEST(wire_varint_rejects_eleven_bytes) {
    // Ten continuation bytes followed by anything is malformed: a u64 varint
    // cannot be longer than ten bytes.
    std::byte storage[11];
    for (int i = 0; i < 10; ++i) {
        storage[i] = std::byte{0x80};
    }
    storage[10] = std::byte{0x01};

    byte_reader reader(span<const std::byte>(storage, sizeof(storage)));
    u64 value = 0;
    CRPC_CHECK_FALSE(reader.get_varint(value));
    CRPC_CHECK_EQ(static_cast<int>(reader.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(wire_varint_rejects_overflow_in_tenth_byte) {
    // Nine continuation bytes then a final byte carrying more than bit 63.
    std::byte storage[10];
    for (int i = 0; i < 9; ++i) {
        storage[i] = std::byte{0xFF};
    }
    storage[9] = std::byte{0x7F};

    byte_reader reader(span<const std::byte>(storage, sizeof(storage)));
    u64 value = 0;
    CRPC_CHECK_FALSE(reader.get_varint(value));
    CRPC_CHECK_EQ(static_cast<int>(reader.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(wire_reader_rejects_truncated_input) {
    const std::byte storage[2] = {std::byte{0x01}, std::byte{0x02}};
    byte_reader reader(span<const std::byte>(storage, sizeof(storage)));
    u32 value = 0;
    CRPC_CHECK_FALSE(reader.get_u32(value));
    CRPC_CHECK_EQ(static_cast<int>(reader.error()), static_cast<int>(rpc_code::protocol_error));
    // A failed read must not advance the cursor.
    CRPC_CHECK_EQ(reader.read(), static_cast<std::size_t>(0));
}

CRPC_TEST(wire_writer_rejects_overflow) {
    std::byte storage[2] = {};
    byte_writer writer(span<std::byte>(storage, sizeof(storage)));
    CRPC_CHECK_FALSE(writer.put_u32(1));
    CRPC_CHECK_EQ(static_cast<int>(writer.error()), static_cast<int>(rpc_code::encode_error));
    // A failed write must not advance the cursor or write partial bytes.
    CRPC_CHECK_EQ(writer.written(), static_cast<std::size_t>(0));
    CRPC_CHECK_EQ(static_cast<int>(storage[0]), 0);
}

CRPC_TEST(wire_length_prefixed_round_trip) {
    const std::byte payload[5] = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'},
                                  std::byte{'l'}, std::byte{'o'}};
    std::byte storage[16] = {};
    byte_writer writer(span<std::byte>(storage, sizeof(storage)));
    CRPC_CHECK(writer.put_length_prefixed(span<const std::byte>(payload, sizeof(payload))));

    byte_reader reader(span<const std::byte>(storage, writer.written()));
    span<const std::byte> out;
    CRPC_CHECK(reader.get_length_prefixed(out));
    CRPC_CHECK_EQ(out.size(), static_cast<std::size_t>(5));
    CRPC_CHECK_EQ(static_cast<char>(out[0]), 'h');
    CRPC_CHECK_EQ(static_cast<char>(out[4]), 'o');
}

CRPC_TEST(wire_length_prefixed_rejects_length_beyond_input) {
    // varint 100 followed by only three bytes.
    std::byte storage[4] = {std::byte{100}, std::byte{1}, std::byte{2}, std::byte{3}};
    byte_reader reader(span<const std::byte>(storage, sizeof(storage)));
    span<const std::byte> out;
    CRPC_CHECK_FALSE(reader.get_length_prefixed(out));
    CRPC_CHECK_EQ(static_cast<int>(reader.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(wire_patch_u32_writes_length_in_place) {
    std::byte storage[8] = {};
    byte_writer writer(span<std::byte>(storage, sizeof(storage)));
    span<std::byte> header;
    CRPC_CHECK(writer.put_zeroes(4, header));
    CRPC_CHECK(writer.put_u8(0xAAu));
    CRPC_CHECK(writer.patch_u32(0, 1u));

    byte_reader reader(span<const std::byte>(storage, writer.written()));
    u32 length = 0;
    u8 marker = 0;
    CRPC_CHECK(reader.get_u32(length));
    CRPC_CHECK(reader.get_u8(marker));
    CRPC_CHECK_EQ(length, 1u);
    CRPC_CHECK_EQ(static_cast<int>(marker), 0xAA);
}

CRPC_TEST_MAIN()
