#include "os.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <string>

#if defined(CRPC_OS_WINDOWS)
#include <windows.h>
#endif

namespace crpc {
namespace fs {
namespace detail {
namespace {

rpc_code errno_to_code(int e) noexcept {
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return rpc_code::not_found;
    case EACCES:
    case EPERM:
    case EBUSY:
        return rpc_code::permission_denied;
    case EINVAL:
    case EISDIR:
        return rpc_code::invalid_argument;
#ifndef _MSC_VER
    case ENAMETOOLONG:
    case ELOOP:
        return rpc_code::invalid_argument;
#endif
    case EEXIST:
    case ENOTEMPTY:
        return rpc_code::failed_precondition;
    case ENOSPC:
    case ENOMEM:
    case EMFILE:
    case ENFILE:
#ifndef _MSC_VER
    case EDQUOT:
#endif
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
}

#if defined(CRPC_OS_WINDOWS)
rpc_code win32_to_code(DWORD c) noexcept {
    switch (c) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_NETPATH:
        return rpc_code::not_found;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_LOCKED:
        return rpc_code::permission_denied;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_DIRECTORY:
    case ERROR_INVALID_DRIVE:
    case ERROR_INVALID_ADDRESS:
        return rpc_code::invalid_argument;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
    case ERROR_DIR_NOT_EMPTY:
        return rpc_code::failed_precondition;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
    case ERROR_NO_SYSTEM_RESOURCES:
        return rpc_code::resource_exhausted;
    case ERROR_OPERATION_ABORTED:
        return rpc_code::canceled;
    default:
        return rpc_code::internal;
    }
}

std::string win32_message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD len =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (len != 0 && buffer != nullptr) {
        text = wide_to_utf8(std::wstring(buffer, len));
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
        LocalFree(buffer);
    }
    return text;
}

std::string errno_message(int e) {
#if defined(_MSC_VER)
    char buffer[192] = {0};
    if (strerror_s(buffer, sizeof(buffer), e) == 0) {
        return std::string(buffer);
    }
    return "unknown errno";
#else
    const char* text = std::strerror(e);
    return text != nullptr ? text : "unknown errno";
#endif
}
#else
std::string errno_message(int e) {
    const char* text = std::strerror(e);
    return text != nullptr ? text : "unknown errno";
}
#endif

} // namespace

#if defined(CRPC_OS_WINDOWS)
rpc_error classify_win32(unsigned long win32_code, std::string_view operation,
                         const std::string& path) {
    const DWORD code = static_cast<DWORD>(win32_code);
    rpc_error error(win32_to_code(code));
    std::ostringstream out;
    out << operation << " failed for '" << path << "': " << win32_message(code)
        << " (Windows error " << code << ")";
    error.message = out.str();
    error.metadata.set("fs.operation", std::string(operation));
    char native[24] = {0};
    std::snprintf(native, sizeof(native), "%lu", code);
    error.metadata.set("fs.native_error", native);
    return error;
}

rpc_error classify_errno(int errno_code, std::string_view operation,
                         const std::string& path) {
    rpc_error error(errno_to_code(errno_code));
    std::ostringstream out;
    out << operation << " failed for '" << path << "': " << errno_message(errno_code)
        << " (errno " << errno_code << ")";
    error.message = out.str();
    error.metadata.set("fs.operation", std::string(operation));
    error.metadata.set("fs.native_error", std::to_string(errno_code));
    return error;
}

result<std::wstring> utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: path is not valid UTF-8 (MultiByteToWideChar failed)");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
        &wide[0], needed);
    if (written <= 0) {
        return rpc_error(rpc_code::invalid_argument, "fs: path is not valid UTF-8");
    }
    wide.resize(static_cast<std::size_t>(written));
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            &utf8[0], needed, nullptr, nullptr);
    if (written <= 0) {
        return std::string();
    }
    utf8.resize(static_cast<std::size_t>(written));
    return utf8;
}

// FILETIME ticks: 100ns since 1601-01-01; system_clock: since 1970-01-01.
// 369 years * 365.2425 days * 24h * 3600s = 11644473600 seconds.
constexpr i64 kFiletimeToUnixEpochSeconds = 11644473600LL;

std::chrono::system_clock::time_point filetime_to_time(u64 ticks_100ns) noexcept {
    const i64 delta = static_cast<i64>(ticks_100ns) -
                      kFiletimeToUnixEpochSeconds * 10000000LL;
    const auto seconds = std::chrono::seconds(delta / 10000000);
    const auto micros = std::chrono::microseconds((delta % 10000000) / 10);
    return std::chrono::system_clock::time_point(seconds) + micros;
}

u64 time_to_filetime(std::chrono::system_clock::time_point time) noexcept {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch());
    const auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()) -
        std::chrono::duration_cast<std::chrono::microseconds>(seconds);
    const i64 delta = seconds.count() * 10000000LL + micros.count() * 10LL;
    return static_cast<u64>(delta + kFiletimeToUnixEpochSeconds * 10000000LL);
}
#else
rpc_error classify_win32(unsigned long, std::string_view, const std::string&) {
    return rpc_error(rpc_code::internal, "fs: classify_win32 used on a non-Windows host");
}

rpc_error classify_errno(int errno_code, std::string_view operation,
                         const std::string& path) {
    rpc_error error(errno_to_code(errno_code));
    std::ostringstream out;
    out << operation << " failed for '" << path << "': " << errno_message(errno_code)
        << " (errno " << errno_code << ")";
    error.message = out.str();
    error.metadata.set("fs.operation", std::string(operation));
    error.metadata.set("fs.native_error", std::to_string(errno_code));
    return error;
}
#endif

} // namespace detail
} // namespace fs
} // namespace crpc

namespace crpc {
namespace fs {
namespace detail {

std::string unique_name(std::string_view prefix) {
    thread_local std::mt19937_64 rng(std::random_device{}());
    thread_local u64 counter = 0;
    const u64 a = rng();
    const u64 b = rng();
    const u64 c = counter++;
    std::string out(prefix.data(), prefix.size());
    static const char hex[] = "0123456789abcdef";
    char tail[48] = {0};
    static const char* p = hex;
    for (int i = 0; i < 16; ++i) {
        tail[i * 2] = p[(a >> (i * 4)) & 0xF];
        tail[i * 2 + 1] = p[(b >> (i * 4)) & 0xF];
    }
    for (int i = 0; i < 8; ++i) {
        tail[32 + i] = p[(c >> (i * 4)) & 0xF];
    }
    out.append(tail, 40);
    return out;
}

} // namespace detail
} // namespace fs
} // namespace crpc