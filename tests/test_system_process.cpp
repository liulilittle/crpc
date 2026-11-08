// CRPC -- child-process execution and process introspection.
//
// These cases drive real OS processes: no mocks, no fakes. They are the
// integration gate for `crpc::system::process`.
#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/system/process.hpp>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using crpc::system::list_processes;
using crpc::system::process;
using crpc::system::process_options;
using crpc::system::process_result;
using crpc::system::query_process;

// Pids above every platform's maximum: neither Windows nor Linux can hand
// one out, so querying it must report not_found.
constexpr std::uint64_t kImpossiblePid = 0x7FFFFFF0ull;

#if defined(CRPC_OS_WINDOWS)
constexpr const char* kShell = "cmd.exe";
#else
constexpr const char* kShell = "/bin/sh";
#endif

process_options shell_options(const std::string& script) {
    process_options options;
    options.program = kShell;
#if defined(CRPC_OS_WINDOWS)
    options.arguments = {"/c", script};
#else
    // /bin/sh is a POSIX shell; the script text is identical on both sides.
    options.arguments = {"-c", script};
#endif
    return options;
}

std::int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

} // namespace

CRPC_TEST(run_echo_captures_stdout) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    options.program = "cmd.exe";
    options.arguments = {"/c", "echo hello"};
#else
    options.program = "/bin/echo";
    options.arguments = {"hello"};
#endif
    crpc::result<process_result> r = process::run(options);
    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.exited);
    CRPC_CHECK_FALSE(result.timed_out);
    CRPC_CHECK_EQ(0, result.exit_code);
    CRPC_CHECK(result.stdout_data.find("hello") != std::string::npos);
}

CRPC_TEST(run_captures_stderr_separately) {
    crpc::result<process_result> r = process::run(shell_options("echo err-line 1>&2"));
    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.exited);
    CRPC_CHECK_EQ(0, result.exit_code);
    CRPC_CHECK(result.stderr_data.find("err-line") != std::string::npos);
    CRPC_CHECK(result.stdout_data.find("err-line") == std::string::npos);
}

CRPC_TEST(run_reports_nonzero_exit_code) {
    crpc::result<process_result> r = process::run(shell_options("exit 7"));
    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.exited);
    CRPC_CHECK_FALSE(result.timed_out);
    CRPC_CHECK_EQ(7, result.exit_code);
}

CRPC_TEST(run_feeds_stdin_to_child) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    // `sort` reads all of stdin and writes it back sorted at EOF.
    options.program = "cmd.exe";
    options.arguments = {"/c", "sort"};
    options.stdin_data = "bbb\naaa\n";
#else
    options.program = "/bin/cat";
    options.stdin_data = "hello-stdin\n";
#endif
    crpc::result<process_result> r = process::run(options);
    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.exited);
    CRPC_CHECK_EQ(0, result.exit_code);
#if defined(CRPC_OS_WINDOWS)
    const std::size_t aaa = result.stdout_data.find("aaa");
    const std::size_t bbb = result.stdout_data.find("bbb");
    CRPC_CHECK(aaa != std::string::npos);
    CRPC_CHECK(bbb != std::string::npos);
    CRPC_CHECK(aaa < bbb);
#else
    CRPC_CHECK_EQ(std::string("hello-stdin\n"), result.stdout_data);
#endif
}

CRPC_TEST(run_timeout_kills_child) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    // ~7 seconds of real work so the timeout cannot race the startup.
    options.program = "ping.exe";
    options.arguments = {"-n", "8", "127.0.0.1"};
#else
    options.program = "/bin/sleep";
    options.arguments = {"5"};
#endif
    options.timeout = std::chrono::milliseconds(300);

    const auto start = std::chrono::steady_clock::now();
    crpc::result<process_result> r = process::run(options);
    const std::int64_t took = elapsed_ms(start);

    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.timed_out);
    CRPC_CHECK_FALSE(result.exited);
    CRPC_CHECK_EQ(-1, result.exit_code);
    // If the child was not killed, `run` would block until the ping/sleep
    // finished on its own; the elapsed bound is what proves the kill.
    CRPC_CHECK(took < 3000);
}

CRPC_TEST(run_missing_program_is_not_found) {
    process_options options;
    options.program = "crpc-no-such-program-9f2c1a7b";
    crpc::result<process_result> r = process::run(options);
    CRPC_CHECK_FALSE(r.ok());
    if (!r.ok()) {
        CRPC_CHECK(r.code() == crpc::rpc_code::not_found);
    }
}

CRPC_TEST(spawn_reports_bad_working_directory) {
#if defined(CRPC_OS_WINDOWS)
    crpc_test::skip("exec-status pipe is POSIX-only");
#else
    // The child's chdir() fails before execve(); the errno must travel back
    // through the exec-status pipe instead of being lost as a clean EOF.
    process_options options;
    options.program = "/bin/true";
    options.working_directory = "/nonexistent-crpc-dir-xyz";
    crpc::result<process_result> r = process::run(options);
    CRPC_CHECK_FALSE(r.ok());
    if (!r.ok()) {
        CRPC_CHECK(r.code() == crpc::rpc_code::not_found);
    }
#endif
}

CRPC_TEST(spawn_reports_missing_absolute_program) {
#if defined(CRPC_OS_WINDOWS)
    crpc_test::skip("exec-status pipe is POSIX-only");
#else
    // A program containing '/' bypasses the parent's PATH search, so the
    // failure happens in execve() and must be reported through the pipe.
    process_options options;
    options.program = "/nonexistent-crpc-prog-xyz";
    crpc::result<process_result> r = process::run(options);
    CRPC_CHECK_FALSE(r.ok());
    if (!r.ok()) {
        CRPC_CHECK(r.code() == crpc::rpc_code::not_found);
    }
#endif
}

CRPC_TEST(spawn_round_trips_stdin_and_stdout) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    options.program = "cmd.exe";
    options.arguments = {"/c", "sort"};
#else
    options.program = "/bin/cat";
#endif
    crpc::result<std::unique_ptr<process>> spawned = process::spawn(options);
    CRPC_REQUIRE(spawned.ok());
    std::unique_ptr<process> child = std::move(spawned.value());
    CRPC_REQUIRE(child != nullptr);
    CRPC_CHECK(child->pid() != 0);
    CRPC_CHECK(child->is_running());

    CRPC_CHECK(child->write_stdin("zzz\nyyy\n").ok());
    CRPC_CHECK(child->close_stdin().ok());

    std::string output;
    for (;;) {
        crpc::result<std::string> chunk = child->read_stdout(4096);
        CRPC_REQUIRE(chunk.ok());
        if (chunk.value().empty()) {
            break;
        }
        output += chunk.value();
    }
    crpc::result<int> waited = child->wait(std::chrono::milliseconds(5000));
    CRPC_REQUIRE(waited.ok());
    CRPC_CHECK_EQ(0, waited.value());
    CRPC_CHECK_FALSE(child->is_running());

    CRPC_CHECK(output.find("zzz") != std::string::npos);
    CRPC_CHECK(output.find("yyy") != std::string::npos);

    const std::uint64_t pid = child->pid();
    child.reset();
    CRPC_CHECK_FALSE(crpc::system::is_process_alive(pid));
}

CRPC_TEST(spawn_kill_reaps_the_child) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    options.program = "ping.exe";
    options.arguments = {"-n", "8", "127.0.0.1"};
#else
    options.program = "/bin/sleep";
    options.arguments = {"5"};
#endif
    crpc::result<std::unique_ptr<process>> spawned = process::spawn(options);
    CRPC_REQUIRE(spawned.ok());
    std::unique_ptr<process> child = std::move(spawned.value());
    CRPC_REQUIRE(child != nullptr);
    const std::uint64_t pid = child->pid();
    CRPC_CHECK(pid != 0);
    CRPC_CHECK(crpc::system::is_process_alive(pid));

    crpc::result<int> early = child->wait(std::chrono::milliseconds(50));
    CRPC_CHECK_FALSE(early.ok());
    if (!early.ok()) {
        CRPC_CHECK(early.code() == crpc::rpc_code::deadline_exceeded);
    }

    CRPC_CHECK(child->kill().ok());
    crpc::result<int> reaped = child->wait(std::chrono::milliseconds(5000));
    CRPC_REQUIRE(reaped.ok());
    CRPC_CHECK_FALSE(child->is_running());

    child.reset();
    CRPC_CHECK_FALSE(crpc::system::is_process_alive(pid));
}

CRPC_TEST(list_processes_contains_this_process) {
#if defined(CRPC_OS_WINDOWS)
    const std::uint64_t self = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const std::uint64_t self = static_cast<std::uint64_t>(::getpid());
#endif
    crpc::result<std::vector<crpc::system::process_info>> listing = list_processes();
    CRPC_REQUIRE(listing.ok());

    const crpc::system::process_info* found = nullptr;
    for (const crpc::system::process_info& entry : listing.value()) {
        if (entry.pid == self) {
            found = &entry;
            break;
        }
    }
    CRPC_REQUIRE(found != nullptr);
    CRPC_CHECK(!found->name.empty());
    CRPC_CHECK(found->resident_bytes > 0);
    CRPC_CHECK(found->start_time_unix_ms > 0);
    CRPC_CHECK(found->parent_pid != 0);

    crpc::result<crpc::system::process_info> queried = query_process(self);
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(self, queried.value().pid);
    CRPC_CHECK(!queried.value().name.empty());
    CRPC_CHECK(crpc::system::is_process_alive(self));
}

CRPC_TEST(run_truncates_oversized_output) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    // Emit ~6000 bytes: "for /l" plus a long literal on each line.
    options.program = "cmd.exe";
    options.arguments = {"/c",
                         "for /l %i in (1,1,200) do @echo 012345678901234567890123456789"};
#else
    options.program = "/bin/sh";
    options.arguments = {"-c",
                         "printf '012345678901234567890123456789%.0s' "
                         "$(seq 1 200)"};
#endif
    options.max_output_bytes = 512;
    crpc::result<process_result> r = process::run(options);
    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.exited);
    CRPC_CHECK_EQ(0, result.exit_code);
    CRPC_CHECK(result.stdout_truncated);
    CRPC_CHECK(result.stdout_data.size() <= 512);
}

CRPC_TEST(run_applies_environment_and_working_directory) {
    process_options options;
#if defined(CRPC_OS_WINDOWS)
    options.program = "cmd.exe";
    options.arguments = {"/c", "echo %CRPC_SYSTEM_TEST_VAR% & cd"};
    options.environment = {{"CRPC_SYSTEM_TEST_VAR", "crpc-env-7742"}};
#else
    options.program = "/bin/sh";
    options.arguments = {"-c", "echo $CRPC_SYSTEM_TEST_VAR; pwd"};
    options.environment = {{"CRPC_SYSTEM_TEST_VAR", "crpc-env-7742"}};
#endif
    options.working_directory = ".";
    crpc::result<process_result> r = process::run(options);
    CRPC_REQUIRE(r.ok());
    const process_result& result = r.value();
    CRPC_CHECK(result.exited);
    CRPC_CHECK_EQ(0, result.exit_code);
    CRPC_CHECK(result.stdout_data.find("crpc-env-7742") != std::string::npos);
}

CRPC_TEST(query_missing_process_is_not_found) {
    crpc::result<crpc::system::process_info> r = query_process(kImpossiblePid);
    CRPC_CHECK_FALSE(r.ok());
    if (!r.ok()) {
        CRPC_CHECK(r.code() == crpc::rpc_code::not_found);
    }
    CRPC_CHECK_FALSE(crpc::system::is_process_alive(kImpossiblePid));
}

CRPC_TEST_MAIN()