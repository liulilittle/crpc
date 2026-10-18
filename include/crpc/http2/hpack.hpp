// CRPC -- HPACK header compression (RFC 7541).
//
// Layout of this file:
//
//   * integer and string primitives (section 5);
//   * the static table (Appendix A, 61 entries);
//   * a size-accounted dynamic table (section 4);
//   * a canonical-Huffman decoder and encoder (Appendix B);
//   * an encoder and a decoder for whole header blocks (section 6).
//
// Huffman *decoding* implements the full RFC table. Huffman *encoding* is
// implemented too, but the encoder deliberately emits literal (non-Huffman,
// H=0) strings by default: literal strings are always correct, keep the encoder
// allocation-free on the hot path and avoid a dependency on the Huffman
// encoder for the common case. A caller that wants Huffman output can call
// huffman_encode directly. This choice is recorded as a limitation.
//
// The dynamic table is *not* shared between directions: an encoder's table and
// a decoder's table are independent contexts (RFC 7541 section 2.2).
//
// Thread safety: an encoder or decoder instance is single-threaded; the two
// directions of one connection are independent and never share an instance.
#ifndef CRPC_HTTP2_HPACK_HPP
#define CRPC_HTTP2_HPACK_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/span.hpp>

namespace crpc::http2 {

// A decoded or to-be-encoded header field. Names are lower-case on the h2 wire
// (RFC 7540 section 8.1.2); this type does not enforce that -- validation is a
// connection concern.
struct header_field {
    std::string name;
    std::string value;
};

// --- Static table ------------------------------------------------------------

inline constexpr std::size_t static_table_size = 61;

// Returns the entry at a 1-based static index (1..61), or nullptr when the
// index is out of range. The returned pointer is stable for the program's
// lifetime.
[[nodiscard]] CRPC_API const header_field* static_table_entry(std::size_t index) noexcept;

// Finds the first static entry whose name and value both match; returns its
// 1-based index or 0. The value comparison is exact.
[[nodiscard]] CRPC_API std::size_t static_find(std::string_view name,
                                               std::string_view value) noexcept;

// Finds the first static entry whose name matches; returns its 1-based index or
// 0. Used to emit a literal with an indexed name.
[[nodiscard]] CRPC_API std::size_t static_find_name(std::string_view name) noexcept;

// --- Integer primitives (RFC 7541 section 5.1) -------------------------------

// Encodes `value` with an N-bit prefix (`prefix_bits` in 1..8) into `out`.
// `pattern` supplies the high bits of the first octet that are not part of the
// prefix (for example 0x80 for an indexed field, 0x40 for incremental
// indexing, 0x00 for a name index). Failure is rpc_code::encode_error.
[[nodiscard]] CRPC_API status hpack_encode_integer(span<std::byte> out,
                                                   std::size_t& written,
                                                   u8 prefix_bits,
                                                   u8 pattern,
                                                   u64 value) noexcept;

// Decodes an N-bit-prefix integer starting at `in[0]`. `consumed` reports how
// many bytes were read and is always written. Over-long encodings (beyond 10
// continuation octets) and values that do not fit in 64 bits are rejected with
// rpc_code::protocol_error.
[[nodiscard]] CRPC_API status hpack_decode_integer(span<const std::byte> in,
                                                   std::size_t& consumed,
                                                   u8 prefix_bits,
                                                   u64& value) noexcept;

// --- String primitives (RFC 7541 section 5.2) --------------------------------

// Encodes a literal (H=0) string: a 7-bit prefix length followed by the raw
// octets. Failure is rpc_code::encode_error.
[[nodiscard]] CRPC_API status hpack_encode_string(span<std::byte> out,
                                                  std::size_t& written,
                                                  std::string_view value) noexcept;

// Decodes one string literal (H may be 0 or 1) starting at `in[0]`. `out` is
// cleared and replaced. Failure modes: truncation, an over-long length, or a
// Huffman decoding error -- all rpc_code::protocol_error.
[[nodiscard]] CRPC_API status hpack_decode_string(span<const std::byte> in,
                                                  std::size_t& consumed,
                                                  std::string& out) noexcept;

// --- Huffman (RFC 7541 Appendix B) -------------------------------------------

// Decodes a Huffman-encoded octet sequence. `out` is appended to. A code that
// is incomplete at the end is treated as EOS padding; padding longer than 7
// bits, padding that is not all ones, or an explicit EOS symbol are rejected
// with rpc_code::protocol_error.
[[nodiscard]] CRPC_API status huffman_decode(span<const std::byte> in,
                                             std::string& out) noexcept;

// Number of octets huffman_encode would produce for `value`.
[[nodiscard]] CRPC_API std::size_t huffman_encoded_size(std::string_view value) noexcept;

// Huffman-encodes `value` into `out`, padding the final octet with the EOS
// prefix (all ones). `out` must hold at least huffman_encoded_size(value)
// bytes. Failure is rpc_code::encode_error.
[[nodiscard]] CRPC_API status huffman_encode(span<std::byte> out,
                                             std::string_view value,
                                             std::size_t& written) noexcept;

// --- Dynamic table (RFC 7541 section 4) --------------------------------------

// FIFO table, newest entry at index 0. Size accounting is name.length +
// value.length + 32 per entry. `max_size` is the encoder-chosen size, which is
// itself bounded by the peer's SETTINGS_HEADER_TABLE_SIZE.
class CRPC_API dynamic_table {
public:
    explicit dynamic_table(std::size_t max_size = default_header_table_size);

    [[nodiscard]] std::size_t max_size() const noexcept { return max_size_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    // 0-based index into the dynamic table (0 = newest).
    [[nodiscard]] const header_field& at(std::size_t index) const noexcept {
        return entries_[index];
    }

    // Reduces the maximum size and evicts until size() <= max_size(). Growing
    // the maximum never evicts.
    void set_max_size(std::size_t max_size) noexcept;

    // Inserts a new entry, evicting from the tail as needed. An entry larger
    // than max_size empties the table and is itself not inserted.
    void insert(std::string_view name, std::string_view value);

    void clear() noexcept {
        entries_.clear();
        size_ = 0;
    }

    // Finds the newest entry matching name and value; returns its 0-based index
    // or count() when absent.
    [[nodiscard]] std::size_t find(std::string_view name, std::string_view value) const noexcept;

    // Finds the newest entry matching name only.
    [[nodiscard]] std::size_t find_name(std::string_view name) const noexcept;

private:
    std::vector<header_field> entries_;
    std::size_t size_ = 0;
    std::size_t max_size_;
};

// Computes the RFC entry size.
[[nodiscard]] constexpr std::size_t hpack_entry_size(std::string_view name,
                                                     std::string_view value) noexcept {
    return name.size() + value.size() + 32;
}

// --- Encoder -----------------------------------------------------------------

// Literal representation choice for a single field.
enum class hpack_indexing : u8 {
    incremental, // 0x40: add to the dynamic table
    without,     // 0x00: do not add
    never        // 0x10: do not add, and mark never-indexed
};

class CRPC_API hpack_encoder {
public:
    hpack_encoder();

    [[nodiscard]] dynamic_table& table() noexcept { return table_; }
    [[nodiscard]] const dynamic_table& table() const noexcept { return table_; }

    // Requests a dynamic table size update. Only the smallest value requested
    // since the last block and the final value are emitted, exactly as RFC 7541
    // section 4.2 requires. The table is evicted when the update is emitted.
    void set_max_table_size(std::size_t size) noexcept;

    // Encodes a whole header list, using incremental indexing for every field
    // and literal (H=0) strings. Exact static/dynamic matches become indexed
    // representations. Appends to `out`.
    [[nodiscard]] status encode(span<const header_field> headers, std::string& out);

    // Encodes a single field with an explicit representation. `mode` selects
    // incremental / without-indexing / never-indexed.
    [[nodiscard]] status encode_field(std::string_view name, std::string_view value,
                                      hpack_indexing mode, std::string& out);

private:
    [[nodiscard]] status encode_name_index(u32 index, u8 prefix_bits, u8 pattern,
                                           std::string& out);

    dynamic_table table_;
    bool pending_update_ = false;
    std::size_t pending_min_ = 0;
    std::size_t pending_final_ = 0;
};

// --- Decoder -----------------------------------------------------------------

class CRPC_API hpack_decoder {
public:
    explicit hpack_decoder(std::size_t max_table_size = default_header_table_size);

    [[nodiscard]] dynamic_table& table() noexcept { return table_; }
    [[nodiscard]] const dynamic_table& table() const noexcept { return table_; }

    // The largest dynamic table size the peer may select (our
    // SETTINGS_HEADER_TABLE_SIZE). A size update above this is a compression
    // error. Lowering it also evicts locally if the current table is larger.
    void set_peer_table_limit(std::size_t limit) noexcept;
    [[nodiscard]] std::size_t peer_table_limit() const noexcept { return peer_table_limit_; }

    // Maximum total decoded header list size (sum over fields of
    // hpack_entry_size). Exceeding it fails with rpc_code::resource_exhausted.
    void set_max_header_list_size(std::size_t size) noexcept { max_header_list_size_ = size; }
    [[nodiscard]] std::size_t max_header_list_size() const noexcept { return max_header_list_size_; }

    // Decodes a complete header block, appending fields to `out`. The block is
    // read from a caller buffer; strings are assembled in a reused scratch
    // buffer so steady-state decoding does no per-field allocation beyond the
    // destination. All failures are rpc_code::protocol_error except an
    // over-limit list (resource_exhausted).
    [[nodiscard]] status decode(span<const std::byte> block, http::header_list& out);

    // Same, into a caller vector (used by tests and non-HTTP adapters).
    [[nodiscard]] status decode(span<const std::byte> block, std::vector<header_field>& out);

private:
    dynamic_table table_;
    std::size_t peer_table_limit_;
    std::size_t max_header_list_size_ = default_max_header_list_size;
    std::string scratch_name_;
    std::string scratch_value_;
};

} // namespace crpc::http2

#endif // CRPC_HTTP2_HPACK_HPP
