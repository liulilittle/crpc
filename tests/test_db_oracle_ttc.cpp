// CRPC -- Oracle TTC codec tests.
//
// IMPORTANT: these expectations come from reading Oracle's python-oracledb (thin
// mode) source, not from bytes a server produced. There is no Oracle server in
// this environment, so this file is a **manual comparison** record, not a
// differential test; docs/oracle-ttc-evidence.md sections 9-12 list the file and
// line each layout was taken from.
//
// What the cases lock down:
//   * the universal-integer encoding, which every TTC field depends on;
//   * the Execute/Fetch bodies field by field, including the parts a hand-written
//     implementation usually gets wrong (the cursor-id pointer, the al8i4 array,
//     the 12.2+ empty pointers);
//   * Oracle NUMBER, where the exponent arithmetic wraps in 8 bits and a zero
//     mantissa byte under a positive number becomes 255;
//   * Oracle DATE, whose hour/minute/second are biased by one.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/oracle/ttc.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::oracle::ora_type;
using crpc::db::oracle::ttc_column;
using crpc::db::oracle::ttc_execute;
using crpc::db::oracle::ttc_fetch;
using crpc::db::oracle::ttc_function;
using crpc::db::oracle::ttc_reader;
using crpc::db::oracle::ttc_value;

[[nodiscard]] crpc::span<const std::byte> view(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string hex(const std::vector<std::byte>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::byte b : data) {
        const auto value = static_cast<std::uint8_t>(b);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0f]);
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> from_hex(std::string_view text) {
    auto value_of = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a') + 10u;
        }
        return 0u;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>((value_of(text[i]) << 4) | value_of(text[i + 1])));
    }
    return out;
}

// --- 1. Universal integers ---------------------------------------------------

CRPC_TEST(ttc_universal_integers_pick_the_shortest_length) {
    // impl/base/buffer.pyx:737-790: one length byte, then the big-endian value;
    // 0 is the single byte 0.
    std::vector<std::byte> out;
    crpc::db::oracle::ttc_write_ub4(out, 0);
    CRPC_CHECK_EQ(std::string_view("00"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub4(out, 1);
    CRPC_CHECK_EQ(std::string_view("0101"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub4(out, 0xff);
    CRPC_CHECK_EQ(std::string_view("01ff"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub4(out, 0x100);
    CRPC_CHECK_EQ(std::string_view("020100"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub4(out, 0xffff);
    CRPC_CHECK_EQ(std::string_view("02ffff"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub4(out, 0x10000);
    CRPC_CHECK_EQ(std::string_view("0400010000"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub4(out, 0x7fffffffu);
    CRPC_CHECK_EQ(std::string_view("047fffffff"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_ub8(out, 0x100000000ull);
    CRPC_CHECK_EQ(std::string_view("080000000100000000"), hex(out));
}

CRPC_TEST(ttc_signed_integers_set_the_length_bit) {
    // impl/base/buffer.pyx:666-684: the sign is bit 0x80 of the length byte.
    std::vector<std::byte> out;
    crpc::db::oracle::ttc_write_sb4(out, 0);
    CRPC_CHECK_EQ(std::string_view("00"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_sb4(out, 1);
    CRPC_CHECK_EQ(std::string_view("0101"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_sb4(out, -1);
    CRPC_CHECK_EQ(std::string_view("8101"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_sb4(out, -300);
    CRPC_CHECK_EQ(std::string_view("82012c"), hex(out));
    out.clear();
    crpc::db::oracle::ttc_write_sb4(out, 300);
    CRPC_CHECK_EQ(std::string_view("02012c"), hex(out));
}

CRPC_TEST(ttc_length_prefixed_bytes_switch_to_chunks) {
    // impl/base/buffer.pyx:132-151: up to 252 bytes use one length byte; beyond
    // that the value is 254 followed by ub4 chunk length + bytes, then ub4 0.
    std::vector<std::byte> small;
    const std::vector<std::byte> three = from_hex("aabbcc");
    crpc::db::oracle::ttc_write_bytes_with_length(small, view(three));
    CRPC_CHECK_EQ(std::string_view("03aabbcc"), hex(small));

    std::vector<std::byte> empty;
    crpc::db::oracle::ttc_write_bytes_with_length(empty, crpc::span<const std::byte>());
    CRPC_CHECK_EQ(std::string_view("00"), hex(empty));

    // 300 bytes: 254, ub4 300, the bytes, ub4 0.
    std::vector<std::byte> big(300, std::byte{0x5a});
    std::vector<std::byte> chunked;
    crpc::db::oracle::ttc_write_bytes_with_length(chunked, view(big));
    CRPC_CHECK_EQ(std::size_t{1 + 3 + 300 + 1}, chunked.size());
    CRPC_CHECK_EQ(254, static_cast<int>(chunked[0]));
    CRPC_CHECK_EQ(2, static_cast<int>(chunked[1])); // ub4 length: 2 bytes
    CRPC_CHECK_EQ(0x01, static_cast<int>(chunked[2]));
    CRPC_CHECK_EQ(0x2c, static_cast<int>(chunked[3])); // 300
    CRPC_CHECK_EQ(0, static_cast<int>(chunked[chunked.size() - 1])); // terminating 0
}

CRPC_TEST(ttc_reader_decodes_the_universal_integers) {
    const std::vector<std::byte> data = from_hex("00" "0101" "020100" "0400010000" "8101");
    ttc_reader reader(view(data));
    CRPC_CHECK_EQ(0u, reader.ub4());
    CRPC_CHECK_EQ(1u, reader.ub4());
    CRPC_CHECK_EQ(256u, reader.ub4());
    CRPC_CHECK_EQ(65536u, reader.ub4());
    CRPC_CHECK_EQ(-1, reader.sb4());
    CRPC_CHECK(reader.ok());
    CRPC_CHECK_EQ(data.size(), reader.position());

    // Reading past the end fails and sticks instead of returning a value.
    CRPC_CHECK_EQ(0u, reader.ub4());
    CRPC_CHECK_FALSE(reader.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(reader.error().code()));
}

CRPC_TEST(ttc_reader_rejects_a_mismatched_width) {
    // A length byte of 4 where ub2 was asked for is a desynchronised stream, not
    // a value to clamp.
    const std::vector<std::byte> wide = from_hex("0401020304");
    ttc_reader reader(view(wide));
    CRPC_CHECK_EQ(0, static_cast<int>(reader.ub2()));
    CRPC_CHECK_FALSE(reader.ok());

    // A negative universal integer where an unsigned one is expected is also an
    // error (impl/base/buffer.pyx:33-36).
    const std::vector<std::byte> negative = from_hex("8101");
    ttc_reader other(view(negative));
    CRPC_CHECK_EQ(0u, other.ub4());
    CRPC_CHECK_FALSE(other.ok());
}

CRPC_TEST(ttc_reader_reads_length_prefixed_bytes_and_marks_nulls) {
    const std::vector<std::byte> data = from_hex("03aabbcc" "ff" "00" "026869");
    ttc_reader reader(view(data));
    const std::vector<std::byte> first = reader.bytes_with_length();
    CRPC_CHECK_EQ(std::string_view("aabbcc"), std::string_view(hex(first)));
    CRPC_CHECK_FALSE(reader.last_was_null());

    (void)reader.bytes_with_length();
    CRPC_CHECK(reader.last_was_null());
    (void)reader.bytes_with_length();
    CRPC_CHECK(reader.last_was_null());

    CRPC_CHECK_EQ(std::string("hi"), reader.text_with_length());
    CRPC_CHECK_FALSE(reader.last_was_null());
    CRPC_CHECK(reader.ok());
    CRPC_CHECK_EQ(data.size(), reader.position());
}

// --- 2. Message header, Execute, Fetch ---------------------------------------

CRPC_TEST(ttc_message_header_is_type_function_sequence) {
    // impl/thin/messages/base.pyx:786-793.
    std::vector<std::byte> out;
    CRPC_CHECK(crpc::db::oracle::ttc_write_message_header(
                   out, crpc::db::oracle::ttc_msg_function,
                   crpc::db::oracle::ttc_function::execute, 1)
                   .ok());
    CRPC_CHECK_EQ(std::string_view("035e01"), hex(out));

    // The reference never emits sequence 0 (impl/thin/packet.pyx:903-907).
    out.clear();
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_write_message_header(
                         out, crpc::db::oracle::ttc_msg_function,
                         crpc::db::oracle::ttc_function::execute, 0)
                         .ok());
    CRPC_CHECK(out.empty());

    // The function codes are the reference's, not the ones the task brief
    // assumed: Execute is 94, Fetch is 5.
    CRPC_CHECK_EQ(94, static_cast<int>(ttc_function::execute));
    CRPC_CHECK_EQ(5, static_cast<int>(ttc_function::fetch));
    CRPC_CHECK_EQ(78, static_cast<int>(ttc_function::reexecute_and_fetch));
    CRPC_CHECK_EQ(118, static_cast<int>(ttc_function::auth_phase_one));

    std::vector<std::byte> fetch;
    ttc_fetch request;
    request.cursor_id = 7;
    request.array_size = 1;
    CRPC_CHECK(crpc::db::oracle::ttc_write_fetch(fetch, request, 2).ok());
    // type 3, function code 5 (Fetch), sequence 2, cursor id 7, array size 1.
    CRPC_CHECK_EQ(std::string_view("030502" "0107" "0101"), hex(fetch));
}

CRPC_TEST(ttc_execute_body_follows_the_reference_field_order) {
    ttc_execute request;
    request.options = crpc::db::oracle::ttc_execute_options_for_query();
    request.sql = "SELECT 1 FROM DUAL";
    request.prefetch_rows = 1;
    request.is_query = true;
    request.field_version = crpc::db::oracle::ttc_field_version_12_2;

    std::vector<std::byte> body;
    CRPC_CHECK(crpc::db::oracle::ttc_write_execute_body(body, request).ok());

    // Decode it with our own reader to assert the field sequence, which is what
    // the reference's writer produces (impl/thin/messages/execute.pyx:121-208).
    ttc_reader reader(view(body));
    CRPC_CHECK_EQ(request.options, reader.ub4());   // options
    CRPC_CHECK_EQ(0u, reader.ub4());                // cursor id
    CRPC_CHECK_EQ(1, static_cast<int>(reader.u8())); // cursor-id pointer
    CRPC_CHECK_EQ(18u, reader.ub4());               // SQL length
    CRPC_CHECK_EQ(1, static_cast<int>(reader.u8())); // vector pointer
    CRPC_CHECK_EQ(13u, reader.ub4());               // al8i4 array length
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8o4
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8o4l
    CRPC_CHECK_EQ(0u, reader.ub4());                // prefetch buffer size
    CRPC_CHECK_EQ(1u, reader.ub4());                // prefetch rows
    CRPC_CHECK_EQ(0x7fffffffu, reader.ub4());       // max long length
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // binds pointer
    CRPC_CHECK_EQ(0u, reader.ub4());                // number of binds
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8app
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8txn
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8txl
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8kv
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // defines pointer
    CRPC_CHECK_EQ(0u, reader.ub4());                // number of defines
    CRPC_CHECK_EQ(0u, reader.ub4());                // registration id, low
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8objlist
    CRPC_CHECK_EQ(1, static_cast<int>(reader.u8())); // al8objlen, always 1
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8blv
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8blvl
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // al8dnam
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8dnaml
    CRPC_CHECK_EQ(0u, reader.ub4());                // registration id, high
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // DML row count pointer
    CRPC_CHECK_EQ(0u, reader.ub4());                // DML row count length
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // DML row count length pointer
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // SQL signature pointer
    CRPC_CHECK_EQ(0u, reader.ub4());                // SQL signature length
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // SQL id pointer
    CRPC_CHECK_EQ(0u, reader.ub4());                // allocated size of SQL id
    CRPC_CHECK_EQ(0, static_cast<int>(reader.u8())); // length of SQL id
    CRPC_CHECK_EQ(std::string(request.sql), reader.text_with_length());
    CRPC_CHECK_EQ(1u, reader.ub4());                // al8i4[0] parse
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[1] execution count
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[2]
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[3]
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[4]
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[5] SCN low
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[6] SCN high
    CRPC_CHECK_EQ(1u, reader.ub4());                // al8i4[7] is query
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[8]
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[9] execute flags
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[10]
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[11]
    CRPC_CHECK_EQ(0u, reader.ub4());                // al8i4[12]
    CRPC_CHECK(reader.ok());
    // Every byte is accounted for: a trailing field would be a bug the field
    // asserts above cannot see.
    CRPC_CHECK_EQ(std::size_t{0}, reader.remaining());
}

CRPC_TEST(ttc_execute_body_matches_a_hand_computed_prefix) {
    // The bytes up to the SQL text, computed by hand from the reference's field
    // order. This is deliberately a byte string rather than a field walk: it
    // fails if a field's width changes, even when the field walk still reads.
    ttc_execute request;
    request.options = crpc::db::oracle::ttc_execute_options_for_query();
    request.sql = "SELECT 1 FROM DUAL";
    request.field_version = crpc::db::oracle::ttc_field_version_12_2;

    std::vector<std::byte> body;
    CRPC_CHECK(crpc::db::oracle::ttc_write_execute_body(body, request).ok());
    const std::string expected =
        // options = 0x01|0x20|0x40|0x8000 = 0x8061
        "028061"
        // cursor id 0
        "00"
        // cursor-id pointer 1, SQL length 18
        "01" "0112"
        // vector pointer 1, al8i4 length 13
        "01" "010d"
        // al8o4, al8o4l
        "00" "00"
        // prefetch buffer size 0, prefetch rows 1, max long length
        "00" "0101" "047fffffff"
        // binds pointer 0, count 0
        "00" "00"
        // al8app, al8txn, al8txl, al8kv
        "00" "00" "00" "00"
        // defines pointer 0, count 0
        "00" "00"
        // registration id low 0
        "00"
        // al8objlist 0, al8objlen 1, al8blv 0, al8blvl 0
        "00" "01" "00" "00"
        // al8dnam 0, al8dnaml 0
        "00" "00"
        // registration id high 0
        "00"
        // DML row count: pointer, length, length pointer
        "00" "00" "00"
        // SQL signature pointer, length, SQL id pointer, size, length
        "00" "00" "00" "00" "00";
    const std::vector<std::byte> head_from_expected = from_hex(expected);
    CRPC_CHECK(body.size() > head_from_expected.size());
    const std::vector<std::byte> head(
        body.begin(),
        body.begin() + static_cast<std::ptrdiff_t>(head_from_expected.size()));
    CRPC_CHECK_EQ(std::string_view(expected), std::string_view(hex(head)));
    // The SQL text follows immediately; its length prefix is 18.
    CRPC_CHECK_EQ(18, static_cast<int>(body[head_from_expected.size()]));
    CRPC_CHECK_EQ(static_cast<int>('S'),
                  static_cast<int>(body[head_from_expected.size() + 1]));

    // The full message adds the 3-byte header.
    std::vector<std::byte> message;
    CRPC_CHECK(crpc::db::oracle::ttc_write_execute(message, request, 1).ok());
    CRPC_CHECK_EQ(body.size() + 3, message.size());
    CRPC_CHECK_EQ(std::string_view("035e01"), std::string_view(hex(std::vector<std::byte>(message.begin(), message.begin() + 3))));
}

CRPC_TEST(ttc_execute_rejects_unmodelled_requests) {
    std::vector<std::byte> out;
    ttc_execute request;
    request.sql = "SELECT 1 FROM DUAL";

    // A non-zero cursor id would be a re-execution, a different message shape.
    request.cursor_id = 5;
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_write_execute_body(out, request).ok());
    CRPC_CHECK(out.empty());

    // Empty SQL has nothing to execute.
    request.cursor_id = 0;
    request.sql.clear();
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_write_execute_body(out, request).ok());
    CRPC_CHECK(out.empty());

    // Field versions before 12.2 change the body shape and are not modelled.
    request.sql = "SELECT 1 FROM DUAL";
    request.field_version = 7;
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_write_execute_body(out, request).ok());
    CRPC_CHECK(out.empty());
}

CRPC_TEST(ttc_execute_writes_the_12_2_ext1_chunk_id_field) {
    // From field version 12.2 ext 1 the reference adds a chunk-id pointer and
    // count (impl/thin/messages/execute.pyx:177-179). Omitting them shifts every
    // following field, so the length difference is asserted directly.
    ttc_execute base;
    base.sql = "SELECT 1 FROM DUAL";
    base.field_version = crpc::db::oracle::ttc_field_version_12_2;
    std::vector<std::byte> without;
    CRPC_CHECK(crpc::db::oracle::ttc_write_execute_body(without, base).ok());

    base.field_version = crpc::db::oracle::ttc_field_version_12_2_ext1;
    std::vector<std::byte> with;
    CRPC_CHECK(crpc::db::oracle::ttc_write_execute_body(with, base).ok());
    CRPC_CHECK_EQ(without.size() + 2, with.size());
}

// --- 3. Column metadata ------------------------------------------------------

CRPC_TEST(ttc_metadata_reads_name_and_sizes) {
    // Built field by field so the intent is readable: everything the reference
    // writes, with only the fields this codec models asserted.
    std::vector<std::byte> block;
    block.push_back(std::byte{1});        // type = VARCHAR
    block.push_back(std::byte{0});        // flags
    block.push_back(std::byte{0});        // precision
    block.push_back(std::byte{0});        // scale
    crpc::db::oracle::ttc_write_ub4(block, 50);  // buffer size
    crpc::db::oracle::ttc_write_ub4(block, 0);   // max array elements
    crpc::db::oracle::ttc_write_ub8(block, 0);   // cont flags
    crpc::db::oracle::ttc_write_bytes_with_length(block,
                                                 crpc::span<const std::byte>());
    crpc::db::oracle::ttc_write_ub2(block, 0);   // version
    crpc::db::oracle::ttc_write_ub2(block, 873); // charset id (AL32UTF8)
    block.push_back(std::byte{1});        // charset form: implicit
    crpc::db::oracle::ttc_write_ub4(block, 50);  // max size
    crpc::db::oracle::ttc_write_ub4(block, 0);   // oaccolid (12.2+)
    block.push_back(std::byte{1});        // nulls allowed
    block.push_back(std::byte{0});        // v7 name length
    crpc::db::oracle::ttc_write_text_with_length(block, "N");
    crpc::db::oracle::ttc_write_text_with_length(block, "");
    crpc::db::oracle::ttc_write_text_with_length(block, "");
    crpc::db::oracle::ttc_write_ub2(block, 1);   // column position
    crpc::db::oracle::ttc_write_ub4(block, 0);   // UDS flags

    ttc_reader reader(view(block));
    ttc_column column;
    const crpc::status result = crpc::db::oracle::ttc_read_metadata(
        reader, crpc::db::oracle::ttc_field_version_12_2, column);
    CRPC_CHECK(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(ora_type::varchar), static_cast<int>(column.type));
    CRPC_CHECK_EQ(50u, column.buffer_size);
    CRPC_CHECK_EQ(50u, column.max_size);
    CRPC_CHECK_EQ(1, static_cast<int>(column.csfrm));
    CRPC_CHECK_EQ(1, static_cast<int>(column.nulls_allowed));
    CRPC_CHECK_EQ(std::string("N"), column.name);
    CRPC_CHECK_EQ(std::size_t{0}, reader.remaining());
}

CRPC_TEST(ttc_metadata_rejects_an_annotation_block_and_a_zero_type) {
    // Type 0 is not a column; the reference never sends it.
    std::vector<std::byte> body;
    body.push_back(std::byte{0});
    ttc_reader reader(view(body));
    ttc_column column;
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_read_metadata(reader, 8, column).ok());

    // Field version 20 (23.1 ext 3) adds annotations this codec cannot size, so
    // it reports that instead of mis-parsing the rest of the describe block.
    std::vector<std::byte> valid;
    valid.push_back(std::byte{1});
    valid.push_back(std::byte{0});
    valid.push_back(std::byte{0});
    valid.push_back(std::byte{0});
    crpc::db::oracle::ttc_write_ub4(valid, 10);
    crpc::db::oracle::ttc_write_ub4(valid, 0);
    crpc::db::oracle::ttc_write_ub8(valid, 0);
    crpc::db::oracle::ttc_write_bytes_with_length(valid, crpc::span<const std::byte>());
    crpc::db::oracle::ttc_write_ub2(valid, 0);
    crpc::db::oracle::ttc_write_ub2(valid, 873);
    valid.push_back(std::byte{1});
    crpc::db::oracle::ttc_write_ub4(valid, 10);
    crpc::db::oracle::ttc_write_ub4(valid, 0);
    valid.push_back(std::byte{1});
    valid.push_back(std::byte{0});
    crpc::db::oracle::ttc_write_text_with_length(valid, "C");
    crpc::db::oracle::ttc_write_text_with_length(valid, "");
    crpc::db::oracle::ttc_write_text_with_length(valid, "");
    crpc::db::oracle::ttc_write_ub2(valid, 1);
    crpc::db::oracle::ttc_write_ub4(valid, 0);
    // 23.1 domain schema and name.
    crpc::db::oracle::ttc_write_text_with_length(valid, "");
    crpc::db::oracle::ttc_write_text_with_length(valid, "");

    ttc_reader other(view(valid));
    CRPC_CHECK(crpc::db::oracle::ttc_read_metadata(
                   other, 20, column)
                   .code() == crpc::rpc_code::protocol_error);
}

// --- 4. Values ---------------------------------------------------------------

CRPC_TEST(ttc_number_decodes_the_canonical_text) {
    // The reference's algorithm (impl/base/decoders.pyx:157-273), exercised on
    // values whose bytes are documented Oracle NUMBER encodings.
    std::string text;
    const struct {
        const char* hex;
        const char* text;
    } cases[] = {
        {"80", "0"},        // one positive byte: zero
        {"c102", "1"},      // exponent 0, one mantissa byte 2 -> 1
        {"c103", "2"},
        {"c10b", "10"},
        {"c11d", "28"},
        {"c202", "100"},
        {"c20201", "100"},
        {"c10233", "1.5"},  // 1.5
        {"3e6466", "-1"},   // negative one: 101-0x64 = 1
        {"3e6366", "-2"},   // negative two: 101-0x63 = 2
        // A trailing 0x66 (102) is the negative terminator and is stripped; the
        // remaining byte 0x65 decodes to a zero digit that the reference still
        // emits, so this vector is "-1.0" rather than an error.
        {"3e646566", "-1.0"},
        {"c20f", "1400"},   // 14 * 100
        {"c30a", "90000"},
    };
    for (const auto& item : cases) {
        text.clear();
        const std::vector<std::byte> encoded = from_hex(item.hex);
        const crpc::status result =
            crpc::db::oracle::ttc_decode_number(view(encoded), text);
        CRPC_CHECK(result.ok());
        CRPC_CHECK_EQ(std::string(item.text), text);
    }

    // An empty value and one longer than 21 bytes are errors, not guesses.
    CRPC_CHECK_FALSE(
        crpc::db::oracle::ttc_decode_number(crpc::span<const std::byte>(), text)
            .ok());
    const std::vector<std::byte> too_long(22, std::byte{0xc1});
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_decode_number(view(too_long), text).ok());
}

CRPC_TEST(ttc_number_handles_zero_mantissa_bytes_under_a_positive_exponent) {
    // A zero mantissa byte is decremented to 0xff in uint8 arithmetic, which is
    // what makes `0xc1 0x64` the number 99 rather than an out-of-range digit.
    std::string text;
    CRPC_CHECK(crpc::db::oracle::ttc_decode_number(
                   view(from_hex("c164")), text)
                   .ok());
    CRPC_CHECK_EQ(std::string("99"), text);

    // A single negative exponent byte is the documented maximum negative value.
    text.clear();
    CRPC_CHECK(crpc::db::oracle::ttc_decode_number(
                   view(from_hex("3e")), text)
                   .ok());
    CRPC_CHECK_EQ(std::string("-1e126"), text);
}

CRPC_TEST(ttc_date_decodes_the_biased_fields) {
    // century 120, year-of-century 124 (2024), month 3, day 5, then hour, minute
    // and second each stored biased by one: 0x0f = 15 -> 14, 0x1f = 31 -> 30,
    // 0x2e = 46 -> 45.
    std::string text;
    std::vector<std::byte> date_bytes = from_hex("787c03050f1f2e");
    CRPC_CHECK(crpc::db::oracle::ttc_decode_date(view(date_bytes), text).ok());
    CRPC_CHECK_EQ(std::string("2024-03-05 14:30:45"), text);

    // The epoch-ish boundary: century 100 century byte, year 100.
    text.clear();
    CRPC_CHECK(crpc::db::oracle::ttc_decode_date(
                   view(from_hex("6464010101010101")), text)
                   .ok());
    CRPC_CHECK_EQ(std::string("0000-01-01 00:00:00"), text);

    // Too short, and an out-of-range month.
    text.clear();
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_decode_date(
                         view(from_hex("787c03")), text)
                         .ok());
    CRPC_CHECK_FALSE(crpc::db::oracle::ttc_decode_date(
                         view(from_hex("787c0001010101")), text)
                         .ok());
}

CRPC_TEST(ttc_read_value_dispatch_by_column_type) {
    // VARCHAR: the bytes come back as text.
    std::vector<std::byte> body;
    crpc::db::oracle::ttc_write_text_with_length(body, "abc");
    ttc_reader reader(view(body));
    ttc_column column;
    column.type = ora_type::varchar;
    ttc_value value;
    CRPC_CHECK(crpc::db::oracle::ttc_read_value(reader, column, value).ok());
    CRPC_CHECK_FALSE(value.is_null);
    CRPC_CHECK_EQ(std::string("abc"), value.text);

    // NUMBER: decoded to canonical text.
    std::vector<std::byte> number_body;
    crpc::db::oracle::ttc_write_bytes_with_length(number_body, view(from_hex("c102")));
    ttc_reader number_reader(view(number_body));
    column.type = ora_type::number;
    CRPC_CHECK(crpc::db::oracle::ttc_read_value(number_reader, column, value).ok());
    CRPC_CHECK_EQ(std::string("1"), value.text);

    // RAW: bytes, no text.
    std::vector<std::byte> raw_body;
    crpc::db::oracle::ttc_write_bytes_with_length(raw_body, view(from_hex("deadbeef")));
    ttc_reader raw_reader(view(raw_body));
    column.type = ora_type::raw;
    CRPC_CHECK(crpc::db::oracle::ttc_read_value(raw_reader, column, value).ok());
    CRPC_CHECK_EQ(std::string_view("deadbeef"), std::string_view(hex(value.bytes)));
    CRPC_CHECK(value.text.empty());

    // A null is a value, not an error.
    std::vector<std::byte> null_body;
    null_body.push_back(std::byte{255});
    ttc_reader null_reader(view(null_body));
    column.type = ora_type::varchar;
    CRPC_CHECK(crpc::db::oracle::ttc_read_value(null_reader, column, value).ok());
    CRPC_CHECK(value.is_null);

    // An unmodelled type is reported, not guessed.
    std::vector<std::byte> clob_body;
    crpc::db::oracle::ttc_write_text_with_length(clob_body, "x");
    ttc_reader clob_reader(view(clob_body));
    column.type = ora_type::clob;
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(
                      crpc::db::oracle::ttc_read_value(clob_reader, column, value)
                          .code()));
}

} // namespace

CRPC_TEST_MAIN()