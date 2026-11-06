// CRPC -- child-process execution and process introspection.
//
// Two layers live in this header:
//
//   * `process` starts one child with optional stdin/stdout/stderr redirection.
//     `run` is the blocking form (it waits for the child, captures its output
//     and enforces the timeout); `spawn` is the long-lived form where the
//     caller drives the pipes and waits explicitly.
//
//   * `list_processes` / `query_process` / `is_process_alive` inspect the
//     processes of the local machine.
//
// Guarantees of `run`:
//
//   * stdout and stderr are drained concurrently, so a child that fills one
//     pipe while the caller waits on the other cannot deadlock.
//   * stdin data is written by a helper thread for the same reason: a large
//     write cannot block the output drains.
//   * a timeout always force-kills the child (and, where the platform allows,
//     its process tree) and reaps it; no zombie or orphan is left behind.
//   * captured output is bounded by `max_output_bytes` per stream. Bytes past
//     the limit are discarded (never buffered) and the corresponding
//     `*_truncated` flag is set.
//
// Process introspection is best-effort by nature: a process may exit while it
// is being inspected, and some fields need privileges the caller does not
// have. Such fields are left at their documented default value instead of
// being guessed. `command_line` is currently only populated on Linux.
//
// Error codes:
//   * not_found            the executable / process does not exist
//   * permission_denied    the OS refused access
//   * invalid_argument     empty program, invalid UTF-8 input
//   * failed_precondition  an operation was applied to a closed stream
//   * deadline_exceeded    `wait` timed out
//   * resource_exhausted   out of memory / handles
//   * broken_pipe          the child closed its read end of stdin
//   * internal             other OS failure (native code in the message)
#ifndef CRPC_SYSTEM_PROCESS_HPP
#define CRPC_SYSTEM_PROCESS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace system {

struct CRPC_API process_options {
    // Executable path, or a bare name resolved through PATH. On Windows both
    // a path and a bare name work; the standard CreateProcess search rules
    // apply. On POSIX the name is resolved through the PATH of the child's
    // effective environment (options.environment overrides included).
    std::string program;
    // argv[1..]. argv[0] is always `program`.
    std::vector<std::string> arguments;
    // Child working directory. Empty means "inherit the parent's".
    std::string working_directory;
    // Extra / overriding environment variables. Names and values are UTF-8.
    std::map<std::string, std::string> environment;
    // When true the child inherits the parent's environment and `environment`
    // is applied on top. When false only `environment` is visible.
    bool inherit_environment = true;
    // Data written to the child's stdin. `run` always closes stdin after the
    // write (an empty string therefore delivers EOF immediately).
    std::string stdin_data;
    bool capture_stdout = true;
    bool capture_stderr = true;
    // Wall-clock budget for `run`. 0 means "no timeout". `spawn` ignores it
    // (the caller owns the process lifetime there).
    std::chrono::milliseconds timeout{0};
    // Cap per captured stream. 0 means "unlimited" (caller accepts the
    // memory cost).
    std::size_t max_output_bytes = 16u * 1024u * 1024u;
};

struct CRPC_API process_result {
    // Measured only when `exited` is true; -1 otherwise.
    int exit_code = -1;
    // The child was force-killed because `timeout` elapsed.
    bool timed_out = false;
    // The child terminated on its own (exit or signal) within the deadline.
    bool exited = false;
    std::string stdout_data;
    std::string stderr_data;
    // Set when a stream produced more than `max_output_bytes` and the excess
    // was discarded.
    bool stdout_truncated = false;
    bool stderr_truncated = false;
};

class CRPC_API process {
public:
    // Runs the child to completion and captures its output.
    static result<process_result> run(const process_options& options);

    // Starts the child and returns a handle to it. `options.timeout` is not
    // applied here.
    static result<std::unique_ptr<process>> spawn(const process_options& options);

    ~process();
    process(const process&) = delete;
    process& operator=(const process&) = delete;
    process(process&&) noexcept;
    process& operator=(process&&) noexcept;

    // Reads up to `max_bytes` from a captured stream. Blocks until at least
    // one byte is available or the stream reaches EOF. An empty string means
    // EOF (or that the stream was not captured / is already closed).
    result<std::string> read_stdout(std::size_t max_bytes);
    result<std::string> read_stderr(std::size_t max_bytes);

    // Appends to the child's stdin. Blocks while the pipe is full. Fails with
    // `broken_pipe` when the child closed its end.
    status write_stdin(std::string_view data);
    status close_stdin();

    // Waits for the child. A timeout of 0 means "wait forever". Returns the
    // raw exit status; a signalled child reports 128 + signal on POSIX.
    // `deadline_exceeded` when the deadline passes first.
    result<int> wait(std::chrono::milliseconds timeout);

    // Asks the child to terminate (CTRL_BREAK on Windows where possible,
    // SIGTERM on POSIX). Does not wait; use `wait` afterwards.
    status terminate();
    // Force-terminates the child (and its process tree on Windows).
    status kill();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint64_t pid() const noexcept;

private:
    struct impl;
    explicit process(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

struct CRPC_API process_info {
    std::uint64_t pid = 0;
    std::uint64_t parent_pid = 0;
    // Base file name (UTF-8), never empty for a listed process.
    std::string name;
    // Full image path when it can be read; empty otherwise.
    std::string executable_path;
    // Full command line when the platform exposes it; empty otherwise
    // (currently always empty on Windows).
    std::string command_line;
    // Resident set size in bytes, 0 when not available.
    std::uint64_t resident_bytes = 0;
    // Average CPU utilisation over the process lifetime, in percent, where
    // 100% means every logical core is busy. 0 when not available.
    double cpu_percent = 0.0;
    // Process creation time as Unix epoch milliseconds; 0 when not available.
    std::uint64_t start_time_unix_ms = 0;
};

// All processes visible to the caller. Processes that vanish or deny access
// mid-enumeration are still listed with whatever the snapshot exposed.
[[nodiscard]] CRPC_API result<std::vector<process_info>> list_processes();

// Information for one process. `not_found` when it does not exist.
[[nodiscard]] CRPC_API result<process_info> query_process(std::uint64_t pid);

// True when a process with `pid` exists. Access-denied processes count as
// alive. PID 0 is not a user process and reports false on Windows.
[[nodiscard]] CRPC_API bool is_process_alive(std::uint64_t pid);

} // namespace system
} // namespace crpc

#endif // CRPC_SYSTEM_PROCESS_HPP
