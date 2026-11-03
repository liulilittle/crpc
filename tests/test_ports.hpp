// CRPC -- deterministic listen ports for tests, kept out of the dynamic range.
//
// Windows hands out outgoing connections' source ports from its dynamic range,
// measured on this host with `netsh int ipv4 show dynamicport tcp` as
// 49152..65535. A test that *listens* on a port inside that range can lose the
// bind to one of its own client connections created a moment earlier: the
// kernel may have already claimed that number for a source port. That is not
// hypothetical -- it is the recorded cause of a `crpc_test_io_epoll` failure in
// this repository (its listen ports came from a span that overlapped the
// kernel's client ports, and a later bind failed with EADDRINUSE while the port
// was sitting in TIME_WAIT).
//
// So every listen port a test picks stays below `k_dynamic_port_start`. The
// helper is pure (no pid lookup, no clock) so it can be tested exhaustively:
// see tests/test_ports.cpp, which asserts the whole input space lands inside
// the safe window. Mutation-checked by widening the span past the dynamic range
// start, which turns that test red.
//
// Callers pass their own band start so two binaries running under a parallel
// `ctest` do not share a window, and their own step so repeated calls inside one
// binary walk forward instead of repeating.
#ifndef CRPC_TEST_PORTS_HPP
#define CRPC_TEST_PORTS_HPP

#include <cstdint>

namespace crpc_test {

// Measured on the development host and true of the Windows default. Linux does
// not reserve a comparable block for this, but staying below it is harmless
// there and keeps one rule for both platforms.
constexpr unsigned k_dynamic_port_start = 49152;

// 1024..37023. Chosen so the top of the window is a comfortable margin below
// 49152 rather than merely one port under it: the range a test consumes grows
// with its case count, and the margin has to survive that.
constexpr unsigned k_test_port_floor = 1024;
constexpr unsigned k_test_port_span = 36000;
constexpr unsigned k_test_port_pid_span = 10000;

static_assert(k_test_port_floor + k_test_port_span <= k_dynamic_port_start,
              "the test port window must stay below the dynamic port range");

// The port for a given pid, band start and allocation step. Pure and total: any
// input produces a port inside [floor, floor + span).
constexpr unsigned test_port_for(unsigned pid, unsigned band_start,
                                 unsigned step) noexcept {
    return k_test_port_floor +
           ((band_start + pid % k_test_port_pid_span) + step) % k_test_port_span;
}

} // namespace crpc_test

#endif // CRPC_TEST_PORTS_HPP
