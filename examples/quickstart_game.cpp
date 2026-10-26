// CRPC game gateway example.
//
// A two-tier game backend in one process:
//
//   * the gateway server accepts the client connection and forwards every call
//     to the logic tier (a cross-node RPC call),
//   * the logic server owns the authoritative, stateful player sessions,
//   * the gateway turns the logic tier's answer into an event and pushes it to
//     the client (gateway -> client push).
//
// The in-process backend keeps the two tiers deterministic. Exit code 0 means
// the gateway forwarded the calls, the logic tier kept its session state, and
// the client received both pushes.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/io.hpp>
#include <crpc/push/wire.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/server.hpp>

namespace {

constexpr crpc::u64 kGatewayService = 20;
constexpr crpc::u64 kGatewayJoin = 1;
constexpr crpc::u64 kGatewayAction = 2;

constexpr crpc::u64 kLogicService = 21;
constexpr crpc::u64 kLogicLogin = 1;
constexpr crpc::u64 kLogicMove = 2;

constexpr const char* kTopic = "game.events";

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "game: FAILED: " << what << "\n";
        ++failures;
    }
}

// --- Push sink ---------------------------------------------------------------

struct push_state {
    std::vector<std::string> bodies;
};

void on_push(void* ctx, const crpc::push::push_message_view& message) noexcept {
    auto* state = static_cast<push_state*>(ctx);
    state->bodies.emplace_back(
        reinterpret_cast<const char*>(message.body.data()),
        message.body.size());
}

bool wait_for_pushes(push_state& state, std::size_t expected, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (state.bodies.size() < expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

// --- Cross-tier call helpers -------------------------------------------------

// Calls a string-in/string-out method on a client and returns 0 on success.
int call_string(crpc::rpc::client& client, crpc::u64 service, crpc::u64 method,
                const std::string& request, std::string& out) {
    crpc::buffer encoded;
    if (const crpc::status status =
            crpc::rpc::call_codec<std::string>::encode(request, encoded);
        !status.ok()) {
        return -1;
    }
    auto started =
        client.start_call<std::string>(service, method, encoded.readable());
    if (!started.ok()) {
        return -1;
    }
    auto result = std::move(started).value().get();
    if (!result.ok()) {
        return -1;
    }
    out = result.value();
    return 0;
}

// --- Gateway tier ------------------------------------------------------------

// Forwards client calls to the logic tier and emits the events the client
// subscribed to. The client-facing connection lives on this server, so the
// push reaches the client.
class gateway_service {
public:
    void set_logic_client(crpc::rpc::client* client) noexcept {
        logic_client_ = client;
    }
    void set_server(crpc::server* server) noexcept { gateway_server_ = server; }

    // Gateway -> logic cross-node call, then a gateway -> client push.
    std::string join(const std::string& name) {
        std::string session;
        if (logic_client_ == nullptr ||
            call_string(*logic_client_, kLogicService, kLogicLogin, name,
                        session) != 0) {
            return "";
        }
        publish("player " + session + " joined at (0,0)");
        return session;
    }

    // Gateway -> logic cross-node call, then a gateway -> client push.
    std::string action(const std::string& args) {
        std::string position;
        if (logic_client_ == nullptr ||
            call_string(*logic_client_, kLogicService, kLogicMove, args,
                        position) != 0) {
            return "";
        }
        publish("player moved to " + position);
        return position;
    }

private:
    void publish(const std::string& message) {
        if (gateway_server_ == nullptr) {
            return;
        }
        const crpc::span<const std::byte> body(
            reinterpret_cast<const std::byte*>(message.data()), message.size());
        (void)gateway_server_->publish(kTopic, body,
                                       crpc::push::delivery_mode::reliable);
    }

    crpc::rpc::client* logic_client_ = nullptr;
    crpc::server* gateway_server_ = nullptr;
};

// --- Logic tier --------------------------------------------------------------

struct session {
    std::int64_t x = 0;
    std::int64_t y = 0;
};

// Owns the authoritative player sessions. Every mutation goes through here, so
// the gateway stays stateless.
class logic_service {
public:
    std::string login(const std::string& name) {
        sessions_[name] = session{0, 0};
        return name;
    }

    std::string move(const std::string& args) {
        // args is "name dx dy".
        std::string name;
        std::int64_t dx = 0;
        std::int64_t dy = 0;
        const std::size_t first = args.find(' ');
        const std::size_t second =
            first == std::string::npos ? std::string::npos
                                       : args.find(' ', first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            name = args.substr(0, first);
            dx = std::stoll(args.substr(first + 1, second - first - 1));
            dy = std::stoll(args.substr(second + 1));
        }
        session& state = sessions_[name];
        state.x += dx;
        state.y += dy;
        return "(" + std::to_string(state.x) + "," + std::to_string(state.y) +
               ")";
    }

private:
    std::unordered_map<std::string, session> sessions_;
};

} // namespace

int main() {
    crpc::inproc_backend backend;

    gateway_service gateway;
    logic_service logic;

    // --- logic server ---------------------------------------------------------
    crpc::server::builder logic_builder;
    logic_builder.add_listener(crpc::endpoint::inproc("game-logic"), backend);
    logic_builder.add_service(kLogicService, "game-logic");
    logic_builder.add_method<logic_service, &logic_service::login>(
        kLogicService, kLogicLogin, "login", logic);
    logic_builder.add_method<logic_service, &logic_service::move>(
        kLogicService, kLogicMove, "move", logic);

    // --- gateway server -------------------------------------------------------
    crpc::server::builder gateway_builder;
    gateway_builder.add_listener(crpc::endpoint::inproc("game-gateway"), backend);
    gateway_builder.add_service(kGatewayService, "game-gateway");
    gateway_builder.add_method<gateway_service, &gateway_service::join>(
        kGatewayService, kGatewayJoin, "join", gateway);
    gateway_builder.add_method<gateway_service, &gateway_service::action>(
        kGatewayService, kGatewayAction, "action", gateway);

    std::unique_ptr<crpc::server> logic_server = logic_builder.build();
    std::unique_ptr<crpc::server> gateway_server = gateway_builder.build();
    if (!logic_server || !gateway_server) {
        std::cerr << "game: build server failed\n";
        return 1;
    }
    if (const crpc::status started = logic_server->start(); !started.ok()) {
        std::cerr << "game: start logic failed: " << started.message() << "\n";
        return 1;
    }
    if (const crpc::status started = gateway_server->start(); !started.ok()) {
        std::cerr << "game: start gateway failed: " << started.message() << "\n";
        return 1;
    }

    // --- gateway -> logic client ---------------------------------------------
    std::unique_ptr<crpc::connection> logic_client_connection =
        backend.connect(crpc::endpoint::inproc("game-logic"),
                        crpc::connect_options{});
    if (!logic_client_connection) {
        std::cerr << "game: connect gateway->logic failed\n";
        return 1;
    }
    crpc::rpc::client logic_client(std::move(logic_client_connection),
                                   crpc::rpc::client_options{});
    gateway.set_logic_client(&logic_client);
    gateway.set_server(gateway_server.get());

    // --- client ---------------------------------------------------------------
    std::unique_ptr<crpc::connection> client_connection =
        backend.connect(crpc::endpoint::inproc("game-gateway"),
                        crpc::connect_options{});
    if (!client_connection) {
        std::cerr << "game: connect client failed\n";
        return 1;
    }
    crpc::rpc::client client(std::move(client_connection),
                             crpc::rpc::client_options{});

    push_state state;
    client.set_push_sink(&on_push, &state);

    const auto subscription =
        client.subscribe_push(kTopic, 1, crpc::push::delivery_mode::reliable,
                              false);
    if (!subscription.ok()) {
        std::cerr << "game: subscribe failed: " << subscription.error().message
                  << "\n";
        return 1;
    }
    std::cout << "game: subscribed to '" << subscription.value().topic
              << "' on stream " << subscription.value().stream_id << "\n";

    // Join: the gateway forwards to the logic tier and pushes the join event.
    std::string session;
    check(call_string(client, kGatewayService, kGatewayJoin, "alice", session) ==
              0,
          "join call");
    check(session == "alice", "join returns the session id");
    std::cout << "game: join -> session " << session << "\n";

    if (!wait_for_pushes(state, 1, 2000)) {
        std::cerr << "game: no join push within 2s\n";
        ++failures;
    } else {
        check(state.bodies[0].find("alice joined") != std::string::npos,
              "join push body");
        std::cout << "game: push: " << state.bodies[0] << "\n";
    }

    // Move: the gateway forwards to the logic tier, which mutates the session,
    // and pushes the move event.
    std::string position;
    check(call_string(client, kGatewayService, kGatewayAction, "alice 3 4",
                      position) == 0,
          "action call");
    check(position == "(3,4)", "action returns the new position");
    std::cout << "game: action -> " << position << "\n";

    if (!wait_for_pushes(state, 2, 2000)) {
        std::cerr << "game: no move push within 2s\n";
        ++failures;
    } else {
        check(state.bodies[1].find("(3,4)") != std::string::npos,
              "move push body");
        std::cout << "game: push: " << state.bodies[1] << "\n";
    }

    // --- teardown -------------------------------------------------------------
    client.close();
    logic_client.close();
    gateway_server->stop();
    logic_server->stop();

    if (failures != 0) {
        std::cerr << "game: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "game OK\n";
    return 0;
}
