// CRPC quickstart.
//
// One process, one in-process connection, a typed call and a push subscription.
// The in-process backend (`crpc::inproc_backend`) is the same transport the test
// suite uses: it connects two endpoints inside one process with no sockets and
// no extra thread, so a single executable can be both the server and the client
// and the whole public API is exercised deterministically.
//
// The same code works over TCP by replacing the backend and the endpoint
// (`crpc::io::iocp_backend` / `crpc::io::epoll_backend` and
// `crpc::endpoint::tcp(host, port)`); the server and client would then live in
// separate processes.
//
// Exit code 0 means the call returned the expected value and the push arrived.

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <crpc/io.hpp>
#include <crpc/push/wire.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

namespace {

constexpr crpc::u64 kService = 1;
constexpr crpc::u64 kGreet = 1;
constexpr const char* kEndpoint = "crpc-quickstart";
constexpr const char* kTopic = "quickstart.events";

// The handler object. `server::builder::add_method` binds a member function to
// a (service, method) pair. The request and response types are deduced from the
// member's signature; `crpc::rpc::call_codec` encodes and decodes them.
class greeter {
public:
    std::string greet(const std::string& name) { return "Hello, " + name + "!"; }
};

struct push_state {
    std::string body;
    crpc::u64 seq = 0;
    bool received = false;
};

// The push sink runs on the thread that delivered the frame, with `body` valid
// only for the call: copy it out, never store the span.
void on_push(void* ctx, const crpc::push::push_message_view& message) noexcept {
    auto* state = static_cast<push_state*>(ctx);
    state->body.assign(reinterpret_cast<const char*>(message.body.data()),
                       message.body.size());
    state->seq = message.seq;
    state->received = true;
}

bool wait_for_push(push_state& state, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!state.received) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

int fail(const char* what, const crpc::rpc_error& error) {
    std::cerr << "quickstart: " << what << " failed: " << error.message << "\n";
    return 1;
}

} // namespace

int main() {
    crpc::inproc_backend backend;

    // --- server: build, register one service method, start --------------------
    greeter service;
    crpc::server::builder server_builder;
    server_builder.add_listener(crpc::endpoint::inproc(kEndpoint), backend);
    server_builder.add_service(kService, "quickstart");
    server_builder.add_method<greeter, &greeter::greet>(
        kService, kGreet, "greet", service);

    std::unique_ptr<crpc::server> server = server_builder.build();
    if (!server) {
        std::cerr << "quickstart: build server failed\n";
        return 1;
    }
    if (const crpc::status started = server->start(); !started.ok()) {
        return fail("start server", started.error());
    }

    // --- client: connect, make a typed call -----------------------------------
    std::unique_ptr<crpc::connection> connection =
        backend.connect(crpc::endpoint::inproc(kEndpoint), crpc::connect_options{});
    if (!connection) {
        std::cerr << "quickstart: connect failed\n";
        return 1;
    }

    crpc::rpc::client_options client_options;
    client_options.slot_capacity = 16;
    crpc::rpc::client client(std::move(connection), client_options);

    crpc::buffer request;
    if (const crpc::status encoded =
            crpc::rpc::call_codec<std::string>::encode("CRPC", request);
        !encoded.ok()) {
        return fail("encode request", encoded.error());
    }

    auto started = client.start_call<std::string>(kService, kGreet, request.readable());
    if (!started.ok()) {
        return fail("start call", started.error());
    }
    auto call = std::move(started).value();
    auto result = call.get();
    if (!result.ok()) {
        std::cerr << "quickstart: call failed: " << result.error().message << "\n";
        return 1;
    }
    std::cout << "call: " << result.value() << "\n";

    // --- push: install a sink, subscribe, publish -----------------------------
    push_state state;
    client.set_push_sink(&on_push, &state);

    auto subscription = client.subscribe_push(
        kTopic, 1, crpc::push::delivery_mode::reliable, false);
    if (!subscription.ok()) {
        return fail("subscribe", subscription.error());
    }
    std::cout << "push: subscribed to '" << subscription.value().topic
              << "' on stream " << subscription.value().stream_id << "\n";

    const std::string payload = "first push";
    if (const crpc::publish_result published =
            server->publish(
                kTopic,
                crpc::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(payload.data()),
                    payload.size()),
                crpc::push::delivery_mode::reliable);
        !crpc::is_ok(published)) {
        std::cerr << "quickstart: publish refused: "
                  << crpc::to_string(published) << "\n";
        return 1;
    }

    if (!wait_for_push(state, 2000)) {
        std::cerr << "quickstart: no push arrived within 2s\n";
        return 1;
    }
    std::cout << "push: " << state.body << " (seq " << state.seq << ")\n";

    // --- teardown -------------------------------------------------------------
    client.close();
    server->stop();
    std::cout << "quickstart OK\n";
    return 0;
}
