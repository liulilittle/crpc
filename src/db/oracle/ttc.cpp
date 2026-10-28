// CRPC -- Oracle TTC message codec. See ttc.hpp for scope and provenance.
#include <crpc/db/oracle/ttc.hpp>

#include <cstring>
#include <string>

namespace crpc {
namespace db {
namespace oracle {
namespace {

constexpr std::byte byte_of(std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

std::uint8_t u8_of(std::byte value) noexcept {
    return static_cast<std::uint8_t>(value);
}

// The length byte of a universal integer, with bit 0x80 carrying the sign
// (impl/base/buffer.pyx:666-684 writes it, 302-317 reads it).
std::uint8_t integer_length(std::uint64_t magnitude) noexcept {
    if (magnitude == 0) {
        return 0;
    }
    if (magnitude <= 0xffu) {
        return 1;
    }
    if (magnitude <= 0xffffu) {
        return 2;
    }
    if (magnitude <= 0xffffffffu) {
        return 4;
    }
    return 8;
}

void put_magnitude(std::vector<std::byte>& out, std::uint64_t magnitude,
                   std::uint8_t length) {
    for (std::uint8_t i = length; i > 0; --i) {
        const unsigned shift = static_cast<unsigned>(i - 1) * 8u;
        out.push_back(byte_of(static_cast<std::uint8_t>((magnitude >> shift) & 0xffu)));
    }
}

void put_universal(std::vector<std::byte>& out, std::uint64_t magnitude,
                   bool negative) {
    std::uint8_t length = integer_length(magnitude);
    std::uint8_t prefix = length;
    if (negative) {
        prefix = static_cast<std::uint8_t>(prefix | 0x80u);
    }
    out.push_back(byte_of(prefix));
    if (length > 0) {
        put_magnitude(out, magnitude, length);
    }
}

std::uint64_t decode_magnitude(span<const std::byte> bytes) noexcept {
    std::uint64_t value = 0;
    for (std::byte b : bytes) {
        value = (value << 8) | u8_of(b);
    }
    return value;
}

} // namespace

// --- Universal integers ------------------------------------------------------

void ttc_write_ub2(std::vector<std::byte>& out, std::uint16_t value) {
    put_universal(out, value, false);
}

void ttc_write_ub4(std::vector<std::byte>& out, std::uint32_t value) {
    put_universal(out, value, false);
}

void ttc_write_ub8(std::vector<std::byte>& out, std::uint64_t value) {
    put_universal(out, value, false);
}

void ttc_write_sb4(std::vector<std::byte>& out, std::int32_t value) {
    // Negative magnitudes are written without a sign so the 0x80 length bit is
    // unambiguous; INT32_MIN has no positive counterpart in 32 bits, so it uses
    // a 64-bit magnitude.
    const bool negative = value < 0;
    const auto magnitude = negative
                               ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(value))
                               : static_cast<std::uint64_t>(value);
    put_universal(out, magnitude, negative);
}

void ttc_write_bytes_with_length(std::vector<std::byte>& out,
                                 span<const std::byte> value) {
    if (value.size() <= ttc_max_short_length) {
        out.push_back(byte_of(static_cast<std::uint8_t>(value.size())));
        for (std::byte b : value) {
            out.push_back(b);
        }
        return;
    }
    // The chunked form: 254, then ub4 length + bytes until a zero length.
    out.push_back(byte_of(ttc_long_length_indicator));
    std::size_t offset = 0;
    while (offset < value.size()) {
        const std::size_t count =
            value.size() - offset > ttc_chunk_size ? ttc_chunk_size
                                                   : value.size() - offset;
        ttc_write_ub4(out, static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(value[offset + i]);
        }
        offset += count;
    }
    ttc_write_ub4(out, 0);
}

void ttc_write_text_with_length(std::vector<std::byte>& out, std::string_view value) {
    std::vector<std::byte> bytes;
    bytes.reserve(value.size());
    for (char c : value) {
        bytes.push_back(
            byte_of(static_cast<std::uint8_t>(static_cast<unsigned char>(c))));
    }
    ttc_write_bytes_with_length(out, span<const std::byte>(bytes.data(), bytes.size()));
}

status ttc_write_message_header(std::vector<std::byte>& out,
                                std::uint8_t message_type, ttc_function function,
                                std::uint8_t sequence) {
    // The reference bumps the sequence number and skips zero
    // (impl/thin/packet.pyx:903-907), so a caller that passes 0 is asking for a
    // header that cannot occur.
    if (sequence == 0) {
        return status{rpc_code::invalid_argument,
                      "TTC sequence number 0 never occurs on the wire"};
    }
    out.push_back(byte_of(message_type));
    out.push_back(byte_of(static_cast<std::uint8_t>(function)));
    out.push_back(byte_of(sequence));
    return status{};
}

// --- Execute and Fetch -------------------------------------------------------

std::uint32_t ttc_execute_options_for_query() noexcept {
    return ttc_exec_option_parse | ttc_exec_option_execute | ttc_exec_option_fetch |
           ttc_exec_option_not_plsql;
}

status ttc_write_execute_body(std::vector<std::byte>& out,
                              const ttc_execute& request) {
    if (request.field_version < ttc_field_version_12_2) {
        return status{rpc_code::invalid_argument,
                      "field versions below 12.2 are not modelled"};
    }
    if (request.cursor_id != 0) {
        return status{rpc_code::invalid_argument,
                      "only a first execution (cursor id 0) is modelled"};
    }
    if (request.sql.empty()) {
        return status{rpc_code::invalid_argument, "Execute needs the SQL text"};
    }
    if (request.sql.size() > 0xffffffffu) {
        return status{rpc_code::invalid_argument, "SQL text is too long"};
    }

    // impl/thin/messages/execute.pyx:121-208, in the reference's order.
    ttc_write_ub4(out, request.options);
    ttc_write_ub4(out, request.cursor_id);
    // cursor id pointer, then the SQL length: written because the id is 0.
    out.push_back(byte_of(1));
    ttc_write_ub4(out, static_cast<std::uint32_t>(request.sql.size()));
    // vector pointer and the length of the al8i4 array.
    out.push_back(byte_of(1));
    ttc_write_ub4(out, ttc_al8i4_length);
    // al8o4, al8o4l
    out.push_back(byte_of(0));
    out.push_back(byte_of(0));
    ttc_write_ub4(out, 0); // prefetch buffer size
    ttc_write_ub4(out, request.prefetch_rows);
    ttc_write_ub4(out, ttc_max_long_length);
    // No binds: pointer 0 and count 0.
    out.push_back(byte_of(0));
    ttc_write_ub4(out, 0);
    // al8app, al8txn, al8txl, al8kv
    out.push_back(byte_of(0));
    out.push_back(byte_of(0));
    out.push_back(byte_of(0));
    out.push_back(byte_of(0));
    // Defines: pointer and count.
    if (request.num_defines > 0) {
        out.push_back(byte_of(1));
        ttc_write_ub4(out, request.num_defines);
    } else {
        out.push_back(byte_of(0));
        ttc_write_ub4(out, 0);
    }
    ttc_write_ub4(out, 0); // registration id, low half
    out.push_back(byte_of(0)); // al8objlist
    out.push_back(byte_of(1)); // al8objlen: the reference always sets this
    out.push_back(byte_of(0)); // al8blv
    ttc_write_ub4(out, 0);     // al8blvl
    out.push_back(byte_of(0)); // al8dnam
    ttc_write_ub4(out, 0);     // al8dnaml
    ttc_write_ub4(out, 0);     // registration id, high half
    // DML row-count pointers.
    out.push_back(byte_of(0));
    ttc_write_ub4(out, 0);
    out.push_back(byte_of(0));
    // SQL signature / SQL id pointers (field version 12.2+).
    out.push_back(byte_of(0));
    ttc_write_ub4(out, 0);
    out.push_back(byte_of(0));
    ttc_write_ub4(out, 0);
    out.push_back(byte_of(0));
    if (request.field_version >= ttc_field_version_12_2_ext1) {
        // chunk-id pointer and count (12.2 ext 1+).
        out.push_back(byte_of(0));
        ttc_write_ub4(out, 0);
    }
    // The SQL text, length-prefixed.
    ttc_write_text_with_length(out, request.sql);
    // al8i4: parse, execution count, then the fixed zeros and the flags.
    ttc_write_ub4(out, 1); // al8i4[0] parse
    ttc_write_ub4(out, request.is_query ? 0u : request.num_iters); // al8i4[1]
    ttc_write_ub4(out, 0); // al8i4[2]
    ttc_write_ub4(out, 0); // al8i4[3]
    ttc_write_ub4(out, 0); // al8i4[4]
    ttc_write_ub4(out, 0); // al8i4[5] SCN, low
    ttc_write_ub4(out, 0); // al8i4[6] SCN, high
    ttc_write_ub4(out, request.is_query ? 1u : 0u); // al8i4[7] is query
    ttc_write_ub4(out, 0); // al8i4[8]
    ttc_write_ub4(out, request.execute_flags); // al8i4[9]
    ttc_write_ub4(out, 0); // al8i4[10] fetch orientation
    ttc_write_ub4(out, 0); // al8i4[11] fetch position
    ttc_write_ub4(out, 0); // al8i4[12]
    return status{};
}

status ttc_write_execute(std::vector<std::byte>& out, const ttc_execute& request,
                         std::uint8_t sequence) {
    const status header =
        ttc_write_message_header(out, ttc_msg_function, ttc_function::execute, sequence);
    if (!header.ok()) {
        return header;
    }
    return ttc_write_execute_body(out, request);
}

status ttc_write_fetch(std::vector<std::byte>& out, const ttc_fetch& request,
                       std::uint8_t sequence) {
    const status header =
        ttc_write_message_header(out, ttc_msg_function, ttc_function::fetch, sequence);
    if (!header.ok()) {
        return header;
    }
    ttc_write_ub4(out, request.cursor_id);
    ttc_write_ub4(out, request.array_size);
    return status{};
}

// --- Reader ------------------------------------------------------------------

bool ttc_reader::need(std::size_t count) noexcept {
    if (!error_.ok()) {
        return false;
    }
    if (offset_ > data_.size() || data_.size() - offset_ < count) {
        error_ = status{rpc_code::protocol_error, "TTC body ended early"};
        return false;
    }
    return true;
}

std::size_t ttc_reader::remaining() const noexcept {
    if (offset_ > data_.size()) {
        return 0;
    }
    return data_.size() - offset_;
}

void ttc_reader::skip(std::size_t count) noexcept {
    if (need(count)) {
        offset_ += count;
    }
}

void ttc_reader::fail(rpc_code code, const char* message) noexcept {
    if (error_.ok()) {
        error_ = status{code, message};
    }
}

std::uint8_t ttc_reader::u8() noexcept {
    if (!need(1)) {
        return 0;
    }
    return u8_of(data_[offset_++]);
}

std::uint8_t ttc_reader::length_byte(bool& negative) noexcept {
    const std::uint8_t value = u8();
    negative = (value & 0x80u) != 0;
    return static_cast<std::uint8_t>(value & 0x7fu);
}

std::uint16_t ttc_reader::ub2() noexcept {
    bool negative = false;
    const std::uint8_t length = length_byte(negative);
    if (length > 2) {
        fail(rpc_code::protocol_error, "ub2 length exceeds 2");
        return 0;
    }
    if (negative) {
        // The reference raises on a negative unsigned integer
        // (impl/base/buffer.pyx:33-36).
        fail(rpc_code::protocol_error, "negative value where ub2 was expected");
        return 0;
    }
    if (!need(length)) {
        return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(
        decode_magnitude(span<const std::byte>(data_.data() + offset_, length)));
    offset_ += length;
    return value;
}

std::uint32_t ttc_reader::ub4() noexcept {
    bool negative = false;
    const std::uint8_t length = length_byte(negative);
    if (length > 4) {
        fail(rpc_code::protocol_error, "ub4 length exceeds 4");
        return 0;
    }
    if (negative) {
        fail(rpc_code::protocol_error, "negative value where ub4 was expected");
        return 0;
    }
    if (!need(length)) {
        return 0;
    }
    const auto value = static_cast<std::uint32_t>(
        decode_magnitude(span<const std::byte>(data_.data() + offset_, length)));
    offset_ += length;
    return value;
}

std::uint64_t ttc_reader::ub8() noexcept {
    bool negative = false;
    const std::uint8_t length = length_byte(negative);
    if (length > 8) {
        fail(rpc_code::protocol_error, "ub8 length exceeds 8");
        return 0;
    }
    if (negative) {
        fail(rpc_code::protocol_error, "negative value where ub8 was expected");
        return 0;
    }
    if (!need(length)) {
        return 0;
    }
    const std::uint64_t value =
        decode_magnitude(span<const std::byte>(data_.data() + offset_, length));
    offset_ += length;
    return value;
}

std::int32_t ttc_reader::sb4() noexcept {
    bool negative = false;
    const std::uint8_t length = length_byte(negative);
    if (length > 4) {
        fail(rpc_code::protocol_error, "sb4 length exceeds 4");
        return 0;
    }
    if (!need(length)) {
        return 0;
    }
    const auto magnitude = static_cast<std::int64_t>(
        decode_magnitude(span<const std::byte>(data_.data() + offset_, length)));
    offset_ += length;
    return static_cast<std::int32_t>(negative ? -magnitude : magnitude);
}

std::uint16_t ttc_reader::u16be() noexcept {
    if (!need(2)) {
        return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(u8_of(data_[offset_])) << 8) |
        static_cast<std::uint16_t>(u8_of(data_[offset_ + 1])));
    offset_ += 2;
    return value;
}

std::uint32_t ttc_reader::u32be() noexcept {
    if (!need(4)) {
        return 0;
    }
    const std::uint32_t value =
        (static_cast<std::uint32_t>(u8_of(data_[offset_])) << 24) |
        (static_cast<std::uint32_t>(u8_of(data_[offset_ + 1])) << 16) |
        (static_cast<std::uint32_t>(u8_of(data_[offset_ + 2])) << 8) |
        static_cast<std::uint32_t>(u8_of(data_[offset_ + 3]));
    offset_ += 4;
    return value;
}

std::vector<std::byte> ttc_reader::bytes_with_length() {
    null_ = false;
    std::vector<std::byte> out;
    const std::uint8_t length = u8();
    if (!error_.ok()) {
        return out;
    }
    if (length == 0 || length == ttc_null_length_indicator) {
        null_ = true;
        return out;
    }
    if (length == ttc_long_length_indicator) {
        // Chunked: repeated ub4 length + bytes, terminated by ub4 0
        // (impl/base/packet.pyx:184-201 equivalent in the read buffer).
        for (;;) {
            const std::uint32_t chunk = ub4();
            if (!error_.ok() || chunk == 0) {
                break;
            }
            if (!need(chunk)) {
                break;
            }
            for (std::uint32_t i = 0; i < chunk; ++i) {
                out.push_back(data_[offset_ + i]);
            }
            offset_ += chunk;
        }
        return out;
    }
    if (!need(length)) {
        return out;
    }
    out.reserve(length);
    for (std::uint8_t i = 0; i < length; ++i) {
        out.push_back(data_[offset_ + i]);
    }
    offset_ += length;
    return out;
}

std::string ttc_reader::text_with_length() {
    const std::vector<std::byte> bytes = bytes_with_length();
    if (null_ || !error_.ok()) {
        return std::string();
    }
    std::string out;
    out.reserve(bytes.size());
    for (std::byte b : bytes) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    }
    return out;
}

// --- Metadata ----------------------------------------------------------------

status ttc_read_metadata(ttc_reader& reader, std::uint8_t field_version,
                         ttc_column& out) {
    const std::uint16_t type_number = reader.u8();
    if (!reader.ok()) {
        return reader.error();
    }
    if (type_number == 0) {
        return status{rpc_code::protocol_error, "column type 0 is invalid"};
    }
    out.type = static_cast<ora_type>(type_number);
    (void)reader.u8(); // flags, not modelled
    out.precision = static_cast<std::int8_t>(reader.u8());
    out.scale = static_cast<std::int8_t>(reader.u8());
    out.buffer_size = reader.ub4();
    (void)reader.ub4(); // maximum array elements
    (void)reader.ub8(); // cont flags
    (void)reader.bytes_with_length(); // OID
    (void)reader.ub2(); // version
    (void)reader.ub2(); // character set id
    out.csfrm = reader.u8();
    out.max_size = reader.ub4();
    if (field_version >= ttc_field_version_12_2) {
        (void)reader.ub4(); // oaccolid
    }
    out.nulls_allowed = reader.u8();
    (void)reader.u8(); // v7 name length
    out.name = reader.text_with_length();
    out.schema = reader.text_with_length();
    out.type_name = reader.text_with_length();
    (void)reader.ub2(); // column position
    (void)reader.ub4(); // UDS flags
    if (!reader.ok()) {
        return reader.error();
    }
    if (field_version >= 17) {
        // 23.1 adds a domain schema and name (base.pyx:368-370).
        (void)reader.text_with_length();
        (void)reader.text_with_length();
        if (!reader.ok()) {
            return reader.error();
        }
    }
    if (field_version >= 20) {
        // 23.1 ext 3 adds annotations; this codec does not model them and says
        // so rather than skipping a variable-size block it cannot size.
        return status{rpc_code::protocol_error,
                      "column annotations (field version 23.1 ext 3) are not modelled"};
    }
    return status{};
}

// --- Values ------------------------------------------------------------------

status ttc_read_value(ttc_reader& reader, const ttc_column& column,
                      ttc_value& out) {
    out.is_null = false;
    out.text.clear();
    out.bytes.clear();
    const std::vector<std::byte> bytes = reader.bytes_with_length();
    if (!reader.ok()) {
        return reader.error();
    }
    if (reader.last_was_null()) {
        out.is_null = true;
        return status{};
    }
    const span<const std::byte> view(bytes.data(), bytes.size());
    switch (column.type) {
    case ora_type::number:
    case ora_type::binary_integer:
        return ttc_decode_number(view, out.text);
    case ora_type::date:
        return ttc_decode_date(view, out.text);
    case ora_type::varchar:
    case ora_type::char_text:
    case ora_type::long_text:
        out.text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return status{};
    case ora_type::raw:
    case ora_type::long_raw:
        out.bytes = bytes;
        return status{};
    default:
        // A type this codec does not model must be reported, not guessed: a
        // wrong width would silently misread every following column.
        return status{rpc_code::protocol_error,
                      "this column type is not modelled"};
    }
}

// --- NUMBER ------------------------------------------------------------------

status ttc_decode_number(span<const std::byte> value, std::string& text) {
    text.clear();
    // The reference rejects anything above 21 bytes (1 exponent + 20 mantissa)
    // (impl/base/decoders.pyx:172-175).
    if (value.empty()) {
        return status{rpc_code::protocol_error, "empty Oracle NUMBER"};
    }
    if (value.size() > 21) {
        return status{rpc_code::protocol_error, "Oracle NUMBER is longer than 21 bytes"};
    }
    // The exponent lives in an int8 and the arithmetic wraps in 8 bits: 0xc3 is
    // exponent 2, not -254. A negative number stores the one's complement of the
    // exponent. Both facts come straight from impl/base/decoders.pyx:178-184,
    // where `exponent` is declared `int8_t`; doing the subtraction in a wider
    // type would turn 0xc3 into a huge negative decimal point.
    const std::uint8_t raw = u8_of(value[0]);
    const bool positive = (raw & 0x80u) != 0;
    const std::uint8_t exponent_bits =
        positive ? raw : static_cast<std::uint8_t>(~raw);
    // `exponent` is an int8_t in the reference and `exponent -= 193` stores back
    // into it, so the subtraction wraps mod 256: 0xc1 -> -63 -> 0, 0xc2 -> 1.
    // Doing this in int32 would turn 0xc1 into -256 and produce 500 leading
    // zeros instead of the number 1.
    const std::uint8_t wrapped =
        static_cast<std::uint8_t>(exponent_bits - 193u);
    const std::int32_t exponent =
        wrapped > 127u ? static_cast<std::int32_t>(wrapped) - 256
                       : static_cast<std::int32_t>(wrapped);
    std::int32_t decimal_point = exponent * 2 + 2;

    if (value.size() == 1) {
        if (positive) {
            text = "0";
            return status{};
        }
        // The reference marks this as the maximum negative value (used by
        // dump); a canonical spelling is still useful to a caller.
        text = "-1e126";
        return status{};
    }

    std::size_t count = value.size();
    if (!positive && u8_of(value[count - 1]) == 102) {
        --count;
    }

    std::string digits;
    digits.reserve(40);
    for (std::size_t i = 1; i < count; ++i) {
        // Unsigned 8-bit arithmetic, exactly like the reference's uint8_t: a
        // zero byte under a positive number becomes 255, not -1.
        std::uint8_t byte = u8_of(value[i]);
        if (positive) {
            byte = static_cast<std::uint8_t>(byte - 1u);
        } else {
            byte = static_cast<std::uint8_t>(101u - byte);
        }
        std::uint8_t digit = static_cast<std::uint8_t>(byte / 10u);
        if (digit == 0 && digits.empty()) {
            --decimal_point;
        } else if (digit == 10) {
            digits.push_back('1');
            digits.push_back('0');
            ++decimal_point;
        } else if (digit != 0 || i > 0) {
            digits.push_back(static_cast<char>('0' + digit));
        }
        digit = static_cast<std::uint8_t>(byte % 10u);
        if (digit != 0 || i < count - 1) {
            digits.push_back(static_cast<char>('0' + digit));
        }
    }

    if (!positive) {
        text.push_back('-');
    }
    if (decimal_point <= 0) {
        text.append("0.");
        for (std::int32_t i = decimal_point; i < 0; ++i) {
            text.push_back('0');
        }
    }
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && static_cast<std::int32_t>(i) == decimal_point) {
            text.push_back('.');
        }
        text.push_back(digits[i]);
    }
    if (decimal_point > static_cast<std::int32_t>(digits.size())) {
        for (std::int32_t i = static_cast<std::int32_t>(digits.size());
             i < decimal_point; ++i) {
            text.push_back('0');
        }
    }
    if (text.empty()) {
        text = "0";
    }
    return status{};
}

// --- DATE --------------------------------------------------------------------

status ttc_decode_date(span<const std::byte> value, std::string& text) {
    text.clear();
    if (value.size() < 7) {
        return status{rpc_code::protocol_error, "Oracle DATE needs 7 bytes"};
    }
    // impl/base/decoders.pyx:93-98.
    const int year = (u8_of(value[0]) - 100) * 100 + u8_of(value[1]) - 100;
    const int month = u8_of(value[2]);
    const int day = u8_of(value[3]);
    const int hour = u8_of(value[4]) - 1;
    const int minute = u8_of(value[5]) - 1;
    const int second = u8_of(value[6]) - 1;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return status{rpc_code::decode_error, "Oracle DATE field is out of range"};
    }
    auto put2 = [&text](int number) {
        text.push_back(static_cast<char>('0' + (number / 10) % 10));
        text.push_back(static_cast<char>('0' + number % 10));
    };
    const int century = year / 100;
    const int year_in_century = year % 100;
    if (century < 0 || century > 99) {
        return status{rpc_code::decode_error, "Oracle DATE year is out of range"};
    }
    put2(century);
    put2(year_in_century);
    text.push_back('-');
    put2(month);
    text.push_back('-');
    put2(day);
    text.push_back(' ');
    put2(hour);
    text.push_back(':');
    put2(minute);
    text.push_back(':');
    put2(second);
    return status{};
}

} // namespace oracle
} // namespace db
} // namespace crpc