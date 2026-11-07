// CRPC -- processor count, thread affinity and thread priority.
//
// The assertions are about the contract, not about this machine's exact
// topology: the logical count is positive, the physical count is positive and
// never exceeds the logical count, and setting/reading back an affinity or a
// priority returns the value that was set. The observed counts and the native
// failure messages are printed so a machine-specific degradation (for example
// a container that denies sched_setaffinity, or unprivileged Linux that cannot
// lower its nice value) is recorded rather than silently ignored.
//
// Every mutation of the calling thread's affinity/priority is restored by an
// RAII guard, including when an assertion aborts the case.
#include "test_framework.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>

#include <crpc/config.hpp>
#include <crpc/system/cpu.hpp>

namespace sys = crpc::system;

namespace {

struct affinity_guard {
    sys::cpu_affinity original{};
    bool active = false;

    ~affinity_guard() {
        if (active) {
            (void)sys::set_current_thread_affinity(original.mask, original.group);
        }
    }
};

void print_status(const char* label, const crpc::status& value) {
    if (!value.ok()) {
        std::printf("  %s: %s\n", label, value.message().c_str());
    }
}

} // namespace

CRPC_TEST(processor_counts_are_consistent) {
    crpc::result<std::uint32_t> logical = sys::logical_processor_count();
    CRPC_REQUIRE(logical.ok());
    CRPC_CHECK(logical.value() >= 1);

    crpc::result<std::uint32_t> physical = sys::physical_processor_count();
    CRPC_REQUIRE(physical.ok());
    CRPC_CHECK(physical.value() >= 1);
    CRPC_CHECK(physical.value() <= logical.value());

    std::printf("  logical=%u physical=%u\n",
                static_cast<unsigned>(logical.value()),
                static_cast<unsigned>(physical.value()));
}

CRPC_TEST(affinity_round_trips_to_processor_zero) {
    crpc::result<sys::cpu_affinity> original_result =
        sys::get_current_thread_affinity();
    CRPC_REQUIRE(original_result.ok());
    const sys::cpu_affinity original = original_result.value();
    CRPC_CHECK(original.mask != 0);

    affinity_guard guard;
    guard.original = original;
    guard.active = true;

    const crpc::status set =
        sys::set_current_thread_affinity(std::uint64_t{1}, original.group);
#if !defined(CRPC_OS_WINDOWS)
    if (!set.ok() && set.code() == crpc::rpc_code::permission_denied) {
        print_status("set_current_thread_affinity", set);
        crpc_test::skip(
            "environment denies setting thread affinity (EPERM/EACCES)");
    }
#endif
    CRPC_REQUIRE(set.ok());

    crpc::result<sys::cpu_affinity> readback =
        sys::get_current_thread_affinity();
    CRPC_REQUIRE(readback.ok());
    CRPC_CHECK_EQ(original.group, readback.value().group);
    CRPC_CHECK_EQ(std::uint64_t{1}, readback.value().mask);

    // Leave the thread exactly as it was found, and prove the restore path
    // itself works instead of relying on the destructor.
    const crpc::status restored =
        sys::set_current_thread_affinity(original.mask, original.group);
    guard.active = false;
    CRPC_CHECK(restored.ok());
}

CRPC_TEST(affinity_rejects_a_zero_mask) {
    const crpc::status result = sys::set_current_thread_affinity(0);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, result.code());
    print_status("set_current_thread_affinity(0)", result);
}

CRPC_TEST(affinity_rejects_an_unknown_group) {
    // No machine has 2^32 processor groups; Linux has no group concept at all
    // and requires group == 0.
    const crpc::status result =
        sys::set_current_thread_affinity(std::uint64_t{1}, 0xFFFFFFFFu);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, result.code());
    print_status("set_current_thread_affinity(group=2^32-1)", result);
}

#if defined(CRPC_OS_WINDOWS)
CRPC_TEST(affinity_rejects_processors_outside_the_group) {
    crpc::result<std::uint32_t> logical = sys::logical_processor_count();
    CRPC_REQUIRE(logical.ok());
    if (logical.value() >= 64) {
        crpc_test::skip("two or more Windows processor groups: the single-group "
                        "mask bound is not reachable from this process");
    }
    // On a single-group machine, bit logical (the first non-existent
    // processor) must be rejected; group 0 has exactly `logical` processors.
    const std::uint64_t mask = std::uint64_t{1} << logical.value();
    const crpc::status result = sys::set_current_thread_affinity(mask, 0);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, result.code());
    print_status("set_current_thread_affinity(bit past the group)", result);
}
#endif

// Priority is per-thread on both target families, so each case probes a
// short-lived helper thread instead of the test runner thread. That also makes
// the probe self-cleaning: a thread that raised its nice value exits with it,
// and no privilege is needed to lower the value back (which unprivileged Linux
// cannot do at all -- that path is asserted explicitly below).
CRPC_TEST(priority_round_trips_above_normal) {
    std::thread worker([] {
        crpc::result<sys::thread_priority> original =
            sys::get_current_thread_priority();
        CRPC_REQUIRE(original.ok());

        const crpc::status set =
            sys::set_current_thread_priority(sys::thread_priority::above_normal);
        if (!set.ok()) {
            // Unprivileged Linux may not lower its nice value. That is an
            // environment restriction, but it must be reported with the
            // documented code, and the priority must be unchanged.
            print_status("set_current_thread_priority(above_normal)", set);
#if !defined(CRPC_OS_WINDOWS)
            CRPC_CHECK_EQ(crpc::rpc_code::permission_denied, set.code());
            crpc::result<sys::thread_priority> unchanged =
                sys::get_current_thread_priority();
            CRPC_REQUIRE(unchanged.ok());
            CRPC_CHECK_EQ(original.value(), unchanged.value());
            return;
#else
            CRPC_CHECK(set.ok());
            return;
#endif
        }

        crpc::result<sys::thread_priority> readback =
            sys::get_current_thread_priority();
        CRPC_REQUIRE(readback.ok());
        CRPC_CHECK_EQ(sys::thread_priority::above_normal, readback.value());
    });
    worker.join();
}

CRPC_TEST(priority_round_trips_below_normal) {
    std::thread worker([] {
        // Raising the nice value (below normal) never needs a privilege, so
        // this path asserts success on every supported platform.
        const crpc::status set =
            sys::set_current_thread_priority(sys::thread_priority::below_normal);
        CRPC_REQUIRE(set.ok());

        crpc::result<sys::thread_priority> readback =
            sys::get_current_thread_priority();
        CRPC_REQUIRE(readback.ok());
        CRPC_CHECK_EQ(sys::thread_priority::below_normal, readback.value());
    });
    worker.join();
}

CRPC_TEST(priority_rejects_an_unknown_value) {
    const crpc::status result = sys::set_current_thread_priority(
        static_cast<sys::thread_priority>(0x7FFF));
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, result.code());
    print_status("set_current_thread_priority(0x7FFF)", result);
}

CRPC_TEST_MAIN()
