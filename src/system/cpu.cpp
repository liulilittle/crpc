#include <crpc/system/cpu.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <system_error>
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

#else

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>

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
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        mapped = rpc_code::invalid_argument;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        mapped = rpc_code::resource_exhausted;
        break;
    default:
        break;
    }
    return rpc_error(mapped, std::string("system: cpu: ") + operation +
                                " failed (win32=" + std::to_string(code) + ")");
}

#else

rpc_error native_error(int code, const char* operation) {
    rpc_code mapped = rpc_code::internal;
    switch (code) {
    case EACCES:
    case EPERM:
        mapped = rpc_code::permission_denied;
        break;
    case EINVAL:
        mapped = rpc_code::invalid_argument;
        break;
    case ESRCH:
        mapped = rpc_code::not_found;
        break;
    case ENOMEM:
        mapped = rpc_code::resource_exhausted;
        break;
    default:
        break;
    }
    return rpc_error(
        mapped, std::string("system: cpu: ") + operation + " failed (errno=" +
                    std::to_string(code) + ": " +
                    std::generic_category().message(code) + ")");
}

std::uint32_t physical_cores_from_cpuinfo(std::uint32_t fallback) {
    std::ifstream stream("/proc/cpuinfo");
    if (!stream) {
        return fallback;
    }

    // One (physical id, core id) pair per logical processor, deduplicated.
    // Kernels that expose core id without physical id (some hypervisors and
    // ARM boards) are handled with a single-socket assumption; a kernel that
    // exposes neither falls back to the logical count.
    std::set<std::pair<long, long>> socket_cores;
    std::set<long> bare_cores;
    long physical_id = -1;
    long core_id = -1;
    auto flush = [&socket_cores, &bare_cores, &physical_id, &core_id] {
        if (core_id >= 0) {
            if (physical_id >= 0) {
                socket_cores.insert({physical_id, core_id});
            } else {
                bare_cores.insert(core_id);
            }
        }
        physical_id = -1;
        core_id = -1;
    };

    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            flush();
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (line.compare(0, 11, "physical id") == 0) {
            physical_id = std::strtol(line.c_str() + colon + 1, nullptr, 10);
        } else if (line.compare(0, 7, "core id") == 0) {
            core_id = std::strtol(line.c_str() + colon + 1, nullptr, 10);
        }
    }
    flush();

    const std::size_t count =
        socket_cores.empty() ? bare_cores.size() : socket_cores.size();
    if (count == 0) {
        return fallback;
    }
    if (count > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(count);
}

#endif // CRPC_OS_WINDOWS

} // namespace

result<std::uint32_t> logical_processor_count() {
#if defined(CRPC_OS_WINDOWS)
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count == 0) {
        return win_error(GetLastError(), "GetActiveProcessorCount");
    }
    return static_cast<std::uint32_t>(count);
#else
    errno = 0;
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (count <= 0) {
        if (errno != 0) {
            return native_error(errno, "sysconf(_SC_NPROCESSORS_ONLN)");
        }
        return rpc_error(rpc_code::internal,
                         "system: cpu: sysconf(_SC_NPROCESSORS_ONLN) "
                         "reported no online processor");
    }
    return static_cast<std::uint32_t>(count);
#endif
}

result<std::uint32_t> physical_processor_count() {
#if defined(CRPC_OS_WINDOWS)
    DWORD length = 0;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length)) {
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            return win_error(error, "GetLogicalProcessorInformationEx(size)");
        }
    }
    if (length == 0) {
        return rpc_error(rpc_code::internal,
                         "system: cpu: GetLogicalProcessorInformationEx returned "
                         "an empty topology");
    }

    std::vector<unsigned char> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &length)) {
        return win_error(GetLastError(), "GetLogicalProcessorInformationEx");
    }

    std::uint32_t cores = 0;
    std::size_t offset = 0;
    while (offset < length) {
        const auto* entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
        if (entry->Size == 0 || entry->Size > length - offset) {
            return rpc_error(rpc_code::internal,
                             "system: cpu: GetLogicalProcessorInformationEx "
                             "returned a malformed entry");
        }
        if (entry->Relationship == RelationProcessorCore) {
            ++cores;
        }
        offset += entry->Size;
    }
    if (cores == 0) {
        return rpc_error(rpc_code::internal,
                         "system: cpu: topology contains no processor core");
    }
    return cores;
#else
    crpc::result<std::uint32_t> logical = logical_processor_count();
    if (!logical.ok()) {
        return logical.error();
    }
    // /proc/cpuinfo may describe the host while a container cpuset narrows the
    // usable set; never report more physical cores than usable logical CPUs.
    std::uint32_t physical = physical_cores_from_cpuinfo(logical.value());
    if (physical > logical.value()) {
        physical = logical.value();
    }
    return physical;
#endif
}

result<cpu_affinity> get_current_thread_affinity() {
#if defined(CRPC_OS_WINDOWS)
    GROUP_AFFINITY native = {};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &native)) {
        return win_error(GetLastError(), "GetThreadGroupAffinity");
    }
    cpu_affinity affinity;
    affinity.mask = static_cast<std::uint64_t>(native.Mask);
    affinity.group = static_cast<std::uint32_t>(native.Group);
    return affinity;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    const int rc =
        pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        return native_error(rc, "pthread_getaffinity_np");
    }

    // cpu_affinity::mask can represent CPUs 0..63 only. CPUs beyond that are
    // clipped (documented); an affinity with no representable CPU is an error,
    // not an empty mask.
    std::uint64_t mask = 0;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            mask |= std::uint64_t{1} << cpu;
        }
    }
    if (mask == 0) {
        return rpc_error(rpc_code::failed_precondition,
                         "system: cpu: thread affinity has no CPU in the "
                         "0..63 representable range");
    }
    cpu_affinity affinity;
    affinity.mask = mask;
    affinity.group = 0;
    return affinity;
#endif
}

status set_current_thread_affinity(std::uint64_t mask, std::uint32_t group) {
    if (mask == 0) {
        return status(rpc_code::invalid_argument,
                      "system: cpu: affinity mask is empty");
    }

#if defined(CRPC_OS_WINDOWS)
    if (mask > static_cast<std::uint64_t>(
                   (std::numeric_limits<KAFFINITY>::max)())) {
        return status(rpc_code::invalid_argument,
                      "system: cpu: affinity mask does not fit this target's "
                      "KAFFINITY");
    }

    const WORD group_count = GetActiveProcessorGroupCount();
    if (group >= static_cast<std::uint32_t>(group_count)) {
        return status(rpc_code::invalid_argument,
                      "system: cpu: processor group " + std::to_string(group) +
                          " does not exist (" + std::to_string(group_count) +
                          " group(s) active)");
    }

    const DWORD processors =
        GetActiveProcessorCount(static_cast<WORD>(group));
    if (processors == 0) {
        return win_error(GetLastError(), "GetActiveProcessorCount");
    }
    if (processors < 64) {
        const std::uint64_t active =
            (std::uint64_t{1} << processors) - std::uint64_t{1};
        if ((mask & ~active) != 0) {
            return status(rpc_code::invalid_argument,
                          "system: cpu: affinity mask names a processor that "
                          "is not active in group " +
                              std::to_string(group));
        }
    }

    GROUP_AFFINITY native = {};
    native.Mask = static_cast<KAFFINITY>(mask);
    native.Group = static_cast<WORD>(group);
    if (!SetThreadGroupAffinity(GetCurrentThread(), &native, nullptr)) {
        return win_error(GetLastError(), "SetThreadGroupAffinity");
    }
    return status();
#else
    if (group != 0) {
        return status(rpc_code::invalid_argument,
                      "system: cpu: Linux has one flat CPU space; processor "
                      "group must be 0");
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu) {
        if ((mask & (std::uint64_t{1} << cpu)) != 0) {
            CPU_SET(cpu, &set);
        }
    }
    const int rc =
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        return native_error(rc, "pthread_setaffinity_np");
    }
    return status();
#endif
}

result<thread_priority> get_current_thread_priority() {
#if defined(CRPC_OS_WINDOWS)
    const int native = GetThreadPriority(GetCurrentThread());
    if (native == THREAD_PRIORITY_ERROR_RETURN) {
        return win_error(GetLastError(), "GetThreadPriority");
    }
    if (native <= THREAD_PRIORITY_IDLE) {
        return thread_priority::idle;
    }
    if (native <= THREAD_PRIORITY_LOWEST) {
        return thread_priority::lowest;
    }
    if (native <= THREAD_PRIORITY_BELOW_NORMAL) {
        return thread_priority::below_normal;
    }
    if (native <= THREAD_PRIORITY_NORMAL) {
        return thread_priority::normal;
    }
    if (native <= THREAD_PRIORITY_ABOVE_NORMAL) {
        return thread_priority::above_normal;
    }
    // THREAD_PRIORITY_HIGHEST and THREAD_PRIORITY_TIME_CRITICAL both map to
    // the neutral `highest` level (the latter has no separate level).
    return thread_priority::highest;
#else
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, 0);
    if (nice == -1 && errno != 0) {
        return native_error(errno, "getpriority");
    }
    if (nice >= 15) {
        return thread_priority::idle;
    }
    if (nice >= 8) {
        return thread_priority::lowest;
    }
    if (nice >= 1) {
        return thread_priority::below_normal;
    }
    if (nice == 0) {
        return thread_priority::normal;
    }
    if (nice >= -8) {
        return thread_priority::above_normal;
    }
    return thread_priority::highest;
#endif
}

status set_current_thread_priority(thread_priority priority) {
#if defined(CRPC_OS_WINDOWS)
    int native = 0;
    switch (priority) {
    case thread_priority::idle:
        native = THREAD_PRIORITY_IDLE;
        break;
    case thread_priority::lowest:
        native = THREAD_PRIORITY_LOWEST;
        break;
    case thread_priority::below_normal:
        native = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case thread_priority::normal:
        native = THREAD_PRIORITY_NORMAL;
        break;
    case thread_priority::above_normal:
        native = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case thread_priority::highest:
        native = THREAD_PRIORITY_HIGHEST;
        break;
    default:
        return status(rpc_code::invalid_argument,
                      "system: cpu: unknown thread priority value");
    }
    if (!SetThreadPriority(GetCurrentThread(), native)) {
        return win_error(GetLastError(), "SetThreadPriority");
    }
    return status();
#else
    int nice = 0;
    switch (priority) {
    case thread_priority::idle:
        nice = 19;
        break;
    case thread_priority::lowest:
        nice = 10;
        break;
    case thread_priority::below_normal:
        nice = 5;
        break;
    case thread_priority::normal:
        nice = 0;
        break;
    case thread_priority::above_normal:
        nice = -5;
        break;
    case thread_priority::highest:
        nice = -10;
        break;
    default:
        return status(rpc_code::invalid_argument,
                      "system: cpu: unknown thread priority value");
    }
    errno = 0;
    if (::setpriority(PRIO_PROCESS, 0, nice) != 0) {
        return native_error(errno, "setpriority");
    }
    return status();
#endif
}

} // namespace system
} // namespace crpc
