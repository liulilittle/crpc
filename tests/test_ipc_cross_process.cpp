// CRPC -- the channel across two real processes.
//
// The other channel tests run two `channel` objects inside one process. That covers
// the layout, the readiness handshake and the direction each end binds, but it cannot
// prove the thing the class exists for: that a second *process* sees the same bytes.
// An in-process test would also pass if the mapping were quietly process-private.
//
// So this case spawns the test binary again as a peer. The parent creates the channel,
// the child joins it by name, and they exchange a value in each direction. The child
// is told what to do through its command line, so the same binary is both ends -- no
// second executable to build and no path to guess.
//
// What this still does not cover: the poller a real reader runs. The child spins on
// `try_receive`, which is correct and simple; a production reader would block in its
// event loop and be woken by `poll_ready()`. The wake path is exercised in-process by
// `test_ipc_async`.
#include <crpc/ipc/channel.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::ipc;
using namespace std::chrono_literals;

namespace {

// The child is invoked as `<binary> --ipc-peer <name>`. A peer that cannot start the
// exchange reports through its exit code, which the parent distinguishes from a crash.
constexpr const char* kPeerFlag = "--ipc-peer";

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    if (!text.empty()) {
        std::memcpy(out.data(), text.data(), text.size());
    }
    return out;
}


// Spins for a bounded time rather than forever: a peer that died would otherwise hang
// the parent until ctest's timeout, which reads as a deadlock in an unrelated place.
bool receive_within(channel& ch, std::string& out, std::chrono::milliseconds budget) {
    std::byte buf[64];
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t got = ch.try_receive(span<std::byte>(buf, sizeof(buf)));
        if (got != 0) {
            out.assign(reinterpret_cast<const char*>(buf), got);
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// Runs when this binary is the child. Returns an exit code the parent checks.
int run_peer(std::string_view name) {
    auto joined = channel::join(name);
    if (!joined.ok()) {
        std::fprintf(stderr, "peer: join failed: %s\n", joined.error().message.c_str());
        return 11;
    }
    // The child reads the parent's greeting first. If the parent is slow to start, the
    // join above already succeeded (the name exists), so this wait is for the bytes.
    std::string greeting;
    if (!receive_within(joined.value(), greeting, 10000ms)) {
        std::fprintf(stderr, "peer: no greeting\n");
        return 12;
    }
    if (greeting != "ping-from-parent") {
        std::fprintf(stderr, "peer: unexpected greeting '%s'\n", greeting.c_str());
        return 13;
    }
    const std::string reply = "pong-from-child:" + std::to_string(greeting.size());
    const std::vector<std::byte> payload = bytes_of(reply);
    // The parent has drained its ring by now, so a refusal here means the ring really
    // was full -- report it instead of spinning forever.
    if (!joined.value().try_send(span<const std::byte>(payload.data(), payload.size()))) {
        std::fprintf(stderr, "peer: send refused\n");
        return 14;
    }
    return 0;
}

} // namespace

// A dedicated entry point, not a CRPC_TEST: the child must run this and nothing else,
// and `main` has to see the flag before the test registry is consulted.
#if defined(_WIN32)
int run_child_process(std::string_view name, int& exit_code) {
    char exe[MAX_PATH] = {0};
    if (::GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) {
        return 0;
    }
    std::string cmdline = std::string("\"") + exe + "\" " + kPeerFlag + " " +
                          std::string(name);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &si, &pi)) {
        return 0;
    }
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000);
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 1;
        (void)::GetExitCodeProcess(pi.hProcess, &code);
        exit_code = static_cast<int>(code);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return waited == WAIT_OBJECT_0 ? 1 : 0;
}
#else
int run_child_process(std::string_view name, int& exit_code) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        // Child: do the peer work and leave immediately. `std::exit` would run static
        // destructors that may touch state the parent still owns; `_exit` after
        // flushing is the correct way out of a forked child that did its own work.
        const int code = run_peer(name);
        std::fflush(stdout);
        std::fflush(stderr);
        ::_exit(code);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        return 0;
    }
    if (!WIFEXITED(status)) {
        return 0;
    }
    exit_code = WEXITSTATUS(status);
    return 1;
}
#endif

CRPC_TEST(channel_carries_bytes_between_two_processes) {
    std::string name = "crpc_xproc_" + std::to_string(
        reinterpret_cast<std::uintptr_t>(&name));
    (void)fs::shared_memory::remove(name);

    auto parent = channel::create(name, 4096);
    CRPC_CHECK(parent.ok());
    if (!parent.ok()) {
        return;
    }

    // Send the greeting *before* spawning: the child joins and immediately looks for
    // it, and a greeting queued first removes one ordering question from the test.
    const std::vector<std::byte> hello = bytes_of("ping-from-parent");
    CRPC_CHECK(parent.value().try_send(span<const std::byte>(hello.data(), hello.size())));

    int child_exit = -1;
    const int started = run_child_process(name, child_exit);
    CRPC_CHECK_EQ(1, started);
    // The child's own checks are invisible to this harness, so its exit code is the
    // report: 11 join failed, 12 no greeting, 13 wrong greeting, 14 send refused.
    CRPC_CHECK_EQ(0, child_exit);

    // And the direction that matters: bytes written by another process are readable
    // here. A process-private mapping would leave this ring empty.
    std::string reply;
    const bool got = receive_within(parent.value(), reply, 10000ms);
    CRPC_CHECK(got);
    if (got) {
        CRPC_CHECK_EQ(std::string("pong-from-child:16"), reply);
    }

    (void)fs::shared_memory::remove(name);
}

int main(int argc, char** argv) {
    CRPC_TEST_EARLY();
    if (argc >= 3 && std::strcmp(argv[1], kPeerFlag) == 0) {
        return run_peer(argv[2]);
    }
    return ::crpc_test::run_all(argc, argv);
}
