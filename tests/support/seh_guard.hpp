// CRPC -- SEH / CRT guard for standalone probes.
//
// AGENTS.md makes this mandatory: any executable run in an automated flow must
// capture its own faults and must never raise a modal dialog. A Debug CRT
// assertion (`_CrtIsValidHeapPointer` and friends) defaults to
// `_CRTDBG_MODE_WNDW`, which opens a modal dialog and blocks the process. The
// dialog is a GUI state: a script cannot dismiss it, so `WaitForExit` hangs
// forever and the whole automation deadlocks.
//
// This header is deliberately self-contained (no dependency on
// `test_framework.hpp`) because the probes that need it are small standalone
// `main` programs compiled by hand, not part of the CTest tree.
//
// Usage:
//
//     #include "support/seh_guard.hpp"
//     int main() {
//         crpc::test::install_crash_guards();
//         return crpc::test::run_guarded([] {
//             ... probe body; return 0 on success ...
//         });
//     }
//
// `run_guarded` wraps the body in `__try`/`__except`, so a structured exception
// (null deref, heap corruption, stack overflow) becomes a printed code and a
// non-zero return instead of a dialog. It does NOT catch C++ exceptions; a
// probe that throws should catch it itself, because a thrown exception carries
// information a fault code does not.
#ifndef CRPC_TEST_SUPPORT_SEH_GUARD_HPP
#define CRPC_TEST_SUPPORT_SEH_GUARD_HPP

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <csetjmp>
#include <exception>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#else
#include <sys/types.h>
#include <unistd.h>

namespace crpc {
namespace test {
namespace detail {

// POSIX has no SEH. `run_guarded` installs this handler so a body that
// dereferences null (the exact regression the probe asserts) is reported and
// turned into the same non-zero code the Windows path returns, instead of
// killing the process. The previous handler (if any) is restored on scope exit
// so nesting stays correct.
class scoped_fault_handler {
public:
    scoped_fault_handler() {
        struct sigaction sa {};
        sa.sa_sigaction = &on_fault;
        sa.sa_flags = SA_SIGINFO;
        (void)::sigemptyset(&sa.sa_mask);
        (void)::sigaction(SIGSEGV, &sa, &previous_);
        (void)::sigaction(SIGBUS, &sa, nullptr);
        active_ = true;
    }
    ~scoped_fault_handler() {
        if (active_) {
            (void)::sigaction(SIGSEGV, &previous_, nullptr);
        }
    }

    // Set by run_guarded before the body runs; the handler longjmps back.
    static thread_local jmp_buf* escape;

private:
    static void on_fault(int, siginfo_t*, void*) {
        static const char msg[] = "SEH-GUARD: structured fault (SIGSEGV/SIGBUS)\n";
        const ssize_t wrote = ::write(2, msg, sizeof(msg) - 1);
        (void)wrote;
        jmp_buf* target = escape;
        escape = nullptr;
        if (target != nullptr) {
            longjmp(*target, 90);
        }
        _exit(90);
    }

    struct sigaction previous_ {};
    bool active_ = false;
};

inline thread_local jmp_buf* scoped_fault_handler::escape = nullptr;

} // namespace detail
} // namespace test
} // namespace crpc
#endif

namespace crpc {
namespace test {
namespace detail {

#if defined(_WIN32)
inline void on_abort(int) {
    std::fputs("SEH-GUARD: SIGABRT (abort called)\n", stderr);
    std::fflush(stderr);
    // Returning re-enters abort's message path; leave the process instead.
    _exit(3);
}

inline void on_invalid_parameter(const wchar_t* expression,
                                 const wchar_t* function, const wchar_t* file,
                                 unsigned int line, uintptr_t) {
    std::fputs("SEH-GUARD: invalid parameter", stderr);
    if (function != nullptr) std::fprintf(stderr, " in %ls", function);
    if (file != nullptr) std::fprintf(stderr, " at %ls:%u", file, line);
    if (expression != nullptr) std::fprintf(stderr, " [%ls]", expression);
    std::fputs("\n", stderr);
    std::fflush(stderr);
    _exit(4);
}

inline void on_purecall() {
    std::fputs("SEH-GUARD: pure virtual call\n", stderr);
    std::fflush(stderr);
    _exit(5);
}

inline void on_terminate() {
    std::fputs("SEH-GUARD: std::terminate\n", stderr);
    std::fflush(stderr);
    _exit(6);
}

inline LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr
                           ? info->ExceptionRecord->ExceptionCode
                           : 0;
    std::fprintf(stderr, "SEH-GUARD: unhandled exception 0x%08lX\n",
                 static_cast<unsigned long>(code));
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace detail

// Redirects every CRT report to stderr, disables the OS error dialogs, and
// installs handlers for abort/invalid-parameter/purecall/terminate and for
// unhandled structured exceptions. Idempotent; call it first in main.
inline void install_crash_guards() {
#if defined(_WIN32)
#if defined(_DEBUG)
    // The assertion dialog is `_CRT_ASSERT` in `_CRTDBG_MODE_WNDW`; move every
    // report class to the file and point the file at stderr.
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    // `_set_abort_behavior` removes the "abort() has been called" box. The
    // SIGABRT handler below is what actually terminates the process.
    (void)_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)std::signal(SIGABRT, detail::on_abort);
    (void)_set_invalid_parameter_handler(&detail::on_invalid_parameter);
    (void)_set_purecall_handler(&detail::on_purecall);
    (void)std::set_terminate(&detail::on_terminate);
    // The OS dialog is suppressed here, not by the exception filter.
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                       SEM_NOOPENFILEERRORBOX);
    (void)SetUnhandledExceptionFilter(&detail::on_unhandled);
#endif
}

// Runs `body` under SEH protection and returns its value, or a non-zero failure
// code when a structured exception fired. A `body` that returns non-void must
// return `int`.
template <typename Fn>
inline int run_guarded(Fn&& body) {
#if defined(_WIN32)
    __try {
        return static_cast<int>(std::forward<Fn>(body)());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const DWORD code = GetExceptionCode();
        std::fprintf(stderr, "SEH-GUARD: structured exception 0x%08lX\n",
                     static_cast<unsigned long>(code));
        std::fflush(stderr);
        return 90;
    }
#else
    detail::scoped_fault_handler guard;
    jmp_buf escape;
    detail::scoped_fault_handler::escape = &escape;
    const int fault = setjmp(escape);
    if (fault != 0) {
        detail::scoped_fault_handler::escape = nullptr;
        return fault;
    }
    const int out = static_cast<int>(std::forward<Fn>(body)());
    detail::scoped_fault_handler::escape = nullptr;
    return out;
#endif
}

} // namespace test
} // namespace crpc

#endif // CRPC_TEST_SUPPORT_SEH_GUARD_HPP
