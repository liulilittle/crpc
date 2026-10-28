// CRPC -- internal helpers shared by the MSMQ implementation units.
//
// Not part of the public API. Windows-only; the declarations exist only under
// CRPC_OS_WINDOWS so a non-Windows build never references them.
#ifndef CRPC_MQ_MSMQ_INTERNAL_HPP
#define CRPC_MQ_MSMQ_INTERNAL_HPP

#include <crpc/config.hpp>

#if defined(CRPC_OS_WINDOWS)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <windows.h>

#include <crpc/error.hpp>
#include <crpc/mq/msmq/types.hpp>

namespace crpc {
namespace mq {
namespace msmq {
namespace detail {

// Converts a UTF-8 string to a NULL-terminated UTF-16 buffer for the wide MSMQ
// entry points. An unconvertible byte sequence yields an empty string.
[[nodiscard]] std::wstring to_wide(std::string_view text);

// Converts a NFULL-terminated UTF-16 buffer of `length` WCHARs to UTF-8.
[[nodiscard]] std::string to_utf8(const wchar_t* text, std::size_t length);

// ASCII case-insensitive prefix test; used to recognise an already-resolved
// format name ("FORMATNAME:", "DIRECT=").
[[nodiscard]] bool starts_with_ci(std::string_view text,
                                  std::string_view prefix) noexcept;

// "0x%08lX" form used for the human-readable message and the `hr` metadata key.
[[nodiscard]] std::string hex_of(std::uint32_t value);

// Builds a typed rpc_error for an MSMQ HRESULT. The raw code is kept in the
// `hr` metadata so a caller can match against the MSMQ documentation.
[[nodiscard]] rpc_error mq_error(const char* what, HRESULT hr);

} // namespace detail
} // namespace msmq
} // namespace mq
} // namespace crpc

#endif // defined(CRPC_OS_WINDOWS)

#endif // CRPC_MQ_MSMQ_INTERNAL_HPP