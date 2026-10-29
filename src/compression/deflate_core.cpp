// CRPC -- DEFLATE core (RFC 1951).
//
// Shared by the gzip, deflate and zip frame algorithms. The encoder emits a
// single dynamic Huffman block and falls back to stored blocks when that would
// be larger, so the stored-block size is the worst case and
// deflate_stream_bound is a true upper bound. The decoder understands stored,
// fixed and dynamic blocks, which is what third-party writers actually send.
//
// Nothing here allocates per call beyond the token/length scratch buffers
// (std::vector, sized once per call), and nothing reads or writes outside the
// spans it is given: every literal and match is checked against the declared
// length before it is written.
#include <algorithm>
#include <array>
#include <cstring>
#include <queue>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/span.hpp>

#include "deflate_core.hpp"

namespace crpc {
namespace detail {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr int deflate_max_bits = 15;
constexpr int deflate_max_match = 258;
constexpr int deflate_min_match = 3;
constexpr int deflate_window = 32768;
constexpr int deflate_max_chain = 128;
constexpr int deflate_hash_bits = 15;
constexpr int deflate_hash_size = 1 << deflate_hash_bits;
constexpr int deflate_hash_shift = 32 - deflate_hash_bits;
constexpr u32 deflate_hash_multiplier = 2654435761u;

// RFC 1951 length codes 257..285: base length and extra bits.
constexpr u16 length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,
                                 27, 31, 35, 43, 51, 59, 67, 83, 99,  115, 131, 163, 195, 227,
                                 258};
constexpr u8 length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
                                 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

// RFC 1951 distance codes 0..29: base distance and extra bits.
constexpr u16 dist_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                               33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                               1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr u8 dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,  4,  5,  5,  6,
                               6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Transmission order of the code-length code (RFC 1951 3.2.7).
constexpr int cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// --- Bit I/O -----------------------------------------------------------------

// LSB-first writer. `written()` counts reserved bytes, so a partially filled
// final byte is already part of the output; the unused low bits stay zero.
class bit_writer {
public:
    explicit bit_writer(span<std::byte> out) noexcept : out_(out) {}

    bool ok() const noexcept { return ok_; }
    std::size_t written() const noexcept { return pos_; }

    void put_bits(u32 value, int count) noexcept {
        for (int i = 0; i < count; ++i) {
            if (bit_count_ == 0) {
                if (pos_ >= out_.size()) {
                    ok_ = false;
                    return;
                }
                out_[pos_] = std::byte{0};
                ++pos_;
                bit_count_ = 8;
            }
            if (((value >> i) & 1u) != 0) {
                out_[pos_ - 1] = static_cast<std::byte>(
                    static_cast<u8>(out_[pos_ - 1]) | static_cast<u8>(1u << (8 - bit_count_)));
            }
            --bit_count_;
        }
    }

    // Huffman codes are packed most-significant bit first (RFC 1951 3.1.1).
    void put_code(u32 code, int length) noexcept {
        for (int i = length - 1; i >= 0; --i) {
            put_bits((code >> i) & 1u, 1);
        }
    }

    // Drops the unused bits of a partially written byte.
    void align_to_byte() noexcept { bit_count_ = 0; }

private:
    span<std::byte> out_;
    std::size_t pos_ = 0;
    int bit_count_ = 0;
    bool ok_ = true;
};

// LSB-first reader. Every read either succeeds or latches ok_ = false.
class bit_reader {
public:
    explicit bit_reader(span<const std::byte> in) noexcept : in_(in) {}

    bool ok() const noexcept { return ok_; }

    int read_bit() noexcept {
        if (bit_count_ == 0) {
            if (pos_ >= in_.size()) {
                ok_ = false;
                return 0;
            }
            current_ = static_cast<u8>(in_[pos_++]);
            bit_count_ = 8;
        }
        const int bit = current_ & 1;
        current_ >>= 1;
        --bit_count_;
        return bit;
    }

    bool read_bits(int count, u32& out) noexcept {
        u32 value = 0;
        for (int i = 0; i < count; ++i) {
            value |= static_cast<u32>(read_bit()) << i;
        }
        if (!ok_) {
            return false;
        }
        out = value;
        return true;
    }

    void align_to_byte() noexcept { bit_count_ = 0; }

    // Bytes pulled from the input so far, including a partially used final
    // byte. A container that wraps DEFLATE uses this to find its trailer.
    std::size_t bytes_consumed() const noexcept { return pos_; }

private:
    span<const std::byte> in_;
    std::size_t pos_ = 0;
    u8 current_ = 0;
    int bit_count_ = 0;
    bool ok_ = true;
};

// --- Huffman -----------------------------------------------------------------

struct huffman_table {
    std::array<int, deflate_max_bits + 1> count{};
    std::array<u32, deflate_max_bits + 1> first_code{};
    std::array<int, deflate_max_bits + 1> first_index{};
    std::vector<u16> symbols;
};

// Builds the canonical decode table from `lengths`. Returns false for an
// over-subscribed set (Kraft sum > 1); an incomplete set is accepted, which is
// what RFC 1951 allows for a single distance code of length one.
[[nodiscard]] bool build_huffman(huffman_table& table,
                                 const std::vector<u8>& lengths) noexcept {
    table.count.fill(0);
    table.symbols.clear();
    int max_length = 0;
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] != 0) {
            table.count[lengths[symbol]]++;
            max_length = std::max(max_length, static_cast<int>(lengths[symbol]));
        }
    }
    for (int length = 1; length <= max_length; ++length) {
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] == static_cast<u8>(length)) {
                table.symbols.push_back(static_cast<u16>(symbol));
            }
        }
    }
    u32 next_code = 0;
    int index = 0;
    int left = 1;
    for (int length = 1; length <= deflate_max_bits; ++length) {
        table.first_code[length] = next_code;
        table.first_index[length] = index;
        next_code = (next_code + static_cast<u32>(table.count[length])) << 1;
        index += table.count[length];
        left <<= 1;
        left -= table.count[length];
        if (left < 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int decode_symbol(bit_reader& reader, const huffman_table& table) noexcept {
    u32 code = 0;
    for (int length = 1; length <= deflate_max_bits; ++length) {
        code = (code << 1) | static_cast<u32>(reader.read_bit());
        if (!reader.ok()) {
            return -1;
        }
        const int count = table.count[length];
        if (count != 0 && code >= table.first_code[length] &&
            code < table.first_code[length] + static_cast<u32>(count)) {
            return table.symbols[table.first_index[length] +
                                 static_cast<std::size_t>(code - table.first_code[length])];
        }
    }
    return -1;
}

struct huffman_encoder {
    std::vector<u32> codes;
    std::vector<u8> lengths;
};

[[nodiscard]] huffman_encoder make_encoder(const huffman_table& table, int symbol_count) noexcept {
    huffman_encoder encoder;
    encoder.codes.assign(static_cast<std::size_t>(symbol_count), 0);
    encoder.lengths.assign(static_cast<std::size_t>(symbol_count), 0);
    std::size_t index = 0;
    for (int length = 1; length <= deflate_max_bits; ++length) {
        for (int k = 0; k < table.count[length]; ++k) {
            const int symbol = table.symbols[index++];
            encoder.codes[static_cast<std::size_t>(symbol)] =
                table.first_code[length] + static_cast<u32>(k);
            encoder.lengths[static_cast<std::size_t>(symbol)] = static_cast<u8>(length);
        }
    }
    return encoder;
}

// Builds Huffman code lengths limited to `max_length`. The tree is built
// unconstrained first; overflowing leaves are then moved to `max_length` with
// the zlib bl_count adjustment so the result stays a complete prefix code.
[[nodiscard]] bool build_code_lengths(const std::vector<u32>& freq, int symbol_count,
                                      int max_length, std::vector<u8>& lengths) noexcept {
    lengths.assign(static_cast<std::size_t>(symbol_count), 0);
    std::vector<int> symbols;
    for (int i = 0; i < symbol_count; ++i) {
        if (freq[static_cast<std::size_t>(i)] != 0) {
            symbols.push_back(i);
        }
    }
    if (symbols.empty()) {
        return true;
    }
    if (symbols.size() == 1) {
        lengths[static_cast<std::size_t>(symbols[0])] = 1;
        return true;
    }

    struct node {
        std::uint64_t freq;
        int left;
        int right;
        int symbol;
    };
    std::vector<node> nodes;
    nodes.reserve(symbols.size() * 2);
    const auto compare = [&](int a, int b) {
        return nodes[static_cast<std::size_t>(a)].freq > nodes[static_cast<std::size_t>(b)].freq;
    };
    std::priority_queue<int, std::vector<int>, decltype(compare)> queue(compare);
    for (const int symbol : symbols) {
        nodes.push_back({freq[static_cast<std::size_t>(symbol)], -1, -1, symbol});
        queue.push(static_cast<int>(nodes.size()) - 1);
    }
    while (queue.size() > 1) {
        const int a = queue.top();
        queue.pop();
        const int b = queue.top();
        queue.pop();
        nodes.push_back({nodes[static_cast<std::size_t>(a)].freq +
                             nodes[static_cast<std::size_t>(b)].freq,
                         a, b, -1});
        queue.push(static_cast<int>(nodes.size()) - 1);
    }
    const int root = queue.top();

    std::vector<int> depth(static_cast<std::size_t>(symbol_count), 0);
    std::vector<std::pair<int, int>> stack;
    stack.push_back({root, 0});
    int max_depth = 0;
    while (!stack.empty()) {
        const auto [index, level] = stack.back();
        stack.pop_back();
        const node& current = nodes[static_cast<std::size_t>(index)];
        if (current.left < 0) {
            depth[static_cast<std::size_t>(current.symbol)] = level;
            max_depth = std::max(max_depth, level);
        } else {
            stack.push_back({current.left, level + 1});
            stack.push_back({current.right, level + 1});
        }
    }

    std::vector<int> bl_count(static_cast<std::size_t>(max_length) + 1, 0);
    int overflow = 0;
    for (const int symbol : symbols) {
        int level = depth[static_cast<std::size_t>(symbol)];
        if (level > max_length) {
            level = max_length;
            ++overflow;
        }
        bl_count[static_cast<std::size_t>(level)]++;
    }

    while (overflow > 0) {
        int bits = max_length - 1;
        while (bits > 0 && bl_count[static_cast<std::size_t>(bits)] == 0) {
            --bits;
        }
        if (bits <= 0) {
            return false;
        }
        bl_count[static_cast<std::size_t>(bits)]--;
        bl_count[static_cast<std::size_t>(bits + 1)] += 2;
        bl_count[static_cast<std::size_t>(max_length)]--;
        overflow -= 2;
    }

    std::sort(symbols.begin(), symbols.end(), [&](int a, int b) {
        if (freq[static_cast<std::size_t>(a)] != freq[static_cast<std::size_t>(b)]) {
            return freq[static_cast<std::size_t>(a)] < freq[static_cast<std::size_t>(b)];
        }
        return a < b;
    });
    std::size_t index = 0;
    for (int level = max_length; level >= 1; --level) {
        for (int k = 0; k < bl_count[static_cast<std::size_t>(level)]; ++k) {
            lengths[static_cast<std::size_t>(symbols[index++])] = static_cast<u8>(level);
        }
    }
    return true;
}

// --- LZ77 --------------------------------------------------------------------

struct lz77_token {
    u16 litlen;    // literal byte (0..255) or length code (257..285)
    u16 distance;  // distance code (0..29) when litlen >= 257, else 0
    u16 length;    // actual match length (3..258), 0 for a literal
    u16 dist_value;// actual distance (1..32768), 0 for a literal
};

[[nodiscard]] u32 hash4(span<const std::byte> in, std::size_t position) noexcept {
    const u32 value = static_cast<u32>(static_cast<u8>(in[position])) |
                      (static_cast<u32>(static_cast<u8>(in[position + 1])) << 8) |
                      (static_cast<u32>(static_cast<u8>(in[position + 2])) << 16) |
                      (static_cast<u32>(static_cast<u8>(in[position + 3])) << 24);
    return (value * deflate_hash_multiplier) >> deflate_hash_shift;
}

[[nodiscard]] int length_code_index(int length) noexcept {
    for (int i = 28; i >= 0; --i) {
        if (length >= static_cast<int>(length_base[i])) {
            return i;
        }
    }
    return 0;
}

[[nodiscard]] int distance_code_index(int distance) noexcept {
    for (int i = 29; i >= 0; --i) {
        if (distance >= static_cast<int>(dist_base[i])) {
            return i;
        }
    }
    return 0;
}

void lz77_tokenize(span<const std::byte> in, std::vector<lz77_token>& tokens) {
    const std::size_t size = in.size();
    if (size == 0) {
        return;
    }
    std::vector<int> head(static_cast<std::size_t>(deflate_hash_size), -1);
    std::vector<int> previous(static_cast<std::size_t>(deflate_window), -1);

    std::size_t position = 0;
    while (position < size) {
        int best_length = 0;
        int best_distance = 0;
        // hash4 reads four bytes, so the guard must reserve four. It used to
        // reserve deflate_min_match (3), which let the final position with
        // exactly three bytes left read one byte past the end of the input --
        // a heap-buffer-overflow that ASan reports at hash4.
        if (position + 4 <= size) {
            const u32 hash = hash4(in, position);
            int candidate = head[hash];
            previous[position & (static_cast<std::size_t>(deflate_window) - 1)] = candidate;
            head[hash] = static_cast<int>(position);

            const std::size_t limit =
                std::min<std::size_t>(static_cast<std::size_t>(deflate_max_match), size - position);
            int chain = 0;
            while (candidate >= 0 && chain < deflate_max_chain) {
                const std::size_t distance = position - static_cast<std::size_t>(candidate);
                if (distance > static_cast<std::size_t>(deflate_window)) {
                    break;
                }
                const std::size_t candidate_index = static_cast<std::size_t>(candidate);
                std::size_t length = 0;
                while (length < limit && in[candidate_index + length] == in[position + length]) {
                    ++length;
                }
                if (static_cast<int>(length) > best_length) {
                    best_length = static_cast<int>(length);
                    best_distance = static_cast<int>(distance);
                    if (static_cast<int>(length) >= static_cast<int>(limit)) {
                        break;
                    }
                }
                candidate = previous[candidate_index & (static_cast<std::size_t>(deflate_window) - 1)];
                ++chain;
            }
        }

        if (best_length >= deflate_min_match) {
            tokens.push_back({static_cast<u16>(257 + length_code_index(best_length)),
                              static_cast<u16>(distance_code_index(best_distance)),
                              static_cast<u16>(best_length), static_cast<u16>(best_distance)});
            const std::size_t end = position + static_cast<std::size_t>(best_length);
            ++position;
            while (position < end) {
                if (position + 4 <= size) {
                    const u32 hash = hash4(in, position);
                    previous[position & (static_cast<std::size_t>(deflate_window) - 1)] = head[hash];
                    head[hash] = static_cast<int>(position);
                }
                ++position;
            }
        } else {
            tokens.push_back({static_cast<u16>(static_cast<u8>(in[position])), 0, 0, 0});
            ++position;
        }
    }
}

// --- Encoder -----------------------------------------------------------------

struct code_length_item {
    u16 symbol;
    u16 extra;
    u8 extra_bits;
};

void encode_code_lengths(const std::vector<u8>& lengths, std::vector<code_length_item>& items) {
    std::size_t index = 0;
    const std::size_t size = lengths.size();
    while (index < size) {
        const u8 length = lengths[index];
        std::size_t run = 1;
        while (index + run < size && lengths[index + run] == length) {
            ++run;
        }
        std::size_t consumed = 0;
        if (length == 0) {
            while (run - consumed >= 11) {
                const std::size_t chunk = std::min<std::size_t>(run - consumed, 138);
                items.push_back({18, static_cast<u16>(chunk - 11), 7});
                consumed += chunk;
            }
            while (run - consumed >= 3) {
                const std::size_t chunk = std::min<std::size_t>(run - consumed, 10);
                items.push_back({17, static_cast<u16>(chunk - 3), 3});
                consumed += chunk;
            }
            for (; consumed < run; ++consumed) {
                items.push_back({0, 0, 0});
            }
        } else {
            items.push_back({length, 0, 0});
            consumed = 1;
            while (run - consumed >= 3) {
                const std::size_t chunk = std::min<std::size_t>(run - consumed, 6);
                items.push_back({16, static_cast<u16>(chunk - 3), 2});
                consumed += chunk;
            }
            for (; consumed < run; ++consumed) {
                items.push_back({length, 0, 0});
            }
        }
        index += run;
    }
}

[[nodiscard]] status write_dynamic_block(bit_writer& writer,
                                         const std::vector<lz77_token>& tokens) noexcept {
    std::vector<u32> litlen_freq(288, 0);
    std::vector<u32> dist_freq(30, 0);
    for (const lz77_token& token : tokens) {
        litlen_freq[token.litlen]++;
        if (token.litlen >= 257) {
            dist_freq[token.distance]++;
        }
    }
    litlen_freq[256] = 1; // end-of-block

    std::vector<u8> litlen_lengths;
    std::vector<u8> dist_lengths;
    if (!build_code_lengths(litlen_freq, 288, deflate_max_bits, litlen_lengths) ||
        !build_code_lengths(dist_freq, 30, deflate_max_bits, dist_lengths)) {
        return rpc_code::encode_error;
    }

    int max_litlen = 287;
    while (max_litlen > 256 && litlen_lengths[static_cast<std::size_t>(max_litlen)] == 0) {
        --max_litlen;
    }
    int max_dist = 29;
    while (max_dist > 0 && dist_lengths[static_cast<std::size_t>(max_dist)] == 0) {
        --max_dist;
    }

    std::vector<u8> combined;
    combined.reserve(static_cast<std::size_t>(max_litlen + 1) +
                     static_cast<std::size_t>(max_dist + 1));
    for (int i = 0; i <= max_litlen; ++i) {
        combined.push_back(litlen_lengths[static_cast<std::size_t>(i)]);
    }
    for (int i = 0; i <= max_dist; ++i) {
        combined.push_back(dist_lengths[static_cast<std::size_t>(i)]);
    }

    std::vector<code_length_item> items;
    encode_code_lengths(combined, items);

    std::vector<u32> cl_freq(19, 0);
    for (const code_length_item& item : items) {
        cl_freq[item.symbol]++;
    }
    std::vector<u8> cl_lengths;
    if (!build_code_lengths(cl_freq, 19, 7, cl_lengths)) {
        return rpc_code::encode_error;
    }
    // RFC 1951: HCLEN is the number of transmitted code-length codes minus 4.
    // `hclen` here is the last index in the transmission order, so the transmitted
    // count is hclen + 1 and HCLEN is hclen - 3. At least four codes are always
    // transmitted, so the value is never negative.
    int hclen = 18;
    while (hclen >= 0 && cl_lengths[static_cast<std::size_t>(cl_order[hclen])] == 0) {
        --hclen;
    }
    if (hclen < 3) {
        hclen = 3;
    }

    writer.put_bits(1, 1); // BFINAL
    writer.put_bits(2, 2); // BTYPE = dynamic
    writer.put_bits(static_cast<u32>(max_litlen + 1 - 257), 5);
    writer.put_bits(static_cast<u32>(max_dist + 1 - 1), 5);
    writer.put_bits(static_cast<u32>(hclen - 3), 4);
    for (int i = 0; i <= hclen; ++i) {
        writer.put_bits(cl_lengths[static_cast<std::size_t>(cl_order[i])], 3);
    }

    huffman_table cl_table;
    if (!build_huffman(cl_table, cl_lengths)) {
        return rpc_code::encode_error;
    }
    const huffman_encoder cl_encoder = make_encoder(cl_table, 19);
    for (const code_length_item& item : items) {
        writer.put_code(cl_encoder.codes[item.symbol], cl_encoder.lengths[item.symbol]);
        if (item.extra_bits != 0) {
            writer.put_bits(item.extra, item.extra_bits);
        }
    }

    huffman_table litlen_table;
    huffman_table dist_table;
    if (!build_huffman(litlen_table, litlen_lengths) ||
        !build_huffman(dist_table, dist_lengths)) {
        return rpc_code::encode_error;
    }
    const huffman_encoder litlen_encoder = make_encoder(litlen_table, 288);
    const huffman_encoder dist_encoder = make_encoder(dist_table, 30);

    for (const lz77_token& token : tokens) {
        writer.put_code(litlen_encoder.codes[token.litlen], litlen_encoder.lengths[token.litlen]);
        if (token.litlen >= 257) {
            const int length_index = token.litlen - 257;
            writer.put_bits(static_cast<u32>(token.length) -
                                static_cast<u32>(length_base[length_index]),
                            length_extra[length_index]);
            writer.put_code(dist_encoder.codes[token.distance],
                            dist_encoder.lengths[token.distance]);
            writer.put_bits(static_cast<u32>(token.dist_value) -
                                static_cast<u32>(dist_base[token.distance]),
                            dist_extra[token.distance]);
        }
    }
    writer.put_code(litlen_encoder.codes[256], litlen_encoder.lengths[256]);
    return writer.ok() ? status() : rpc_code::encode_error;
}

[[nodiscard]] status write_stored_block(bit_writer& writer, span<const std::byte> in) noexcept {
    const std::size_t size = in.size();
    std::size_t position = 0;
    while (position < size) {
        const std::size_t block = std::min<std::size_t>(size - position, 65535);
        const bool final = (position + block == size);
        writer.put_bits(final ? 1u : 0u, 1);
        writer.put_bits(0, 2);
        writer.align_to_byte();
        writer.put_bits(static_cast<u32>(block) & 0xFFFFu, 16);
        writer.put_bits((~static_cast<u32>(block)) & 0xFFFFu, 16);
        for (std::size_t i = 0; i < block; ++i) {
            writer.put_bits(static_cast<u32>(static_cast<u8>(in[position + i])), 8);
        }
        position += block;
    }
    return writer.ok() ? status() : rpc_code::encode_error;
}

[[nodiscard]] std::size_t stored_block_size(std::size_t size) noexcept {
    const std::size_t blocks = (size + 65534) / 65535;
    return size + blocks * 5;
}

// --- Decoder -----------------------------------------------------------------

[[nodiscard]] bool read_extra(bit_reader& reader, int bits, u32& out) noexcept {
    if (bits == 0) {
        out = 0;
        return true;
    }
    return reader.read_bits(bits, out);
}

[[nodiscard]] bool read_dynamic_tables(bit_reader& reader, huffman_table& litlen_table,
                                       huffman_table& dist_table) noexcept {
    u32 hlit = 0;
    u32 hdist = 0;
    u32 hclen = 0;
    if (!reader.read_bits(5, hlit) || !reader.read_bits(5, hdist) ||
        !reader.read_bits(4, hclen)) {
        return false;
    }
    const int num_litlen = static_cast<int>(hlit) + 257;
    const int num_dist = static_cast<int>(hdist) + 1;
    const int num_cl = static_cast<int>(hclen) + 4;
    if (num_litlen < 257 || num_litlen > 288 || num_dist < 1 || num_dist > 32 ||
        num_cl < 4 || num_cl > 19) {
        return false;
    }

    std::vector<u8> cl_lengths(19, 0);
    for (int i = 0; i < num_cl; ++i) {
        u32 value = 0;
        if (!reader.read_bits(3, value)) {
            return false;
        }
        cl_lengths[static_cast<std::size_t>(cl_order[i])] = static_cast<u8>(value);
    }
    huffman_table cl_table;
    if (!build_huffman(cl_table, cl_lengths)) {
        return false;
    }

    const int total = num_litlen + num_dist;
    std::vector<u8> lengths(static_cast<std::size_t>(total), 0);
    int index = 0;
    while (index < total) {
        const int symbol = decode_symbol(reader, cl_table);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[static_cast<std::size_t>(index++)] = static_cast<u8>(symbol);
        } else if (symbol == 16) {
            if (index == 0) {
                return false;
            }
            u32 extra = 0;
            if (!reader.read_bits(2, extra)) {
                return false;
            }
            const int repeat = 3 + static_cast<int>(extra);
            if (index + repeat > total) {
                return false;
            }
            const u8 previous = lengths[static_cast<std::size_t>(index - 1)];
            for (int k = 0; k < repeat; ++k) {
                lengths[static_cast<std::size_t>(index++)] = previous;
            }
        } else if (symbol == 17) {
            u32 extra = 0;
            if (!reader.read_bits(3, extra)) {
                return false;
            }
            const int repeat = 3 + static_cast<int>(extra);
            if (index + repeat > total) {
                return false;
            }
            for (int k = 0; k < repeat; ++k) {
                lengths[static_cast<std::size_t>(index++)] = 0;
            }
        } else if (symbol == 18) {
            u32 extra = 0;
            if (!reader.read_bits(7, extra)) {
                return false;
            }
            const int repeat = 11 + static_cast<int>(extra);
            if (index + repeat > total) {
                return false;
            }
            for (int k = 0; k < repeat; ++k) {
                lengths[static_cast<std::size_t>(index++)] = 0;
            }
        } else {
            return false;
        }
    }

    std::vector<u8> litlen_lengths(lengths.begin(), lengths.begin() + num_litlen);
    std::vector<u8> dist_lengths(lengths.begin() + num_litlen, lengths.end());
    return build_huffman(litlen_table, litlen_lengths) &&
           build_huffman(dist_table, dist_lengths);
}

[[nodiscard]] bool inflate_block(bit_reader& reader, const huffman_table& litlen_table,
                                 const huffman_table& dist_table, span<std::byte> out,
                                 std::size_t& produced, std::size_t expected_size) noexcept {
    for (;;) {
        const int symbol = decode_symbol(reader, litlen_table);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            if (produced >= expected_size) {
                return false;
            }
            out[produced++] = static_cast<std::byte>(static_cast<u8>(symbol));
        } else if (symbol == 256) {
            return true;
        } else if (symbol <= 285) {
            const int length_index = symbol - 257;
            u32 extra = 0;
            if (!read_extra(reader, length_extra[length_index], extra)) {
                return false;
            }
            const std::size_t length =
                static_cast<std::size_t>(length_base[length_index]) + extra;
            if (produced + length > expected_size) {
                return false;
            }
            const int distance_symbol = decode_symbol(reader, dist_table);
            if (distance_symbol < 0 || distance_symbol > 29) {
                return false;
            }
            u32 distance_extra = 0;
            if (!read_extra(reader, dist_extra[distance_symbol], distance_extra)) {
                return false;
            }
            const std::size_t distance =
                static_cast<std::size_t>(dist_base[distance_symbol]) + distance_extra;
            if (distance == 0 || distance > produced) {
                return false;
            }
            const std::byte* source = out.data() + (produced - distance);
            for (std::size_t i = 0; i < length; ++i) {
                out[produced + i] = source[i];
            }
            produced += length;
        } else {
            return false;
        }
    }
}

} // namespace

std::size_t deflate_stream_bound(std::size_t plain_size) noexcept {
    const std::size_t dynamic = plain_size + plain_size / 8 + 1024;
    const std::size_t stored = stored_block_size(plain_size);
    return std::max(dynamic, stored);
}

status deflate_compress(span<const std::byte> in, span<std::byte> out,
                        std::size_t& written) noexcept {
    written = 0;
    if (in.empty()) {
        return rpc_code::protocol_error;
    }

    std::vector<lz77_token> tokens;
    lz77_tokenize(in, tokens);

    {
        bit_writer writer(out);
        const status result = write_dynamic_block(writer, tokens);
        if (result.ok()) {
            const std::size_t dynamic_size = writer.written();
            if (dynamic_size <= stored_block_size(in.size())) {
                written = dynamic_size;
                return status();
            }
        }
    }

    bit_writer writer(out);
    const status result = write_stored_block(writer, in);
    if (!result.ok()) {
        written = 0;
        return result;
    }
    written = writer.written();
    return status();
}

status deflate_decompress(span<const std::byte> in, std::size_t expected_size,
                          span<std::byte> out, std::size_t& written,
                          std::size_t* consumed) noexcept {
    written = 0;
    if (expected_size == 0 || expected_size > out.size()) {
        return rpc_code::protocol_error;
    }
    if (in.empty()) {
        return rpc_code::protocol_error;
    }

    bit_reader reader(in);
    std::size_t produced = 0;
    bool final = false;
    while (!final) {
        final = reader.read_bit() != 0;
        if (!reader.ok()) {
            return rpc_code::protocol_error;
        }
        u32 block_type = 0;
        if (!reader.read_bits(2, block_type)) {
            return rpc_code::protocol_error;
        }

        if (block_type == 0) {
            reader.align_to_byte();
            u32 length = 0;
            u32 complement = 0;
            if (!reader.read_bits(16, length) || !reader.read_bits(16, complement)) {
                return rpc_code::protocol_error;
            }
            if ((length ^ 0xFFFFu) != complement) {
                return rpc_code::protocol_error;
            }
            if (produced + length > expected_size) {
                return rpc_code::protocol_error;
            }
            for (u32 i = 0; i < length; ++i) {
                u32 byte = 0;
                if (!reader.read_bits(8, byte)) {
                    return rpc_code::protocol_error;
                }
                out[produced++] = static_cast<std::byte>(static_cast<u8>(byte));
            }
        } else if (block_type == 1) {
            std::vector<u8> litlen_lengths(288, 0);
            for (int i = 0; i <= 143; ++i) {
                litlen_lengths[static_cast<std::size_t>(i)] = 8;
            }
            for (int i = 144; i <= 255; ++i) {
                litlen_lengths[static_cast<std::size_t>(i)] = 9;
            }
            for (int i = 256; i <= 279; ++i) {
                litlen_lengths[static_cast<std::size_t>(i)] = 7;
            }
            for (int i = 280; i <= 287; ++i) {
                litlen_lengths[static_cast<std::size_t>(i)] = 8;
            }
            std::vector<u8> dist_lengths(32, 5);
            huffman_table litlen_table;
            huffman_table dist_table;
            if (!build_huffman(litlen_table, litlen_lengths) ||
                !build_huffman(dist_table, dist_lengths)) {
                return rpc_code::protocol_error;
            }
            if (!inflate_block(reader, litlen_table, dist_table, out, produced, expected_size)) {
                return rpc_code::protocol_error;
            }
        } else if (block_type == 2) {
            huffman_table litlen_table;
            huffman_table dist_table;
            if (!read_dynamic_tables(reader, litlen_table, dist_table)) {
                return rpc_code::protocol_error;
            }
            if (!inflate_block(reader, litlen_table, dist_table, out, produced, expected_size)) {
                return rpc_code::protocol_error;
            }
        } else {
            return rpc_code::protocol_error;
        }
    }

    if (produced != expected_size) {
        return rpc_code::protocol_error;
    }
    if (consumed != nullptr) {
        *consumed = reader.bytes_consumed();
    }
    written = produced;
    return status();
}

} // namespace detail
} // namespace crpc
