#include <crpc/fs/file.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "os.hpp"

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace crpc {
namespace fs {
namespace {

// Read/write calls are issued in 1 MiB slices on every platform: the slice size
// bounds stack/VSS pressure for the chunk buffer without hurting throughput.
constexpr std::size_t kIoChunk = 1u << 20;

#if defined(CRPC_OS_WINDOWS)

status win_status(DWORD code, std::string_view op, const std::string& path) {
    return status(detail::classify_win32(code, op, path));
}

result<std::wstring> wide(const path& p, std::string_view op) {
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return rpc_error(rpc_code::invalid_argument,
                         std::string(op) + ": path is not valid UTF-8");
    }
    return w;
}

// Retrieves the attribute set without following reparse points is not used:
// stat reports the reparse flag through the WIN32_FILE_ATTRIBUTE_DATA probe.
#endif // CRPC_OS_WINDOWS

} // namespace

// --- Existence and metadata -------------------------------------------------
result<bool> exists(const path& p) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "exists");
    if (!w) {
        return w.error();
    }
    const DWORD attrs = GetFileAttributesW(w.value().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return detail::classify_win32(code, "exists", p.str());
    }
    return true;
#else
    struct stat st = {};
    if (::stat(p.str().c_str(), &st) == 0) {
        return true;
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        return false;
    }
    return detail::classify_errno(errno, "exists", p.str());
#endif
}

result<file_info> stat(const path& p) {
    file_info info;
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "stat");
    if (!w) {
        return w.error();
    }
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(w.value().c_str(), GetFileExInfoStandard, &data)) {
        return detail::classify_win32(GetLastError(), "stat", p.str());
    }
    info.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.is_read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    info.is_symlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    info.size = (static_cast<u64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.last_write_time =
        detail::filetime_to_time((static_cast<u64>(data.ftLastWriteTime.dwHighDateTime)
                                  << 32) |
                                 data.ftLastWriteTime.dwLowDateTime);
    info.last_access_time =
        detail::filetime_to_time((static_cast<u64>(data.ftLastAccessTime.dwHighDateTime)
                                  << 32) |
                                 data.ftLastAccessTime.dwLowDateTime);
    info.creation_time =
        detail::filetime_to_time((static_cast<u64>(data.ftCreationTime.dwHighDateTime)
                                  << 32) |
                                 data.ftCreationTime.dwLowDateTime);
    return info;
#else
    struct stat st = {};
    if (::lstat(p.str().c_str(), &st) == 0) {
        info.is_directory = S_ISDIR(st.st_mode);
        info.is_symlink = S_ISLNK(st.st_mode);
        info.is_read_only = (st.st_mode & S_IWUSR) == 0;
        info.size = static_cast<u64>(st.st_size);
        info.last_write_time =
            file_time(std::chrono::seconds(st.st_mtim.tv_sec)) +
            std::chrono::microseconds(st.st_mtim.tv_nsec / 1000);
        info.last_access_time =
            file_time(std::chrono::seconds(st.st_atim.tv_sec)) +
            std::chrono::microseconds(st.st_atim.tv_nsec / 1000);
        info.creation_time = info.last_write_time; // no birth time on all POSIX
        return info;
    }
    return detail::classify_errno(errno, "stat", p.str());
#endif
}

result<bool> is_file(const path& p) {
    result<file_info> i = stat(p);
    if (!i) {
        if (i.code() == rpc_code::not_found) {
            return false;
        }
        return i.error();
    }
    return !i.value().is_directory && !i.value().is_symlink;
}

result<bool> is_directory(const path& p) {
    result<file_info> i = stat(p);
    if (!i) {
        if (i.code() == rpc_code::not_found) {
            return false;
        }
        return i.error();
    }
    return i.value().is_directory;
}

result<u64> file_size(const path& p) {
    result<file_info> i = stat(p);
    if (!i) {
        return i.error();
    }
    if (i.value().is_directory) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: file_size: '" + p.str() + "' is a directory");
    }
    return i.value().size;
}

// --- Whole-file I/O ---------------------------------------------------------

namespace {

#if defined(CRPC_OS_WINDOWS)

result<byte_buffer> read_all_bytes_impl(HANDLE h, u64 size, const std::string& path_text) {
    byte_buffer out;
    if (size != 0) {
        (void)out.reserve(static_cast<std::size_t>(size));
    }
    std::vector<std::byte> chunk(kIoChunk);
    u64 remaining = size;
    while (remaining != 0) {
        const std::size_t want = static_cast<std::size_t>(
            remaining < kIoChunk ? remaining : kIoChunk);
        DWORD got = 0;
        if (!ReadFile(h, chunk.data(), static_cast<DWORD>(want), &got, nullptr)) {
            return detail::classify_win32(GetLastError(), "read_all", path_text);
        }
        if (got == 0) {
            break; // EOF race (file shrank concurrently)
        }
        if (!out.append(span<const std::byte>(chunk.data(), got)).ok()) {
            return rpc_error(rpc_code::resource_exhausted,
                             "fs: read_all: out of memory for '" + path_text + "'");
        }
        remaining -= got;
    }
    return out;
}

#else

status posix_write_all(const std::string& path_text, span<const std::byte> data,
                       int flags) {
    const int fd = ::open(path_text.c_str(), flags, 0644);
    if (fd < 0) {
        return detail::classify_errno(errno, "write_all", path_text);
    }
    const std::byte* pos = data.data();
    std::size_t left = data.size();
    while (left != 0) {
        const ssize_t n = ::write(fd, pos, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(fd);
            return detail::classify_errno(saved, "write_all", path_text);
        }
        pos += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        return detail::classify_errno(errno, "write_all", path_text);
    }
    return status();
}
#endif

} // namespace

result<byte_buffer> read_all_bytes(const path& p) {
    result<file_info> i = stat(p);
    if (!i) {
        return i.error();
    }
    if (i.value().is_directory) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: read_all_bytes: '" + p.str() + "' is a directory");
    }
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "read_all");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return detail::classify_win32(GetLastError(), "read_all", p.str());
    }
    result<byte_buffer> out = read_all_bytes_impl(h, i.value().size, p.str());
    CloseHandle(h);
    return out;
#else
    const int fd = ::open(p.str().c_str(), O_RDONLY);
    if (fd < 0) {
        return detail::classify_errno(errno, "read_all", p.str());
    }
    byte_buffer out;
    if (i.value().size != 0) {
        (void)out.reserve(static_cast<std::size_t>(i.value().size));
    }
    std::vector<std::byte> chunk(kIoChunk);
    u64 remaining = i.value().size;
    while (remaining != 0) {
        const std::size_t want = static_cast<std::size_t>(
            remaining < kIoChunk ? remaining : kIoChunk);
        const ssize_t n = ::read(fd, chunk.data(), want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(fd);
            return detail::classify_errno(saved, "read_all", p.str());
        }
        if (n == 0) {
            break;
        }
        if (!out.append(span<const std::byte>(chunk.data(), static_cast<std::size_t>(n)))
                 .ok()) {
            ::close(fd);
            return rpc_error(rpc_code::resource_exhausted,
                             "fs: read_all: out of memory for '" + p.str() + "'");
        }
        remaining -= static_cast<std::size_t>(n);
    }
    ::close(fd);
    return out;
#endif
}

result<std::string> read_all_text(const path& p) {
    result<byte_buffer> b = read_all_bytes(p);
    if (!b) {
        return b.error();
    }
    return b.value().to_string();
}

status write_all_bytes(const path& p, span<const std::byte> data) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "write_all");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return win_status(GetLastError(), "write_all", p.str());
    }
    const std::byte* pos = data.data();
    std::size_t left = data.size();
    while (left != 0) {
        const std::size_t want = left < kIoChunk ? left : kIoChunk;
        DWORD wrote = 0;
        if (!WriteFile(h, pos, static_cast<DWORD>(want), &wrote, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(h);
            return win_status(code, "write_all", p.str());
        }
        if (wrote == 0) {
            CloseHandle(h);
            return win_status(ERROR_DISK_FULL, "write_all", p.str());
        }
        pos += wrote;
        left -= wrote;
    }
    if (!CloseHandle(h)) {
        return win_status(GetLastError(), "write_all", p.str());
    }
    return status();
#else
    return posix_write_all(p.str(), data, O_WRONLY | O_CREAT | O_TRUNC);
#endif
}

status write_all_text(const path& p, const std::string& text) {
    return write_all_bytes(p, span<const std::byte>(
                                  reinterpret_cast<const std::byte*>(text.data()),
                                  text.size()));
}

status append_bytes(const path& p, span<const std::byte> data) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "append");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return win_status(GetLastError(), "append", p.str());
    }
    const std::byte* pos = data.data();
    std::size_t left = data.size();
    while (left != 0) {
        const std::size_t want = left < kIoChunk ? left : kIoChunk;
        DWORD wrote = 0;
        if (!WriteFile(h, pos, static_cast<DWORD>(want), &wrote, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(h);
            return win_status(code, "append", p.str());
        }
        pos += wrote;
        left -= wrote;
    }
    if (!CloseHandle(h)) {
        return win_status(GetLastError(), "append", p.str());
    }
    return status();
#else
    return posix_write_all(p.str(), data, O_WRONLY | O_CREAT | O_APPEND);
#endif
}

status append_text(const path& p, const std::string& text) {
    return append_bytes(p, span<const std::byte>(
                               reinterpret_cast<const std::byte*>(text.data()),
                               text.size()));
}

result<std::size_t> read_range(const path& p, u64 offset, span<std::byte> out) {
    result<file_info> i = stat(p);
    if (!i) {
        return i.error();
    }
    if (i.value().is_directory) {
        return rpc_error(rpc_code::invalid_argument,
                         "fs: read_range: '" + p.str() + "' is a directory");
    }
    if (out.empty() || offset >= i.value().size) {
        return std::size_t{0};
    }
    const u64 want_u64 = i.value().size - offset;
    const std::size_t want =
        static_cast<std::size_t>((want_u64 < out.size()) ? want_u64 : out.size());
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "read_range");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return detail::classify_win32(GetLastError(), "read_range", p.str());
    }
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) {
        const DWORD code = GetLastError();
        CloseHandle(h);
        return detail::classify_win32(code, "read_range", p.str());
    }
    std::size_t got_total = 0;
    while (got_total < want) {
        DWORD got = 0;
        const DWORD ask = static_cast<DWORD>(want - got_total);
        if (!ReadFile(h, out.data() + got_total, ask, &got, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(h);
            return detail::classify_win32(code, "read_range", p.str());
        }
        if (got == 0) {
            break;
        }
        got_total += got;
    }
    CloseHandle(h);
    return got_total;
#else
    const int fd = ::open(p.str().c_str(), O_RDONLY);
    if (fd < 0) {
        return detail::classify_errno(errno, "read_range", p.str());
    }
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        const int saved = errno;
        ::close(fd);
        return detail::classify_errno(saved, "read_range", p.str());
    }
    std::size_t got_total = 0;
    while (got_total < want) {
        const ssize_t n = ::read(fd, out.data() + got_total, want - got_total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(fd);
            return detail::classify_errno(saved, "read_range", p.str());
        }
        if (n == 0) {
            break;
        }
        got_total += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return got_total;
#endif
}

// --- Atomic write -----------------------------------------------------------

namespace detail {

path atomic_temp_path(const path& target) {
    const path dir = target.parent();
    const std::string base =
        "." + target.filename().str() + ".tmp." + crpc::fs::detail::unique_name("");
    if (dir == path(".")) {
        return path(base);
    }
    return dir / path(base);
}

status create_new_empty_file(const path& p) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "create_new");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return rpc_error(rpc_code::failed_precondition,
                             "fs: create_new_empty_file: already exists: '" +
                                 p.str() + "'");
        }
        return win_status(code, "create_new", p.str());
    }
    CloseHandle(h);
    return status();
#else
    const int fd = ::open(p.str().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            return rpc_error(rpc_code::failed_precondition,
                             "fs: create_new_empty_file: already exists: '" +
                                 p.str() + "'");
        }
        return detail::classify_errno(errno, "create_new", p.str());
    }
    if (::close(fd) != 0) {
        return detail::classify_errno(errno, "create_new", p.str());
    }
    return status();
#endif
}

status atomic_write_stage(const path& temp, span<const std::byte> data,
                          std::size_t fault_after) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(temp, "atomic_stage");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return win_status(GetLastError(), "atomic_stage", temp.str());
    }
    const std::size_t limit =
        (fault_after != 0 && fault_after < data.size()) ? fault_after : data.size();
    const std::byte* pos = data.data();
    std::size_t left = limit;
    bool failed = false;
    while (left != 0) {
        const std::size_t want = left < kIoChunk ? left : kIoChunk;
        DWORD wrote = 0;
        if (!WriteFile(h, pos, static_cast<DWORD>(want), &wrote, nullptr)) {
            failed = true;
            break;
        }
        if (wrote == 0) {
            failed = true;
            break;
        }
        pos += wrote;
        left -= wrote;
    }
    if (!failed && fault_after != 0 && fault_after < data.size()) {
        failed = true; // simulated mid-write failure (test seam)
    }
    if (!failed) {
        // Make the data durable before the rename becomes visible.
        failed = !FlushFileBuffers(h);
    }
    if (failed) {
        (void)CloseHandle(h);
        DeleteFileW(w.value().c_str());
        return rpc_error(rpc_code::internal,
                         "fs: atomic_write: simulated or real failure while "
                         "staging '" +
                             temp.str() + "'");
    }
    if (!CloseHandle(h)) {
        DeleteFileW(w.value().c_str());
        return win_status(GetLastError(), "atomic_stage", temp.str());
    }
    return status();
#else
    const int fd = ::open(temp.str().c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return detail::classify_errno(errno, "atomic_stage", temp.str());
    }
    const std::size_t limit =
        (fault_after != 0 && fault_after < data.size()) ? fault_after : data.size();
    const std::byte* pos = data.data();
    std::size_t left = limit;
    bool failed = false;
    while (left != 0) {
        const ssize_t n = ::write(fd, pos, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = true;
            break;
        }
        pos += n;
        left -= static_cast<std::size_t>(n);
    }
    if (!failed && fault_after != 0 && fault_after < data.size()) {
        failed = true;
    }
    if (!failed && ::fsync(fd) != 0) {
        failed = true;
    }
    if (failed) {
        (void)::close(fd);
        (void)::unlink(temp.str().c_str());
        return rpc_error(rpc_code::internal,
                         "fs: atomic_write: simulated or real failure while "
                         "staging '" +
                             temp.str() + "'");
    }
    if (::close(fd) != 0) {
        (void)::unlink(temp.str().c_str());
        return detail::classify_errno(errno, "atomic_stage", temp.str());
    }
    return status();
#endif
}

status atomic_write_commit(const path& temp, const path& target) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> wt = wide(temp, "atomic_commit");
    if (!wt) {
        return wt.error();
    }
    result<std::wstring> wg = wide(target, "atomic_commit");
    if (!wg) {
        return wg.error();
    }
    if (!MoveFileExW(wt.value().c_str(), wg.value().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return win_status(GetLastError(), "atomic_commit", target.str());
    }
    return status();
#else
    if (::rename(temp.str().c_str(), target.str().c_str()) != 0) {
        return detail::classify_errno(errno, "atomic_commit", target.str());
    }
    return status();
#endif
}

status atomic_write_impl(const path& target, span<const std::byte> data,
                         std::size_t fault_after) {
    const path temp = atomic_temp_path(target);
    status staged = atomic_write_stage(temp, data, fault_after);
    if (!staged) {
        return staged;
    }
    status committed = atomic_write_commit(temp, target);
    if (!committed) {
        (void)remove_file(temp);
        return committed;
    }
    return status();
}

} // namespace detail

status write_all_bytes_atomic(const path& p, span<const std::byte> data) {
    return detail::atomic_write_impl(p, data, 0);
}

status write_all_text_atomic(const path& p, const std::string& text) {
    return write_all_bytes_atomic(
        p, span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size()));
}

// --- Mutation ---------------------------------------------------------------

status remove_file(const path& p) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "remove");
    if (!w) {
        return w.error();
    }
    // DeleteFileW refuses a read-only file; clear the attribute first so the
    // semantics match the caller's intent ("make it gone").
    const DWORD attrs = GetFileAttributesW(w.value().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        (void)SetFileAttributesW(w.value().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!DeleteFileW(w.value().c_str())) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return status(); // idempotent
        }
        return win_status(code, "remove", p.str());
    }
    return status();
#else
    if (::unlink(p.str().c_str()) == 0) {
        return status();
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        return status();
    }
    return detail::classify_errno(errno, "remove", p.str());
#endif
}

status rename(const path& from, const path& to) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> wf = wide(from, "rename");
    if (!wf) {
        return wf.error();
    }
    result<std::wstring> wt = wide(to, "rename");
    if (!wt) {
        return wt.error();
    }
    if (!MoveFileExW(wf.value().c_str(), wt.value().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return win_status(GetLastError(), "rename", from.str());
    }
    return status();
#else
    if (::rename(from.str().c_str(), to.str().c_str()) == 0) {
        return status();
    }
    return detail::classify_errno(errno, "rename", from.str());
#endif
}

status copy_file(const path& from, const path& to, bool overwrite) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> wf = wide(from, "copy");
    if (!wf) {
        return wf.error();
    }
    result<std::wstring> wt = wide(to, "copy");
    if (!wt) {
        return wt.error();
    }
    if (!CopyFileW(wf.value().c_str(), wt.value().c_str(), overwrite ? FALSE : TRUE)) {
        const DWORD code = GetLastError();
        if (!overwrite &&
            (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)) {
            return rpc_error(rpc_code::failed_precondition,
                             "fs: copy_file: target already exists: '" + to.str() + "'");
        }
        return win_status(code, "copy", from.str());
    }
    return status();
#else
    const int in = ::open(from.str().c_str(), O_RDONLY);
    if (in < 0) {
        return detail::classify_errno(errno, "copy", from.str());
    }
    const int out =
        ::open(to.str().c_str(), O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL),
               0644);
    if (out < 0) {
        const int saved = errno;
        ::close(in);
        if (!overwrite && (saved == EEXIST)) {
            return rpc_error(rpc_code::failed_precondition,
                             "fs: copy_file: target already exists: '" + to.str() + "'");
        }
        return detail::classify_errno(saved, "copy", from.str());
    }
    std::vector<std::byte> chunk(kIoChunk);
    for (;;) {
        const ssize_t n = ::read(in, chunk.data(), chunk.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(in);
            ::close(out);
            ::unlink(to.str().c_str());
            return detail::classify_errno(saved, "copy", from.str());
        }
        if (n == 0) {
            break;
        }
        std::size_t off = 0;
        while (off < static_cast<std::size_t>(n)) {
            const ssize_t w = ::write(out, chunk.data() + off,
                                      static_cast<std::size_t>(n) - off);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int saved = errno;
                ::close(in);
                ::close(out);
                ::unlink(to.str().c_str());
                return detail::classify_errno(saved, "copy", from.str());
            }
            off += static_cast<std::size_t>(w);
        }
    }
    ::close(in);
    if (::close(out) != 0) {
        const int saved = errno;
        ::unlink(to.str().c_str());
        return detail::classify_errno(saved, "copy", from.str());
    }
    return status();
#endif
}

status set_read_only(const path& p, bool read_only) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "set_read_only");
    if (!w) {
        return w.error();
    }
    DWORD attrs = GetFileAttributesW(w.value().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return win_status(GetLastError(), "set_read_only", p.str());
    }
    if (read_only) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    } else {
        attrs &= ~FILE_ATTRIBUTE_READONLY;
    }
    if (!SetFileAttributesW(w.value().c_str(), attrs)) {
        return win_status(GetLastError(), "set_read_only", p.str());
    }
    return status();
#else
    struct stat st = {};
    if (::stat(p.str().c_str(), &st) != 0) {
        return detail::classify_errno(errno, "set_read_only", p.str());
    }
    mode_t mode = st.st_mode;
    if (read_only) {
        mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    } else {
        mode |= S_IWUSR;
    }
    if (::chmod(p.str().c_str(), mode) != 0) {
        return detail::classify_errno(errno, "set_read_only", p.str());
    }
    return status();
#endif
}

status set_last_write_time(const path& p, file_time time) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = wide(p, "set_last_write_time");
    if (!w) {
        return w.error();
    }
    HANDLE h = CreateFileW(w.value().c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return win_status(GetLastError(), "set_last_write_time", p.str());
    }
    const u64 ticks = detail::time_to_filetime(time);
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    if (!SetFileTime(h, nullptr, nullptr, &ft)) {
        const DWORD code = GetLastError();
        CloseHandle(h);
        return win_status(code, "set_last_write_time", p.str());
    }
    CloseHandle(h);
    return status();
#else
    struct timespec times[2];
    times[0].tv_sec = time.time_since_epoch().count() / 1000000000LL;
    times[0].tv_nsec = 0;
    times[1].tv_sec = times[0].tv_sec;
    times[1].tv_nsec = 0;
    if (::utimensat(AT_FDCWD, p.str().c_str(), times, 0) != 0) {
        return detail::classify_errno(errno, "set_last_write_time", p.str());
    }
    return status();
#endif
}

} // namespace fs
} // namespace crpc