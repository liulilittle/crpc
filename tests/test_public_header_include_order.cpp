// CRPC -- public-header include-order regression.
//
// A default member initialiser is expanded while a header is parsed, not when a
// template is instantiated. That makes it vulnerable to function-like macros
// that a platform header installed earlier in the translation unit. The case
// that bit us: windows.h defines `max` unless NOMINMAX is set, and
// `u64 writer_epoch = std::numeric_limits<u64>::max();` then failed with
// C2589/C2059 for every consumer that included windows.h first.
//
// This file deliberately includes windows.h WITHOUT NOMINMAX, before any crpc
// header, so the macros are live while the crpc headers are parsed. If a public
// header ever grows another macro-sensitive expression, this file stops
// compiling and the build fails instead of the consumer's.
//
// Do not "fix" this file by defining NOMINMAX: doing so removes the only
// automated check that the public headers survive windows.h.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#if defined(_WIN32)
// The project build passes /DNOMINMAX to every target, which would disarm the
// very trap this file exists to catch. Undefine it here so windows.h defines
// min/max again. An out-of-tree consumer that does not set NOMINMAX gets
// exactly this state, and the in-tree build must prove the headers survive it.
#ifdef NOMINMAX
#undef NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
// Confirm the trap is actually armed on this toolchain. If a future SDK stops
// defining these macros the check below would silently stop testing anything.
#if !defined(max) || !defined(min)
#error "this regression test assumes windows.h defines min/max; rework it"
#endif
#endif

#include <limits>

#include <crpc/cluster/election.hpp>
#include <crpc/cluster/health.hpp>
#include <crpc/cluster/health_bridge.hpp>
#include <crpc/cluster/lease.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/cluster/watch.hpp>

#include "test_framework.hpp"

namespace {

// The value the header promises: a default-constructed context has not been
// given an epoch, so it must hold the sentinel that the registry rejects with
// epoch_required rather than a zero that would be silently exempt.
CRPC_TEST(write_context_default_epoch_is_the_unset_sentinel) {
    crpc::cluster::write_context context;
    CRPC_CHECK_EQ((std::numeric_limits<crpc::u64>::max)(), context.writer_epoch);
}

// The sentinel must not be treated as a usable allocation: a fresh registry
// must never hand out a value that cannot be distinguished from "unset".
CRPC_TEST(next_writer_epoch_never_returns_the_unset_sentinel) {
    crpc::cluster::registry reg;
    const auto first = reg.next_writer_epoch();
    CRPC_REQUIRE(first.ok());
    CRPC_CHECK(first.value() != (std::numeric_limits<crpc::u64>::max)());
    const auto second = reg.next_writer_epoch();
    CRPC_REQUIRE(second.ok());
    CRPC_CHECK(second.value() > first.value());
    CRPC_CHECK(second.value() != (std::numeric_limits<crpc::u64>::max)());
}

} // namespace

CRPC_TEST_MAIN()
