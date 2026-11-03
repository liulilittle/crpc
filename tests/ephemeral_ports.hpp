// CRPC -- free ephemeral TCP ports, for tests that churn connections.
//
// Windows has 16384 ephemeral ports (49152..65535) and every connect parks its
// client-side source port in TIME_WAIT for TcpTimedWaitDelay (~120 s). A test
// that opens hundreds or thousands of connections therefore consumes the local
// range; once it is drained, connect() fails with WSAEADDRINUSE before a packet
// is sent.
//
// That matters for correctness of the tests themselves, not just for
// convenience: a loop that treats a failed connect as "skip this iteration" will
// quietly perform a small fraction of its intended work and still report green.
// The probe below lets such a test tell "the machine is drained" apart from
// "this iteration failed for a reason that matters", and skip with a stated
// reason in the first case.
//
// Off Windows this reports -1 ("unknown"), which callers must treat as "no
// guard available" rather than "plenty free".
#ifndef CRPC_TEST_EPHEMERAL_PORTS_HPP
#define CRPC_TEST_EPHEMERAL_PORTS_HPP

#if defined(_WIN32)

// <winsock2.h> must precede <windows.h>; <iphlpapi.h> needs both.
#include <winsock2.h>
#include <windows.h>

#include <iphlpapi.h>

#include <cstddef>
#include <vector>

namespace crpc_test {

struct ephemeral_port_state {
    // -1 when the system table could not be read.
    int free_ports = -1;
    int time_wait = 0;
    int range = 0;
};

inline ephemeral_port_state probe_ephemeral_ports() {
    ephemeral_port_state out;
    DWORD size = 0;
    if (::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0) !=
        ERROR_INSUFFICIENT_BUFFER) {
        return out;
    }
    std::vector<char> buf(size);
    if (::GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                              TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return out;
    }
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
    constexpr int kLo = 49152;
    constexpr int kHi = 65535;
    out.range = kHi - kLo + 1;
    std::vector<bool> used(static_cast<std::size_t>(out.range), false);
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const int local = static_cast<int>(
            ntohs(static_cast<unsigned short>(table->table[i].dwLocalPort)));
        if (local >= kLo && local <= kHi) {
            used[static_cast<std::size_t>(local - kLo)] = true;
            if (table->table[i].dwState == MIB_TCP_STATE_TIME_WAIT) {
                ++out.time_wait;
            }
        }
    }
    int free_ports = 0;
    for (bool b : used) {
        if (!b) {
            ++free_ports;
        }
    }
    out.free_ports = free_ports;
    return out;
}

// True when the machine is close enough to drained that a connection-churning
// case cannot run to completion. 4096 is a quarter of the range: measured, a
// drained machine still refuses connects while ~1600 ports report free, so a
// threshold near the case's own demand would pass the guard and then fail.
inline bool ephemeral_ports_scarce(const ephemeral_port_state& s) noexcept {
    return s.free_ports >= 0 && s.free_ports < 4096;
}

} // namespace crpc_test

#else

namespace crpc_test {

struct ephemeral_port_state {
    int free_ports = -1;
    int time_wait = 0;
    int range = 0;
};

inline ephemeral_port_state probe_ephemeral_ports() { return ephemeral_port_state{}; }

inline bool ephemeral_ports_scarce(const ephemeral_port_state&) noexcept {
    return false;
}

} // namespace crpc_test

#endif // _WIN32

#endif // CRPC_TEST_EPHEMERAL_PORTS_HPP
