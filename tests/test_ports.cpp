// CRPC -- checks on the test-suite's listen-port allocator.
//
// `tests/test_ports.hpp` exists so no test listens on a port the OS may already
// have handed to an outgoing connection. That property is only worth claiming if
// it holds for every input, so this walks a grid that is deliberately WIDER than
// what the helper's own constants promise: the pid range and step range below
// are fixed literals, not derived from `k_test_port_pid_span` /
// `k_test_port_span`. Deriving them from the constants made an earlier version
// of this test blind to exactly the mutation it exists to catch -- widening the
// pid span inside the helper also widened the range the test walked, so the two
// moved together and the check stayed green.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdio>

#include "test_ports.hpp"

namespace {

// The band starts actually used by the suite. A new band must be added here.
constexpr unsigned k_bands[] = {12000u, 26000u, 26200u, 28000u};

// Independent of the header's constants on purpose (see the file comment). pids
// on Windows are small numbers; walking to 80000 covers any plausible value,
// stepped so the whole grid stays fast.
constexpr unsigned kPidMax = 80000u;
constexpr unsigned kPidStep = 13u;
constexpr unsigned kStepMax = 4000u;
constexpr unsigned kStepStep = 61u;

} // namespace

CRPC_TEST(test_ports_stay_below_the_dynamic_range) {
    unsigned worst = 0;
    unsigned checked = 0;
    for (unsigned band : k_bands) {
        for (unsigned pid = 0; pid <= kPidMax; pid += kPidStep) {
            for (unsigned step = 0; step <= kStepMax; step += kStepStep) {
                const unsigned port = crpc_test::test_port_for(pid, band, step);
                ++checked;
                if (port < crpc_test::k_test_port_floor) {
                    std::printf(
                        "  port %u below the floor (pid=%u band=%u step=%u)\n",
                        port, pid, band, step);
                    CRPC_CHECK(false);
                    return;
                }
                if (port >= crpc_test::k_dynamic_port_start) {
                    std::printf(
                        "  port %u is inside the dynamic range (pid=%u band=%u "
                        "step=%u)\n",
                        port, pid, band, step);
                    CRPC_CHECK(false);
                    return;
                }
                if (port > worst) {
                    worst = port;
                }
            }
        }
    }
    std::printf(
        "  %u ports over %zu bands: worst %u, dynamic range starts at %u\n",
        checked, sizeof(k_bands) / sizeof(k_bands[0]), worst,
        crpc_test::k_dynamic_port_start);
    CRPC_CHECK(checked > 1000000u);
    CRPC_CHECK(worst + 1 < crpc_test::k_dynamic_port_start);
}

CRPC_TEST(test_ports_walk_forward_within_a_band) {
    // Consecutive allocations in one binary must not repeat, or the second
    // listen would fail on the first one's port.
    const unsigned band = 28000u;
    unsigned previous = crpc_test::test_port_for(4242u, band, 0);
    for (unsigned step = 11; step <= 1100; step += 11) {
        const unsigned port = crpc_test::test_port_for(4242u, band, step);
        CRPC_CHECK(port != previous);
        previous = port;
    }
}

CRPC_TEST_MAIN()