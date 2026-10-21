// CRPC -- frame codec internals shared by the plain and compressed encoders.
//
// `validate_header` is the single definition of every semantic header rule
// (known type, stream scope, compression flag/algorithm agreement, ext only
// with CRC32C, ONE_WAY requires UNRELIABLE, payload_len bound). The plain
// encoder, the decoder and the compressed encoder must all agree on it: a
// second copy would drift and let one entry point emit a frame the other
// refuses. Declared here rather than in the public headers so that it stays an
// implementation detail.
#ifndef CRPC_FRAME_INTERNAL_HPP
#define CRPC_FRAME_INTERNAL_HPP

#include <crpc/frame.hpp>

namespace crpc {
namespace detail {

// Returns rpc_code::ok or rpc_code::protocol_error.
[[nodiscard]] rpc_code validate_header(const frame_header& header, u32 max_frame) noexcept;

// Maps a header-validation verdict onto the encoder's error domain
// (protocol_error -> invalid_argument); every other code passes through.
[[nodiscard]] rpc_code encode_error_for(rpc_code code) noexcept;

} // namespace detail
} // namespace crpc

#endif // CRPC_FRAME_INTERNAL_HPP
