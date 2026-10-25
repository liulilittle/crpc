# CRPC tutorial

[简体中文](tutorial.zh-CN.md) | **English**

This tutorial builds one small program from nothing: a message type, a service
method, a server, a client call and a push subscription. It uses the same
in-process transport as [`examples/quickstart.cpp`](../examples/quickstart.cpp),
so it is deterministic and needs no network. Every API name below is the real
public API; the header is linked at each step.

If you have not built CRPC yet, see [building.md](building.md) §2 for the
dependency checkouts and §5 for the one-click scripts.

## Step 0 — the shape of the program

One executable plays both roles:

```text
+------------------+        inproc connection        +------------------+
|  server          |  <-------------------------->   |  client          |
|  greeter::hello  |   REQUEST/RESPONSE (typed call)  |  start_call      |
|  server.publish  |   PUSH/PUSH_ACK (subscription)   |  push sink       |
+------------------+                                   +------------------+
```

The `crpc::inproc_backend` object owns the channel; both the server listener and
the client connection are created from it.

## Step 1 — write a message type

A request or response type is any type with a `crpc::rpc::call_codec`
specialization. The codec is two functions: `encode` writes the value into a
[`buffer`](../include/crpc/buffer.hpp), `decode` reads it back from a
[`span`](../include/crpc/span.hpp). The built-in specializations cover the
arithmetic types, `std::string` and `std::vector<std::byte>`
([`rpc/call.hpp`](../include/crpc/rpc/call.hpp)); a user type adds its own.

```cpp
#include <crpc/buffer.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/span.hpp>

#include <string>

struct hello_request {
    std::string name;
};

struct hello_response {
    std::string message;
};

namespace crpc::rpc {

template <>
struct call_codec<hello_request, void> {
    static status encode(const hello_request& value, buffer& out) {
        return call_codec<std::string>::encode(value.name, out);
    }
    static status decode(span<const std::byte> in, hello_request& value) {
        return call_codec<std::string>::decode(in, value.name);
    }
};

template <>
struct call_codec<hello_response, void> {
    static status encode(const hello_response& value, buffer& out) {
        return call_codec<std::string>::encode(value.message, out);
    }
    static status decode(span<const std::byte> in, hello_response& value) {
        return call_codec<std::string>::decode(in, value.message);
    }
};

} // namespace crpc::rpc
```

The `<void>` second argument is the primary template's default; a specialization
must name it. The body delegates to the `std::string` codec, so the wire format
is the raw UTF-8 bytes with no length prefix — fine for a tutorial, and the
place to add a version tag or a length prefix for a real message.

## Step 2 — write the handler

The handler is an ordinary member function. Its signature determines the request
and response types: `Resp (Ctl::*)(const Req&)` is the form
[`register_typed_method`](../include/crpc/rpc/dispatcher.hpp) accepts. Add a
`crpc::rpc::call_context&` second parameter if the handler needs the deadline or
trace metadata.

```cpp
class greeter {
public:
    hello_response hello(const hello_request& request) {
        hello_response response;
        response.message = "Hello, " + request.name + "!";
        return response;
    }
};
```

## Step 3 — build and start the server

A [`server::builder`](../include/crpc/server.hpp) collects the listeners, the
services and the methods; `build()` freezes them into a `server`, and `start()`
binds the listeners and starts the shards.

```cpp
#include <crpc/io.hpp>
#include <crpc/server.hpp>

crpc::inproc_backend backend;

greeter service;

crpc::server::builder server_builder;
server_builder.add_listener(crpc::endpoint::inproc("tutorial"), backend);
server_builder.add_service(1, "tutorial");
server_builder.add_method<greeter, &greeter::hello>(
    1, 1, "hello", service);

std::unique_ptr<crpc::server> server = server_builder.build();
if (!server) { /* ... */ }
if (const crpc::status started = server->start(); !started.ok()) {
    // started.message() names the failure
}
```

Registration is startup-only: the registry is frozen in `start()`, so every
service and method must be added before it. `add_service` and `add_method` take
plain `u64` ids; the [`service_id` / `method_id`](../include/crpc/id.hpp)
newtypes exist for call sites that want the compiler to reject a swapped pair.

## Step 4 — connect a client and make a typed call

The low-level [`rpc::client`](../include/crpc/rpc/client.hpp) owns one
connection. `start_call<Resp>` takes the already-encoded request body and
returns a move-only handle; `get()` waits for the response and decodes it.

```cpp
#include <crpc/rpc/client.hpp>

std::unique_ptr<crpc::connection> connection =
    backend.connect(crpc::endpoint::inproc("tutorial"), crpc::connect_options{});
if (!connection) { /* ... */ }

crpc::rpc::client_options client_options;
client_options.slot_capacity = 16;
crpc::rpc::client client(std::move(connection), client_options);

hello_request request;
request.name = "CRPC";

crpc::buffer body;
if (const crpc::status encoded = crpc::rpc::call_codec<hello_request>::encode(
        request, body);
    !encoded.ok()) {
    // encoded.error() names the failure
}

auto started = client.start_call<hello_response>(1, 1, body.readable());
if (!started.ok()) {
    // started.error() names the failure (pool exhausted, closed, ...)
} else {
    auto call = std::move(started).value();
    auto result = call.get();
    if (result.ok()) {
        std::cout << result.value().message << "\n";
    } else {
        // result.error().code / .message
    }
}
```

The high-level [`client` facade](../include/crpc/client_facade.hpp) wraps the
same call with a connection pool, load balancing, a breaker and retries:

```cpp
auto stub = facade.service<hello_response, hello_request>(1, 1);
auto started = stub.call(request);
```

Use the facade in a real service; the low-level client is what the push API is
on, so the tutorial keeps one client object and uses it for both.

## Step 5 — a push subscription

Push is server-to-client. The server publishes to a topic with
`server::publish`; the client subscribes with `rpc::client::subscribe_push` and
receives messages through a sink.

```cpp
#include <crpc/push/wire.hpp>

struct push_state {
    std::string body;
    crpc::u64 seq = 0;
};

// Runs on the thread that delivered the frame; copy the body out.
void on_push(void* ctx, const crpc::push::push_message_view& message) noexcept {
    auto* state = static_cast<push_state*>(ctx);
    state->body.assign(reinterpret_cast<const char*>(message.body.data()),
                       message.body.size());
    state->seq = message.seq;
}

push_state state;
client.set_push_sink(&on_push, &state);

auto subscription = client.subscribe_push(
    "tutorial.events", 1, crpc::push::delivery_mode::reliable, false);
if (!subscription.ok()) {
    // subscription.error() names the failure
}

const std::string payload = "first push";
if (const crpc::publish_result published = server->publish(
        "tutorial.events",
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
        crpc::push::delivery_mode::reliable);
    !crpc::is_ok(published)) {
    // crpc::to_string(published) names the refusal
}
```

A reliable message is acknowledged automatically after the sink returns; a
reconnect with the same subscriber id replays the unacknowledged tail. The sink
must not block and must not destroy the client.

## Step 6 — teardown

```cpp
client.close();   // cancels in-flight calls and closes the connection
server->stop();   // stops accepting, drains in-flight calls, closes shards
```

`server::stop()` is idempotent and bounded by `drain_timeout`. Destroying the
`server` without `stop()` is not supported; the builder's borrowed
`backend_factory` must outlive every `stop()` → `start()` cycle.

## Running it

With the program in `main()` and `CRPC_BUILD_EXAMPLES=ON`:

```powershell
cmake -S . -B build -DCRPC_BUILD_EXAMPLES=ON
cmake --build build
.\build\crpc_example_quickstart.exe
```

## Notes

- **Errors are values.** Nothing above throws across a call; `status`,
  `result<T>` and `rpc_error` carry the failure. See
  [`error.hpp`](../include/crpc/error.hpp).
- **Two processes.** Replace `crpc::inproc_backend` with
  `crpc::io::iocp_backend` (Windows) or `crpc::io::epoll_backend` (Linux) and the
  endpoint with `crpc::endpoint::tcp("127.0.0.1", port)`; the call and push API
  are unchanged. See [examples/README.md](../examples/README.md).
- **The full API map** is [docs/api/README.md](api/README.md).
