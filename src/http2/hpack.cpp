// CRPC -- HPACK implementation (RFC 7541).
#include <crpc/http2/hpack.hpp>

#include <array>
#include <cstring>
#include <limits>

namespace crpc::http2 {
namespace {

// --- Static table (RFC 7541 Appendix A) --------------------------------------

struct static_row {
    const char* name;
    const char* value;
};

const static_row static_rows[static_table_size] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

// --- String helpers ----------------------------------------------------------

void append_integer(std::string& out, u8 prefix_bits, u8 pattern, u64 value) {
    const u32 max_prefix = (1u << prefix_bits) - 1u;
    if (value < max_prefix) {
        out.push_back(static_cast<char>(pattern | static_cast<u8>(value)));
        return;
    }
    out.push_back(static_cast<char>(pattern | static_cast<u8>(max_prefix)));
    value -= max_prefix;
    while (value >= 128) {
        out.push_back(static_cast<char>(static_cast<u8>((value & 0x7Fu) | 0x80u)));
        value >>= 7;
    }
    out.push_back(static_cast<char>(static_cast<u8>(value)));
}

void append_string(std::string& out, std::string_view value) {
    append_integer(out, 7, 0x00, value.size());
    out.append(value.data(), value.size());
}

// --- Huffman tables (RFC 7541 Appendix B) ------------------------------------

// Code values are aligned to the LSB; the matching length is in huff_bits.
const u32 huff_code[257] = {
    0x1ff8, 0x7fffd8, 0xfffffe2, 0xfffffe3, 0xfffffe4, 0xfffffe5, 0xfffffe6, 0xfffffe7,
    0xfffffe8, 0xffffea, 0x3ffffffc, 0xfffffe9, 0xfffffea, 0x3ffffffd, 0xfffffeb, 0xfffffec,
    0xfffffed, 0xfffffee, 0xfffffef, 0xffffff0, 0xffffff1, 0xffffff2, 0x3ffffffe, 0xffffff3,
    0xffffff4, 0xffffff5, 0xffffff6, 0xffffff7, 0xffffff8, 0xffffff9, 0xffffffa, 0xffffffb,
    0x14, 0x3f8, 0x3f9, 0xffa, 0x1ff9, 0x15, 0xf8, 0x7fa,
    0x3fa, 0x3fb, 0xf9, 0x7fb, 0xfa, 0x16, 0x17, 0x18,
    0x0, 0x1, 0x2, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
    0x1e, 0x1f, 0x5c, 0xfb, 0x7ffc, 0x20,
    0xffb, 0x3fc, 0x1ffa, 0x21, 0x5d, 0x5e, 0x5f, 0x60,
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    0x71, 0x72, 0xfc, 0x73, 0xfd, 0x1ffb, 0x7fff0, 0x1ffc,
    0x3ffc, 0x22, 0x7ffd, 0x3, 0x23, 0x4, 0x24, 0x5,
    0x25, 0x26, 0x27, 0x6, 0x74, 0x75, 0x28, 0x29,
    0x2a, 0x7, 0x2b, 0x76, 0x2c, 0x8, 0x9, 0x2d,
    0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7ffe, 0x7fc, 0x3ffd,
    0x1ffd, 0xffffffc, 0xfffe6,
    0x3fffd2, 0xfffe7, 0xfffe8, 0x3fffd3, 0x3fffd4, 0x3fffd5, 0x7fffd9, 0x3fffd6,
    0x7fffda, 0x7fffdb, 0x7fffdc, 0x7fffdd, 0x7fffde, 0xffffeb, 0x7fffdf, 0xffffec,
    0xffffed, 0x3fffd7, 0x7fffe0, 0xffffee, 0x7fffe1, 0x7fffe2, 0x7fffe3, 0x7fffe4,
    0x1fffdc, 0x3fffd8, 0x7fffe5, 0x3fffd9, 0x7fffe6, 0x7fffe7, 0xffffef, 0x3fffda,
    0x1fffdd, 0xfffe9, 0x3fffdb, 0x3fffdc, 0x7fffe8, 0x7fffe9, 0x1fffde, 0x7fffea,
    0x3fffdd, 0x3fffde, 0xfffff0, 0x1fffdf, 0x3fffdf, 0x7fffeb, 0x7fffec, 0x1fffe0,
    0x1fffe1, 0x3fffe0, 0x1fffe2, 0x7fffed, 0x3fffe1, 0x7fffee, 0x7fffef, 0xfffea,
    0x3fffe2, 0x3fffe3, 0x3fffe4, 0x7ffff0, 0x3fffe5, 0x3fffe6, 0x7ffff1, 0x3ffffe0,
    0x3ffffe1, 0xfffeb, 0x7fff1, 0x3fffe7, 0x7ffff2, 0x3fffe8, 0x1ffffec, 0x3ffffe2,
    0x3ffffe3, 0x3ffffe4, 0x7ffffde, 0x7ffffdf, 0x3ffffe5, 0xfffff1, 0x1ffffed, 0x7fff2,
    0x1fffe3, 0x3ffffe6, 0x7ffffe0, 0x7ffffe1, 0x3ffffe7, 0x7ffffe2, 0xfffff2, 0x1fffe4,
    0x1fffe5, 0x3ffffe8, 0x3ffffe9, 0xffffffd, 0x7ffffe3, 0x7ffffe4, 0x7ffffe5, 0xfffec,
    0xfffff3, 0xfffed, 0x1fffe6, 0x3fffe9, 0x1fffe7, 0x1fffe8, 0x7ffff3, 0x3fffea,
    0x3fffeb, 0x1ffffee, 0x1ffffef, 0xfffff4, 0xfffff5, 0x3ffffea, 0x7ffff4, 0x3ffffeb,
    0x7ffffe6, 0x3ffffec, 0x3ffffed, 0x7ffffe7, 0x7ffffe8, 0x7ffffe9, 0x7ffffea, 0x7ffffeb,
    0xffffffe, 0x7ffffec, 0x7ffffed, 0x7ffffee, 0x7ffffef, 0x7fffff0, 0x3ffffee, 0x3fffffff};

const u8 huff_bits[257] = {
    13, 23, 28, 28, 28, 28, 28, 28,
    28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28,
    28, 28, 28, 28, 28, 28, 28, 28,
    6, 10, 10, 12, 13, 6, 8, 11,
    10, 10, 8, 11, 8, 6, 6, 6,
    5, 5, 5, 6, 6, 6, 6, 6,
    6, 6, 7, 8, 15, 6, 12, 10,
    13, 6, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7,
    8, 7, 8, 13, 19, 13, 14, 6,
    15, 5, 6, 5, 6, 5, 6, 6,
    6, 5, 7, 7, 6, 6, 6, 5,
    6, 7, 6, 5, 5, 6, 7, 7,
    7, 7, 7, 15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23,
    22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23,
    23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21,
    23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23,
    20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25,
    26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24,
    21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23,
    22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27,
    27, 28, 27, 27, 27, 27, 27, 26,
    30,};

struct huffman_tables {
    std::array<u16, 31> counts{};
    std::array<u32, 31> first_code{};
    std::array<u16, 257> symbols{};
    int max_len = 0;
};

const huffman_tables& get_huffman_tables() {
    static const huffman_tables tables = [] {
        huffman_tables value;
        for (std::size_t symbol = 0; symbol < 257; ++symbol) {
            const u8 length = huff_bits[symbol];
            ++value.counts[length];
            if (length > value.max_len) {
                value.max_len = length;
            }
        }
        std::size_t index = 0;
        for (int length = 1; length <= 30; ++length) {
            for (std::size_t symbol = 0; symbol < 257; ++symbol) {
                if (huff_bits[symbol] == static_cast<u8>(length)) {
                    value.symbols[index++] = static_cast<u16>(symbol);
                }
            }
        }
        u32 code = 0;
        for (int length = 1; length <= 30; ++length) {
            value.first_code[static_cast<std::size_t>(length)] = code;
            code = (code + value.counts[static_cast<std::size_t>(length)]) << 1;
        }
        return value;
    }();
    return tables;
}

const header_field* static_lookup() {
    static const std::array<header_field, static_table_size> table = [] {
        std::array<header_field, static_table_size> value;
        for (std::size_t i = 0; i < static_table_size; ++i) {
            value[i].name = static_rows[i].name;
            value[i].value = static_rows[i].value;
        }
        return value;
    }();
    return table.data();
}

} // namespace

// --- Static table access -----------------------------------------------------

const header_field* static_table_entry(std::size_t index) noexcept {
    if (index < 1 || index > static_table_size) {
        return nullptr;
    }
    return &static_lookup()[index - 1];
}

std::size_t static_find(std::string_view name, std::string_view value) noexcept {
    for (std::size_t i = 0; i < static_table_size; ++i) {
        if (name == static_rows[i].name && value == static_rows[i].value) {
            return i + 1;
        }
    }
    return 0;
}

std::size_t static_find_name(std::string_view name) noexcept {
    for (std::size_t i = 0; i < static_table_size; ++i) {
        if (name == static_rows[i].name) {
            return i + 1;
        }
    }
    return 0;
}

// --- Integer primitives ------------------------------------------------------

status hpack_encode_integer(span<std::byte> out, std::size_t& written, u8 prefix_bits,
                            u8 pattern, u64 value) noexcept {
    written = 0;
    if (prefix_bits < 1 || prefix_bits > 8) {
        return rpc_code::invalid_argument;
    }
    const u32 max_prefix = (1u << prefix_bits) - 1u;
    std::size_t needed = 1;
    if (value >= max_prefix) {
        u64 rest = value - max_prefix;
        do {
            ++needed;
            rest >>= 7;
        } while (rest != 0);
    }
    if (out.size() < needed) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    if (value < max_prefix) {
        if (!writer.put_u8(static_cast<u8>(pattern | static_cast<u8>(value)))) {
            return rpc_code::encode_error;
        }
    } else {
        if (!writer.put_u8(static_cast<u8>(pattern | static_cast<u8>(max_prefix)))) {
            return rpc_code::encode_error;
        }
        u64 rest = value - max_prefix;
        while (rest >= 128) {
            if (!writer.put_u8(static_cast<u8>((rest & 0x7Fu) | 0x80u))) {
                return rpc_code::encode_error;
            }
            rest >>= 7;
        }
        if (!writer.put_u8(static_cast<u8>(rest))) {
            return rpc_code::encode_error;
        }
    }
    written = writer.written();
    return status{};
}

status hpack_decode_integer(span<const std::byte> in, std::size_t& consumed,
                            u8 prefix_bits, u64& value) noexcept {
    consumed = 0;
    if (prefix_bits < 1 || prefix_bits > 8 || in.empty()) {
        return rpc_code::protocol_error;
    }
    value = 0;
    const u8 mask = static_cast<u8>((1u << prefix_bits) - 1u);
    const u64 max_prefix = static_cast<u64>(mask);
    u64 result = static_cast<u64>(static_cast<u8>(in[0]) & mask);
    std::size_t pos = 1;
    if (result < max_prefix) {
        value = result;
        consumed = pos;
        return status{};
    }
    unsigned shift = 0;
    unsigned continuation = 0;
    u8 byte = 0;
    do {
        if (pos >= in.size()) {
            return rpc_code::protocol_error;
        }
        if (continuation >= 10 || shift >= 64) {
            return rpc_code::protocol_error;
        }
        byte = static_cast<u8>(in[pos++]);
        const u64 payload = static_cast<u64>(byte & 0x7Fu);
        const u64 add = payload << shift;
        if ((add >> shift) != payload || result > std::numeric_limits<u64>::max() - add) {
            return rpc_code::protocol_error;
        }
        result += add;
        shift += 7;
        ++continuation;
    } while ((byte & 0x80u) != 0);
    value = result;
    consumed = pos;
    return status{};
}

// --- String primitives -------------------------------------------------------

status hpack_encode_string(span<std::byte> out, std::size_t& written,
                           std::string_view value) noexcept {
    written = 0;
    std::size_t length_bytes = 1;
    if (value.size() >= 127) {
        u64 rest = value.size() - 127;
        do {
            ++length_bytes;
            rest >>= 7;
        } while (rest != 0);
    }
    if (out.size() < length_bytes + value.size()) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    if (value.size() < 127) {
        if (!writer.put_u8(static_cast<u8>(value.size()))) {
            return rpc_code::encode_error;
        }
    } else {
        if (!writer.put_u8(0x7Fu)) {
            return rpc_code::encode_error;
        }
        u64 rest = value.size() - 127;
        while (rest >= 128) {
            if (!writer.put_u8(static_cast<u8>((rest & 0x7Fu) | 0x80u))) {
                return rpc_code::encode_error;
            }
            rest >>= 7;
        }
        if (!writer.put_u8(static_cast<u8>(rest))) {
            return rpc_code::encode_error;
        }
    }
    for (char c : value) {
        if (!writer.put_u8(static_cast<u8>(static_cast<unsigned char>(c)))) {
            return rpc_code::encode_error;
        }
    }
    written = writer.written();
    return status{};
}

status hpack_decode_string(span<const std::byte> in, std::size_t& consumed,
                           std::string& out) noexcept {
    consumed = 0;
    out.clear();
    if (in.empty()) {
        return rpc_code::protocol_error;
    }
    const bool huffman = (static_cast<u8>(in[0]) & 0x80u) != 0;
    std::size_t prefix_consumed = 0;
    u64 length = 0;
    const status decoded = hpack_decode_integer(in, prefix_consumed, 7, length);
    if (!decoded) {
        return decoded;
    }
    if (length > static_cast<u64>(in.size() - prefix_consumed)) {
        return rpc_code::protocol_error;
    }
    const std::size_t byte_length = static_cast<std::size_t>(length);
    const span<const std::byte> data = in.subspan(prefix_consumed, byte_length);
    if (huffman) {
        const status huffman_status = huffman_decode(data, out);
        if (!huffman_status) {
            return huffman_status;
        }
    } else {
        out.assign(reinterpret_cast<const char*>(data.data()), data.size());
    }
    consumed = prefix_consumed + byte_length;
    return status{};
}

// --- Huffman -----------------------------------------------------------------

status huffman_decode(span<const std::byte> in, std::string& out) noexcept {
    const huffman_tables& tables = get_huffman_tables();
    const std::size_t total_bits = in.size() * 8;
    std::size_t bit = 0;
    while (bit < total_bits) {
        const std::size_t start = bit;
        u32 code = 0;
        std::size_t index = 0;
        bool matched = false;
        u8 symbol = 0;
        for (int length = 1; length <= tables.max_len; ++length) {
            if (bit >= total_bits) {
                break;
            }
            const u8 byte = static_cast<u8>(in[bit >> 3]);
            const u32 bit_value = (static_cast<u32>(byte) >> (7 - (bit & 7u))) & 1u;
            ++bit;
            code = (code << 1) | bit_value;
            const std::size_t length_index = static_cast<std::size_t>(length);
            const u32 first = tables.first_code[length_index];
            const u16 count = tables.counts[length_index];
            if (code - first < static_cast<u32>(count)) {
                const u16 decoded = tables.symbols[index + (code - first)];
                if (decoded == 256) {
                    return rpc_code::protocol_error; // EOS must not appear
                }
                symbol = static_cast<u8>(decoded);
                matched = true;
                break;
            }
            index += static_cast<std::size_t>(count);
        }
        if (matched) {
            out.push_back(static_cast<char>(symbol));
            continue;
        }
        // The bits consumed since `start` are EOS padding: at most seven bits,
        // all ones.
        if (total_bits - start > 7) {
            return rpc_code::protocol_error;
        }
        for (std::size_t p = start; p < total_bits; ++p) {
            const u8 byte = static_cast<u8>(in[p >> 3]);
            if (((static_cast<u32>(byte) >> (7 - (p & 7u))) & 1u) == 0) {
                return rpc_code::protocol_error;
            }
        }
        break;
    }
    return status{};
}

std::size_t huffman_encoded_size(std::string_view value) noexcept {
    std::size_t bits = 0;
    for (char c : value) {
        bits += huff_bits[static_cast<u8>(static_cast<unsigned char>(c))];
    }
    return (bits + 7) / 8;
}

status huffman_encode(span<std::byte> out, std::string_view value,
                      std::size_t& written) noexcept {
    written = 0;
    const std::size_t needed = huffman_encoded_size(value);
    if (out.size() < needed) {
        return rpc_code::encode_error;
    }
    for (std::size_t i = 0; i < needed; ++i) {
        out[i] = std::byte{0};
    }
    std::size_t bit = 0;
    for (char c : value) {
        const u8 symbol = static_cast<u8>(static_cast<unsigned char>(c));
        const u32 code = huff_code[symbol];
        const u8 length = huff_bits[symbol];
        for (int i = length - 1; i >= 0; --i) {
            if (((code >> static_cast<unsigned>(i)) & 1u) != 0) {
                out[bit >> 3] |= static_cast<std::byte>(1u << (7 - (bit & 7u)));
            }
            ++bit;
        }
    }
    while ((bit & 7u) != 0) {
        out[bit >> 3] |= static_cast<std::byte>(1u << (7 - (bit & 7u)));
        ++bit;
    }
    written = needed;
    return status{};
}

// --- Dynamic table -----------------------------------------------------------

dynamic_table::dynamic_table(std::size_t max_size) : max_size_(max_size) {}

void dynamic_table::set_max_size(std::size_t max_size) noexcept {
    max_size_ = max_size;
    while (size_ > max_size_ && !entries_.empty()) {
        size_ -= entries_.back().name.size() + entries_.back().value.size() + 32;
        entries_.pop_back();
    }
}

void dynamic_table::insert(std::string_view name, std::string_view value) {
    const std::size_t entry_size = hpack_entry_size(name, value);
    if (entry_size > max_size_) {
        clear();
        return;
    }
    while (size_ + entry_size > max_size_ && !entries_.empty()) {
        size_ -= entries_.back().name.size() + entries_.back().value.size() + 32;
        entries_.pop_back();
    }
    header_field field;
    field.name.assign(name.data(), name.size());
    field.value.assign(value.data(), value.size());
    entries_.insert(entries_.begin(), std::move(field));
    size_ += entry_size;
}

std::size_t dynamic_table::find(std::string_view name, std::string_view value) const noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name && entries_[i].value == value) {
            return i;
        }
    }
    return entries_.size();
}

std::size_t dynamic_table::find_name(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            return i;
        }
    }
    return entries_.size();
}

// --- Encoder -----------------------------------------------------------------

hpack_encoder::hpack_encoder() : table_(default_header_table_size) {}

void hpack_encoder::set_max_table_size(std::size_t size) noexcept {
    if (!pending_update_) {
        pending_update_ = true;
        pending_min_ = size;
        pending_final_ = size;
    } else {
        if (size < pending_min_) {
            pending_min_ = size;
        }
        pending_final_ = size;
    }
}

status hpack_encoder::encode_name_index(u32 index, u8 prefix_bits, u8 pattern,
                                        std::string& out) {
    std::size_t before = out.size();
    std::size_t needed = 1;
    const u32 max_prefix = (1u << prefix_bits) - 1u;
    if (index >= max_prefix) {
        u64 rest = static_cast<u64>(index) - max_prefix;
        do {
            ++needed;
            rest >>= 7;
        } while (rest != 0);
    }
    out.resize(before + needed);
    std::size_t written = 0;
    const span<std::byte> target(reinterpret_cast<std::byte*>(&out[before]), needed);
    const status result = hpack_encode_integer(target, written, prefix_bits, pattern,
                                               static_cast<u64>(index));
    if (!result) {
        out.resize(before);
        return result;
    }
    out.resize(before + written);
    return status{};
}

status hpack_encoder::encode_field(std::string_view name, std::string_view value,
                                   hpack_indexing mode, std::string& out) {
    if (mode != hpack_indexing::never) {
        const std::size_t static_index = static_find(name, value);
        if (static_index != 0) {
            return encode_name_index(static_cast<u32>(static_index), 7, 0x80u, out);
        }
        const std::size_t dynamic_index = table_.find(name, value);
        if (dynamic_index != table_.count()) {
            return encode_name_index(static_cast<u32>(62 + dynamic_index), 7, 0x80u, out);
        }
    }

    std::size_t name_index = 0;
    const std::size_t static_name = static_find_name(name);
    if (static_name != 0) {
        name_index = static_name;
    } else {
        const std::size_t dynamic_name = table_.find_name(name);
        if (dynamic_name != table_.count()) {
            name_index = 62 + dynamic_name;
        }
    }

    u8 pattern = 0x40u;
    u8 prefix_bits = 6;
    if (mode == hpack_indexing::without) {
        pattern = 0x00u;
        prefix_bits = 4;
    } else if (mode == hpack_indexing::never) {
        pattern = 0x10u;
        prefix_bits = 4;
    }

    const std::size_t before = out.size();
    status result = encode_name_index(static_cast<u32>(name_index), prefix_bits, pattern, out);
    if (result && name_index == 0) {
        append_string(out, name);
    }
    if (result) {
        append_string(out, value);
    }
    if (result && mode == hpack_indexing::incremental) {
        table_.insert(name, value);
    }
    if (!result) {
        out.resize(before);
    }
    return result;
}

status hpack_encoder::encode(span<const header_field> headers, std::string& out) {
    if (pending_update_) {
        if (pending_min_ != pending_final_) {
            table_.set_max_size(pending_min_);
            append_integer(out, 5, 0x20u, pending_min_);
            table_.set_max_size(pending_final_);
            append_integer(out, 5, 0x20u, pending_final_);
        } else {
            table_.set_max_size(pending_final_);
            append_integer(out, 5, 0x20u, pending_final_);
        }
        pending_update_ = false;
    }
    for (const header_field& field : headers) {
        const status result = encode_field(field.name, field.value, hpack_indexing::incremental, out);
        if (!result) {
            return result;
        }
    }
    return status{};
}

// --- Decoder -----------------------------------------------------------------

hpack_decoder::hpack_decoder(std::size_t max_table_size)
    : table_(max_table_size), peer_table_limit_(max_table_size) {}

void hpack_decoder::set_peer_table_limit(std::size_t limit) noexcept {
    peer_table_limit_ = limit;
    if (table_.max_size() > limit) {
        table_.set_max_size(limit);
    }
}

namespace {

const header_field* resolve_index(const dynamic_table& table, u64 index) noexcept {
    if (index == 0) {
        return nullptr;
    }
    if (index <= static_table_size) {
        return static_table_entry(static_cast<std::size_t>(index));
    }
    const u64 dynamic = index - static_table_size - 1;
    if (dynamic < table.count()) {
        return &table.at(static_cast<std::size_t>(dynamic));
    }
    return nullptr;
}

// Shared decode loop. `sink` is invoked once per decoded field with string
// views that borrow the reused scratch strings; it must copy what it keeps.
template <typename Sink>
status decode_block(dynamic_table& table, std::size_t peer_limit,
                    std::size_t max_list_size, std::string& name, std::string& value,
                    span<const std::byte> block, Sink sink) {
    std::size_t pos = 0;
    std::size_t list_size = 0;
    bool saw_field = false;
    // Once the advertised header-list limit is exceeded the block is still
    // decoded to completion so that the dynamic table stays in sync with the
    // encoder; only the fields beyond the limit are withheld from the sink.
    bool over_limit = false;
    while (pos < block.size()) {
        const u8 first = static_cast<u8>(block[pos]);
        if ((first & 0x80u) != 0) {
            std::size_t used = 0;
            u64 index = 0;
            const status decoded = hpack_decode_integer(block.subspan(pos), used, 7, index);
            if (!decoded) {
                return decoded;
            }
            const header_field* entry = resolve_index(table, index);
            if (entry == nullptr) {
                return rpc_code::protocol_error;
            }
            list_size += hpack_entry_size(entry->name, entry->value);
            if (list_size > max_list_size) {
                over_limit = true;
            } else {
                sink(entry->name, entry->value);
            }
            pos += used;
            saw_field = true;
        } else if ((first & 0xE0u) == 0x20u) {
            if (saw_field) {
                // A size update is only legal at the start of a header block.
                return rpc_code::protocol_error;
            }
            std::size_t used = 0;
            u64 size = 0;
            const status decoded = hpack_decode_integer(block.subspan(pos), used, 5, size);
            if (!decoded) {
                return decoded;
            }
            if (size > peer_limit) {
                return rpc_code::protocol_error;
            }
            table.set_max_size(static_cast<std::size_t>(size));
            pos += used;
        } else {
            bool add = false;
            u8 prefix_bits = 4;
            u8 pattern = 0x00u;
            if ((first & 0xC0u) == 0x40u) {
                add = true;
                prefix_bits = 6;
                pattern = 0x40u;
            } else if ((first & 0xF0u) == 0x10u) {
                prefix_bits = 4;
                pattern = 0x00u;
            } else {
                prefix_bits = 4;
                pattern = 0x00u;
            }
            (void)pattern;
            std::size_t used = 0;
            u64 name_index = 0;
            status decoded = hpack_decode_integer(block.subspan(pos), used, prefix_bits, name_index);
            if (!decoded) {
                return decoded;
            }
            pos += used;
            if (name_index == 0) {
                std::size_t name_used = 0;
                decoded = hpack_decode_string(block.subspan(pos), name_used, name);
                if (!decoded) {
                    return decoded;
                }
                pos += name_used;
            } else {
                const header_field* entry = resolve_index(table, name_index);
                if (entry == nullptr) {
                    return rpc_code::protocol_error;
                }
                name.assign(entry->name.data(), entry->name.size());
            }
            std::size_t value_used = 0;
            decoded = hpack_decode_string(block.subspan(pos), value_used, value);
            if (!decoded) {
                return decoded;
            }
            pos += value_used;
            list_size += hpack_entry_size(name, value);
            if (list_size > max_list_size) {
                over_limit = true;
            } else {
                sink(name, value);
            }
            if (add) {
                table.insert(name, value);
            }
            saw_field = true;
        }
    }
    return over_limit ? status(rpc_code::resource_exhausted) : status{};
}

} // namespace

status hpack_decoder::decode(span<const std::byte> block, http::header_list& out) {
    return decode_block(table_, peer_table_limit_, max_header_list_size_, scratch_name_,
                        scratch_value_, block, [&out](std::string_view field_name,
                                                      std::string_view field_value) {
                            out.add(field_name, field_value);
                        });
}

status hpack_decoder::decode(span<const std::byte> block, std::vector<header_field>& out) {
    return decode_block(table_, peer_table_limit_, max_header_list_size_, scratch_name_,
                        scratch_value_, block, [&out](std::string_view field_name,
                                                      std::string_view field_value) {
                            header_field field;
                            field.name.assign(field_name.data(), field_name.size());
                            field.value.assign(field_value.data(), field_value.size());
                            out.push_back(std::move(field));
                        });
}

} // namespace crpc::http2
