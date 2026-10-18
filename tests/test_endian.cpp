// CRPC -- tests for the endian primitives.
//
// The point of these tests is not that the host happens to be little-endian; it
// is that the byte layout is fixed by the format. Every expected value below is
// written as an explicit byte sequence so a big-endian host would fail here if
// the primitives stopped swapping.
#include <crpc/endian.hpp>

#include "test_framework.hpp"

using namespace crpc;

CRPC_TEST(endian_store_u16_is_little_endian) {
    std::byte buffer[2] = {};
    endian::store_le16(buffer, 0x1234u);
    CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x34);
    CRPC_CHECK_EQ(static_cast<int>(buffer[1]), 0x12);
}

CRPC_TEST(endian_store_u32_is_little_endian) {
    std::byte buffer[4] = {};
    endian::store_le32(buffer, 0x01020304u);
    CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x04);
    CRPC_CHECK_EQ(static_cast<int>(buffer[1]), 0x03);
    CRPC_CHECK_EQ(static_cast<int>(buffer[2]), 0x02);
    CRPC_CHECK_EQ(static_cast<int>(buffer[3]), 0x01);
}

CRPC_TEST(endian_store_u64_is_little_endian) {
    std::byte buffer[8] = {};
    endian::store_le64(buffer, 0x0102030405060708ull);
    CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x08);
    CRPC_CHECK_EQ(static_cast<int>(buffer[1]), 0x07);
    CRPC_CHECK_EQ(static_cast<int>(buffer[6]), 0x02);
    CRPC_CHECK_EQ(static_cast<int>(buffer[7]), 0x01);
}

CRPC_TEST(endian_round_trip_all_widths) {
    const u16 v16 = 0xABCDu;
    std::byte b16[2] = {};
    endian::store_le16(b16, v16);
    CRPC_CHECK_EQ(endian::load_le16(b16), v16);

    const u32 v32 = 0xDEADBEEFu;
    std::byte b32[4] = {};
    endian::store_le32(b32, v32);
    CRPC_CHECK_EQ(endian::load_le32(b32), v32);

    const u64 v64 = 0x0123456789ABCDEFull;
    std::byte b64[8] = {};
    endian::store_le64(b64, v64);
    CRPC_CHECK_EQ(endian::load_le64(b64), v64);
}

CRPC_TEST(endian_reads_foreign_byte_order) {
    // A big-endian peer's buffer is just bytes; the loader must interpret them
    // as little-endian regardless of how they were produced.
    const std::byte wire[4] = {std::byte{0x78}, std::byte{0x56}, std::byte{0x34},
                               std::byte{0x12}};
    CRPC_CHECK_EQ(endian::load_le32(wire), 0x12345678u);
}

CRPC_TEST(endian_float_bit_pattern_survives) {
    const float values[] = {0.0f, -0.0f, 1.5f, -2.25f, 3.402823466e+38f};
    for (float value : values) {
        const u32 bits = endian::float_to_bits(value);
        const float back = endian::bits_to_float(bits);
        CRPC_CHECK_EQ(endian::float_to_bits(back), bits);
    }
}

CRPC_TEST(endian_double_bit_pattern_survives) {
    const double values[] = {0.0, -0.0, 1.5, -2.25, 1.7976931348623157e+308};
    for (double value : values) {
        const u64 bits = endian::double_to_bits(value);
        const double back = endian::bits_to_double(bits);
        CRPC_CHECK_EQ(endian::double_to_bits(back), bits);
    }
}

CRPC_TEST(endian_negative_zero_is_preserved) {
    // Negative zero compares equal to positive zero, so the check has to look
    // at the sign bit rather than at the value.
    const double negative_zero = -0.0;
    const u64 bits = endian::double_to_bits(negative_zero);
    CRPC_CHECK_EQ((bits >> 63) & 1u, 1u);
}

CRPC_TEST_MAIN()
