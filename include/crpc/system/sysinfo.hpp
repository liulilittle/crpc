// CRPC -- host system information (CPU / memory / disks / identity).
//
// Every function performs a real platform query and reports the result as a
// value. Values a platform cannot provide are left at their default (0 or an
// empty string) rather than being invented. Error codes follow the rest of the
// library: `internal` for an unexpected OS failure (the message carries the
// native code), `permission_denied` when access was refused.
//
// `cpu().usage_percent` is measured over a short sampling window inside the
// call (two /proc/stat or GetSystemTimes samples), so the first call is
// meaningful without the caller keeping state. 100% means every logical core
// is busy.
#ifndef CRPC_SYSTEM_SYSINFO_HPP
#define CRPC_SYSTEM_SYSINFO_HPP

#include <cstdint>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace system {

struct CRPC_API cpu_info {
    // Marketing model name (e.g. "AMD Ryzen 9 5950X 16-Core Processor").
    std::string model;
    std::uint32_t logical_cores = 0;
    std::uint32_t physical_cores = 0;
    double usage_percent = 0.0;
};

struct CRPC_API memory_info {
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
    std::uint64_t used_bytes = 0;
    // Swap / page-file figures. On Windows these are derived from the commit
    // limit (RAM + page file) with the RAM part subtracted; see the
    // implementation note there.
    std::uint64_t swap_total_bytes = 0;
    std::uint64_t swap_used_bytes = 0;
};

struct CRPC_API disk_info {
    // Volume root ("C:\\") on Windows, mount point ("/", "/data") on POSIX.
    std::string mount_point;
    // File-system name ("NTFS", "ext4", ...); empty when unknown.
    std::string filesystem;
    std::uint64_t total_bytes = 0;
    // Space available to the calling user.
    std::uint64_t free_bytes = 0;
};

// CPU model, topology and a short-window usage sample.
[[nodiscard]] CRPC_API result<cpu_info> cpu();

// Physical memory and swap figures.
[[nodiscard]] CRPC_API result<memory_info> memory();

// Mounted disks with non-zero capacity.
[[nodiscard]] CRPC_API result<std::vector<disk_info>> disks();

// Computer name (without a domain suffix where the platform distinguishes).
[[nodiscard]] CRPC_API result<std::string> hostname();

// Human-readable OS version, e.g. "Windows 10.0.19045" or
// "Ubuntu 24.04.1 LTS".
[[nodiscard]] CRPC_API result<std::string> os_version();

// Seconds since the system booted.
[[nodiscard]] CRPC_API result<std::uint64_t> uptime_seconds();

} // namespace system
} // namespace crpc

#endif // CRPC_SYSTEM_SYSINFO_HPP
