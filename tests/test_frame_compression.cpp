// CRPC -- tests for frame-level compression (G29a).
//
// Commit 1 of the slice: only the cases that fail against the current tree
// using the existing API. They pin the defect that motivates the feature: the
// frame layer treats the COMPRESSED flag as a fact, so encode_frame writes a
// "compressed" frame containing plaintext and the decoders deliver that
// plaintext (or garbage) as a payload instead of failing closed.
//
// RED-1 encode_frame accepts COMPRESSED|ALGO and writes the plaintext.
// RED-2 decode_frame accepts the same frame and hands back the plaintext.
// RED-3 the incremental decoder (borrowing mode) reports frame_ready for it.
// RED-4 ALGO=zstd (2) is accepted although no such codec exists.
// RED-5 a COMPRESSED payload of arbitrary garbage is accepted.
// RED-6 a COMPRESSED PING with an 8-byte payload is accepted, which lets the
//       client echo the compressed bytes back as a plaintext PONG.
//
// Every expectation below is the post-fix behaviour: encode_frame rejects the
// flags with invalid_argument, decode_frame and the borrowing decoder reject
// the frame with protocol_error, and decode_frame_header keeps accepting the
// header (it validates flag consistency only).
#include <cstring>
#include <vector>

#include <crpc/frame.hpp>
#include <crpc/wire.hpp>

#include "test_framework.hpp"

using namespace crpc;

namespace {

std::vector<std::byte> bytes_of(const char* text) {
    const std::size_t size = std::strlen(text);
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

span<const std::byte> as_span(const std::vector<std::byte>& bytes) {
    return span<const std::byte>(bytes.data(), bytes.size());
}

span<std::byte> as_writable_span(std::vector<std::byte>& bytes) {
    return span<std::byte>(bytes.data(), bytes.size());
}

std::vector<std::byte> raw_header(u32 payload_len, u8 type, u8 flags, u16 reserved,
                                  u32 stream_id, u32 ext) {
    std::vector<std::byte> out(frame_header_size);
    byte_writer writer(as_writable_span(out));
    writer.put_u32(payload_len);
    writer.put_u8(type);
    writer.put_u8(flags);
    writer.put_u16(reserved);
    writer.put_u32(stream_id);
    writer.put_u32(ext);
    CRPC_CHECK(writer.ok());
    return out;
}

std::vector<std::byte> raw_frame(u32 payload_len, u8 type, u8 flags, u32 stream_id,
                                 const std::vector<std::byte>& payload) {
    std::vector<std::byte> out = raw_header(payload_len, type, flags, 0, stream_id, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

u8 lz4_flags() { return static_cast<u8>(frame_flag_compressed | 0x20u); }
u8 zstd_flags() { return static_cast<u8>(frame_flag_compressed | 0x40u); }

// An 8-byte payload that is not an LZ4 stream of 8 bytes: it claims a 3-byte
// match at offset 0x4141, which is invalid.
std::vector<std::byte> compressed_ping_payload() {
    return {std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x41}, std::byte{0x41}, std::byte{0x00}};
}

} // namespace

CRPC_TEST(red1_encode_frame_rejects_compressed_flag) {
    const std::vector<std::byte> plain = bytes_of("the frame layer must not claim a stream it did not write");
    frame value;
    value.header.type = frame_type::request;
    value.header.stream_id = 1;
    value.header.flags = lz4_flags();
    value.payload = as_span(plain);

    std::vector<std::byte> out(encoded_frame_size(value));
    std::size_t written = 0;
    const status result = encode_frame(as_writable_span(out), value, written);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::invalid_argument));
    CRPC_CHECK_EQ(written, std::size_t{0});
}

CRPC_TEST(red2_decode_frame_rejects_compressed_frame) {
    const std::vector<std::byte> plain = bytes_of("plaintext wearing a COMPRESSED flag");
    const std::vector<std::byte> wire = raw_frame(static_cast<u32>(plain.size()),
                                                  static_cast<u8>(frame_type::request),
                                                  lz4_flags(), 1, plain);
    frame decoded;
    const status result = decode_frame(as_span(wire), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(red3_borrowing_decoder_rejects_compressed_frame) {
    const std::vector<std::byte> plain = bytes_of("borrowing mode has nowhere to decompress into");
    const std::vector<std::byte> wire = raw_frame(static_cast<u32>(plain.size()),
                                                  static_cast<u8>(frame_type::request),
                                                  lz4_flags(), 1, plain);
    frame_decoder decoder;
    decoder.feed(as_span(wire));
    frame decoded;
    const frame_decoder::result result = decoder.next(decoded);
    CRPC_CHECK_EQ(static_cast<int>(result), static_cast<int>(frame_decoder::result::error));
    CRPC_CHECK_EQ(static_cast<int>(decoder.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(red4_zstd_is_not_a_supported_codec) {
    const std::vector<std::byte> plain = bytes_of("zstd is reserved and must fail closed");
    const std::vector<std::byte> wire = raw_frame(static_cast<u32>(plain.size()),
                                                  static_cast<u8>(frame_type::request),
                                                  zstd_flags(), 1, plain);
    frame decoded;
    const status result = decode_frame(as_span(wire), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(red5_garbage_compressed_payload_is_rejected) {
    const std::vector<std::byte> garbage = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}};
    const std::vector<std::byte> wire = raw_frame(static_cast<u32>(garbage.size()),
                                                  static_cast<u8>(frame_type::data),
                                                  lz4_flags(), 3, garbage);
    frame decoded;
    const status result = decode_frame(as_span(wire), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(red6_compressed_ping_is_rejected_before_echo) {
    const std::vector<std::byte> payload = compressed_ping_payload();
    const std::vector<std::byte> wire = raw_frame(static_cast<u32>(payload.size()),
                                                  static_cast<u8>(frame_type::ping),
                                                  lz4_flags(), 0, payload);

    // The header itself stays acceptable: decode_frame_header only checks flag
    // consistency, and a compressed header must remain inspectable.
    frame_header header;
    CRPC_CHECK(decode_frame_header(as_span(wire), header).ok());
    CRPC_CHECK_EQ(static_cast<int>(frame_compression(header)),
                  static_cast<int>(compression_algorithm::lz4));

    // The whole-frame path rejects it, so the ping echo path can never see it.
    frame decoded;
    const status result = decode_frame(as_span(wire), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST_MAIN()
