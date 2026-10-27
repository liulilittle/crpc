// CRPC -- base64, both directions.
//
// It lives in `crpc::crypto` rather than in one protocol's namespace because several
// protocols need it (SCRAM authentication, HTTP `data:` URIs, PEM framing), and a second
// copy of the same alphabet table is exactly the kind of duplication that drifts.
#ifndef CRPC_CRYPTO_BASE64_HPP
#define CRPC_CRYPTO_BASE64_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace crypto {

// Standard base64 with padding.
[[nodiscard]] CRPC_API std::string base64_encode(span<const std::byte> data);
[[nodiscard]] CRPC_API std::string base64_encode(std::string_view text);

// Decodes standard base64. Whitespace is skipped, which is what PEM needs; any other
// character outside the alphabet is an error, and so is a length that cannot be valid.
// A malformed input is refused rather than partially decoded: half a signature verifies
// nothing, and silently returning it would look like an authentication failure.
[[nodiscard]] CRPC_API result<std::string> base64_decode(std::string_view text);

} // namespace crypto
} // namespace crpc

#endif // CRPC_CRYPTO_BASE64_HPP