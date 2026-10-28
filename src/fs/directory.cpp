#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>
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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace crpc {
namespace fs {
namespace {

#if defined(CRPC_OS_WINDOWS)
status win_status(DWORD code, std::string_view op, const std::string& path) {
    return status(detail::classify_win32(code, op, path));
}
#endif

} // namespace

result<path> current_directory() {
#if defined(CRPC_OS_WINDOWS)
    const DWORD needed = GetCurrentDirectoryW(0, nullptr);
    if (needed == 0) {
        return detail::classify_win32(GetLastError(), "current_directory", "");
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetCurrentDirectoryW(needed, &buffer[0]);
    if (written == 0) {
        return detail::classify_win32(GetLastError(), "current_directory", "");
    }
    buffer.resize(written);
    return path(detail::wide_to_utf8(buffer));
#else
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        return detail::classify_errno(errno, "current_directory", "");
    }
    return path(cwd);
#endif
}

status set_current_directory(const path& p) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return w.error();
    }
    if (!SetCurrentDirectoryW(w.value().c_str())) {
        return win_status(GetLastError(), "set_current_directory", p.str());
    }
    return status();
#else
    if (::chdir(p.str().c_str()) != 0) {
        return detail::classify_errno(errno, "set_current_directory", p.str());
    }
    return status();
#endif
}

result<path> temporary_directory() {
#if defined(CRPC_OS_WINDOWS)
    wchar_t buffer[MAX_PATH + 1] = {0};
    const DWORD len = GetTempPathW(static_cast<DWORD>(MAX_PATH), buffer);
    if (len == 0 || len > MAX_PATH) {
        return detail::classify_win32(len != 0 ? ERROR_BUFFER_OVERFLOW : GetLastError(),
                                      "temporary_directory", "");
    }
    std::wstring w(buffer, len);
    while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) {
        w.pop_back();
    }
    return path(detail::wide_to_utf8(w));
#else
    if (const char* tmp = std::getenv("TMPDIR");
        tmp != nullptr && tmp[0] != '\0') {
        return path(tmp);
    }
    if (const char* tmp = std::getenv("TMP"); tmp != nullptr && tmp[0] != '\0') {
        return path(tmp);
    }
    return path("/tmp");
#endif
}

status create_directory(const path& p) {
    if (p.empty()) {
        return rpc_error(rpc_code::invalid_argument, "fs: create_directory: empty path");
    }
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return w.error();
    }
    if (CreateDirectoryW(w.value().c_str(), nullptr)) {
        return status();
    }
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
        return rpc_error(rpc_code::failed_precondition,
                         "fs: create_directory: already exists: '" + p.str() + "'");
    }
    return win_status(code, "create_directory", p.str());
#else
    if (::mkdir(p.str().c_str(), 0777) == 0) {
        return status();
    }
    if (errno == EEXIST) {
        return rpc_error(rpc_code::failed_precondition,
                         "fs: create_directory: already exists: '" + p.str() + "'");
    }
    return detail::classify_errno(errno, "create_directory", p.str());
#endif
}

status create_directories(const path& p) {
    if (p.empty()) {
        return rpc_error(rpc_code::invalid_argument, "fs: create_directories: empty path");
    }
    // Walk from the nearest existing ancestor down to `p`, creating each level.
    // A "." ancestor terminates the chain (its parent is itself).
    std::vector<path> chain;
    for (path cur = p.normalized();
         !cur.empty() && !cur.is_root() && cur.str() != ".";
         cur = cur.parent()) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());
    for (const path& level : chain) {
        if (level.str() == "." || level.is_root()) {
            continue;
        }
        result<bool> d = is_directory(level);
        if (d.ok() && d.value()) {
            continue; // already a directory (possibly created just above)
        }
        status made = create_directory(level);
        if (made) {
            continue;
        }
        if (made.code() == rpc_code::failed_precondition) {
            // An existing non-directory at this level means the caller asked
            // for something impossible; report it. If it is a directory, a
            // concurrent creator is harmless.
            result<bool> now_dir = is_directory(level);
            if (now_dir.ok() && now_dir.value()) {
                continue;
            }
        }
        return made;
    }
    return status();
}

status remove_directory(const path& p) {
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return w.error();
    }
    if (RemoveDirectoryW(w.value().c_str())) {
        return status();
    }
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return status(); // idempotent
    }
    return win_status(code, "remove_directory", p.str());
#else
    if (::rmdir(p.str().c_str()) == 0) {
        return status();
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        return status();
    }
    return detail::classify_errno(errno, "remove_directory", p.str());
#endif
}

status remove_all(const path& p) {
    result<file_info> i = stat(p);
    if (!i) {
        if (i.code() == rpc_code::not_found) {
            return status(); // idempotent
        }
        return i.error();
    }
    if (i.value().is_directory) {
        result<std::vector<directory_entry>> entries = list_directory(p);
        if (!entries) {
            return entries.error();
        }
        for (const directory_entry& e : entries.value()) {
            status removed = remove_all(e.full_path);
            if (!removed) {
                return removed;
            }
        }
        return remove_directory(p);
    }
    return remove_file(p);
}

result<std::vector<directory_entry>> list_directory(const path& p, bool recursive) {
    std::vector<directory_entry> out;
#if defined(CRPC_OS_WINDOWS)
    result<std::wstring> w = detail::utf8_to_wide(p.str());
    if (!w) {
        return w.error();
    }
    std::wstring pattern = w.value();
    if ((!pattern.empty()) && pattern.back() != L'\\' && pattern.back() != L'/') {
        pattern.push_back(L'\\');
    }
    pattern.push_back(L'*');

    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return detail::classify_win32(GetLastError(), "list_directory", p.str());
    }
    do {
        const std::wstring name_w(fd.cFileName, wcslen(fd.cFileName));
        if (name_w == L"." || name_w == L"..") {
            continue;
        }
        const std::string name = detail::wide_to_utf8(name_w);
        directory_entry e;
        e.name = name;
        e.full_path = p / path(name);
        e.is_directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (static_cast<u64>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(e));
    } while (FindNextFileW(find, &fd) != 0);

    const DWORD end_code = GetLastError();
    FindClose(find);
    if (end_code != ERROR_NO_MORE_FILES) {
        return detail::classify_win32(end_code, "list_directory", p.str());
    }
#else
    DIR* dir = ::opendir(p.str().c_str());
    if (dir == nullptr) {
        return detail::classify_errno(errno, "list_directory", p.str());
    }
    errno = 0;
    while (dirent* ent = ::readdir(dir)) {
        const std::string name(ent->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        directory_entry e;
        e.name = name;
        e.full_path = p / path(name);
        struct stat st = {};
        if (::stat(e.full_path.str().c_str(), &st) == 0) {
            e.is_directory = S_ISDIR(st.st_mode);
            e.size = S_ISREG(st.st_mode) ? static_cast<u64>(st.st_size) : 0;
        }
        out.push_back(std::move(e));
    }
    const int saved = errno;
    ::closedir(dir);
    if (saved != 0) {
        return detail::classify_errno(saved, "list_directory", p.str());
    }
#endif

    std::sort(out.begin(), out.end(), [](const directory_entry& a, const directory_entry& b) {
        return a.name < b.name;
    });

    if (recursive) {
        const std::size_t initial = out.size();
        for (std::size_t i = 0; i < initial; ++i) {
            if (!out[i].is_directory) {
                continue;
            }
            result<std::vector<directory_entry>> sub = list_directory(out[i].full_path, true);
            if (!sub) {
                return sub.error();
            }
            for (directory_entry& child : sub.value()) {
                out.push_back(std::move(child));
            }
        }
    }
    return out;
}

} // namespace fs
} // namespace crpc