# CRPC examples

[简体中文](README.zh-CN.md) | **English**

Runnable consumers of the public API. They are built only when
`CRPC_BUILD_EXAMPLES=ON` and are registered in CTest, so a build proves they
compile and run.

## Quickstart

`quickstart.cpp` is one executable that is both the server and the client. It:

1. builds a `crpc::server` with one listener on an in-process endpoint,
2. registers one service (`quickstart`) and one typed method (`greet`),
3. starts the server,
4. connects a `crpc::rpc::client` and makes a typed call,
5. installs a push sink, subscribes to a topic, publishes, and handles the push,
6. tears everything down.

### Build

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
cmake -S . -B .opencode/tmp/p20b -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug `
      -DCRPC_BUILD_EXAMPLES=ON -DCRPC_BUILD_TESTS=OFF
cmake --build .opencode/tmp/p20b
```

```sh
export CRPC_RTTI_DIR=/path/to/RTTI
export CRPC_COROCC_DIR=/path/to/corocc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
      -DCRPC_BUILD_EXAMPLES=ON -DCRPC_BUILD_TESTS=OFF
cmake --build build
```

### Run

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart.exe
```

Expected output:

```text
call: Hello, CRPC!
push: subscribed to 'quickstart.events' on stream 1
push: first push (seq 1)
quickstart OK
```

The process exits non-zero on the first failure, so it is usable as a smoke
check.

## Microservice

`quickstart_microservice.cpp` builds a deterministic service mesh in one
process:

- two provider servers register in `crpc::cluster::registry`;
- `crpc::cluster::health_monitor` aggregates a healthy and a sick probe;
- the consumer queries the registry with `passing_only = true` and
  load-balances the discovered instances with
  `crpc::resilience::load_balancer` (`round_robin`);
- every call runs through `crpc::resilience::circuit_breaker` and a
  `retry_policy`;
- a call to a missing provider opens the breaker, proving the failure path.

Key API: `cluster::registry`, `cluster::service_instance`,
`cluster::health_monitor`, `resilience::load_balancer`,
`resilience::circuit_breaker`, `resilience::retry_sequence`, `server::builder`,
`rpc::client::start_call`.

Build and run (same build tree as the quickstart; the existing
`quickstart*.cpp` glob creates one `crpc_example_<file-stem>` target per file):

```powershell
cmake --build .opencode/tmp/p20b
.\.opencode\tmp\p20b\crpc_example_quickstart_microservice.exe
```

Expected output:

```text
microservice: registry has 2 passing instance(s) of 'greeter'
microservice: health aggregate is passing
microservice: round-robin picked 1 2 1 2
microservice: greeter-1 -> Hello, CRPC from greeter-1
microservice: greeter-2 -> Hello, CRPC from greeter-2
microservice: breaker opened after failures
microservice OK
```

## MVC

`quickstart_mvc.cpp` drives the RTTI-attributed MVC router and then serves the
same router through a real `crpc::http::http_server`:

- reflected DTOs use `CRPC_FROM_ROUTE` for a path parameter and
  `CRPC_FROM_QUERY` for query parameters;
- `CRPC_HTTP_GET`/`CRPC_HTTP_POST` declare the routes;
- `crpc::mvc::dispatch` handles GET/POST, a path parameter, a query parameter,
  a JSON body, a `result<T>` error (404) and a binding failure (400);
- the HTTP facade starts on an in-process endpoint and stops cleanly.

Key API: `mvc::default_router`, `mvc::dispatch`, `mvc::with_status`,
`crpc::mvc::status_body<T>`, `CRPC_ROUTE`, `CRPC_HTTP_GET`, `CRPC_HTTP_POST`,
`http::http_server::builder`.

Run:

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart_mvc.exe
```

Expected output:

```text
mvc: GET /users/42 -> 200 { "id": 42, "name": "alice" }
mvc: GET /users?page=2&limit=5 -> 200 { "id": 10, "name": "page" }
mvc: POST /users -> 201 { "id": 1, "name": "carol" }
mvc: GET /users/-1/maybe -> 404
mvc: GET /users/abc -> 400
mvc: GET /nope -> 404
mvc: http_server listening on inproc:mvc-example
mvc OK
```

## Game gateway

`quickstart_game.cpp` is a two-tier game backend in one process:

- the gateway server accepts the client and forwards `join`/`action` calls to
  the logic tier over a cross-node `rpc::client` call;
- the logic server owns the authoritative, stateful player sessions;
- after the logic tier answers, the gateway publishes a reliable event to
  `game.events`, and the client's push sink receives it.

Key API: `server::builder::add_method`, `rpc::client::start_call`,
`rpc::client::set_push_sink`, `rpc::client::subscribe_push`,
`server::publish`, `push::delivery_mode::reliable`,
`push::push_message_view`.

Run:

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart_game.exe
```

Expected output:

```text
game: subscribed to 'game.events' on stream 1
game: join -> session alice
game: push: player alice joined at (0,0)
game: action -> (3,4)
game: push: player moved to (3,4)
game OK
```

The gateway emits the event after the forward call returns. A logic-to-gateway
callback would deadlock on the synchronous in-process transport, because the
gateway shard thread is blocked in the forward call; over TCP (separate
processes/threads) that callback shape is possible.

## Shared memory (IPC)

`quickstart_ipc.cpp` is the only example that crosses a real process boundary
without a socket. Two independent processes exchange one message each way over a
named shared-memory channel:

- the parent creates the channel (`crpc::ipc::channel::create`), queues a request
  and then spawns the same binary with `peer <name>`;
- the child joins by name (`crpc::ipc::channel::open`) and answers;
- both sides remove the name at the end, and the parent fails if the answer is
  not exactly the one derived from the request, so a channel that delivered the
  wrong bytes cannot pass on "something arrived".

It spawns itself on purpose. Two `channel` objects in one process would pass even
if the mapping were quietly private, so `CreateProcessA` (Windows) / `fork`
(POSIX) is what makes the shared memory meaningful.

Run (the existing `quickstart*.cpp` glob already built it):

```powershell
.\.opencode\tmp\p20b\crpc_example_quickstart_ipc.exe
```

Expected output (the peer line can be printed before or after the parent's
`created` line: the child starts as soon as the mapping exists and the two
processes are not synchronised on stdout):

```text
peer: got "render frame 42", answering
parent: created channel "crpc_example_ipc" (4096 bytes each way)
parent: got "worker: handled 15 bytes"
ok: two processes exchanged a message over shared memory
```

Exit code 0 means both directions delivered the expected bytes. See
[ipc.md](../docs/ipc.md) for the channel, ring buffer and SPSC queue contracts.

## Why one process

The quickstart uses `crpc::inproc_backend`, which connects two endpoints inside
one process with no sockets and no extra thread. That is the same transport the
test suite uses, and it makes the whole API deterministic and portable: the
server and the client are objects in `main()`.

To run the two roles in separate processes, keep the server code and the client
code and replace the backend and the endpoint: `crpc::io::iocp_backend` on
Windows, `crpc::io::epoll_backend` on Linux, and
`crpc::endpoint::tcp("127.0.0.1", port)` on both sides. The wire format and the
call/push API are unchanged.

## Where to go next

- [docs/tutorial.md](../docs/tutorial.md) builds the same program step by step.
- [docs/api/README.md](../docs/api/README.md) indexes the public headers.
