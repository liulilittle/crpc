// CRPC -- processor topology, thread affinity and thread priority.
//
// This is the control surface next to `system/sysinfo.hpp`: sysinfo reports
// machine information, this header adds the processor counts and lets the
// calling thread be pinned to a processor and given a scheduling priority.
//
// Processor counts
//
//   `logical_processor_count` counts every active logical processor on the
//   host (not the subset a job object or cpuset happens to allow):
//     Windows  GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), i.e. all
//              Windows processor groups, not only the calling thread's group.
//     Linux    sysconf(_SC_NPROCESSORS_ONLN).
//
//   `physical_processor_count` counts physical cores:
//     Windows  one entry per RelationProcessorCore in
//              GetLogicalProcessorInformationEx.
//     Linux    unique (physical id, core id) pairs from /proc/cpuinfo; if the
//              kernel exposes core id but no physical id, unique core ids are
//              used (single-socket assumption); if neither is exposed (some
//              ARM kernels) the logical count is reported. The result is
//              clamped to the logical count because a container cpuset can
//              make /proc/cpuinfo describe more sockets than the process may
//              actually use.
//
// Thread affinity
//
//   `cpu_affinity::mask` is a bit set of processors inside `group`; bit i
//   means processor i of that group. Windows has real processor groups and
//   `group` is the group index. Linux has a single flat CPU space, so
//   `group` is always 0 there and a non-zero group is rejected.
//
//   Windows uses SetThreadGroupAffinity, the group-aware call that supersedes
//   SetThreadAffinityMask (the older call can only select processors inside
//   the thread's current group). Linux uses pthread_getaffinity_np /
//   pthread_setaffinity_np.
//
//   The 64-bit mask is the API's representable range. On Linux the kernel
//   affinity is clipped to CPUs 0..63 when read: CPUs above 63 cannot be
//   represented and are not reported, and a set call can only select CPUs
//   0..63. A read that would leave no representable CPU at all fails with
//   `failed_precondition` instead of returning an empty mask.
//
// Thread priority
//
//   The six levels are deliberately platform-neutral. The mapping is:
//
//     level          Windows (SetThreadPriority)      Linux (nice, SCHED_OTHER)
//     idle           THREAD_PRIORITY_IDLE        (-15)  19
//     lowest         THREAD_PRIORITY_LOWEST       (-2)  10
//     below_normal   THREAD_PRIORITY_BELOW_NORMAL (-1)   5
//     normal         THREAD_PRIORITY_NORMAL        (0)   0
//     above_normal   THREAD_PRIORITY_ABOVE_NORMAL  (1)  -5
//     highest        THREAD_PRIORITY_HIGHEST       (2) -10
//
//   Windows additionally has THREAD_PRIORITY_TIME_CRITICAL (15); it has no
//   neutral equivalent and is reported as `highest`. Linux reports the nice
//   value back through the ranges: >= 15 idle, 8..14 lowest, 1..7
//   below_normal, 0 normal, -8..-1 above_normal, <= -9 highest.
//
//   On Linux nice values below zero need CAP_SYS_NICE (or a matching
//   RLIMIT_NICE). The failure is reported as `permission_denied`, not hidden.
//   `pthread_setschedparam` cannot express nice under SCHED_OTHER (the
//   scheduling priority of that policy is fixed at 0), so the Unix backend
//   uses getpriority/setpriority, which is per-thread on Linux/NPTL.
//
// Error codes:
//   * invalid_argument     zero affinity mask, group out of range, mask naming
//                          a processor that does not exist in the group,
//                          unknown priority value
//   * permission_denied    the OS refused the affinity change or the nice
//                          change (EPERM/EACCES, ERROR_ACCESS_DENIED)
//   * failed_precondition  the readable affinity has no representable CPU
//   * internal             any other OS failure (native code in the message)
//   * not_found            reserved for a vanished target thread
#ifndef CRPC_SYSTEM_CPU_HPP
#define CRPC_SYSTEM_CPU_HPP

#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace system {

// A thread affinity: processors `mask` inside processor `group`.
struct CRPC_API cpu_affinity {
    std::uint64_t mask = 0;
    std::uint32_t group = 0;
};

// Number of active logical processors on the host.
[[nodiscard]] CRPC_API result<std::uint32_t> logical_processor_count();

// Number of physical processor cores. Never greater than the logical count;
// see the header comment for the per-platform degradation path.
[[nodiscard]] CRPC_API result<std::uint32_t> physical_processor_count();

// Affinity of the calling thread.
[[nodiscard]] CRPC_API result<cpu_affinity> get_current_thread_affinity();

// Sets the affinity of the calling thread. `mask` must be non-zero and may
// only name processors that are active in `group`; `group` must be a valid
// Windows processor group and is required to be 0 on Linux.
[[nodiscard]] CRPC_API status set_current_thread_affinity(std::uint64_t mask,
                                                          std::uint32_t group = 0);

// Neutral thread priority levels. See the header comment for the exact
// Windows/Linux mapping.
enum class thread_priority {
    idle,
    lowest,
    below_normal,
    normal,
    above_normal,
    highest
};

// Priority of the calling thread, mapped back to the nearest neutral level.
[[nodiscard]] CRPC_API result<thread_priority> get_current_thread_priority();

// Sets the priority of the calling thread.
[[nodiscard]] CRPC_API status set_current_thread_priority(thread_priority priority);

} // namespace system
} // namespace crpc

#endif // CRPC_SYSTEM_CPU_HPP
