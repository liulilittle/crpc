// CRPC -- regression gate for the crash-dialog suppression.
//
// This test exists because the suppression silently failing is invisible until
// it costs an entire automated run. The failure mode: on MSVC Debug a CRT
// assertion opens a modal dialog, the process blocks with zero CPU, and a
// script's `WaitForExit` never returns. The dialog is a GUI state, so no script
// can dismiss it -- the run has to be killed by hand. The same thing happened
// in this repository: a probe written for an audit called `_CrtDbgReport` and
// the whole pipeline hung on the "Debug Error!" box.
//
// Two layers must both hold, and each is asserted separately here:
//
//   * `tests/support/seh_guard.hpp` -- the guard used by standalone probes that
//     are compiled by hand and do not link the test framework.
//   * `tests/test_framework.hpp` -- the guard every `crpc_test_*` binary gets
//     through `CRPC_TEST_MAIN()`.
//
// A third property matters just as much: the guard must be OBSERVABLE. A guard
// that "works" but reports nothing hides the fault it caught. Every assertion
// below therefore checks the returned value or the printed marker, not merely
// that the process survived. If the report mode were left at the dialog
// default, the first case would hang until CTest's timeout killed it -- and a
// timeout is a failure, which is exactly the behaviour this gate wants.
#include "support/seh_guard.hpp"

#include <cstdio>
#include <cstring>

#include "test_framework.hpp"

namespace {

// `_CrtDbgReport` returns 1 when it displayed a dialog, 0 when the report was
// written and the caller continues, -1 when the report was suppressed. Any
// value other than 1 proves the dialog path is not taken. The distinction that
// matters is "returns at all": a dialog blocks inside the call.
int crt_assert_report_result() {
#if defined(_WIN32) && defined(_DEBUG)
    return _CrtDbgReport(_CRT_ASSERT, __FILE__, __LINE__, "seh_guard",
                         "deliberate assertion: the report must not open a dialog");
#else
    // Release builds do not compile the assertion, so there is nothing to
    // check; report "written" so the case's expectation holds.
    return 0;
#endif
}

} // namespace

CRPC_TEST(seh_guard_crt_assertion_reports_and_returns) {
    // `CRPC_TEST_MAIN()` already installed the framework's guard, which is the
    // one that protects this binary. Calling the standalone guard on top must
    // be harmless (it is idempotent) -- probes and tests share one contract.
    crpc::test::install_crash_guards();

    const int result = crt_assert_report_result();

    // 1 == a modal dialog was shown and the process blocked until it was
    // dismissed. In an unattended run that never happens, so reaching this line
    // with 1 would mean the guard did not take effect.
    CRPC_CHECK(result != 1);
}

CRPC_TEST(seh_guard_standalone_run_guarded_catches_structured_exception) {
    crpc::test::install_crash_guards();

    const int exit_code = crpc::test::run_guarded([] {
        volatile int* invalid = nullptr;
        *invalid = 1; // access violation: 0xC0000005
        return 0;
    });

    // A caught structured exception must become a distinct failure code, never
    // 0. The dialog, had it been allowed, would not have returned at all, so a
    // successful return here is also evidence the OS error popup is off.
    CRPC_CHECK_EQ(90, exit_code);
}

CRPC_TEST(seh_guard_run_guarded_passes_through_normal_return) {
    crpc::test::install_crash_guards();

    // The guard must not turn a healthy body's result into a failure code, or
    // every probe would report a fault it never had.
    const int exit_code = crpc::test::run_guarded([] { return 0; });
    CRPC_CHECK_EQ(0, exit_code);

    const int other = crpc::test::run_guarded([] { return 7; });
    CRPC_CHECK_EQ(7, other);
}

CRPC_TEST_MAIN()
