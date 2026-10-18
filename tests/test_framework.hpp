// CRPC -- zero-dependency test harness.
//
// Design rules:
//   * a failing assertion prints the expression, the expected value and the
//     actual value; a bare "assert failed" is not acceptable because it forces
//     a re-run under a debugger to learn anything,
//   * tests are registered at static-init time and run from one binary, so the
//     whole suite is one process and one CTest entry per binary,
//   * the exit code is non-zero when any assertion failed,
//   * no exceptions are used for control flow: a test body that throws is
//     reported as a failure with the exception message.
#ifndef CRPC_TEST_FRAMEWORK_HPP
#define CRPC_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace crpc_test {

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

struct registrar {
    registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const char* expression,
                           const std::string& detail) {
    ++failure_count();
    std::fprintf(stderr, "FAIL %s:%d\n  case: %s\n  expr: %s\n  %s\n",
                 file, line, current_case(), expression, detail.c_str());
    std::fflush(stderr);
}

template <typename A, typename B>
void check_equal(const char* file, int line, const char* expression,
                 const A& expected, const B& actual) {
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

inline int run_all(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const case_entry& entry : registry()) {
        if (filter != nullptr && std::string(entry.name).find(filter) == std::string::npos) {
            continue;
        }
        current_case() = entry.name;
        const int before = failure_count();
        try {
            entry.fn();
        } catch (const std::exception& ex) {
            report_failure(entry.name, 0, "uncaught exception", ex.what());
        } catch (...) {
            report_failure(entry.name, 0, "uncaught exception", "unknown exception type");
        }
        ++ran;
        std::printf("%s %s\n", failure_count() == before ? "PASS" : "FAIL", entry.name);
    }
    std::printf("\n%d case(s), %d check(s), %d failure(s)\n", ran, check_count(),
                failure_count());
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
    int main(int argc, char** argv) { return ::crpc_test::run_all(argc, argv); }

#endif // CRPC_TEST_FRAMEWORK_HPP
