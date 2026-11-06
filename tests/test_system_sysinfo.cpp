// CRPC -- host system information (CPU / memory / disks / identity).
//
// Every case exercises the real platform query; nothing is mocked. Values that
// are inherently machine-specific are checked for internal consistency and
// non-emptiness rather than exact numbers.
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <crpc/system/sysinfo.hpp>

namespace {
namespace sys = crpc::system;
}

CRPC_TEST(cpu_reports_topology_and_usage) {
    crpc::result<sys::cpu_info> r = sys::cpu();
    CRPC_REQUIRE(r.ok());
    const sys::cpu_info& info = r.value();

    CRPC_CHECK(info.logical_cores > 0);
    CRPC_CHECK(info.physical_cores > 0);
    CRPC_CHECK(info.physical_cores <= info.logical_cores);
    CRPC_CHECK(info.usage_percent >= 0.0);
    CRPC_CHECK(info.usage_percent <= 100.0);
#if defined(CRPC_ARCH_X86) || defined(CRPC_ARCH_X64)
    CRPC_CHECK(!info.model.empty());
#endif
}

CRPC_TEST(memory_reports_consistent_totals) {
    crpc::result<sys::memory_info> r = sys::memory();
    CRPC_REQUIRE(r.ok());
    const sys::memory_info& info = r.value();

    CRPC_CHECK(info.total_bytes > 0);
    CRPC_CHECK(info.available_bytes > 0);
    CRPC_CHECK(info.used_bytes == info.total_bytes - info.available_bytes);
    CRPC_CHECK(info.available_bytes <= info.total_bytes);
    CRPC_CHECK(info.swap_used_bytes <= info.swap_total_bytes);
}

CRPC_TEST(disks_lists_at_least_one_volume) {
    crpc::result<std::vector<sys::disk_info>> r = sys::disks();
    CRPC_REQUIRE(r.ok());
    CRPC_CHECK_FALSE(r.value().empty());

    for (const sys::disk_info& disk : r.value()) {
        CRPC_CHECK(!disk.mount_point.empty());
        CRPC_CHECK(disk.total_bytes > 0);
        CRPC_CHECK(disk.free_bytes <= disk.total_bytes);
    }
}

CRPC_TEST(identity_and_uptime_are_available) {
    crpc::result<std::string> host = sys::hostname();
    CRPC_REQUIRE(host.ok());
    CRPC_CHECK_FALSE(host.value().empty());

    crpc::result<std::string> os = sys::os_version();
    CRPC_REQUIRE(os.ok());
    CRPC_CHECK_FALSE(os.value().empty());

    crpc::result<std::uint64_t> up = sys::uptime_seconds();
    CRPC_REQUIRE(up.ok());
    CRPC_CHECK(up.value() > 0);
}

CRPC_TEST_MAIN()