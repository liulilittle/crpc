// CRPC IPC quickstart.
//
// Two independent processes exchanging a message over shared memory, with no socket
// and no broker. The parent creates a named channel, the child joins it by name, and
// each sends one message. This is the shape a renderer/worker pair or a supervisor and
// its child uses when the payload is too small to justify a socket and too hot to
// serialise twice.
//
// Why this example spawns itself
// ------------------------------
// Shared memory only proves anything across a real process boundary. Two channel
// objects in one process would pass even if the mapping were quietly private, so the
// example runs the same binary as both ends: `crpc_example_quickstart_ipc` creates,
// and `crpc_example_quickstart_ipc peer <name>` joins.
//
// Exit code 0 means both directions delivered the expected bytes.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <crpc/ipc/channel.hpp>

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

namespace {

constexpr const char* kPeerFlag = "peer";
constexpr const char* kChannelName = "crpc_example_ipc";
constexpr std::size_t kCapacity = 4096;

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    if (!text.empty()) {
        std::memcpy(out.data(), text.data(), text.size());
    }
    return out;
}

// Bounded, so a peer that died fails the example instead of hanging it.
bool receive_within(crpc::ipc::channel& ch, std::string& out,
                    std::chrono::milliseconds budget) {
    std::byte buf[128];
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t got = ch.try_receive(crpc::span<std::byte>(buf, sizeof(buf)));
        if (got != 0) {
            out.assign(reinterpret_cast<const char*>(buf), got);
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

// The child: join, read the request, answer it.
int run_peer(const char* name) {
    auto joined = crpc::ipc::channel::join(name);
    if (!joined) {
        std::fprintf(stderr, "peer: join failed: %s\n", joined.error().message.c_str());
        return 1;
    }
    std::string request;
    if (!receive_within(joined.value(), request, std::chrono::seconds(10))) {
        std::fprintf(stderr, "peer: no request arrived\n");
        return 2;
    }
    std::printf("peer: got \"%s\", answering\n", request.c_str());
    const std::string answer = "worker: handled " + std::to_string(request.size()) +
                               " bytes";
    const std::vector<std::byte> payload = bytes_of(answer);
    if (!joined.value().try_send(
            crpc::span<const std::byte>(payload.data(), payload.size()))) {
        std::fprintf(stderr, "peer: answer could not be queued\n");
        return 3;
    }
    return 0;
}

#if defined(_WIN32)
bool spawn_peer(const char* name, int& exit_code) {
    char exe[MAX_PATH] = {0};
    if (::GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0) {
        return false;
    }
    std::string cmdline =
        std::string("\"") + exe + "\" " + kPeerFlag + " " + name;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &si, &pi)) {
        return false;
    }
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    if (waited == WAIT_OBJECT_0) {
        (void)::GetExitCodeProcess(pi.hProcess, &code);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    exit_code = static_cast<int>(code);
    return waited == WAIT_OBJECT_0;
}
#else
bool spawn_peer(const char* name, int& exit_code) {
    // Flush before forking. `stdout` is line-buffered on a terminal and fully
    // buffered when redirected, and a buffered write made before the fork is
    // duplicated into the child: the child then printed the parent's line a second
    // time before running its own work. This is not visible on a terminal, which is
    // why it showed up only when the example ran under a harness that captures output.
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        const int code = run_peer(name);
        std::fflush(stdout);
        std::fflush(stderr);
        ::_exit(code);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return false;
    }
    exit_code = WEXITSTATUS(status);
    return true;
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], kPeerFlag) == 0) {
        return run_peer(argv[2]);
    }

    // A leftover from an interrupted run would make `create` fail with
    // failed_precondition, which is the correct answer for a collision but a
    // confusing one for a rerun of this example.
    (void)crpc::fs::shared_memory::remove(kChannelName);

    auto parent = crpc::ipc::channel::create(kChannelName, kCapacity);
    if (!parent) {
        std::fprintf(stderr, "create failed: %s\n", parent.error().message.c_str());
        return 1;
    }
    std::printf("parent: created channel \"%s\" (%zu bytes each way)\n", kChannelName,
                kCapacity);

    // Queue the request before spawning: the child joins and looks for it at once,
    // so nothing has to be ordered beyond the mapping itself.
    const std::vector<std::byte> request = bytes_of("render frame 42");
    if (!parent.value().try_send(
            crpc::span<const std::byte>(request.data(), request.size()))) {
        std::fprintf(stderr, "parent: request could not be queued\n");
        return 1;
    }

    int peer_exit = -1;
    if (!spawn_peer(kChannelName, peer_exit)) {
        std::fprintf(stderr, "parent: peer did not start or did not finish\n");
        return 1;
    }
    if (peer_exit != 0) {
        std::fprintf(stderr, "parent: peer exited %d\n", peer_exit);
        return 1;
    }

    std::string answer;
    if (!receive_within(parent.value(), answer, std::chrono::seconds(10))) {
        std::fprintf(stderr, "parent: no answer arrived\n");
        return 1;
    }
    std::printf("parent: got \"%s\"\n", answer.c_str());

    (void)crpc::fs::shared_memory::remove(kChannelName);

    // The expected answer is derived from the request, so a channel that delivered the
    // wrong bytes or a truncated message fails here rather than passing on "something
    // arrived".
    const std::string expected = "worker: handled 15 bytes";
    if (answer != expected) {
        std::fprintf(stderr, "parent: expected \"%s\"\n", expected.c_str());
        return 1;
    }
    std::printf("ok: two processes exchanged a message over shared memory\n");
    return 0;
}
