// CRPC -- HTTP Content-Encoding negotiation and streaming gzip/deflate.
//
// Request side: `plan_request_decoding` classifies a request `Content-Encoding`
// header. Exactly one layer is allowed; `gzip`, `deflate` and `identity` are
// supported, anything else is 415. `content_decoder` then decompresses the
// body incrementally (it never buffers the whole entity).
//
// Response side: `negotiate_response` parses `Accept-Encoding` with q-values
// and picks the best supported coding (gzip preferred, then deflate, then
// identity). `content_encoder` compresses incrementally: `start` emits the
// container header, every non-empty `write` flushes one complete deflate block
// so the bytes are usable immediately, and `finish` emits the final block and
// the checksum trailer.
//
// Bomb protection (all enforced while streaming, before bytes are handed to
// the caller):
//
//   max_input_bytes         8 MiB of compressed request body
//   max_decompressed_bytes 64 MiB of decoded output
//   max_compression_ratio  100:1 output/input (0 disables the ratio check)
//
// An exceeded limit fails with `rpc_code::resource_exhausted`; the HTTP layer
// maps it to 413 and closes the connection. A malformed compressed stream or a
// checksum mismatch fails with `rpc_code::decode_error`; a malformed container
// header fails with `rpc_code::protocol_error`. The decoder stops at the first
// failure and every later call returns the same error.
//
// Codec implementation: the decoder implements the full DEFLATE specification
// (stored, fixed and dynamic Huffman blocks, zlib and gzip wrappers) and is
// verified against vectors produced by zlib. The encoder emits fixed-Huffman
// blocks with a minimal LZ77 matcher (3-byte hash chains over a 32 KiB history
// ring, greedy matches of 3..258 bytes; matches may cross `write` boundaries)
// and is verified by decompressing its output with zlib. The output is a valid
// gzip/zlib stream, and repetitive input compresses well.
//
// Documented limitations (fail closed, never silent corruption):
//
//   * a gzip member's optional FHCRC (header CRC16) is consumed but not
//     verified;
//   * concatenated gzip members are not supported: any byte after the first
//     member's trailer is `protocol_error` (trailing data);
//   * `deflate` accepts a zlib-wrapped stream; when the two-byte header is not
//     a valid zlib header the decoder falls back to raw DEFLATE (the two bytes
//     are replayed as the start of the raw stream), which is what several HTTP
//     clients send;
//   * a zlib preset dictionary (FDICT) is `protocol_error`.
//
// Thread safety: one decoder/encoder per thread; no shared mutable state.
//
// Allocation: `content_decoder`/`content_encoder` allocate their state in the
// constructor; `feed`/`write` append to the caller's string and back-reference
// history is a fixed 32 KiB ring (no per-byte allocation). Every byte of a
// `write` enters that ring before the next call, so a match may safely span the
// boundary. Hash-chain positions are 32-bit: beyond 2 GiB of streamed input,
// matching degrades to literals (never corruption).
#ifndef CRPC_HTTP_CONTENT_CODING_HPP
#define CRPC_HTTP_CONTENT_CODING_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc::http {

enum class content_coding : std::uint8_t { identity, gzip, deflate };

[[nodiscard]] CRPC_API const char* to_string(content_coding coding) noexcept;
[[nodiscard]] CRPC_API bool content_coding_from_token(std::string_view token,
                                             content_coding& coding) noexcept;

struct coding_limits {
    std::size_t max_input_bytes = 8u * 1024u * 1024u;
    std::size_t max_decompressed_bytes = 64u * 1024u * 1024u;
    // Output/input ceiling. 0 disables the check.
    std::size_t max_compression_ratio = 100;
    // Responses smaller than this stay uncompressed.
    std::size_t min_response_bytes = 1024;
    // When true, "no coding acceptable" falls back to identity instead of 406.
    bool fallback_identity = false;
};

[[nodiscard]] CRPC_API const coding_limits& default_coding_limits() noexcept;

// --- Response negotiation ---------------------------------------------------

struct negotiation {
    content_coding coding = content_coding::identity;
    bool compressed = false;
    // False means the client accepted none of the supported codings and the
    // fallback is disabled: respond 406.
    bool acceptable = true;
    bool fallback_used = false;
    double quality = 1.0;
};

[[nodiscard]] CRPC_API negotiation negotiate_response(
    std::string_view accept_encoding,
    const coding_limits& limits = default_coding_limits()) noexcept;

[[nodiscard]] CRPC_API int negotiation_http_status(const negotiation& result) noexcept;

// True when the response body should be compressed: a coding was selected and
// the body is at least `min_response_bytes`.
[[nodiscard]] CRPC_API bool should_compress(
    const negotiation& result, std::size_t body_size,
    const coding_limits& limits = default_coding_limits()) noexcept;

// --- Request decoding -------------------------------------------------------

struct CRPC_API request_encoding_plan {
    bool ok = true;
    bool encoded = false;
    content_coding coding = content_coding::identity;
    // Authoritative HTTP result: 200 when ok, 415 when the coding is
    // unsupported or layered. `error()` carries an `invalid_argument` rpc_error
    // for callers that log through the rpc_error model.
    int http_status = 200;
    std::string message;

    [[nodiscard]] rpc_error error() const;
};

[[nodiscard]] CRPC_API request_encoding_plan plan_request_decoding(
    std::string_view content_encoding);

// --- Streaming decoder ------------------------------------------------------

class CRPC_API content_decoder {
public:
    content_decoder() noexcept;

    explicit content_decoder(content_coding coding,
                             const coding_limits& limits = default_coding_limits());

    content_decoder(const content_decoder&) = delete;
    content_decoder& operator=(const content_decoder&) = delete;
    content_decoder(content_decoder&& other) noexcept;
    content_decoder& operator=(content_decoder&& other) noexcept;
    ~content_decoder();

    // Consumes compressed bytes and appends decoded bytes to `out`. On the
    // first failure the decoder latches the error.
    [[nodiscard]] status feed(span<const std::byte> data, std::string& out);

    // Signals end of input. Returns `decode_error` when the stream is
    // truncated. Idempotent after success.
    [[nodiscard]] status finish(std::string& out);

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::size_t input_bytes() const noexcept;
    [[nodiscard]] std::size_t output_bytes() const noexcept;
    [[nodiscard]] content_coding coding() const noexcept;
    [[nodiscard]] const rpc_error& error() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// --- Streaming encoder ------------------------------------------------------

class CRPC_API content_encoder {
public:
    content_encoder() noexcept;

    explicit content_encoder(content_coding coding,
                             const coding_limits& limits = default_coding_limits());

    content_encoder(const content_encoder&) = delete;
    content_encoder& operator=(const content_encoder&) = delete;
    content_encoder(content_encoder&& other) noexcept;
    content_encoder& operator=(content_encoder&& other) noexcept;
    ~content_encoder();

    // Emits the container header (gzip/zlib) or nothing for identity.
    [[nodiscard]] status start(std::string& out);

    // Compresses one chunk and flushes complete bytes to `out`.
    [[nodiscard]] status write(span<const std::byte> data, std::string& out);

    // Emits the final block and checksum. Idempotent.
    [[nodiscard]] status finish(std::string& out);

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] content_coding coding() const noexcept;
    [[nodiscard]] const rpc_error& error() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// --- One-shot helpers -------------------------------------------------------

[[nodiscard]] CRPC_API status compress_all(
    content_coding coding, span<const std::byte> data, std::string& out,
    const coding_limits& limits = default_coding_limits());

[[nodiscard]] CRPC_API result<std::string> decompress_all(
    content_coding coding, span<const std::byte> data,
    const coding_limits& limits = default_coding_limits());

} // namespace crpc::http

#endif // CRPC_HTTP_CONTENT_CODING_HPP
