// CRPC -- LZ4 block codec (G29a).
//
// This is the LZ4 *block* format, not the LZ4 frame format: there is no magic
// number, no frame descriptor and no frame checksum. The container defined by
// docs/protocol.md section 24 supplies the logical length and the integrity
// check, which is exactly the `store_size=True` convention of the reference
// implementation:
//
//   payload := uncompressed_len u32 LE | lz4 block stream
//
// Encoding constraints kept for third-party decoders:
//   * the last 5 bytes of the input are always literals;
//   * a match starts at least 12 bytes before the end of the input;
//   * a match distance never exceeds 65535.
//
// Decoding rejects, with rpc_code::protocol_error, every structurally invalid
// stream: a truncated token or extension, a zero offset, an offset beyond the
// bytes already produced, literals or matches that run past the input or past
// the declared length, and a stream that does not produce exactly the declared
// length. Nothing here allocates and nothing here reads or writes outside the
// spans it is given.
#include <array>
#include <cstring>

#include <crpc/error.hpp>
#include <crpc/frame_compression.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace detail {
namespace {

constexpr std::size_t lz4_hash_bits = 12;
constexpr std::size_t lz4_hash_size = std::size_t{1} << lz4_hash_bits;
constexpr std::size_t lz4_hash_shift = 32 - lz4_hash_bits;
constexpr u32 lz4_hash_multiplier = 2654435761u;
constexpr std::size_t lz4_min_match = 4;
constexpr std::size_t lz4_last_literals = 5;
constexpr std::size_t lz4_match_find_limit = 12;
constexpr std::size_t lz4_max_distance = 65535;

// The hash output is masked to the table width. The static assertion keeps the
// mask honest if the width is ever changed; without the mask a wider hash would
// index past the table.
static_assert(lz4_hash_size != 0 && (lz4_hash_size & (lz4_hash_size - 1)) == 0,
              "the LZ4 hash table width must be a power of two");

// Scratch for the encoder: a fixed-size table, reused per thread and zeroed per
// call. It is a std::array (not a std::vector) so its size is the declared one
// - `std::vector<T> table{n, v}` would be a two-element initializer_list - and
// so that no allocation happens per frame. Entries store position + 1, so zero
// means "empty".
std::array<std::uint32_t, lz4_hash_size>& lz4_hash_table() noexcept {
    thread_local std::array<std::uint32_t, lz4_hash_size> table{};
    return table;
}

u32 load_u32(const std::byte* data) noexcept {
    u32 value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<u32>(static_cast<u8>(data[i])) << (8u * i);
    }
    return value;
}

std::size_t hash_sequence(u32 sequence) noexcept {
    return static_cast<std::size_t>((sequence * lz4_hash_multiplier) >> lz4_hash_shift) &
           (lz4_hash_size - 1);
}

// Bounds-checked byte writer over the caller's output span.
class block_writer {
public:
    explicit block_writer(span<std::byte> out) noexcept : out_(out) {}

    bool ok() const noexcept { return ok_; }
    std::size_t written() const noexcept { return pos_; }

    void put_u8(u8 value) noexcept {
        if (!ok_ || pos_ >= out_.size()) {
            ok_ = false;
            return;
        }
        out_[pos_++] = static_cast<std::byte>(value);
    }

    void put_bytes(span<const std::byte> bytes) noexcept {
        if (!ok_ || bytes.size() > out_.size() - pos_) {
            ok_ = false;
            return;
        }
        if (!bytes.empty()) {
            std::memcpy(out_.data() + pos_, bytes.data(), bytes.size());
        }
        pos_ += bytes.size();
    }

private:
    span<std::byte> out_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// LZ4 length extension: the caller already emitted the 15 nibble.
void put_length(block_writer& writer, std::size_t remainder) noexcept {
    while (remainder >= 255) {
        writer.put_u8(255);
        remainder -= 255;
    }
    writer.put_u8(static_cast<u8>(remainder));
}

void put_last_literals(block_writer& writer, span<const std::byte> in,
                       std::size_t anchor) noexcept {
    const std::size_t length = in.size() - anchor;
    if (length >= 15) {
        writer.put_u8(0xF0);
        put_length(writer, length - 15);
    } else {
        writer.put_u8(static_cast<u8>(length << 4));
    }
    writer.put_bytes(in.subspan(anchor, length));
}

} // namespace

// Compresses `in` into `out`. `out` must be at least
// compressed_payload_bound(compression_algorithm::lz4, in.size()) bytes; a
// smaller buffer is rpc_code::encode_error.
status lz4_block_compress(span<const std::byte> in, span<std::byte> out,
                          std::size_t& written) noexcept {
    written = 0;
    block_writer writer(out);
    const std::size_t size = in.size();

    if (size < lz4_match_find_limit) {
        put_last_literals(writer, in, 0);
        if (!writer.ok()) {
            return rpc_code::encode_error;
        }
        written = writer.written();
        return status();
    }

    std::array<std::uint32_t, lz4_hash_size>& table = lz4_hash_table();
    table.fill(0);

    const std::size_t match_find_limit = size - lz4_match_find_limit;
    const std::size_t match_limit = size - lz4_last_literals;
    std::size_t anchor = 0;
    std::size_t position = 0;

    while (position <= match_find_limit) {
        const u32 sequence = load_u32(in.data() + position);
        const std::size_t slot = hash_sequence(sequence);
        const u32 previous = table[slot];
        table[slot] = static_cast<u32>(position + 1);

        if (previous != 0) {
            const std::size_t candidate = static_cast<std::size_t>(previous - 1);
            if (position - candidate <= lz4_max_distance &&
                load_u32(in.data() + candidate) == sequence) {
                std::size_t match_length = lz4_min_match;
                while (position + match_length < match_limit &&
                       in[position + match_length] == in[candidate + match_length]) {
                    ++match_length;
                }

                const std::size_t literal_length = position - anchor;
                const std::size_t match_code = match_length - lz4_min_match;
                const u8 literal_nibble =
                    literal_length >= 15 ? u8{15} : static_cast<u8>(literal_length);
                const u8 match_nibble =
                    match_code >= 15 ? u8{15} : static_cast<u8>(match_code);
                writer.put_u8(static_cast<u8>((literal_nibble << 4) | match_nibble));
                if (literal_length >= 15) {
                    put_length(writer, literal_length - 15);
                }
                writer.put_bytes(in.subspan(anchor, literal_length));
                const std::size_t distance = position - candidate;
                writer.put_u8(static_cast<u8>(distance & 0xFFu));
                writer.put_u8(static_cast<u8>((distance >> 8) & 0xFFu));
                if (match_code >= 15) {
                    put_length(writer, match_code - 15);
                }

                position += match_length;
                anchor = position;
                continue;
            }
        }
        ++position;
    }

    put_last_literals(writer, in, anchor);
    if (!writer.ok()) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status();
}

// Decompresses `in` into `out`. `expected_size` is the declared logical length
// and must not exceed `out.size()`; the caller checks both before calling.
status lz4_block_decompress(span<const std::byte> in, std::size_t expected_size,
                            span<std::byte> out, std::size_t& written) noexcept {
    written = 0;
    if (expected_size == 0 || expected_size > out.size()) {
        return rpc_code::protocol_error;
    }
    if (in.empty()) {
        return rpc_code::protocol_error;
    }

    const std::size_t input_size = in.size();
    std::size_t ip = 0;
    std::size_t op = 0;

    for (;;) {
        if (ip >= input_size) {
            return rpc_code::protocol_error;
        }
        const u8 token = static_cast<u8>(in[ip++]);

        std::size_t literal_length = static_cast<std::size_t>(token >> 4);
        if (literal_length == 15) {
            for (;;) {
                if (ip >= input_size) {
                    return rpc_code::protocol_error;
                }
                const u8 extension = static_cast<u8>(in[ip++]);
                literal_length += static_cast<std::size_t>(extension);
                if (literal_length > expected_size - op) {
                    return rpc_code::protocol_error;
                }
                if (extension != 255) {
                    break;
                }
            }
        }
        if (literal_length > expected_size - op || literal_length > input_size - ip) {
            return rpc_code::protocol_error;
        }
        if (literal_length != 0) {
            std::memcpy(out.data() + op, in.data() + ip, literal_length);
        }
        ip += literal_length;
        op += literal_length;

        // A sequence with no offset ends the block: the remaining bytes are the
        // last literals. Trailing bytes after them are a malformed stream.
        if (ip == input_size) {
            break;
        }
        if (input_size - ip < 2) {
            return rpc_code::protocol_error;
        }
        const std::size_t offset = static_cast<std::size_t>(static_cast<u8>(in[ip])) |
                                   (static_cast<std::size_t>(static_cast<u8>(in[ip + 1])) << 8);
        ip += 2;
        if (offset == 0 || offset > op) {
            return rpc_code::protocol_error;
        }

        std::size_t match_length = static_cast<std::size_t>(token & 0x0Fu);
        if (match_length == 15) {
            for (;;) {
                if (ip >= input_size) {
                    return rpc_code::protocol_error;
                }
                const u8 extension = static_cast<u8>(in[ip++]);
                match_length += static_cast<std::size_t>(extension);
                if (match_length + lz4_min_match > expected_size - op) {
                    return rpc_code::protocol_error;
                }
                if (extension != 255) {
                    break;
                }
            }
        }
        match_length += lz4_min_match;
        if (match_length > expected_size - op) {
            return rpc_code::protocol_error;
        }

        const std::byte* const source = out.data() + (op - offset);
        for (std::size_t i = 0; i < match_length; ++i) {
            out[op + i] = source[i];
        }
        op += match_length;
    }

    if (op != expected_size) {
        return rpc_code::protocol_error;
    }
    written = op;
    return status();
}

} // namespace detail
} // namespace crpc
