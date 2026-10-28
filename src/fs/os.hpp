// CRPC -- fs internal OS bridge (not installed; src/fs only).
//
// Everything here exists to (a) classify native failures into rpc_code while
// preserving the native code, and (b) convert between UTF-8 `std::string`
// paths and the platform's wide / byte path representation. Only files under
// src/fs include this header.
#ifndef CRPC_FS_DETAIL_OS_HPP
#define CRPC_FS_DETAIL_OS_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/error.hpp>
#include <crpc/fs/path.hpp>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace crpc {
namespace fs {
namespace detail {

// Builds a classified rpc_error for `operation` on `path`. The native code is
// never dropped: it appears in the message and in metadata["fs.native_error"];
// the human-readable system text is in metadata["fs.system_message"].
CRPC_API rpc_error classify_win32(unsigned long win32_code, std::string_view operation,
                                  const std::string& path);
CRPC_API rpc_error classify_errno(int errno_code, std::string_view operation,
                                  const std::string& path);

// A random, collision-resistant name component for temporary files/directories:
// `<prefix><16 bytes as 32 hex chars>`. Deliberately not "secure"; unique per
// process invocation is enough.
CRPC_API std::string unique_name(std::string_view prefix);

#if defined(CRPC_OS_WINDOWS)
// UTF-8 -> wide. Invalid UTF-8 is `invalid_argument` (never a lossy '?').
CRPC_API result<std::wstring> utf8_to_wide(const std::string& utf8);
CRPC_API std::string wide_to_utf8(const std::wstring& wide);

// FILETIME (100ns since 1601-01-01) <-> system_clock. `file_time` is an alias
// of the latter, so the return types convert implicitly.
CRPC_API std::chrono::system_clock::time_point filetime_to_time(u64 ticks_100ns) noexcept;
CRPC_API u64 time_to_filetime(std::chrono::system_clock::time_point time) noexcept;
#endif

} // namespace detail
} // namespace fs
} // namespace crpc

#endif // CRPC_FS_DETAIL_OS_HPP