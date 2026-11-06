#include <crpc/system/sysinfo.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The processor model comes from the registry, so advapi32 is required. Naming
// it here keeps the build independent of the link line (the repository's
// tests/test_framework.hpp does the same for dbghelp).
#if defined(_MSC_VER)
#pragma comment(lib, "advapi32.lib")
#endif
#else
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#endif

namespace crpc {
namespace system {

namespace {

#if defined(CRPC_OS_WINDOWS)

rpc_error win_error(DWORD code, const char* operation) {
    rpc_code mapped = rpc_code::internal;
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        mapped = rpc_code::permission_denied;
        break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NOT_READY:
        mapped = rpc_code::not_found;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        mapped = rpc_code::resource_exhausted;
        break;
    default:
        break;
    }
    return rpc_error(mapped, std::string("system: ") + operation +
                                " failed (win32=" + std::to_string(code) + ")");
}

std::string to_utf8(const wchar_t* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return std::string();
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0,
                            nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length),
                                            &utf8[0], needed, nullptr, nullptr);
    if (written != needed) {
        return std::string();
    }
    return utf8;
}

// Sums kernel+idle+user times into a single "busy vs total" sample. Kept in
// 100 ns units so 64 bits suffice for years of uptime.
struct cpu_sample {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
};

bool sample_cpu_win(cpu_sample* out) {
    FILETIME idle_time;
    FILETIME kernel_time;
    FILETIME user_time;
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return false;
    }
    ULARGE_INTEGER idle;
    idle.LowPart = idle_time.dwLowDateTime;
    idle.HighPart = idle_time.dwHighDateTime;
    ULARGE_INTEGER kernel;
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    ULARGE_INTEGER user;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;
    // Kernel time includes idle time, so total = kernel + user.
    out->idle = idle.QuadPart;
    out->total = kernel.QuadPart + user.QuadPart;
    return true;
}

std::string processor_model_win() {
    HKEY key = nullptr;
    const wchar_t* path =
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::string();
    }
    wchar_t buffer[512];
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    std::string model;
    if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::size_t length = bytes / sizeof(wchar_t);
        while (length > 0 && (buffer[length - 1] == L'\0' ||
                              buffer[length - 1] == L' ')) {
            --length;
        }
        std::size_t start = 0;
        while (start < length && buffer[start] == L' ') {
            ++start;
        }
        model = to_utf8(buffer + start, length - start);
    }
    (void)RegCloseKey(key);
    return model;
}

std::uint32_t count_physical_cores_win() {
    DWORD length = 0;
    (void)GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return 0;
    }
    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &length)) {
        return 0;
    }
    std::uint32_t cores = 0;
    std::size_t offset = 0;
    while (offset < length) {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) {
            ++cores;
        }
        if (entry->Size == 0) {
            break;
        }
        offset += entry->Size;
    }
    return cores;
}

#endif // CRPC_OS_WINDOWS

#if !defined(CRPC_OS_WINDOWS)

struct cpu_sample {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
};

bool sample_cpu_posix(cpu_sample* out) {
    std::ifstream stream("/proc/stat");
    if (!stream) {
        return false;
    }
    std::string line;
    if (!std::getline(stream, line) || line.compare(0, 3, "cpu") != 0) {
        return false;
    }
    std::istringstream fields(line.substr(3));
    std::uint64_t values[8] = {0};
    std::size_t count = 0;
    while (count < 8 && (fields >> values[count])) {
        ++count;
    }
    if (count < 4) {
        return false;
    }
    // user nice system idle iowait irq softirq steal
    out->idle = values[3] + values[4];
    out->total = 0;
    for (std::size_t i = 0; i < count && i < 8; ++i) {
        out->total += values[i];
    }
    return true;
}

std::string cpu_model_posix() {
    std::ifstream stream("/proc/cpuinfo");
    if (!stream) {
        return std::string();
    }
    std::string line;
    while (std::getline(stream, line)) {
        if (line.compare(0, 5, "model") == 0) {
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string value = line.substr(colon + 1);
            const std::size_t start = value.find_first_not_of(" \t");
            if (start == std::string::npos) {
                continue;
            }
            return value.substr(start);
        }
        if (line.compare(0, 8, "Hardware") == 0) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string value = line.substr(colon + 1);
                const std::size_t start = value.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    return value.substr(start);
                }
            }
        }
    }
    return std::string();
}

std::uint32_t count_physical_cores_posix() {
    std::ifstream stream("/proc/cpuinfo");
    if (!stream) {
        return 0;
    }
    std::set<std::pair<long, long>> packages_cores;
    long physical_id = -1;
    long core_id = -1;
    bool saw_core_id = false;
    std::string line;
    auto flush = [&]() {
        if (physical_id >= 0 && core_id >= 0) {
            packages_cores.insert({physical_id, core_id});
        }
    };
    while (std::getline(stream, line)) {
        if (line.empty()) {
            flush();
            physical_id = -1;
            core_id = -1;
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, colon);
        const std::string value = line.substr(colon + 1);
        if (key.compare(0, 11, "physical id") == 0) {
            physical_id = std::strtol(value.c_str(), nullptr, 10);
        } else if (key.compare(0, 7, "core id") == 0) {
            core_id = std::strtol(value.c_str(), nullptr, 10);
            saw_core_id = true;
        }
    }
    flush();
    if (!saw_core_id) {
        return 0; // topology not exposed (some ARM kernels)
    }
    return static_cast<std::uint32_t>(packages_cores.size());
}

#endif // !CRPC_OS_WINDOWS

} // namespace

// ---------------------------------------------------------------------------
// cpu
// ---------------------------------------------------------------------------

result<cpu_info> cpu() {
    cpu_info info;
#if defined(CRPC_OS_WINDOWS)
    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    info.logical_cores = static_cast<std::uint32_t>(system.dwNumberOfProcessors);
    info.physical_cores = count_physical_cores_win();
    info.model = processor_model_win();

    cpu_sample first;
    if (!sample_cpu_win(&first)) {
        return win_error(GetLastError(), "GetSystemTimes");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    cpu_sample second;
    if (!sample_cpu_win(&second)) {
        return win_error(GetLastError(), "GetSystemTimes");
    }
    if (second.total > first.total) {
        const std::uint64_t total = second.total - first.total;
        const std::uint64_t idle = second.idle - first.idle;
        const std::uint64_t busy = total > idle ? total - idle : 0;
        info.usage_percent =
            static_cast<double>(busy) / static_cast<double>(total) * 100.0;
    }
#else
    const long logical = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (logical <= 0) {
        return rpc_error(rpc_code::internal, "system: cpu: _SC_NPROCESSORS_ONLN failed");
    }
    info.logical_cores = static_cast<std::uint32_t>(logical);
    info.physical_cores = count_physical_cores_posix();
    info.model = cpu_model_posix();

    cpu_sample first;
    if (!sample_cpu_posix(&first)) {
        return rpc_error(rpc_code::internal, "system: cpu: cannot read /proc/stat");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    cpu_sample second;
    if (!sample_cpu_posix(&second)) {
        return rpc_error(rpc_code::internal, "system: cpu: cannot read /proc/stat");
    }
    if (second.total > first.total) {
        const std::uint64_t total = second.total - first.total;
        const std::uint64_t idle = second.idle - first.idle;
        const std::uint64_t busy = total > idle ? total - idle : 0;
        info.usage_percent =
            static_cast<double>(busy) / static_cast<double>(total) * 100.0;
    }
#endif
    // A platform that does not expose physical topology still knows the
    // logical count; reporting zero would be a lie, so fall back to it.
    if (info.physical_cores == 0) {
        info.physical_cores = info.logical_cores;
    }
    return info;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

result<memory_info> memory() {
    memory_info info;
#if defined(CRPC_OS_WINDOWS)
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return win_error(GetLastError(), "GlobalMemoryStatusEx");
    }
    info.total_bytes = status.ullTotalPhys;
    info.available_bytes = status.ullAvailPhys;
    info.used_bytes = info.total_bytes - info.available_bytes;

    // Windows has no separate "swap" pool; the committed page file is the
    // closest equivalent. The page-file part is the commit total minus the
    // physical memory, which is exactly what Task Manager labels "virtual
    // memory" usage.
    if (status.ullTotalPageFile > status.ullTotalPhys) {
        info.swap_total_bytes = status.ullTotalPageFile - status.ullTotalPhys;
        const std::uint64_t commit_used =
            status.ullTotalPageFile > status.ullAvailPageFile
                ? status.ullTotalPageFile - status.ullAvailPageFile
                : 0;
        const std::uint64_t ram_used = info.used_bytes;
        info.swap_used_bytes =
            commit_used > ram_used ? commit_used - ram_used : 0;
        if (info.swap_used_bytes > info.swap_total_bytes) {
            info.swap_used_bytes = info.swap_total_bytes;
        }
    }
#else
    std::ifstream stream("/proc/meminfo");
    if (!stream) {
        return rpc_error(rpc_code::internal, "system: memory: cannot read /proc/meminfo");
    }
    std::uint64_t total_kb = 0;
    std::uint64_t available_kb = 0;
    std::uint64_t free_kb = 0;
    std::uint64_t buffers_kb = 0;
    std::uint64_t cached_kb = 0;
    std::uint64_t swap_total_kb = 0;
    std::uint64_t swap_free_kb = 0;
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t value = 0;
        if (!(fields >> key >> value)) {
            continue;
        }
        if (key == "MemTotal:") {
            total_kb = value;
        } else if (key == "MemAvailable:") {
            available_kb = value;
        } else if (key == "MemFree:") {
            free_kb = value;
        } else if (key == "Buffers:") {
            buffers_kb = value;
        } else if (key == "Cached:") {
            cached_kb = value;
        } else if (key == "SwapTotal:") {
            swap_total_kb = value;
        } else if (key == "SwapFree:") {
            swap_free_kb = value;
        }
    }
    if (total_kb == 0) {
        return rpc_error(rpc_code::internal,
                         "system: memory: /proc/meminfo has no MemTotal");
    }
    if (available_kb == 0) {
        // Kernels older than 3.14 have no MemAvailable.
        available_kb = free_kb + buffers_kb + cached_kb;
    }
    info.total_bytes = total_kb * 1024ull;
    info.available_bytes = std::min(available_kb, total_kb) * 1024ull;
    info.used_bytes = info.total_bytes - info.available_bytes;
    info.swap_total_bytes = swap_total_kb * 1024ull;
    info.swap_used_bytes =
        swap_total_kb > swap_free_kb ? (swap_total_kb - swap_free_kb) * 1024ull : 0;
#endif
    return info;
}

// ---------------------------------------------------------------------------
// disks
// ---------------------------------------------------------------------------

result<std::vector<disk_info>> disks() {
    std::vector<disk_info> volumes;
#if defined(CRPC_OS_WINDOWS)
    const DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        return win_error(GetLastError(), "GetLogicalDrives");
    }
    for (int index = 0; index < 26; ++index) {
        if ((mask & (1u << index)) == 0) {
            continue;
        }
        wchar_t root[4] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        const UINT drive_type = GetDriveTypeW(root);
        // Network drives block for seconds when offline and RAM disks report
        // meaningless numbers; both are still real volumes, so keep them but
        // only after a capacity query succeeds.
        if (drive_type == DRIVE_NO_ROOT_DIR) {
            continue;
        }
        ULARGE_INTEGER free_for_caller;
        ULARGE_INTEGER total;
        ULARGE_INTEGER total_free;
        if (!GetDiskFreeSpaceExW(root, &free_for_caller, &total, &total_free)) {
            continue;
        }
        if (total.QuadPart == 0) {
            continue;
        }
        disk_info disk;
        disk.mount_point = to_utf8(root, 3);
        disk.total_bytes = total.QuadPart;
        disk.free_bytes = free_for_caller.QuadPart;

        wchar_t file_system[64] = {0};
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr,
                                  file_system,
                                  static_cast<DWORD>(sizeof(file_system) /
                                                     sizeof(file_system[0])))) {
            disk.filesystem =
                to_utf8(file_system, std::wcslen(file_system));
        }
        volumes.push_back(std::move(disk));
    }
#else
    std::ifstream mounts("/proc/mounts");
    if (!mounts) {
        return rpc_error(rpc_code::internal, "system: disks: cannot read /proc/mounts");
    }
    // A device mounted several times contributes one entry per mount point,
    // which is what a caller inspecting "/" and "/data" expects. Pseudo file
    // systems are skipped: they have no meaningful capacity.
    static const char* const kPseudo[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2",
        "pstore", "bpf", "tracefs", "debugfs", "securityfs", "configfs",
        "fusectl", "mqueue", "hugetlbfs", "ramfs", "binfmt_misc", "autofs",
        "efivarfs", "rpc_pipefs", "nsfs"};
    std::string line;
    std::set<std::string> seen;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string device;
        std::string mount_point;
        std::string filesystem;
        if (!(fields >> device >> mount_point >> filesystem)) {
            continue;
        }
        bool pseudo = false;
        for (const char* candidate : kPseudo) {
            if (filesystem == candidate) {
                pseudo = true;
                break;
            }
        }
        if (pseudo || mount_point.empty() || mount_point[0] != '/') {
            continue;
        }
        if (!seen.insert(mount_point).second) {
            continue;
        }
        struct statvfs stats = {};
        if (::statvfs(mount_point.c_str(), &stats) != 0) {
            continue;
        }
        const std::uint64_t block_size =
            stats.f_frsize != 0 ? stats.f_frsize : stats.f_bsize;
        const std::uint64_t total = static_cast<std::uint64_t>(stats.f_blocks) *
                                    block_size;
        if (total == 0) {
            continue;
        }
        disk_info disk;
        disk.mount_point = mount_point;
        disk.filesystem = filesystem;
        disk.total_bytes = total;
        disk.free_bytes =
            static_cast<std::uint64_t>(stats.f_bavail) * block_size;
        volumes.push_back(std::move(disk));
    }
#endif
    return volumes;
}

// ---------------------------------------------------------------------------
// identity
// ---------------------------------------------------------------------------

result<std::string> hostname() {
#if defined(CRPC_OS_WINDOWS)
    wchar_t buffer[256];
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    if (!GetComputerNameW(buffer, &size)) {
        return win_error(GetLastError(), "GetComputerNameW");
    }
    return to_utf8(buffer, static_cast<std::size_t>(size));
#else
    char buffer[256];
    if (::gethostname(buffer, sizeof(buffer)) != 0) {
        return rpc_error(rpc_code::internal,
                         std::string("system: hostname: gethostname failed (errno=") +
                             std::to_string(errno) + ")");
    }
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
#endif
}

result<std::string> os_version() {
#if defined(CRPC_OS_WINDOWS)
    // RtlGetVersion is the only reliable source: GetVersionExW lies unless the
    // executable ships a compatibility manifest for Windows 10. The layout
    // below is RTL_OSVERSIONINFOW, which is not exposed by <windows.h>.
    struct os_version_info {
        ULONG size;
        ULONG major;
        ULONG minor;
        ULONG build;
        ULONG platform_id;
        WCHAR service_pack[128];
    };
    using rtl_get_version_fn = LONG(WINAPI*)(os_version_info*);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) {
        return win_error(GetLastError(), "GetModuleHandleW(ntdll)");
    }
    const rtl_get_version_fn get_version = reinterpret_cast<rtl_get_version_fn>(
        reinterpret_cast<void*>(GetProcAddress(module, "RtlGetVersion")));
    if (get_version == nullptr) {
        return win_error(GetLastError(), "GetProcAddress(RtlGetVersion)");
    }
    os_version_info version = {};
    version.size = sizeof(version);
    if (get_version(&version) != 0) {
        return rpc_error(rpc_code::internal,
                         "system: os_version: RtlGetVersion failed");
    }
    std::string product = "Windows";
    switch (version.major) {
    case 10:
        product = version.build >= 22000 ? "Windows 11" : "Windows 10";
        break;
    case 6:
        product = version.minor == 3   ? "Windows 8.1"
                  : version.minor == 2 ? "Windows 8"
                  : version.minor == 1 ? "Windows 7"
                                       : "Windows Vista";
        break;
    case 5:
        product = version.minor == 1 ? "Windows XP" : "Windows 2000";
        break;
    default:
        break;
    }
    return product + " " + std::to_string(version.major) + "." +
           std::to_string(version.minor) + "." + std::to_string(version.build);
#else
    // PRETTY_NAME in /etc/os-release is the distribution's own name; the
    // kernel version comes from uname so the string carries both.
    std::string pretty;
    std::ifstream release("/etc/os-release");
    if (release) {
        std::string line;
        while (std::getline(release, line)) {
            if (line.compare(0, 12, "PRETTY_NAME=") == 0) {
                pretty = line.substr(12);
                if (pretty.size() >= 2 && pretty.front() == '"' &&
                    pretty.back() == '"') {
                    pretty = pretty.substr(1, pretty.size() - 2);
                }
                break;
            }
        }
    }
    struct utsname name = {};
    if (::uname(&name) != 0) {
        return rpc_error(rpc_code::internal,
                         std::string("system: os_version: uname failed (errno=") +
                             std::to_string(errno) + ")");
    }
    if (pretty.empty()) {
        pretty = name.sysname;
    }
    return pretty + " " + name.release;
#endif
}

result<std::uint64_t> uptime_seconds() {
#if defined(CRPC_OS_WINDOWS)
    const ULONGLONG milliseconds = GetTickCount64();
    return static_cast<std::uint64_t>(milliseconds / 1000ull);
#else
    std::ifstream stream("/proc/uptime");
    if (!stream) {
        return rpc_error(rpc_code::internal,
                         "system: uptime: cannot read /proc/uptime");
    }
    double seconds = 0.0;
    if (!(stream >> seconds) || seconds < 0.0) {
        return rpc_error(rpc_code::internal,
                         "system: uptime: /proc/uptime is malformed");
    }
    return static_cast<std::uint64_t>(seconds);
#endif
}

} // namespace system
} // namespace crpc
