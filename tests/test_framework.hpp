// CRPC -- test harness.
//
// Two things here are not about testing as such but about what happens when a
// test fails badly:
//
//   1. On MSVC Debug, a CRT assertion, an invalid-parameter report or an
//      abort() normally opens a modal "Debug Error!" dialog. In an unattended
//      run that dialog blocks the desktop and the test never returns, so CI
//      hangs instead of failing. Every report mode is redirected to stderr and
//      abort() is turned into a message plus a non-zero exit code. This is the
//      same treatment the corocc harness uses, for the same reason.
//
//   2. A crash (access violation, stack overflow, pure virtual call) is written
//      to a minidump next to the test log, with the crashing thread and the
//      fault address. Without a dump a Windows crash leaves nothing behind but
//      an exit code; with one, the failure is diagnosable after the fact.
//
// Both are debug-build only. A Release build behaves exactly as it would
// without this header.
#ifndef CRPC_TEST_FRAMEWORK_HPP
#define CRPC_TEST_FRAMEWORK_HPP

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// --- Early CRT silencing and crash capture ----------------------------------

#if defined(_MSC_VER) && defined(_DEBUG)

#include <crtdbg.h>
#include <process.h>
#include <signal.h>

// NOMINMAX is mandatory: <windows.h> otherwise defines min/max as macros and
// every std::min/std::max in a translation unit that includes this header
// fails to compile. WIN32_LEAN_AND_MEAN keeps the header light.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace crpc_test {
namespace detail {

inline char* dump_path_storage() {
    static char path[MAX_PATH] = {0};
    return path;
}

// Writes a minidump for `info`. Called from the unhandled-exception filter,
// where almost nothing is safe: no allocation, no C++ objects, no CRT calls
// that can take a lock the crashing thread already holds. CreateFileA,
// MiniDumpWriteDump and CloseHandle are the documented-safe subset.
inline void write_minidump(EXCEPTION_POINTERS* info) {
    char* path = dump_path_storage();
    if (path[0] == '\0') {
        return;
    }
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION exception_info = {};
    exception_info.ThreadId = GetCurrentThreadId();
    exception_info.ExceptionPointers = info;
    exception_info.ClientPointers = FALSE;

    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);

    (void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                            &exception_info, nullptr, nullptr);
    (void)CloseHandle(file);
}

inline LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr
                           ? info->ExceptionRecord->ExceptionCode
                           : 0;
    void* address = info != nullptr && info->ExceptionRecord != nullptr
                        ? info->ExceptionRecord->ExceptionAddress
                        : nullptr;

    std::fprintf(stderr,
                 "CRASH: unhandled exception 0x%08lX at %p (thread %lu)\n",
                 static_cast<unsigned long>(code), address,
                 static_cast<unsigned long>(GetCurrentThreadId()));
    std::fflush(stderr);

    write_minidump(info);
    if (dump_path_storage()[0] != '\0') {
        std::fprintf(stderr, "CRASH: minidump written to %s\n", dump_path_storage());
        std::fflush(stderr);
    }
    // EXCEPTION_EXECUTE_HANDLER would swallow the crash; the process must still
    // terminate with a failure code so the caller sees a non-zero exit.
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void on_abort(int sig) {
    (void)sig;
    std::fputs("CRPC-TEST: SIGABRT (abort() called)\n", stderr);
    std::fflush(stderr);
    // Returning re-enters abort()'s message path; leave the process instead.
    _exit(3);
}

// Invalid parameters (e.g. a bad format string, a null buffer passed to the CRT)
// raise a report that opens the same modal dialog as an assertion. The handler
// prints and exits; it must not return, because returning lets the CRT continue
// with an invalid argument.
inline void on_invalid_parameter(const wchar_t* expression,
                                 const wchar_t* function,
                                 const wchar_t* file,
                                 unsigned int line,
                                 uintptr_t reserved) {
    (void)reserved;
    std::fputs("CRPC-TEST: invalid parameter", stderr);
    if (function != nullptr) {
        std::fprintf(stderr, " in %ls", function);
    }
    if (file != nullptr) {
        std::fprintf(stderr, " at %ls:%u", file, line);
    }
    if (expression != nullptr) {
        std::fprintf(stderr, " [%ls]", expression);
    }
    std::fputs("\n", stderr);
    std::fflush(stderr);
    _exit(4);
}

// A pure virtual call or a failed std::terminate would also show a dialog in
// some configurations. Route both to stderr and a non-zero exit.
inline void on_purecall() {
    std::fputs("CRPC-TEST: pure virtual function call\n", stderr);
    std::fflush(stderr);
    _exit(5);
}

inline void on_terminate() {
    std::fputs("CRPC-TEST: std::terminate called\n", stderr);
    std::fflush(stderr);
    _exit(6);
}

inline int quiet_crt() {
    (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    // Write the abort message but never call report-fault (the dialog).
    (void)_set_abort_behavior(_WRITE_ABORT_MSG,
                              _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    (void)signal(SIGABRT, on_abort);
    (void)_set_invalid_parameter_handler(&on_invalid_parameter);
    (void)_set_purecall_handler(&on_purecall);
    (void)std::set_terminate(&on_terminate);
    (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                       SEM_NOOPENFILEERRORBOX);
    (void)SetUnhandledExceptionFilter(&unhandled_exception_filter);
    return 0;
}

// The guards are installed from main, not from a CRT initializer.
//
// An earlier version put a constructor pointer in .CRT$XCU so the guards were
// active before any other static initializer. That is a well-known MSVC trick,
// but it has a side effect that cost real debugging time: declaring the section
// `read` makes the compiler skip dynamic initialization for later `static
// inline` members in the same translation unit. RTTI's reflected field tables
// are exactly such members, so a test translation unit that included this
// header silently got empty field tables. Installing the guards at the top of
// main is equivalent for a test binary -- no test code runs before main -- and
// has no effect on other static data.
//
// A test binary that does not use CRPC_TEST_MAIN must call
// crpc_test::install_crash_guards() itself.

} // namespace detail

// Installs the CRT report redirection, the abort/invalid-parameter/purecall/
// terminate handlers, and the minidump writer. Idempotent.
inline void install_crash_guards() {
#if defined(_MSC_VER) && defined(_DEBUG)
    (void)detail::quiet_crt();
#endif
}

} // namespace crpc_test

#define CRPC_TEST_EARLY() ((void)::crpc_test::install_crash_guards())

#else

#define CRPC_TEST_EARLY() ((void)0)

#endif

namespace crpc_test {

// Sets where a minidump is written. On Windows debug builds this must be called
// before the first failing assertion to be useful; it is safe to call later for
// crashes. On other platforms it only records the path for the report.
inline void set_minidump_path(const char* path) {
#if defined(_MSC_VER) && defined(_DEBUG)
    if (path == nullptr) {
        return;
    }
    char* storage = detail::dump_path_storage();
    const std::size_t limit = sizeof(char) * MAX_PATH;
    std::size_t i = 0;
    for (; path[i] != '\0' && i + 1 < limit; ++i) {
        storage[i] = path[i];
    }
    storage[i] = '\0';
#else
    (void)path;
#endif
}

struct case_entry {
    const char* name;
    void (*fn)();
};

inline std::vector<case_entry>& registry() {
    static std::vector<case_entry> entries;
    return entries;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline int& check_count() {
    static int count = 0;
    return count;
}

inline const char*& current_case() {
    static const char* name = "<none>";
    return name;
}

inline int& skip_count() {
    static int count = 0;
    return count;
}

// Thrown by skip(). It deliberately does not derive from std::exception so the
// generic "uncaught exception" handler in run_all() cannot turn a skip into a
// failure.
struct skipped_case {};

// Marks the current case as skipped: prints one visible SKIP line containing the
// reason, then unwinds to run_all(), which counts the case as passed (exit code
// is unaffected) and includes it in the skipped total.
[[noreturn]] inline void skip(const char* reason) {
    ++skip_count();
    std::printf("SKIP %s: %s\n", current_case(),
                reason != nullptr ? reason : "no reason given");
    std::fflush(stdout);
    throw skipped_case{};
}

struct registrar {
    registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const char* expression,
                           const std::string& detail) {
    ++failure_count();
    std::fprintf(stderr, "FAIL %s:%d\n  case: %s\n  expr: %s\n  %s\n", file, line,
                 current_case(), expression, detail.c_str());
    std::fflush(stderr);
}

template <typename A, typename B>
void check_equal(const char* file, int line, const char* expression, const A& expected,
                 const B& actual) {
    ++check_count();
    if (!(expected == actual)) {
        std::ostringstream out;
        out << "expected: " << expected << "\n  actual:   " << actual;
        report_failure(file, line, expression, out.str());
    }
}

inline void check_true(const char* file, int line, const char* expression, bool value) {
    ++check_count();
    if (!value) {
        report_failure(file, line, expression, "expected: true\n  actual:   false");
    }
}

inline void check_false(const char* file, int line, const char* expression, bool value) {
    ++check_count();
    if (value) {
        report_failure(file, line, expression, "expected: false\n  actual:   true");
    }
}

// Waits until `pred` is true or `timeout_ms` elapses.
//
// A plain "sleep 1 ms then poll" loop is wrong on Windows: the default timer
// resolution is ~15.6 ms, so sleep_for(1ms) actually sleeps ~15.6 ms. A test
// that waits for 10,000 sequential completions then takes 156 seconds instead
// of a few hundred milliseconds, which looks exactly like a hang and hides
// real stalls. Spin with yield() first (completions arrive in microseconds),
// and only fall back to sleeping once the wait is clearly long.
template <class Pred>
inline bool wait_for(Pred pred, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    unsigned spins = 0;
    for (;;) {
        if (pred()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (spins < 2000) {
            ++spins;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

inline int run_all(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;

    // A dump path is derived from the environment so the runner controls where
    // artefacts land; the harness does not invent a location. Without the
    // variable, crashes still print to stderr but no dump is written.
#if defined(_MSC_VER) && defined(_DEBUG)
    // _dupenv_s rather than getenv: MSVC deprecates getenv and the build treats
    // warnings as errors. The buffer is freed immediately after copying.
    char* dir = nullptr;
    std::size_t dir_size = 0;
    if (_dupenv_s(&dir, &dir_size, "CRPC_TEST_MINIDUMP_DIR") == 0 && dir != nullptr) {
        char path[MAX_PATH] = {0};
        const char* name = argc > 0 && argv[0] != nullptr ? argv[0] : "crpc_test";
        const char* base = name;
        for (const char* p = name; *p != '\0'; ++p) {
            if (*p == '\\' || *p == '/') {
                base = p + 1;
            }
        }
        std::snprintf(path, sizeof(path), "%s\\%s.dmp", dir, base);
        set_minidump_path(path);
        std::free(dir);
    }
#endif

    int ran = 0;
    for (const case_entry& entry : registry()) {
        if (filter != nullptr &&
            std::string(entry.name).find(filter) == std::string::npos) {
            continue;
        }
        current_case() = entry.name;
        const int before = failure_count();
        try {
            entry.fn();
        } catch (const skipped_case&) {
            // skip() already reported the case; it is not a failure.
        } catch (const std::exception& ex) {
            report_failure(entry.name, 0, "uncaught exception", ex.what());
        } catch (...) {
            report_failure(entry.name, 0, "uncaught exception", "unknown exception type");
        }
        ++ran;
        // A skipped case counts as passed; skip() already printed the visible
        // "SKIP <case>: <reason>" line, so the per-case stream stays uniform.
        std::printf("%s %s\n", failure_count() == before ? "PASS" : "FAIL",
                    entry.name);
    }
    std::printf("\n%d case(s), %d check(s), %d failure(s), %d skipped\n", ran,
                check_count(), failure_count(), skip_count());
    return failure_count() == 0 ? 0 : 1;
}

} // namespace crpc_test

#define CRPC_TEST(name)                                                        \
    static void crpc_test_##name();                                            \
    static ::crpc_test::registrar crpc_test_registrar_##name(#name,            \
                                                             &crpc_test_##name); \
    static void crpc_test_##name()

#define CRPC_CHECK(expr) ::crpc_test::check_true(__FILE__, __LINE__, #expr, (expr))
#define CRPC_CHECK_FALSE(expr) ::crpc_test::check_false(__FILE__, __LINE__, #expr, (expr))
#define CRPC_CHECK_EQ(expected, actual)                                        \
    ::crpc_test::check_equal(__FILE__, __LINE__, #actual, (expected), (actual))

#define CRPC_TEST_MAIN()                                                       \
    int main(int argc, char** argv) {                                          \
        CRPC_TEST_EARLY();                                                     \
        return ::crpc_test::run_all(argc, argv);                               \
    }

#endif // CRPC_TEST_FRAMEWORK_HPP




