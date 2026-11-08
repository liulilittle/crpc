// pipe2 / O_CLOEXEC are POSIX.1-2008 extensions that glibc hides behind
// _GNU_SOURCE when the compiler is invoked in strict standard mode (-std=c++17,
// which is what this project builds with). It must be defined before any libc
// header is pulled in.
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <crpc/system/process.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

// K32GetProcessMemoryInfo lives in psapi.lib. The repository's convention
// (see tests/test_framework.hpp) is to name the import library next to the
// code that needs it; CMakeLists stays untouched.
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace crpc {
namespace system {

namespace {

constexpr std::size_t kReadChunk = 64u * 1024u;

#if defined(CRPC_OS_WINDOWS)

rpc_error win_error(DWORD code, const char* operation, const std::string& subject) {
    rpc_code mapped = rpc_code::internal;
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_MOD_NOT_FOUND:
        mapped = rpc_code::not_found;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        mapped = rpc_code::permission_denied;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        mapped = rpc_code::invalid_argument;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
        mapped = rpc_code::resource_exhausted;
        break;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
        mapped = rpc_code::broken_pipe;
        break;
    default:
        break;
    }
    return rpc_error(mapped, std::string("system: ") + operation + " '" + subject +
                                "' failed (win32=" + std::to_string(code) + ")");
}

result<std::wstring> to_wide(const std::string& utf8, const char* operation) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return rpc_error(rpc_code::invalid_argument,
                         std::string("system: ") + operation + ": argument is not UTF-8");
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                            static_cast<int>(utf8.size()), &wide[0],
                                            needed);
    if (written != needed) {
        return rpc_error(rpc_code::invalid_argument,
                         std::string("system: ") + operation + ": argument is not UTF-8");
    }
    return wide;
}

std::string to_utf8(const std::wstring& wide) {
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
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                            static_cast<int>(wide.size()), &utf8[0],
                                            needed, nullptr, nullptr);
    if (written != needed) {
        return std::string();
    }
    return utf8;
}

// Quotes one argument using the CommandLineToArgvW rules, which is what the
// CRT of the child applies to the command line CreateProcessW hands over.
std::wstring quote_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted;
    quoted.push_back(L'"');
    for (std::wstring::const_iterator it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

struct handle_guard {
    HANDLE value = nullptr;
    handle_guard() = default;
    explicit handle_guard(HANDLE h) : value(h) {}
    ~handle_guard() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value);
        }
    }
    handle_guard(const handle_guard&) = delete;
    handle_guard& operator=(const handle_guard&) = delete;
    HANDLE release() {
        HANDLE h = value;
        value = nullptr;
        return h;
    }
    void reset(HANDLE h) {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value);
        }
        value = h;
    }
};

// An inheritable duplicate of a standard handle, so a child that keeps the
// parent's console stream can still write to it after CreateProcessW.
HANDLE duplicate_inheritable(HANDLE source) {
    if (source == nullptr || source == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0,
                         TRUE, DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return copy;
}

result<std::vector<wchar_t>> build_environment(const process_options& options) {
    std::map<std::wstring, std::wstring> merged;
    if (options.inherit_environment) {
        LPWCH block = GetEnvironmentStringsW();
        if (block == nullptr) {
            return win_error(GetLastError(), "GetEnvironmentStringsW", "environment");
        }
        for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
            if (entry[0] == L'=') {
                continue; // hidden per-drive variables are not real names
            }
            const wchar_t* equals = wcschr(entry, L'=');
            if (equals == nullptr) {
                continue;
            }
            merged[std::wstring(entry, static_cast<std::size_t>(equals - entry))] =
                std::wstring(equals + 1);
        }
        (void)FreeEnvironmentStringsW(block);
    }
    for (const std::pair<const std::string, std::string>& kv : options.environment) {
        if (kv.first.empty() || kv.first.find('=') != std::string::npos) {
            return rpc_error(rpc_code::invalid_argument,
                             "system: environment variable name is invalid: '" +
                                 kv.first + "'");
        }
        result<std::wstring> name = to_wide(kv.first, "environment");
        if (!name) {
            return name.error();
        }
        result<std::wstring> value = to_wide(kv.second, "environment");
        if (!value) {
            return value.error();
        }
        merged[name.value()] = value.value();
    }

    std::vector<wchar_t> block;
    for (const std::pair<const std::wstring, std::wstring>& kv : merged) {
        block.insert(block.end(), kv.first.begin(), kv.first.end());
        block.push_back(L'=');
        block.insert(block.end(), kv.second.begin(), kv.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1) {
        block.push_back(L'\0');
    }
    return block;
}

std::uint64_t filetime_to_unix_ms(FILETIME time) {
    ULARGE_INTEGER ticks;
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    // 100 ns ticks since 1601-01-01; 116444736000000000 of them separate that
    // epoch from 1970-01-01.
    if (ticks.QuadPart < 116444736000000000ull) {
        return 0;
    }
    return (ticks.QuadPart - 116444736000000000ull) / 10000ull;
}

void fill_process_details(process_info* info) {
    // PROCESS_QUERY_INFORMATION | PROCESS_VM_READ is what
    // K32GetProcessMemoryInfo documents; protected processes reject it, so a
    // limited handle is the fallback that still yields times and the path.
    handle_guard process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                     static_cast<DWORD>(info->pid)));
    if (process.value == nullptr) {
        process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  static_cast<DWORD>(info->pid)));
    }
    if (process.value == nullptr) {
        return;
    }

    wchar_t path[MAX_PATH * 4];
    DWORD path_size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    if (QueryFullProcessImageNameW(process.value, 0, path, &path_size)) {
        info->executable_path =
            to_utf8(std::wstring(path, static_cast<std::size_t>(path_size)));
        if (info->name.empty()) {
            const std::size_t slash = info->executable_path.find_last_of("\\/");
            info->name = slash == std::string::npos
                             ? info->executable_path
                             : info->executable_path.substr(slash + 1);
        }
    }

    FILETIME creation;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    if (GetProcessTimes(process.value, &creation, &exit_time, &kernel_time,
                        &user_time)) {
        info->start_time_unix_ms = filetime_to_unix_ms(creation);
        ULARGE_INTEGER user;
        user.LowPart = user_time.dwLowDateTime;
        user.HighPart = user_time.dwHighDateTime;
        ULARGE_INTEGER kernel;
        kernel.LowPart = kernel_time.dwLowDateTime;
        kernel.HighPart = kernel_time.dwHighDateTime;
        const std::uint64_t cpu_100ns = user.QuadPart + kernel.QuadPart;

        FILETIME now_filetime;
        GetSystemTimeAsFileTime(&now_filetime);
        const std::uint64_t now_ms = filetime_to_unix_ms(now_filetime);
        if (now_ms > info->start_time_unix_ms && info->start_time_unix_ms != 0) {
            const std::uint64_t lifetime_ms = now_ms - info->start_time_unix_ms;
            if (lifetime_ms != 0) {
                info->cpu_percent =
                    static_cast<double>(cpu_100ns / 10ull) /
                    static_cast<double>(lifetime_ms) * 100.0;
            }
        }
    }

    PROCESS_MEMORY_COUNTERS counters = {};
    if (K32GetProcessMemoryInfo(process.value, &counters, sizeof(counters))) {
        info->resident_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
}

#endif // CRPC_OS_WINDOWS

#if !defined(CRPC_OS_WINDOWS)

rpc_error posix_error(int code, const char* operation, const std::string& subject) {
    rpc_code mapped = rpc_code::internal;
    switch (code) {
    case ENOENT:
    case ENOTDIR:
        mapped = rpc_code::not_found;
        break;
    case ESRCH:
        mapped = rpc_code::not_found;
        break;
    case EACCES:
    case EPERM:
        mapped = rpc_code::permission_denied;
        break;
    case EINVAL:
    case ENOEXEC:
        mapped = rpc_code::invalid_argument;
        break;
    case ENOMEM:
    case EMFILE:
    case ENFILE:
    case E2BIG:
        mapped = rpc_code::resource_exhausted;
        break;
    case EPIPE:
        mapped = rpc_code::broken_pipe;
        break;
    default:
        break;
    }
    return rpc_error(mapped, std::string("system: ") + operation + " '" + subject +
                                "' failed (errno=" + std::to_string(code) + ": " +
                                std::strerror(code) + ")");
}

bool read_whole_file(const std::string& path, std::string* out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    out->clear();
    char buffer[kReadChunk];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        out->append(buffer, static_cast<std::size_t>(n));
        if (out->size() > (1u << 20)) {
            break; // /proc files are small; a runaway entry is not trusted
        }
    }
    (void)::close(fd);
    return true;
}

std::map<std::string, std::string> inherited_environment() {
    std::map<std::string, std::string> result;
    if (environ == nullptr) {
        return result;
    }
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const char* text = *entry;
        const char* equals = std::strchr(text, '=');
        if (equals == nullptr || equals == text) {
            continue;
        }
        result[std::string(text, static_cast<std::size_t>(equals - text))] =
            std::string(equals + 1);
    }
    return result;
}

std::vector<std::string> split_path_list(const std::string& path_list) {
    std::vector<std::string> dirs;
    std::size_t start = 0;
    while (start <= path_list.size()) {
        const std::size_t end = path_list.find(':', start);
        const std::string dir = path_list.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        dirs.push_back(dir.empty() ? std::string(".") : dir);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return dirs;
}

struct resolved_program {
    std::string path;
    bool blocked_by_permissions = false;
};

resolved_program resolve_program(const std::string& program,
                                 const std::map<std::string, std::string>& env) {
    resolved_program result;
    if (program.find('/') != std::string::npos) {
        result.path = program;
        return result;
    }
    std::string path_list = "/usr/local/bin:/usr/bin:/bin";
    const std::map<std::string, std::string>::const_iterator it = env.find("PATH");
    if (it != env.end()) {
        path_list = it->second;
    }
    for (const std::string& dir : split_path_list(path_list)) {
        const std::string candidate = dir + "/" + program;
        if (::access(candidate.c_str(), X_OK) == 0) {
            result.path = candidate;
            return result;
        }
        if (errno == EACCES) {
            result.blocked_by_permissions = true;
        }
    }
    return result;
}

struct posix_stat_fields {
    std::uint64_t parent_pid = 0;
    std::uint64_t utime_ticks = 0;
    std::uint64_t stime_ticks = 0;
    std::uint64_t start_ticks = 0;
    std::uint64_t rss_pages = 0;
};

// /proc/<pid>/stat is "pid (comm) state ppid ...". The comm field may contain
// spaces and parentheses, so the parse anchors on the last ')'.
bool parse_proc_stat(std::uint64_t pid, std::string* comm, posix_stat_fields* fields) {
    std::string text;
    if (!read_whole_file("/proc/" + std::to_string(pid) + "/stat", &text)) {
        return false;
    }
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return false;
    }
    *comm = text.substr(open + 1, close - open - 1);

    std::vector<std::string> tokens;
    std::size_t pos = close + 1;
    while (pos < text.size()) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                     text[pos] == '\n')) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t' &&
               text[pos] != '\n') {
            ++pos;
        }
        if (pos > start) {
            tokens.push_back(text.substr(start, pos - start));
        }
    }
    // tokens[0] is state; ppid is token 1, utime 11, stime 12, starttime 19,
    // rss 21.
    if (tokens.size() < 22) {
        return false;
    }
    auto number = [&tokens](std::size_t index) -> std::uint64_t {
        char* end = nullptr;
        errno = 0;
        const unsigned long long value =
            std::strtoull(tokens[index].c_str(), &end, 10);
        if (errno != 0 || end == tokens[index].c_str()) {
            return 0;
        }
        return static_cast<std::uint64_t>(value);
    };
    fields->parent_pid = number(1);
    fields->utime_ticks = number(11);
    fields->stime_ticks = number(12);
    fields->start_ticks = number(19);
    fields->rss_pages = number(21);
    return true;
}

double read_proc_uptime_seconds() {
    std::string text;
    if (!read_whole_file("/proc/uptime", &text)) {
        return 0.0;
    }
    return std::strtod(text.c_str(), nullptr);
}

long clock_ticks_per_second() {
    const long ticks = ::sysconf(_SC_CLK_TCK);
    return ticks > 0 ? ticks : 100;
}

void fill_process_from_proc(std::uint64_t pid, process_info* info) {
    posix_stat_fields fields;
    std::string comm;
    if (!parse_proc_stat(pid, &comm, &fields)) {
        return;
    }
    info->pid = pid;
    info->parent_pid = fields.parent_pid;
    info->name = comm;

    const std::uint64_t page_size = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    if (page_size > 0 && fields.rss_pages > 0) {
        info->resident_bytes = fields.rss_pages * page_size;
    }

    const long ticks = clock_ticks_per_second();
    const double uptime = read_proc_uptime_seconds();
    if (uptime > 0.0 && ticks > 0) {
        const double start_seconds = static_cast<double>(fields.start_ticks) /
                                     static_cast<double>(ticks);
        const double elapsed = uptime - start_seconds;
        if (elapsed > 0.0) {
            const double cpu_seconds =
                (static_cast<double>(fields.utime_ticks) +
                 static_cast<double>(fields.stime_ticks)) /
                static_cast<double>(ticks);
            info->cpu_percent = cpu_seconds / elapsed * 100.0;
        }
        if (start_seconds <= uptime) {
            const std::uint64_t now_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            const std::uint64_t age_ms =
                static_cast<std::uint64_t>(elapsed * 1000.0);
            if (now_ms >= age_ms) {
                info->start_time_unix_ms = now_ms - age_ms;
            }
        }
    }

    char link[4096];
    const std::string exe_path = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t length = ::readlink(exe_path.c_str(), link, sizeof(link) - 1);
    if (length > 0) {
        info->executable_path.assign(link, static_cast<std::size_t>(length));
    }

    std::string cmdline;
    if (read_whole_file("/proc/" + std::to_string(pid) + "/cmdline", &cmdline)) {
        for (std::size_t i = 0; i < cmdline.size(); ++i) {
            if (cmdline[i] == '\0') {
                cmdline[i] = ' ';
            }
        }
        while (!cmdline.empty() && cmdline.back() == ' ') {
            cmdline.pop_back();
        }
        info->command_line = cmdline;
    }
}

#endif // !CRPC_OS_WINDOWS

} // namespace

// ---------------------------------------------------------------------------
// process
// ---------------------------------------------------------------------------

struct process::impl {
#if defined(CRPC_OS_WINDOWS)
    handle_guard process_handle;
    handle_guard thread_handle;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE job = nullptr;
#else
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
#endif
    std::uint64_t pid_value = 0;
    bool exited = false;
    int exit_code = -1;

#if defined(CRPC_OS_WINDOWS)
    ~impl() {
        if (stdin_write != nullptr) {
            (void)CloseHandle(stdin_write);
        }
        if (stdout_read != nullptr) {
            (void)CloseHandle(stdout_read);
        }
        if (stderr_read != nullptr) {
            (void)CloseHandle(stderr_read);
        }
        // Closing the job with KILL_ON_JOB_CLOSE reaps any surviving child or
        // grandchild, so destroying the handle never leaks a process.
        if (job != nullptr) {
            (void)CloseHandle(job);
        }
    }
#else
    ~impl() {
        if (stdin_fd >= 0) {
            (void)::close(stdin_fd);
        }
        if (stdout_fd >= 0) {
            (void)::close(stdout_fd);
        }
        if (stderr_fd >= 0) {
            (void)::close(stderr_fd);
        }
    }
#endif
};

process::process(std::unique_ptr<impl> state) noexcept : impl_(std::move(state)) {}

process::~process() = default;
process::process(process&&) noexcept = default;
process& process::operator=(process&&) noexcept = default;

std::uint64_t process::pid() const noexcept {
    return impl_ == nullptr ? 0 : impl_->pid_value;
}

bool process::is_running() const noexcept {
    if (impl_ == nullptr || impl_->exited) {
        return false;
    }
#if defined(CRPC_OS_WINDOWS)
    return WaitForSingleObject(impl_->process_handle.value, 0) == WAIT_TIMEOUT;
#else
    if (impl_->pid <= 0) {
        return false;
    }
    if (::kill(impl_->pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

#if defined(CRPC_OS_WINDOWS)

result<std::unique_ptr<process>> process::spawn(const process_options& options) {
    if (options.program.empty()) {
        return rpc_error(rpc_code::invalid_argument, "system: process: empty program");
    }
    result<std::wstring> program = to_wide(options.program, "process");
    if (!program) {
        return program.error();
    }

    std::wstring command_line = quote_argument(program.value());
    for (const std::string& argument : options.arguments) {
        result<std::wstring> wide = to_wide(argument, "process");
        if (!wide) {
            return wide.error();
        }
        command_line.push_back(L' ');
        command_line += quote_argument(wide.value());
    }

    result<std::vector<wchar_t>> environment = build_environment(options);
    if (!environment) {
        return environment.error();
    }

    std::wstring working_directory;
    if (!options.working_directory.empty()) {
        result<std::wstring> wide = to_wide(options.working_directory, "process");
        if (!wide) {
            return wide.error();
        }
        working_directory = wide.value();
    }

    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    handle_guard stdin_read;
    handle_guard stdin_write;
    handle_guard stdout_read;
    handle_guard stdout_write;
    handle_guard stderr_read;
    handle_guard stderr_write;

    if (!CreatePipe(&stdin_read.value, &stdin_write.value, &attributes, 0)) {
        return win_error(GetLastError(), "CreatePipe", "stdin");
    }
    if (!SetHandleInformation(stdin_write.value, HANDLE_FLAG_INHERIT, 0)) {
        return win_error(GetLastError(), "SetHandleInformation", "stdin");
    }

    HANDLE child_stdout = nullptr;
    HANDLE child_stderr = nullptr;
    handle_guard inherited_stdout;
    handle_guard inherited_stderr;

    if (options.capture_stdout) {
        if (!CreatePipe(&stdout_read.value, &stdout_write.value, &attributes, 0)) {
            return win_error(GetLastError(), "CreatePipe", "stdout");
        }
        if (!SetHandleInformation(stdout_read.value, HANDLE_FLAG_INHERIT, 0)) {
            return win_error(GetLastError(), "SetHandleInformation", "stdout");
        }
        child_stdout = stdout_write.value;
    } else {
        inherited_stdout.reset(duplicate_inheritable(GetStdHandle(STD_OUTPUT_HANDLE)));
        child_stdout = inherited_stdout.value;
    }

    if (options.capture_stderr) {
        if (!CreatePipe(&stderr_read.value, &stderr_write.value, &attributes, 0)) {
            return win_error(GetLastError(), "CreatePipe", "stderr");
        }
        if (!SetHandleInformation(stderr_read.value, HANDLE_FLAG_INHERIT, 0)) {
            return win_error(GetLastError(), "SetHandleInformation", "stderr");
        }
        child_stderr = stderr_write.value;
    } else {
        inherited_stderr.reset(duplicate_inheritable(GetStdHandle(STD_ERROR_HANDLE)));
        child_stderr = inherited_stderr.value;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read.value;
    startup.hStdOutput = child_stdout;
    startup.hStdError = child_stderr;

    PROCESS_INFORMATION created = {};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, flags,
                        environment.value().data(),
                        working_directory.empty() ? nullptr : working_directory.c_str(),
                        &startup, &created)) {
        return win_error(GetLastError(), "CreateProcessW", options.program);
    }

    // The parent must not keep the child's ends of the pipes open, or EOF never
    // arrives on the read side.
    stdin_read.reset(nullptr);
    stdout_write.reset(nullptr);
    stderr_write.reset(nullptr);

    std::unique_ptr<impl> state(new impl());
    state->process_handle.reset(created.hProcess);
    state->thread_handle.reset(created.hThread);
    state->stdin_write = stdin_write.release();
    state->stdout_read = stdout_read.release();
    state->stderr_read = stderr_read.release();
    state->pid_value = static_cast<std::uint64_t>(created.dwProcessId);

    // Best effort: a job object lets kill() take the whole tree down. When the
    // caller's own process already sits in a non-nestable job the assignment
    // fails and kill() falls back to the direct child.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits)) &&
            AssignProcessToJobObject(job, state->process_handle.value)) {
            state->job = job;
        } else {
            (void)CloseHandle(job);
        }
    }

    return std::unique_ptr<process>(new process(std::move(state)));
}

result<std::string> process::read_stdout(std::size_t max_bytes) {
    if (impl_ == nullptr || impl_->stdout_read == nullptr) {
        return std::string();
    }
    if (max_bytes == 0) {
        return std::string();
    }
    const std::size_t want = std::min(max_bytes, kReadChunk);
    std::string buffer(want, '\0');
    DWORD got = 0;
    if (!ReadFile(impl_->stdout_read, &buffer[0], static_cast<DWORD>(want), &got,
                  nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
            (void)CloseHandle(impl_->stdout_read);
            impl_->stdout_read = nullptr;
            return std::string();
        }
        return win_error(code, "ReadFile", "stdout");
    }
    buffer.resize(got);
    if (got == 0) {
        (void)CloseHandle(impl_->stdout_read);
        impl_->stdout_read = nullptr;
    }
    return buffer;
}

result<std::string> process::read_stderr(std::size_t max_bytes) {
    if (impl_ == nullptr || impl_->stderr_read == nullptr) {
        return std::string();
    }
    if (max_bytes == 0) {
        return std::string();
    }
    const std::size_t want = std::min(max_bytes, kReadChunk);
    std::string buffer(want, '\0');
    DWORD got = 0;
    if (!ReadFile(impl_->stderr_read, &buffer[0], static_cast<DWORD>(want), &got,
                  nullptr)) {
        const DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF) {
            (void)CloseHandle(impl_->stderr_read);
            impl_->stderr_read = nullptr;
            return std::string();
        }
        return win_error(code, "ReadFile", "stderr");
    }
    buffer.resize(got);
    if (got == 0) {
        (void)CloseHandle(impl_->stderr_read);
        impl_->stderr_read = nullptr;
    }
    return buffer;
}

status process::write_stdin(std::string_view data) {
    if (impl_ == nullptr || impl_->stdin_write == nullptr) {
        return rpc_error(rpc_code::failed_precondition,
                         "system: process: stdin is closed");
    }
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const std::size_t remaining = data.size() - written_total;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(impl_->stdin_write, data.data() + written_total, chunk, &wrote,
                       nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA) {
                (void)CloseHandle(impl_->stdin_write);
                impl_->stdin_write = nullptr;
                return rpc_error(rpc_code::broken_pipe,
                                 "system: process: child closed stdin");
            }
            return win_error(code, "WriteFile", "stdin");
        }
        written_total += wrote;
    }
    return status();
}

status process::close_stdin() {
    if (impl_ == nullptr) {
        return status();
    }
    if (impl_->stdin_write != nullptr) {
        (void)CloseHandle(impl_->stdin_write);
        impl_->stdin_write = nullptr;
    }
    return status();
}

result<int> process::wait(std::chrono::milliseconds timeout) {
    if (impl_ == nullptr) {
        return rpc_error(rpc_code::failed_precondition,
                         "system: process: no child");
    }
    if (impl_->exited) {
        return impl_->exit_code;
    }
    const DWORD wait_ms =
        timeout.count() <= 0
            ? INFINITE
            : static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), INFINITE - 1));
    const DWORD waited = WaitForSingleObject(impl_->process_handle.value, wait_ms);
    if (waited == WAIT_TIMEOUT) {
        return rpc_error(rpc_code::deadline_exceeded,
                         "system: process: wait timed out");
    }
    if (waited != WAIT_OBJECT_0) {
        return win_error(GetLastError(), "WaitForSingleObject", "process");
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(impl_->process_handle.value, &exit_code)) {
        return win_error(GetLastError(), "GetExitCodeProcess", "process");
    }
    impl_->exited = true;
    impl_->exit_code = static_cast<int>(exit_code);
    return impl_->exit_code;
}

status process::terminate() {
    if (impl_ == nullptr || impl_->exited) {
        return status();
    }
    // CTRL_BREAK reaches the child's process group (it was created with
    // CREATE_NEW_PROCESS_GROUP). Without a console it cannot be delivered, so
    // fall back to a hard termination: "terminate" still means "make it stop".
    if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                 static_cast<DWORD>(impl_->pid_value))) {
        return status();
    }
    if (!TerminateProcess(impl_->process_handle.value, 1)) {
        return win_error(GetLastError(), "TerminateProcess", "process");
    }
    return status();
}

status process::kill() {
    if (impl_ == nullptr || impl_->exited) {
        return status();
    }
    if (impl_->job != nullptr) {
        if (TerminateJobObject(impl_->job, 1)) {
            return status();
        }
    }
    if (!TerminateProcess(impl_->process_handle.value, 1)) {
        return win_error(GetLastError(), "TerminateProcess", "process");
    }
    return status();
}

#else // POSIX

result<std::unique_ptr<process>> process::spawn(const process_options& options) {
    if (options.program.empty()) {
        return rpc_error(rpc_code::invalid_argument, "system: process: empty program");
    }

    std::map<std::string, std::string> environment =
        options.inherit_environment ? inherited_environment()
                                    : std::map<std::string, std::string>();
    for (const std::pair<const std::string, std::string>& kv : options.environment) {
        if (kv.first.empty() || kv.first.find('=') != std::string::npos) {
            return rpc_error(rpc_code::invalid_argument,
                             "system: environment variable name is invalid: '" +
                                 kv.first + "'");
        }
        environment[kv.first] = kv.second;
    }

    const resolved_program resolved = resolve_program(options.program, environment);
    if (resolved.path.empty()) {
        if (resolved.blocked_by_permissions) {
            return rpc_error(rpc_code::permission_denied,
                             "system: process: cannot execute '" + options.program +
                                 "' (permission denied)");
        }
        return rpc_error(rpc_code::not_found, "system: process: executable not found: '" +
                                                  options.program + "'");
    }

    std::vector<std::string> argument_storage;
    argument_storage.reserve(options.arguments.size() + 1);
    argument_storage.push_back(options.program);
    for (const std::string& argument : options.arguments) {
        argument_storage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(argument_storage.size() + 1);
    for (std::string& argument : argument_storage) {
        argv.push_back(&argument[0]);
    }
    argv.push_back(nullptr);

    std::vector<std::string> environment_storage;
    environment_storage.reserve(environment.size());
    for (const std::pair<const std::string, std::string>& kv : environment) {
        environment_storage.push_back(kv.first + "=" + kv.second);
    }
    std::vector<char*> envp;
    envp.reserve(environment_storage.size() + 1);
    for (std::string& entry : environment_storage) {
        envp.push_back(&entry[0]);
    }
    envp.push_back(nullptr);

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};

#if defined(CRPC_OS_LINUX)
    const bool pipe_ok = ::pipe2(stdin_pipe, O_CLOEXEC) == 0 &&
                         ::pipe2(stdout_pipe, O_CLOEXEC) == 0 &&
                         ::pipe2(stderr_pipe, O_CLOEXEC) == 0 &&
                         ::pipe2(exec_pipe, O_CLOEXEC) == 0;
#else
    // Other POSIX systems have no pipe2; set FD_CLOEXEC by hand.
    const bool pipe_ok = ::pipe(stdin_pipe) == 0 && ::pipe(stdout_pipe) == 0 &&
                         ::pipe(stderr_pipe) == 0 && ::pipe(exec_pipe) == 0;
    if (pipe_ok) {
        for (int* pair : {stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe}) {
            (void)::fcntl(pair[0], F_SETFD, FD_CLOEXEC);
            (void)::fcntl(pair[1], F_SETFD, FD_CLOEXEC);
        }
    }
#endif
    if (!pipe_ok) {
        return posix_error(errno, "pipe", "process");
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        (void)::close(stderr_pipe[0]);
        (void)::close(stderr_pipe[1]);
        (void)::close(exec_pipe[0]);
        (void)::close(exec_pipe[1]);
        return posix_error(saved, "fork", "process");
    }

    if (pid == 0) {
        // Child: only async-signal-safe calls from here to execve. All strings
        // were built before fork.
        (void)::setpgid(0, 0);
        (void)::dup2(stdin_pipe[0], STDIN_FILENO);
        if (options.capture_stdout) {
            (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
        }
        if (options.capture_stderr) {
            (void)::dup2(stderr_pipe[1], STDERR_FILENO);
        }
        (void)::close(stdin_pipe[0]);
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stdout_pipe[1]);
        (void)::close(stderr_pipe[0]);
        (void)::close(stderr_pipe[1]);
        (void)::close(exec_pipe[0]); // exec_pipe[1] stays: it reports exec failure

        if (!options.working_directory.empty() &&
            ::chdir(options.working_directory.c_str()) != 0) {
            const int saved = errno;
            // Best-effort error report to parent: the write payload is exactly sizeof(int) <= PIPE_BUF (4096),
            // so POSIX guarantees an atomic write of all 4 bytes. EINTR does not occur because we do not block
            // and no signal handler is installed in the child (no sigaction/signal here); EPIPE is practically
            // impossible since the parent blocks on read(exec_pipe[0]) until this point (and if the parent were
            // dead, SIGPIPE would kill the child before returning -1). The pipe is the only channel for exec
            // failures (chdir or execve): deleting the write causes the parent to treat EOF as success and
            // return a process handle for a child that already exited 127 (see mutation test below).
            const ssize_t reported = ::write(exec_pipe[1], &saved, sizeof(saved));
            (void)reported;
            (void)::close(exec_pipe[1]);
            _exit(127);
        }

        (void)::execve(resolved.path.c_str(), argv.data(), envp.data());
        const int saved = errno;
        // Best-effort error report to parent: the write payload is exactly sizeof(int) <= PIPE_BUF (4096),
        // so POSIX guarantees an atomic write of all 4 bytes. EINTR does not occur because we do not block
        // and no signal handler is installed in the child (no sigaction/signal here); EPIPE is practically
        // impossible since the parent blocks on read(exec_pipe[0]) until this point (and if the parent were
        // dead, SIGPIPE would kill the child before returning -1). The pipe is the only channel for exec
        // failures (chdir or execve): deleting the write causes the parent to treat EOF as success and
        // return a process handle for a child that already exited 127 (see mutation test below).
        const ssize_t reported = ::write(exec_pipe[1], &saved, sizeof(saved));
        (void)reported;
        (void)::close(exec_pipe[1]);
        _exit(127);
    }

    (void)::close(stdin_pipe[0]);
    (void)::close(stdout_pipe[1]);
    (void)::close(stderr_pipe[1]);
    (void)::close(exec_pipe[1]);
    // Put the child in its own group from the parent side too, so a kill
    // racing the child's own setpgid still targets the right group.
    (void)::setpgid(pid, pid);

    int exec_errno = 0;
    for (;;) {
        const ssize_t got = ::read(exec_pipe[0], &exec_errno, sizeof(exec_errno));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (got == 0) {
            exec_errno = 0;
            break;
        }
        break;
    }
    (void)::close(exec_pipe[0]);

    if (exec_errno != 0) {
        (void)::close(stdin_pipe[1]);
        (void)::close(stdout_pipe[0]);
        (void)::close(stderr_pipe[0]);
        int ignored_status = 0;
        (void)::waitpid(pid, &ignored_status, 0);
        return posix_error(exec_errno, "execve", resolved.path);
    }

    std::unique_ptr<impl> state(new impl());
    state->pid = pid;
    state->pid_value = static_cast<std::uint64_t>(pid);
    state->stdin_fd = stdin_pipe[1];
    state->stdout_fd = options.capture_stdout ? stdout_pipe[0] : -1;
    state->stderr_fd = options.capture_stderr ? stderr_pipe[0] : -1;
    if (!options.capture_stdout) {
        (void)::close(stdout_pipe[0]);
    }
    if (!options.capture_stderr) {
        (void)::close(stderr_pipe[0]);
    }

    return std::unique_ptr<process>(new process(std::move(state)));
}

result<std::string> process::read_stdout(std::size_t max_bytes) {
    if (impl_ == nullptr || impl_->stdout_fd < 0) {
        return std::string();
    }
    if (max_bytes == 0) {
        return std::string();
    }
    const std::size_t want = std::min(max_bytes, kReadChunk);
    std::string buffer(want, '\0');
    for (;;) {
        const ssize_t got = ::read(impl_->stdout_fd, &buffer[0], want);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posix_error(errno, "read", "stdout");
        }
        if (got == 0) {
            (void)::close(impl_->stdout_fd);
            impl_->stdout_fd = -1;
            return std::string();
        }
        buffer.resize(static_cast<std::size_t>(got));
        return buffer;
    }
}

result<std::string> process::read_stderr(std::size_t max_bytes) {
    if (impl_ == nullptr || impl_->stderr_fd < 0) {
        return std::string();
    }
    if (max_bytes == 0) {
        return std::string();
    }
    const std::size_t want = std::min(max_bytes, kReadChunk);
    std::string buffer(want, '\0');
    for (;;) {
        const ssize_t got = ::read(impl_->stderr_fd, &buffer[0], want);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posix_error(errno, "read", "stderr");
        }
        if (got == 0) {
            (void)::close(impl_->stderr_fd);
            impl_->stderr_fd = -1;
            return std::string();
        }
        buffer.resize(static_cast<std::size_t>(got));
        return buffer;
    }
}

status process::write_stdin(std::string_view data) {
    if (impl_ == nullptr || impl_->stdin_fd < 0) {
        return rpc_error(rpc_code::failed_precondition,
                         "system: process: stdin is closed");
    }
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const ssize_t wrote =
            ::write(impl_->stdin_fd, data.data() + written_total,
                    data.size() - written_total);
        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPIPE) {
                (void)::close(impl_->stdin_fd);
                impl_->stdin_fd = -1;
                return rpc_error(rpc_code::broken_pipe,
                                 "system: process: child closed stdin");
            }
            return posix_error(errno, "write", "stdin");
        }
        written_total += static_cast<std::size_t>(wrote);
    }
    return status();
}

status process::close_stdin() {
    if (impl_ == nullptr) {
        return status();
    }
    if (impl_->stdin_fd >= 0) {
        (void)::close(impl_->stdin_fd);
        impl_->stdin_fd = -1;
    }
    return status();
}

result<int> process::wait(std::chrono::milliseconds timeout) {
    if (impl_ == nullptr) {
        return rpc_error(rpc_code::failed_precondition,
                         "system: process: no child");
    }
    if (impl_->exited) {
        return impl_->exit_code;
    }
    const bool infinite = timeout.count() <= 0;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;

    for (;;) {
        int raw_status = 0;
        const pid_t reaped = ::waitpid(impl_->pid, &raw_status, WNOHANG);
        if (reaped < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posix_error(errno, "waitpid", "process");
        }
        if (reaped == impl_->pid) {
            impl_->exited = true;
            if (WIFEXITED(raw_status)) {
                impl_->exit_code = WEXITSTATUS(raw_status);
            } else if (WIFSIGNALED(raw_status)) {
                impl_->exit_code = 128 + WTERMSIG(raw_status);
            } else {
                impl_->exit_code = -1;
            }
            return impl_->exit_code;
        }
        if (!infinite && std::chrono::steady_clock::now() >= deadline) {
            return rpc_error(rpc_code::deadline_exceeded,
                             "system: process: wait timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

status process::terminate() {
    if (impl_ == nullptr || impl_->exited) {
        return status();
    }
    if (::kill(-impl_->pid, SIGTERM) != 0 && ::kill(impl_->pid, SIGTERM) != 0) {
        if (errno == ESRCH) {
            return status();
        }
        return posix_error(errno, "kill", "process");
    }
    return status();
}

status process::kill() {
    if (impl_ == nullptr || impl_->exited) {
        return status();
    }
    if (::kill(-impl_->pid, SIGKILL) != 0 && ::kill(impl_->pid, SIGKILL) != 0) {
        if (errno == ESRCH) {
            return status();
        }
        return posix_error(errno, "kill", "process");
    }
    return status();
}

#endif // CRPC_OS_WINDOWS

result<process_result> process::run(const process_options& options) {
    result<std::unique_ptr<process>> spawned = spawn(options);
    if (!spawned) {
        return spawned.error();
    }
    std::unique_ptr<process> child = std::move(spawned.value());

    process_result output;
    std::atomic<bool> stdout_truncated{false};
    std::atomic<bool> stderr_truncated{false};
    const std::size_t cap = options.max_output_bytes;

    auto drain = [cap](process* target, bool want_stdout, std::string* sink,
                       std::atomic<bool>* truncated) {
        for (;;) {
            result<std::string> chunk =
                want_stdout ? target->read_stdout(kReadChunk)
                            : target->read_stderr(kReadChunk);
            if (!chunk) {
                return; // a read error after termination is expected noise
            }
            if (chunk.value().empty()) {
                return;
            }
            const std::string& bytes = chunk.value();
            if (cap == 0) {
                sink->append(bytes);
                continue;
            }
            if (sink->size() < cap) {
                const std::size_t room = cap - sink->size();
                const std::size_t take = std::min(room, bytes.size());
                sink->append(bytes, 0, take);
                if (take < bytes.size()) {
                    truncated->store(true);
                }
            } else {
                truncated->store(true);
            }
        }
    };

    std::thread stdout_thread;
    std::thread stderr_thread;
    if (options.capture_stdout) {
        stdout_thread = std::thread(drain, child.get(), true, &output.stdout_data,
                                    &stdout_truncated);
    }
    if (options.capture_stderr) {
        stderr_thread = std::thread(drain, child.get(), false, &output.stderr_data,
                                    &stderr_truncated);
    }

    // stdin is written from its own thread: a child that fills the output
    // pipes before reading stdin would otherwise deadlock the writer.
    std::thread stdin_thread([target = child.get(), data = options.stdin_data]() {
        if (!data.empty()) {
            (void)target->write_stdin(data);
        }
        (void)target->close_stdin();
    });

    result<int> waited = child->wait(options.timeout);
    if (!waited && waited.code() == rpc_code::deadline_exceeded) {
        output.timed_out = true;
        (void)child->kill();
        // Reap even if the force-kill reported an error: a zombie here would
        // outlive the test process. The bounded wait catches a kill that raced
        // the child's own exit.
        result<int> reaped = child->wait(std::chrono::milliseconds(5000));
        if (!reaped) {
            stdin_thread.join();
            if (stdout_thread.joinable()) {
                stdout_thread.join();
            }
            if (stderr_thread.joinable()) {
                stderr_thread.join();
            }
            return reaped.error();
        }
    } else if (!waited) {
        (void)child->kill();
        (void)child->wait(std::chrono::milliseconds(5000));
        stdin_thread.join();
        if (stdout_thread.joinable()) {
            stdout_thread.join();
        }
        if (stderr_thread.joinable()) {
            stderr_thread.join();
        }
        return waited.error();
    } else {
        output.exit_code = waited.value();
        output.exited = true;
    }

    stdin_thread.join();
    if (stdout_thread.joinable()) {
        stdout_thread.join();
    }
    if (stderr_thread.joinable()) {
        stderr_thread.join();
    }

    output.stdout_truncated = stdout_truncated.load();
    output.stderr_truncated = stderr_truncated.load();
    if (output.timed_out) {
        output.exit_code = -1;
        output.exited = false;
    }
    return output;
}

// ---------------------------------------------------------------------------
// Process introspection
// ---------------------------------------------------------------------------

#if defined(CRPC_OS_WINDOWS)

result<std::vector<process_info>> list_processes() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return win_error(GetLastError(), "CreateToolhelp32Snapshot", "processes");
    }
    std::vector<process_info> processes;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    BOOL more = Process32FirstW(snapshot, &entry);
    while (more) {
        process_info info;
        info.pid = static_cast<std::uint64_t>(entry.th32ProcessID);
        info.parent_pid = static_cast<std::uint64_t>(entry.th32ParentProcessID);
        info.name = to_utf8(entry.szExeFile);
        fill_process_details(&info);
        processes.push_back(std::move(info));
        more = Process32NextW(snapshot, &entry);
    }
    (void)CloseHandle(snapshot);
    return processes;
}

result<process_info> query_process(std::uint64_t pid) {
    if (pid == 0 || pid > 0xFFFFFFFFull) {
        return rpc_error(rpc_code::not_found,
                         "system: query_process: no such process: " +
                             std::to_string(pid));
    }
    handle_guard process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(pid)));
    if (process.value == nullptr) {
        const DWORD code = GetLastError();
        if (code == ERROR_INVALID_PARAMETER) {
            return rpc_error(rpc_code::not_found,
                             "system: query_process: no such process: " +
                                 std::to_string(pid));
        }
        return win_error(code, "OpenProcess", std::to_string(pid));
    }

    process_info info;
    info.pid = pid;
    fill_process_details(&info);
    if (info.name.empty()) {
        // The limited handle could not read the image path. The process exists;
        // the snapshot name is the fallback.
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry = {};
            entry.dwSize = sizeof(entry);
            BOOL more = Process32FirstW(snapshot, &entry);
            while (more) {
                if (static_cast<std::uint64_t>(entry.th32ProcessID) == pid) {
                    info.name = to_utf8(entry.szExeFile);
                    info.parent_pid =
                        static_cast<std::uint64_t>(entry.th32ParentProcessID);
                    break;
                }
                more = Process32NextW(snapshot, &entry);
            }
            (void)CloseHandle(snapshot);
        }
    }
    return info;
}

bool is_process_alive(std::uint64_t pid) {
    if (pid == 0 || pid > 0xFFFFFFFFull) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (process == nullptr) {
        // A process we may not query still exists; the alternative is to claim
        // it is gone, which would be wrong.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0;
    const bool alive =
        GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    (void)CloseHandle(process);
    return alive;
}

#else // POSIX

result<std::vector<process_info>> list_processes() {
    DIR* directory = ::opendir("/proc");
    if (directory == nullptr) {
        return posix_error(errno, "opendir", "/proc");
    }
    std::vector<process_info> processes;
    const dirent* entry = nullptr;
    while ((entry = ::readdir(directory)) != nullptr) {
        const char* name = entry->d_name;
        bool numeric = name[0] != '\0';
        for (const char* p = name; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                numeric = false;
                break;
            }
        }
        if (!numeric) {
            continue;
        }
        const std::uint64_t pid = std::strtoull(name, nullptr, 10);
        process_info info;
        fill_process_from_proc(pid, &info);
        if (info.pid == 0) {
            continue; // exited between readdir and the read
        }
        processes.push_back(std::move(info));
    }
    (void)::closedir(directory);
    return processes;
}

result<process_info> query_process(std::uint64_t pid) {
    posix_stat_fields fields;
    std::string comm;
    if (pid == 0 || !parse_proc_stat(pid, &comm, &fields)) {
        return rpc_error(rpc_code::not_found,
                         "system: query_process: no such process: " +
                             std::to_string(pid));
    }
    process_info info;
    fill_process_from_proc(pid, &info);
    if (info.pid == 0) {
        return rpc_error(rpc_code::not_found,
                         "system: query_process: no such process: " +
                             std::to_string(pid));
    }
    return info;
}

bool is_process_alive(std::uint64_t pid) {
    if (pid == 0 || pid > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

#endif // CRPC_OS_WINDOWS

} // namespace system
} // namespace crpc
