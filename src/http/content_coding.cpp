#include <crpc/http/content_coding.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace crpc::http {
namespace {

std::string_view trim_ows(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

bool parse_quality(std::string_view text, double& value) noexcept {
    const std::string_view s = trim_ows(text);
    if (s.empty()) {
        return false;
    }
    std::size_t index = 0;
    double result = 0.0;
    if (s[index] == '0' || s[index] == '1') {
        result = static_cast<double>(s[index] - '0');
        ++index;
    } else {
        return false;
    }
    if (index < s.size()) {
        if (s[index] != '.') {
            return false;
        }
        ++index;
        double scale = 0.1;
        int digits = 0;
        while (index < s.size() && s[index] >= '0' && s[index] <= '9') {
            if (digits < 4) {
                result += static_cast<double>(s[index] - '0') * scale;
                scale *= 0.1;
            }
            ++digits;
            ++index;
        }
        if (digits == 0) {
            return false;
        }
    }
    if (index != s.size()) {
        return false;
    }
    if (result < 0.0) {
        result = 0.0;
    }
    if (result > 1.0) {
        result = 1.0;
    }
    value = result;
    return true;
}

struct coding_quality {
    bool present = false;
    double quality = 1.0;
};

void parse_accept_encoding(std::string_view value, coding_quality& gzip,
                           coding_quality& deflate, coding_quality& identity,
                           coding_quality& wildcard) noexcept {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' ||
                                      value[pos] == ',')) {
            ++pos;
        }
        if (pos >= value.size()) {
            break;
        }
        std::size_t end = pos;
        while (end < value.size() && value[end] != ',') {
            ++end;
        }
        std::string_view element = trim_ows(value.substr(pos, end - pos));
        pos = end + 1;

        std::string_view name = element;
        double quality = 1.0;
        const std::size_t semicolon = element.find(';');
        if (semicolon != std::string_view::npos) {
            name = trim_ows(element.substr(0, semicolon));
            std::string_view params = element.substr(semicolon + 1);
            std::size_t p = 0;
            while (p < params.size()) {
                while (p < params.size() &&
                       (params[p] == ' ' || params[p] == '\t' ||
                        params[p] == ';')) {
                    ++p;
                }
                if (p >= params.size()) {
                    break;
                }
                const std::size_t param_end = params.find(';', p);
                std::string_view param =
                    params.substr(p, param_end == std::string_view::npos
                                         ? std::string_view::npos
                                         : param_end - p);
                const std::size_t equals = param.find('=');
                if (equals != std::string_view::npos &&
                    ascii_iequals(trim_ows(param.substr(0, equals)), "q")) {
                    if (!parse_quality(param.substr(equals + 1), quality)) {
                        quality = 0.0;
                    }
                }
                if (param_end == std::string_view::npos) {
                    break;
                }
                p = param_end + 1;
            }
        }
        if (name.empty()) {
            continue;
        }
        if (ascii_iequals(name, "gzip") || ascii_iequals(name, "x-gzip")) {
            if (!gzip.present) {
                gzip.present = true;
                gzip.quality = quality;
            }
        } else if (ascii_iequals(name, "deflate")) {
            if (!deflate.present) {
                deflate.present = true;
                deflate.quality = quality;
            }
        } else if (ascii_iequals(name, "identity")) {
            if (!identity.present) {
                identity.present = true;
                identity.quality = quality;
            }
        } else if (name == "*") {
            if (!wildcard.present) {
                wildcard.present = true;
                wildcard.quality = quality;
            }
        }
    }
}

// --- DEFLATE ----------------------------------------------------------------

struct huffman {
    std::uint16_t counts[16] = {};
    std::uint16_t symbols[320] = {};

    bool build(const std::uint8_t* lengths, std::size_t count) noexcept {
        for (int i = 0; i < 16; ++i) {
            counts[i] = 0;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t length = lengths[i];
            if (length > 15) {
                return false;
            }
            ++counts[length];
        }
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) {
                return false;
            }
        }
        std::uint16_t next[16] = {};
        std::uint16_t acc = 0;
        for (int len = 1; len <= 15; ++len) {
            next[len] = acc;
            acc = static_cast<std::uint16_t>(acc + counts[len]);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t length = lengths[i];
            if (length != 0) {
                symbols[next[length]++] = static_cast<std::uint16_t>(i);
            }
        }
        return true;
    }
};

const std::uint16_t k_length_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint8_t k_length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                         1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                         4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t k_dist_base[30] = {
    1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const std::uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,
                                       4, 4, 5, 5, 6, 6, 7, 7,  8,  8,
                                       9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

const std::uint64_t k_history_size = 32768;

std::uint64_t mul_saturate(std::uint64_t a, std::uint64_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    if (a > std::numeric_limits<std::uint64_t>::max() / b) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a * b;
}

} // namespace

const char* to_string(content_coding coding) noexcept {
    switch (coding) {
    case content_coding::identity:
        return "identity";
    case content_coding::gzip:
        return "gzip";
    case content_coding::deflate:
        return "deflate";
    }
    return "identity";
}

bool content_coding_from_token(std::string_view token,
                               content_coding& coding) noexcept {
    if (ascii_iequals(token, "identity")) {
        coding = content_coding::identity;
        return true;
    }
    if (ascii_iequals(token, "gzip")) {
        coding = content_coding::gzip;
        return true;
    }
    if (ascii_iequals(token, "deflate")) {
        coding = content_coding::deflate;
        return true;
    }
    return false;
}

const coding_limits& default_coding_limits() noexcept {
    static const coding_limits limits{};
    return limits;
}

negotiation negotiate_response(std::string_view accept_encoding,
                               const coding_limits& limits) noexcept {
    negotiation result;
    const std::string_view header = trim_ows(accept_encoding);
    if (header.empty()) {
        result.coding = content_coding::identity;
        return result;
    }
    coding_quality gzip;
    coding_quality deflate;
    coding_quality identity;
    coding_quality wildcard;
    parse_accept_encoding(header, gzip, deflate, identity, wildcard);

    const double gzip_q =
        gzip.present ? gzip.quality : (wildcard.present ? wildcard.quality : 0.0);
    const double deflate_q = deflate.present
                                 ? deflate.quality
                                 : (wildcard.present ? wildcard.quality : 0.0);
    // A coding the client explicitly accepts always wins over the implicit
    // identity fallback; gzip is preferred on a tie.
    content_coding best = content_coding::identity;
    double best_q = 0.0;
    const auto consider = [&](content_coding coding, double q) {
        if (q > best_q) {
            best_q = q;
            best = coding;
        }
    };
    consider(content_coding::gzip, gzip_q);
    consider(content_coding::deflate, deflate_q);

    if (best_q > 0.0) {
        result.coding = best;
        result.compressed = true;
        result.acceptable = true;
        result.quality = best_q;
        return result;
    }

    // No compressed coding is acceptable. Identity is acceptable unless the
    // client explicitly refused it (identity;q=0 or *;q=0).
    const bool identity_ok =
        identity.present ? (identity.quality > 0.0)
                         : (wildcard.present ? (wildcard.quality > 0.0) : true);
    if (identity_ok) {
        result.coding = content_coding::identity;
        result.acceptable = true;
        result.quality = identity.present ? identity.quality : 1.0;
        return result;
    }
    result.coding = content_coding::identity;
    result.acceptable = limits.fallback_identity;
    result.fallback_used = limits.fallback_identity;
    result.quality = 0.0;
    return result;
}

int negotiation_http_status(const negotiation& result) noexcept {
    return result.acceptable ? 200 : 406;
}

bool should_compress(const negotiation& result, std::size_t body_size,
                     const coding_limits& limits) noexcept {
    return result.acceptable && result.compressed &&
           body_size >= limits.min_response_bytes;
}

request_encoding_plan plan_request_decoding(std::string_view content_encoding) {
    request_encoding_plan plan;
    const std::string_view header = trim_ows(content_encoding);
    if (header.empty()) {
        return plan;
    }
    if (header.find(',') != std::string_view::npos) {
        plan.ok = false;
        plan.http_status = 415;
        plan.message = "multiple content encodings are not supported";
        return plan;
    }
    const std::string_view token = trim_ows(header);
    if (token.find(';') != std::string_view::npos) {
        plan.ok = false;
        plan.http_status = 415;
        plan.message = "content encoding parameters are not supported";
        return plan;
    }
    content_coding coding = content_coding::identity;
    if (!content_coding_from_token(token, coding)) {
        plan.ok = false;
        plan.http_status = 415;
        plan.message = "unsupported content coding: " + std::string(token);
        return plan;
    }
    plan.coding = coding;
    plan.encoded = coding != content_coding::identity;
    return plan;
}

rpc_error request_encoding_plan::error() const {
    if (ok) {
        return rpc_error{};
    }
    return rpc_error(rpc_code::invalid_argument, message);
}

// --- Decoder ----------------------------------------------------------------

struct content_decoder::impl {
    enum class phase : std::uint8_t {
        gzip_header,
        gzip_extra_len,
        gzip_extra,
        gzip_name,
        gzip_comment,
        gzip_hcrc,
        zlib_header,
        block_header,
        stored_len,
        stored_data,
        codes,
        trailer,
        done,
        failed
    };

    enum class dec_result : std::uint8_t { ok, need_more, invalid };
    enum class run_result : std::uint8_t { done, need_more, error };

    struct snapshot {
        std::uint64_t bits;
        unsigned count;
        std::size_t pos;
    };

    content_coding coding = content_coding::identity;
    coding_limits limits{};
    std::string input;
    std::size_t input_pos = 0;
    std::uint64_t bits = 0;
    unsigned bit_count = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::array<std::uint8_t, k_history_size> history{};
    std::size_t history_pos = 0;
    std::size_t history_size = 0;
    std::uint32_t crc = 0xffffffffu;
    std::uint32_t adler_a = 1;
    std::uint32_t adler_b = 0;
    bool complete = false;
    bool failed = false;
    rpc_error error;
    phase ph = phase::gzip_header;
    huffman lit;
    huffman dist;
    bool final_block = false;
    bool raw_deflate = false;
    bool zlib_wrapped = false;
    bool gzip_wrapped = false;
    std::uint32_t stored_remaining = 0;
    std::array<std::uint8_t, 10> gzip_header_bytes{};
    std::size_t gzip_header_len = 0;
    std::uint8_t gzip_flags = 0;
    std::uint32_t skip_remaining = 0;
    std::uint32_t header_extension = 0;

    explicit impl(content_coding c, const coding_limits& l) : coding(c), limits(l) {
        if (coding == content_coding::gzip) {
            ph = phase::gzip_header;
        } else if (coding == content_coding::deflate) {
            ph = phase::zlib_header;
        } else {
            ph = phase::done;
        }
    }

    void fail(rpc_code code, std::string message) {
        if (!failed) {
            failed = true;
            ph = phase::failed;
            error = rpc_error(code, std::move(message));
        }
    }

    snapshot save() const noexcept { return {bits, bit_count, input_pos}; }
    void restore(const snapshot& s) noexcept {
        bits = s.bits;
        bit_count = s.count;
        input_pos = s.pos;
    }

    bool get_bits(unsigned count, std::uint32_t& value) noexcept {
        while (bit_count < count) {
            if (input_pos >= input.size()) {
                return false;
            }
            bits |= static_cast<std::uint64_t>(
                        static_cast<std::uint8_t>(input[input_pos++]))
                    << bit_count;
            bit_count += 8;
        }
        const std::uint64_t mask =
            count >= 32 ? 0xffffffffu : ((static_cast<std::uint64_t>(1) << count) - 1);
        value = static_cast<std::uint32_t>(bits & mask);
        bits >>= count;
        bit_count -= count;
        return true;
    }

    void align_bits() noexcept {
        const unsigned drop = bit_count % 8u;
        bits >>= drop;
        bit_count -= drop;
    }

    dec_result decode_symbol(const huffman& table, int& symbol) noexcept {
        const snapshot s = save();
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= 15; ++len) {
            std::uint32_t bit = 0;
            if (!get_bits(1, bit)) {
                restore(s);
                return dec_result::need_more;
            }
            code |= static_cast<int>(bit);
            const int count = static_cast<int>(table.counts[len]);
            if (code - first < count) {
                symbol = static_cast<int>(table.symbols[index + (code - first)]);
                return dec_result::ok;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return dec_result::invalid;
    }

    bool output_allowed(std::uint64_t add) noexcept {
        if (add > limits.max_decompressed_bytes ||
            output_bytes > limits.max_decompressed_bytes - add) {
            fail(rpc_code::resource_exhausted,
                 "decompressed body exceeds max_decompressed_bytes");
            return false;
        }
        if (limits.max_compression_ratio != 0) {
            const std::uint64_t allowed =
                mul_saturate(limits.max_compression_ratio, input_bytes);
            if (output_bytes + add > allowed) {
                fail(rpc_code::resource_exhausted,
                     "decompressed body exceeds max_compression_ratio");
                return false;
            }
        }
        return true;
    }

    void crc_byte(std::uint8_t byte) noexcept {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }

    void adler_byte(std::uint8_t byte) noexcept {
        adler_a += byte;
        if (adler_a >= 65521u) {
            adler_a -= 65521u;
        }
        adler_b += adler_a;
        if (adler_b >= 65521u) {
            adler_b -= 65521u;
        }
    }

    bool emit_byte(std::uint8_t byte, std::string& out) {
        if (!output_allowed(1)) {
            return false;
        }
        out.push_back(static_cast<char>(byte));
        history[history_pos] = byte;
        history_pos = (history_pos + 1) % k_history_size;
        if (history_size < k_history_size) {
            ++history_size;
        }
        crc_byte(byte);
        adler_byte(byte);
        ++output_bytes;
        return true;
    }

    bool copy_bytes(std::uint32_t distance, std::uint32_t length,
                    std::string& out) {
        if (!output_allowed(length)) {
            return false;
        }
        for (std::uint32_t i = 0; i < length; ++i) {
            const std::size_t index =
                (history_pos + k_history_size - distance) % k_history_size;
            const std::uint8_t byte = history[index];
            out.push_back(static_cast<char>(byte));
            history[history_pos] = byte;
            history_pos = (history_pos + 1) % k_history_size;
            if (history_size < k_history_size) {
                ++history_size;
            }
            crc_byte(byte);
            adler_byte(byte);
        }
        output_bytes += length;
        return true;
    }

    void build_fixed() noexcept {
        std::uint8_t lengths[288];
        for (int i = 0; i < 144; ++i) {
            lengths[i] = 8;
        }
        for (int i = 144; i < 256; ++i) {
            lengths[i] = 9;
        }
        for (int i = 256; i < 280; ++i) {
            lengths[i] = 7;
        }
        for (int i = 280; i < 288; ++i) {
            lengths[i] = 8;
        }
        lit.build(lengths, 288);
        std::uint8_t distances[32];
        for (int i = 0; i < 32; ++i) {
            distances[i] = 5;
        }
        dist.build(distances, 32);
    }

    dec_result parse_dynamic_headers() {
        const snapshot s = save();
        std::uint32_t hlit = 0;
        std::uint32_t hdist = 0;
        std::uint32_t hclen = 0;
        if (!get_bits(5, hlit) || !get_bits(5, hdist) || !get_bits(4, hclen)) {
            restore(s);
            return dec_result::need_more;
        }
        const std::uint32_t literal_count = hlit + 257u;
        const std::uint32_t distance_count = hdist + 1u;
        const std::uint32_t code_count = hclen + 4u;
        if (literal_count > 286u || distance_count > 32u) {
            return dec_result::invalid;
        }
        static const std::uint8_t order[19] = {16, 17, 18, 0, 8,  7,  9,  6, 10, 5,
                                               11, 4,  12, 3, 13, 2,  14, 1, 15};
        std::uint8_t code_lengths[19] = {};
        for (std::uint32_t i = 0; i < code_count; ++i) {
            std::uint32_t value = 0;
            if (!get_bits(3, value)) {
                restore(s);
                return dec_result::need_more;
            }
            code_lengths[order[i]] = static_cast<std::uint8_t>(value);
        }
        huffman code_table;
        if (!code_table.build(code_lengths, 19)) {
            return dec_result::invalid;
        }

        std::uint8_t lengths[320] = {};
        const std::uint32_t total = literal_count + distance_count;
        std::uint32_t index = 0;
        while (index < total) {
            int symbol = 0;
            const dec_result r = decode_symbol(code_table, symbol);
            if (r == dec_result::need_more) {
                restore(s);
                return dec_result::need_more;
            }
            if (r == dec_result::invalid) {
                return dec_result::invalid;
            }
            if (symbol < 16) {
                lengths[index++] = static_cast<std::uint8_t>(symbol);
                continue;
            }
            std::uint32_t repeat = 0;
            switch (symbol) {
            case 16: {
                if (index == 0) {
                    return dec_result::invalid;
                }
                if (!get_bits(2, repeat)) {
                    restore(s);
                    return dec_result::need_more;
                }
                repeat += 3u;
                const std::uint8_t previous = lengths[index - 1];
                if (repeat > total - index) {
                    return dec_result::invalid;
                }
                for (std::uint32_t i = 0; i < repeat; ++i) {
                    lengths[index++] = previous;
                }
                break;
            }
            case 17: {
                if (!get_bits(3, repeat)) {
                    restore(s);
                    return dec_result::need_more;
                }
                repeat += 3u;
                if (repeat > total - index) {
                    return dec_result::invalid;
                }
                for (std::uint32_t i = 0; i < repeat; ++i) {
                    lengths[index++] = 0;
                }
                break;
            }
            case 18: {
                if (!get_bits(7, repeat)) {
                    restore(s);
                    return dec_result::need_more;
                }
                repeat += 11u;
                if (repeat > total - index) {
                    return dec_result::invalid;
                }
                for (std::uint32_t i = 0; i < repeat; ++i) {
                    lengths[index++] = 0;
                }
                break;
            }
            default:
                return dec_result::invalid;
            }
        }
        if (!lit.build(lengths, literal_count)) {
            return dec_result::invalid;
        }
        if (!dist.build(lengths + literal_count, distance_count)) {
            return dec_result::invalid;
        }
        return dec_result::ok;
    }

    void end_of_block() noexcept {
        if (final_block) {
            if (gzip_wrapped || zlib_wrapped) {
                ph = phase::trailer;
            } else {
                ph = phase::done;
            }
        } else {
            ph = phase::block_header;
        }
    }

    run_result run(std::string& out) {
        for (;;) {
            switch (ph) {
            case phase::gzip_header: {
                while (gzip_header_len < 10) {
                    std::uint32_t byte = 0;
                    if (!get_bits(8, byte)) {
                        return run_result::need_more;
                    }
                    gzip_header_bytes[gzip_header_len++] =
                        static_cast<std::uint8_t>(byte);
                }
                if (gzip_header_bytes[0] != 0x1f ||
                    gzip_header_bytes[1] != 0x8b ||
                    gzip_header_bytes[2] != 0x08) {
                    fail(rpc_code::protocol_error, "invalid gzip header");
                    return run_result::error;
                }
                gzip_flags = gzip_header_bytes[3];
                if ((gzip_flags & 0xe0u) != 0) {
                    fail(rpc_code::protocol_error, "reserved gzip flags set");
                    return run_result::error;
                }
                gzip_wrapped = true;
                if ((gzip_flags & 0x04u) != 0) {
                    ph = phase::gzip_extra_len;
                } else if ((gzip_flags & 0x08u) != 0) {
                    ph = phase::gzip_name;
                } else if ((gzip_flags & 0x10u) != 0) {
                    ph = phase::gzip_comment;
                } else if ((gzip_flags & 0x02u) != 0) {
                    ph = phase::gzip_hcrc;
                } else {
                    ph = phase::block_header;
                }
                break;
            }
            case phase::gzip_extra_len: {
                const snapshot s = save();
                std::uint32_t length = 0;
                if (!get_bits(16, length)) {
                    restore(s);
                    return run_result::need_more;
                }
                skip_remaining = length;
                ph = phase::gzip_extra;
                break;
            }
            case phase::gzip_extra: {
                while (skip_remaining != 0) {
                    std::uint32_t byte = 0;
                    if (!get_bits(8, byte)) {
                        return run_result::need_more;
                    }
                    --skip_remaining;
                }
                if ((gzip_flags & 0x08u) != 0) {
                    ph = phase::gzip_name;
                } else if ((gzip_flags & 0x10u) != 0) {
                    ph = phase::gzip_comment;
                } else if ((gzip_flags & 0x02u) != 0) {
                    ph = phase::gzip_hcrc;
                } else {
                    ph = phase::block_header;
                }
                break;
            }
            case phase::gzip_name:
            case phase::gzip_comment: {
                for (;;) {
                    std::uint32_t byte = 0;
                    if (!get_bits(8, byte)) {
                        return run_result::need_more;
                    }
                    if (byte == 0) {
                        break;
                    }
                    if (++header_extension > 65536u) {
                        fail(rpc_code::protocol_error,
                             "gzip optional header too large");
                        return run_result::error;
                    }
                }
                if (ph == phase::gzip_name && (gzip_flags & 0x10u) != 0) {
                    ph = phase::gzip_comment;
                } else if ((gzip_flags & 0x02u) != 0) {
                    ph = phase::gzip_hcrc;
                } else {
                    ph = phase::block_header;
                }
                break;
            }
            case phase::gzip_hcrc: {
                const snapshot s = save();
                std::uint32_t low = 0;
                std::uint32_t high = 0;
                if (!get_bits(8, low) || !get_bits(8, high)) {
                    restore(s);
                    return run_result::need_more;
                }
                ph = phase::block_header;
                break;
            }
            case phase::zlib_header: {
                const snapshot s = save();
                std::uint32_t cmf = 0;
                std::uint32_t flg = 0;
                if (!get_bits(8, cmf) || !get_bits(8, flg)) {
                    restore(s);
                    return run_result::need_more;
                }
                const std::uint32_t header = (cmf << 8) | flg;
                if ((cmf & 0x0fu) != 8u || (cmf >> 4) > 7u || (header % 31u) != 0) {
                    restore(s);
                    raw_deflate = true;
                    ph = phase::block_header;
                    break;
                }
                if ((flg & 0x20u) != 0) {
                    fail(rpc_code::protocol_error,
                         "zlib preset dictionary is not supported");
                    return run_result::error;
                }
                bits = 0;
                bit_count = 0;
                zlib_wrapped = true;
                ph = phase::block_header;
                break;
            }
            case phase::block_header: {
                const snapshot s = save();
                std::uint32_t final_bit = 0;
                std::uint32_t type = 0;
                if (!get_bits(1, final_bit) || !get_bits(2, type)) {
                    restore(s);
                    return run_result::need_more;
                }
                final_block = final_bit != 0;
                if (type == 0) {
                    align_bits();
                    ph = phase::stored_len;
                } else if (type == 1) {
                    build_fixed();
                    ph = phase::codes;
                } else if (type == 2) {
                    const dec_result r = parse_dynamic_headers();
                    if (r == dec_result::need_more) {
                        // The block header bits were already consumed; rewind
                        // so the whole header is re-read once more input has
                        // arrived. Otherwise the next call would treat the
                        // continuation of the dynamic header as BFINAL/BTYPE.
                        restore(s);
                        return run_result::need_more;
                    }
                    if (r == dec_result::invalid) {
                        fail(rpc_code::decode_error,
                             "invalid dynamic Huffman header");
                        return run_result::error;
                    }
                    ph = phase::codes;
                } else {
                    fail(rpc_code::protocol_error, "invalid deflate block type");
                    return run_result::error;
                }
                break;
            }
            case phase::stored_len: {
                const snapshot s = save();
                std::uint32_t length = 0;
                std::uint32_t inverse = 0;
                if (!get_bits(16, length) || !get_bits(16, inverse)) {
                    restore(s);
                    return run_result::need_more;
                }
                if (((length ^ inverse) & 0xffffu) != 0) {
                    fail(rpc_code::decode_error,
                         "stored block length check failed");
                    return run_result::error;
                }
                stored_remaining = length;
                ph = phase::stored_data;
                break;
            }
            case phase::stored_data: {
                if (stored_remaining == 0) {
                    end_of_block();
                    break;
                }
                const snapshot s = save();
                std::uint32_t byte = 0;
                if (!get_bits(8, byte)) {
                    restore(s);
                    return run_result::need_more;
                }
                if (!emit_byte(static_cast<std::uint8_t>(byte), out)) {
                    return run_result::error;
                }
                --stored_remaining;
                break;
            }
            case phase::codes: {
                for (;;) {
                    const snapshot s = save();
                    int symbol = 0;
                    const dec_result r = decode_symbol(lit, symbol);
                    if (r == dec_result::need_more) {
                        restore(s);
                        return run_result::need_more;
                    }
                    if (r == dec_result::invalid) {
                        fail(rpc_code::decode_error, "invalid deflate symbol");
                        return run_result::error;
                    }
                    if (symbol < 256) {
                        if (!emit_byte(static_cast<std::uint8_t>(symbol), out)) {
                            return run_result::error;
                        }
                        continue;
                    }
                    if (symbol == 256) {
                        end_of_block();
                        break;
                    }
                    if (symbol > 285) {
                        fail(rpc_code::decode_error, "invalid length symbol");
                        return run_result::error;
                    }
                    const int index = symbol - 257;
                    std::uint32_t extra = 0;
                    if (k_length_extra[index] != 0 &&
                        !get_bits(k_length_extra[index], extra)) {
                        restore(s);
                        return run_result::need_more;
                    }
                    const std::uint32_t length = k_length_base[index] + extra;

                    int distance_symbol = 0;
                    const dec_result d = decode_symbol(dist, distance_symbol);
                    if (d == dec_result::need_more) {
                        restore(s);
                        return run_result::need_more;
                    }
                    if (d == dec_result::invalid || distance_symbol > 29) {
                        fail(rpc_code::decode_error, "invalid distance symbol");
                        return run_result::error;
                    }
                    std::uint32_t distance_extra = 0;
                    if (k_dist_extra[distance_symbol] != 0 &&
                        !get_bits(k_dist_extra[distance_symbol],
                                  distance_extra)) {
                        restore(s);
                        return run_result::need_more;
                    }
                    const std::uint32_t distance =
                        k_dist_base[distance_symbol] + distance_extra;
                    if (distance == 0 || distance > history_size) {
                        fail(rpc_code::decode_error,
                             "distance beyond output history");
                        return run_result::error;
                    }
                    if (!copy_bytes(distance, length, out)) {
                        return run_result::error;
                    }
                }
                break;
            }
            case phase::trailer: {
                align_bits();
                if (gzip_wrapped) {
                    const snapshot s = save();
                    std::uint32_t bytes[8] = {};
                    for (int i = 0; i < 8; ++i) {
                        if (!get_bits(8, bytes[i])) {
                            restore(s);
                            return run_result::need_more;
                        }
                    }
                    const std::uint32_t expected_crc =
                        bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
                        (bytes[3] << 24);
                    if (expected_crc != (crc ^ 0xffffffffu)) {
                        fail(rpc_code::decode_error, "gzip crc mismatch");
                        return run_result::error;
                    }
                    const std::uint32_t expected_size =
                        bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) |
                        (bytes[7] << 24);
                    if (expected_size !=
                        static_cast<std::uint32_t>(output_bytes)) {
                        fail(rpc_code::decode_error, "gzip size mismatch");
                        return run_result::error;
                    }
                } else if (zlib_wrapped) {
                    const snapshot s = save();
                    std::uint32_t a = 0;
                    std::uint32_t b = 0;
                    std::uint32_t c = 0;
                    std::uint32_t d = 0;
                    if (!get_bits(8, a) || !get_bits(8, b) || !get_bits(8, c) ||
                        !get_bits(8, d)) {
                        restore(s);
                        return run_result::need_more;
                    }
                    const std::uint32_t expected = (a << 24) | (b << 16) |
                                                   (c << 8) | d;
                    const std::uint32_t actual = (adler_b << 16) | adler_a;
                    if (expected != actual) {
                        fail(rpc_code::decode_error, "zlib adler-32 mismatch");
                        return run_result::error;
                    }
                }
                if (bit_count >= 8 || input_pos < input.size()) {
                    fail(rpc_code::protocol_error,
                         "trailing data after compressed stream");
                    return run_result::error;
                }
                complete = true;
                ph = phase::done;
                break;
            }
            case phase::done: {
                if (bit_count >= 8 || input_pos < input.size()) {
                    fail(rpc_code::protocol_error,
                         "trailing data after compressed stream");
                    return run_result::error;
                }
                complete = true;
                return run_result::done;
            }
            case phase::failed:
                return run_result::error;
            }
        }
    }

    status feed_identity(span<const std::byte> data, std::string& out) {
        if (failed) {
            return status{error};
        }
        if (complete) {
            if (!data.empty()) {
                fail(rpc_code::protocol_error,
                     "data after complete content stream");
                return status{error};
            }
            return status{};
        }
        if (data.size() > limits.max_input_bytes ||
            input_bytes > limits.max_input_bytes - data.size()) {
            fail(rpc_code::resource_exhausted,
                 "request body exceeds max_input_bytes");
            return status{error};
        }
        input_bytes += data.size();
        if (!data.empty()) {
            if (data.size() > limits.max_decompressed_bytes ||
                output_bytes > limits.max_decompressed_bytes - data.size()) {
                fail(rpc_code::resource_exhausted,
                     "decompressed body exceeds max_decompressed_bytes");
                return status{error};
            }
            out.append(reinterpret_cast<const char*>(data.data()), data.size());
            output_bytes += data.size();
        }
        return status{};
    }

    status feed(span<const std::byte> data, std::string& out) {
        if (coding == content_coding::identity) {
            return feed_identity(data, out);
        }
        if (failed) {
            return status{error};
        }
        if (complete) {
            if (!data.empty()) {
                fail(rpc_code::protocol_error,
                     "data after complete content stream");
                return status{error};
            }
            return status{};
        }
        if (data.size() > limits.max_input_bytes ||
            input_bytes > limits.max_input_bytes - data.size()) {
            fail(rpc_code::resource_exhausted,
                 "request body exceeds max_input_bytes");
            return status{error};
        }
        input_bytes += data.size();
        if (!data.empty()) {
            input.append(reinterpret_cast<const char*>(data.data()), data.size());
        }
        const run_result result = run(out);
        if (input_pos > 0) {
            input.erase(0, input_pos);
            input_pos = 0;
        }
        if (result == run_result::error) {
            return status{error};
        }
        return status{};
    }

    status finish(std::string& out) {
        if (coding == content_coding::identity) {
            if (failed) {
                return status{error};
            }
            complete = true;
            return status{};
        }
        if (failed) {
            return status{error};
        }
        if (complete) {
            return status{};
        }
        const run_result result = run(out);
        if (input_pos > 0) {
            input.erase(0, input_pos);
            input_pos = 0;
        }
        if (result == run_result::error) {
            return status{error};
        }
        if (!complete) {
            fail(rpc_code::decode_error, "truncated compressed stream");
            return status{error};
        }
        return status{};
    }
};

content_decoder::content_decoder() noexcept
    : impl_(new (std::nothrow) impl(content_coding::identity,
                                    default_coding_limits())) {}

content_decoder::content_decoder(content_coding coding,
                                 const coding_limits& limits)
    : impl_(new (std::nothrow) impl(coding, limits)) {}

content_decoder::content_decoder(content_decoder&& other) noexcept = default;
content_decoder& content_decoder::operator=(content_decoder&& other) noexcept =
    default;
content_decoder::~content_decoder() = default;

status content_decoder::feed(span<const std::byte> data, std::string& out) {
    if (!impl_) {
        return status{rpc_error(rpc_code::internal, "decoder is not initialized")};
    }
    return impl_->feed(data, out);
}

status content_decoder::finish(std::string& out) {
    if (!impl_) {
        return status{rpc_error(rpc_code::internal, "decoder is not initialized")};
    }
    return impl_->finish(out);
}

bool content_decoder::complete() const noexcept {
    return impl_ != nullptr && impl_->complete;
}

std::size_t content_decoder::input_bytes() const noexcept {
    return impl_ != nullptr ? static_cast<std::size_t>(impl_->input_bytes) : 0;
}

std::size_t content_decoder::output_bytes() const noexcept {
    return impl_ != nullptr ? static_cast<std::size_t>(impl_->output_bytes) : 0;
}

content_coding content_decoder::coding() const noexcept {
    return impl_ != nullptr ? impl_->coding : content_coding::identity;
}

const rpc_error& content_decoder::error() const noexcept {
    static const rpc_error empty{};
    return impl_ != nullptr ? impl_->error : empty;
}

// --- Encoder ----------------------------------------------------------------

struct content_encoder::impl {
    static constexpr std::size_t k_window = static_cast<std::size_t>(k_history_size);
    static constexpr std::size_t k_hash_bits = 15;
    static constexpr std::size_t k_hash_size = std::size_t{1} << k_hash_bits;
    static constexpr std::size_t k_chain_limit = 128;
    static constexpr std::uint32_t k_min_match = 3;
    static constexpr std::uint32_t k_max_match = 258;

    content_coding coding = content_coding::identity;
    coding_limits limits{};
    bool started = false;
    bool finished = false;
    bool failed = false;
    rpc_error error;
    std::uint64_t accumulator = 0;
    unsigned bit_count = 0;
    std::uint32_t crc = 0xffffffffu;
    std::uint32_t adler_a = 1;
    std::uint32_t adler_b = 0;
    std::uint64_t input_bytes = 0;

    // Minimal LZ77 state: one 32 KiB ring of recent bytes and a hash chain over
    // 3-byte prefixes. Positions are global input offsets; a slot is
    // `position % k_window`. The chain persists across `write` calls so a match
    // may cross a write boundary.
    std::array<std::uint8_t, k_window> window{};
    std::array<std::int32_t, k_hash_size> head{};
    std::vector<std::int32_t> prev;

    impl(content_coding c, const coding_limits& l) : coding(c), limits(l) {
        head.fill(-1);
        prev.assign(k_window, -1);
    }

    void fail(rpc_code code, std::string message) {
        failed = true;
        error = rpc_error(code, std::move(message));
    }

    void put(std::uint32_t value, unsigned count, std::string& out) {
        accumulator |= static_cast<std::uint64_t>(value) << bit_count;
        bit_count += count;
        while (bit_count >= 8) {
            out.push_back(static_cast<char>(accumulator & 0xffu));
            accumulator >>= 8;
            bit_count -= 8;
        }
    }

    void put_code(std::uint32_t code, unsigned count, std::string& out) {
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < count; ++i) {
            reversed = (reversed << 1) | (code & 1u);
            code >>= 1;
        }
        put(reversed, count, out);
    }

    void put_literal(std::uint8_t value, std::string& out) {
        // `value` is a byte (0..255), so this covers the whole domain: RFC 1951
        // fixed-Huffman literals use the 8-bit codes 0..143 and the 9-bit codes
        // 144..255. The 7-bit codes are reserved for the end-of-block symbol and
        // the length alphabet, which never reach this path.
        if (value <= 143) {
            put_code(0x30u + value, 8, out);
        } else {
            put_code(0x190u + (value - 144u), 9, out);
        }
    }

    void put_fixed_symbol(std::uint32_t symbol, std::string& out) {
        // RFC 1951 fixed Huffman: 256..279 are 7-bit codes 0..23 and 280..287
        // are 8-bit codes 0xC0..0xC7.
        if (symbol <= 279) {
            put_code(symbol - 256, 7, out);
        } else {
            put_code(0xC0u + (symbol - 280), 8, out);
        }
    }

    void put_length(std::uint32_t length, std::string& out) {
        std::size_t index = 0;
        if (length == k_max_match) {
            index = 28; // canonical code 285
        } else {
            while (index < 28 &&
                   length > k_length_base[index] +
                                ((1u << k_length_extra[index]) - 1u)) {
                ++index;
            }
        }
        put_fixed_symbol(257u + static_cast<std::uint32_t>(index), out);
        if (k_length_extra[index] != 0) {
            put(length - k_length_base[index], k_length_extra[index], out);
        }
    }

    void put_distance(std::uint32_t distance, std::string& out) {
        std::size_t index = 0;
        while (index < 29 &&
               distance > k_dist_base[index] +
                              ((1u << k_dist_extra[index]) - 1u)) {
            ++index;
        }
        put_code(static_cast<std::uint32_t>(index), 5, out);
        if (k_dist_extra[index] != 0) {
            put(distance - k_dist_base[index], k_dist_extra[index], out);
        }
    }

    [[nodiscard]] std::uint8_t byte_at(std::uint64_t position,
                                       span<const std::byte> current,
                                       std::uint64_t current_start) const noexcept {
        if (position >= current_start) {
            return static_cast<std::uint8_t>(
                current[static_cast<std::size_t>(position - current_start)]);
        }
        return window[static_cast<std::size_t>(position % k_window)];
    }

    [[nodiscard]] static std::size_t hash3(std::uint8_t a, std::uint8_t b,
                                           std::uint8_t c) noexcept {
        return ((static_cast<std::size_t>(a) << 10) ^
                (static_cast<std::size_t>(b) << 5) ^
                static_cast<std::size_t>(c)) &
               (k_hash_size - 1);
    }

    // Inserts one position into the hash chain. Requires `position + 2` to be a
    // valid input byte.
    void insert(std::uint64_t position, span<const std::byte> current,
                std::uint64_t current_start) {
        const std::uint8_t a = byte_at(position, current, current_start);
        const std::uint8_t b = byte_at(position + 1, current, current_start);
        const std::uint8_t c = byte_at(position + 2, current, current_start);
        const std::size_t slot = hash3(a, b, c);
        const std::size_t offset =
            static_cast<std::size_t>(position % k_window);
        prev[offset] = head[slot];
        head[slot] = static_cast<std::int32_t>(position);
        window[offset] = a;
    }

    void update_checksums(span<const std::byte> data) noexcept {
        for (std::size_t i = 0; i < data.size(); ++i) {
            const std::uint8_t byte = static_cast<std::uint8_t>(data[i]);
            crc ^= byte;
            for (int k = 0; k < 8; ++k) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
            adler_a += byte;
            if (adler_a >= 65521u) {
                adler_a -= 65521u;
            }
            adler_b += adler_a;
            if (adler_b >= 65521u) {
                adler_b -= 65521u;
            }
        }
    }

    status start(std::string& out) {
        if (finished) {
            return status{rpc_error(rpc_code::failed_precondition,
                                    "encoder is already finished")};
        }
        if (started) {
            return status{};
        }
        if (coding == content_coding::gzip) {
            static const std::uint8_t header[10] = {0x1f, 0x8b, 0x08, 0x00, 0x00,
                                                    0x00, 0x00, 0x00, 0x00, 0xff};
            out.append(reinterpret_cast<const char*>(header), sizeof(header));
        } else if (coding == content_coding::deflate) {
            out.push_back(static_cast<char>(0x78));
            out.push_back(static_cast<char>(0x9c));
        }
        started = true;
        return status{};
    }

    status write(span<const std::byte> data, std::string& out) {
        if (finished) {
            return status{rpc_error(rpc_code::failed_precondition,
                                    "encoder is already finished")};
        }
        if (!started) {
            const status s = start(out);
            if (!s.ok()) {
                return s;
            }
        }
        if (coding == content_coding::identity) {
            if (!data.empty()) {
                out.append(reinterpret_cast<const char*>(data.data()),
                           data.size());
            }
            return status{};
        }
        if (data.empty()) {
            return status{};
        }
        update_checksums(data);
        const std::uint64_t base = input_bytes;
        input_bytes += data.size();
        put(0, 1, out);          // BFINAL = 0
        put(1, 2, out);          // BTYPE = fixed Huffman

        const std::size_t n = data.size();
        std::size_t i = 0;
        while (i < n) {
            std::uint32_t best_length = 0;
            std::uint64_t best_distance = 0;
            if (i + k_min_match <= n) {
                const std::size_t slot = hash3(
                    static_cast<std::uint8_t>(data[i]),
                    static_cast<std::uint8_t>(data[i + 1]),
                    static_cast<std::uint8_t>(data[i + 2]));
                std::int32_t candidate = head[slot];
                const std::size_t here_offset =
                    static_cast<std::size_t>((base + i) % k_window);
                prev[here_offset] = candidate;
                head[slot] = static_cast<std::int32_t>(base + i);
                window[here_offset] = static_cast<std::uint8_t>(data[i]);

                const std::uint64_t here = base + i;
                const std::uint32_t max_length = static_cast<std::uint32_t>(
                    std::min<std::size_t>(k_max_match, n - i));
                std::size_t chain = 0;
                while (candidate >= 0 && chain < k_chain_limit) {
                    const std::uint64_t position =
                        static_cast<std::uint64_t>(candidate);
                    if (position >= here) {
                        break;
                    }
                    const std::uint64_t distance = here - position;
                    if (distance > k_window) {
                        break;
                    }
                    std::uint32_t length = 0;
                    while (length < max_length &&
                           byte_at(position + length, data, base) ==
                               static_cast<std::uint8_t>(
                                   data[i + length])) {
                        ++length;
                    }
                    if (length > best_length) {
                        best_length = length;
                        best_distance = distance;
                        if (length == max_length) {
                            break;
                        }
                    }
                    candidate = prev[static_cast<std::size_t>(
                        position % k_window)];
                    ++chain;
                }
            }
            if (best_length >= k_min_match) {
                put_length(best_length, out);
                put_distance(static_cast<std::uint32_t>(best_distance), out);
                for (std::uint32_t k = 1; k < best_length; ++k) {
                    const std::size_t offset = i + k;
                    if (offset + 2 < n) {
                        insert(base + offset, data, base);
                    }
                }
                i += best_length;
            } else {
                put_literal(static_cast<std::uint8_t>(data[i]), out);
                ++i;
            }
        }
        // The last two positions are never inserted into the hash chain (a
        // 3-byte hash needs lookahead), but their bytes MUST still enter the
        // history ring: a match in the next `write` may extend across the
        // boundary and read them through `byte_at`. Leaving them stale would
        // silently corrupt the encoded stream (gzip CRC mismatch).
        for (std::size_t tail = n >= 2 ? n - 2 : 0; tail < n; ++tail) {
            window[static_cast<std::size_t>((base + tail) % k_window)] =
                static_cast<std::uint8_t>(data[tail]);
        }
        put_code(0, 7, out);     // end of block
        return status{};
    }

    status finish(std::string& out) {
        if (finished) {
            return status{};
        }
        if (!started) {
            const status s = start(out);
            if (!s.ok()) {
                return s;
            }
        }
        if (coding != content_coding::identity) {
            put(1, 1, out);      // BFINAL = 1
            put(1, 2, out);      // BTYPE = fixed Huffman
            put_code(0, 7, out); // end of block
            if (bit_count != 0) {
                out.push_back(static_cast<char>(accumulator & 0xffu));
                accumulator = 0;
                bit_count = 0;
            }
            if (coding == content_coding::gzip) {
                const std::uint32_t checksum = crc ^ 0xffffffffu;
                const std::uint32_t isize =
                    static_cast<std::uint32_t>(input_bytes);
                for (unsigned i = 0; i < 4; ++i) {
                    out.push_back(
                        static_cast<char>((checksum >> (i * 8)) & 0xffu));
                }
                for (unsigned i = 0; i < 4; ++i) {
                    out.push_back(static_cast<char>((isize >> (i * 8)) & 0xffu));
                }
            } else {
                const std::uint32_t adler = (adler_b << 16) | adler_a;
                for (int i = 3; i >= 0; --i) {
                    out.push_back(
                        static_cast<char>((adler >> (i * 8)) & 0xffu));
                }
            }
        }
        finished = true;
        return status{};
    }
};

content_encoder::content_encoder() noexcept
    : impl_(new (std::nothrow) impl(content_coding::identity,
                                    default_coding_limits())) {}

content_encoder::content_encoder(content_coding coding,
                                 const coding_limits& limits)
    : impl_(new (std::nothrow) impl(coding, limits)) {}

content_encoder::content_encoder(content_encoder&& other) noexcept = default;
content_encoder& content_encoder::operator=(content_encoder&& other) noexcept =
    default;
content_encoder::~content_encoder() = default;

status content_encoder::start(std::string& out) {
    if (!impl_) {
        return status{rpc_error(rpc_code::internal, "encoder is not initialized")};
    }
    return impl_->start(out);
}

status content_encoder::write(span<const std::byte> data, std::string& out) {
    if (!impl_) {
        return status{rpc_error(rpc_code::internal, "encoder is not initialized")};
    }
    return impl_->write(data, out);
}

status content_encoder::finish(std::string& out) {
    if (!impl_) {
        return status{rpc_error(rpc_code::internal, "encoder is not initialized")};
    }
    return impl_->finish(out);
}

bool content_encoder::started() const noexcept {
    return impl_ != nullptr && impl_->started;
}

bool content_encoder::finished() const noexcept {
    return impl_ != nullptr && impl_->finished;
}

content_coding content_encoder::coding() const noexcept {
    return impl_ != nullptr ? impl_->coding : content_coding::identity;
}

const rpc_error& content_encoder::error() const noexcept {
    static const rpc_error empty{};
    return impl_ != nullptr ? impl_->error : empty;
}

status compress_all(content_coding coding, span<const std::byte> data,
                    std::string& out, const coding_limits& limits) {
    content_encoder encoder(coding, limits);
    status result = encoder.start(out);
    if (!result.ok()) {
        return result;
    }
    result = encoder.write(data, out);
    if (!result.ok()) {
        return result;
    }
    return encoder.finish(out);
}

result<std::string> decompress_all(content_coding coding,
                                   span<const std::byte> data,
                                   const coding_limits& limits) {
    content_decoder decoder(coding, limits);
    std::string output;
    status result = decoder.feed(data, output);
    if (!result.ok()) {
        return result.error();
    }
    result = decoder.finish(output);
    if (!result.ok()) {
        return result.error();
    }
    return output;
}

} // namespace crpc::http
