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


## Cross-process deployment check

Every quickstart above uses `crpc::inproc_backend`, which connects two endpoints
inside one process with no sockets. That is deliberate -- it makes the examples
deterministic -- but it also means they never prove the network paths work.

`deploy_check.cpp` is the exception, and it is the one to run before trusting a
build in production. It spawns *itself* as a child process, so the server and
the client are two operating-system processes talking over loopback TCP:

```powershell
.\.opencode\tmp\p20b\crpc_example_deploy_check.exe
```

Expected output:

```text
server: READY 19562
call: /health -> 200 ok
call: /echo/deploy -> 200 deploy
server: STATS accepted=2 requests=2 responses=2 bad=0
```

It checks three things the in-process examples cannot:

1. a real `iocp_backend` (Windows) or `epoll_backend` (Linux) listener accepts a
   connection from a different process;
2. the reply is parsed and compared **exactly** -- status line plus body, byte
   for byte. A substring check was the first version of this file, and it passed
   while the server answered `not-ok` for `/health` because `"ok"` is inside
   `"not-ok"`. A gate that accepts the wrong body only proves a port was open;
3. the server's own counters agree (`accepted == requests == responses`,
   `bad == 0`) and the process shuts down cleanly.

The two modes are usable on their own, which is what makes a third-party client
meaningful:

```powershell
# terminal 1
.\crpc_example_deploy_check.exe serve 19555
# terminal 2 -- an unrelated HTTP client, proving the wire is ordinary HTTP/1.1
curl -sS -D - http://127.0.0.1:19555/health
curl -sS http://127.0.0.1:19555/echo/curl-real
```

A `curl` that gets `HTTP/1.1 200` and the expected body is evidence no
in-process test can give: the bytes on the wire are correct HTTP, not something
only this library can parse.

## Registry service daemon

`registryd.cpp` is the G19 step2 executable: a real `cluster::registry_service`
on loopback TCP, with the ACL, the epoch store and the single-instance lease.
It is a thin main -- configuration, signals, start, stop; all behaviour lives in
the library:

```powershell
.\crpc_example_registryd.exe --state-dir <dir> --token <bearer-token> [--port <n>] [--http-port <n>]
```

Two channels serve the same service. `--port` is the native CRPC binary channel;
`--http-port` adds the Consul-compatible HTTP facade, so an existing Consul
client works without a shim. Both reach the same handlers through
`registry_service::dispatch`, so the ACL check, the anchor water-mark and the log
append cannot diverge between them.

The KV data plane is backed by a single-node `raft_node` over a file log
(`<state-dir>/raft.log`). A KV write is committed and applied through the same
replicated state machine a multi-node cluster uses, and `prepare` replays the
committed log on start, so a value written before a restart is still there
afterwards. Without the raft node the write would land in memory and be lost.

The token is a **bearer token**, not an authentication credential: a holder of a
valid token acts as the principal it was issued to, and the RPC layer carries no
peer identity and no TLS. See `include/crpc/cluster/registry_acl.hpp` for the
full trust boundary.


A single round trip proves the wire works. It says nothing about whether the
process survives ten thousand of them. `soak <port> <count>` runs that:
one client backend, `count` connect/send/receive/close cycles, and a handle
count before and after.

```powershell
.\crpc_example_deploy_check.exe serve 19555 600   # in one terminal
.\crpc_example_deploy_check.exe soak  19555 2000  # in another
```

```text
soak: handle counter verified (96 -> 110 for 16 deliberate leaks)
soak: requests=2000 failures=0 connect_failures=0 worst_connect_ms=0 handles 93 -> 93 per-request-ms p50=0.42 p99=0.73 max=0.75 [connect-avg=0.21 transfer-avg=0.22] [connect-ms p50=0.20 p90=0.29 p99=0.44 max=0.46]
```

Two details matter more than the numbers:

* **the counter proves itself first.** Before measuring, the soak leaks sixteen
  sockets on purpose and requires the handle count to see them. A counter stuck
  at a constant value would make every leak check pass by construction -- the
  worst kind of green -- so it aborts rather than reporting a meaningless pass.
* **it reports a distribution, not an average.** `p50/p99/max` for the whole
  request and for the connect alone, because a tail problem is invisible in a
  mean. The split is reported so the two are never confused. The numbers above
  are from a host where the connect path is correct; an earlier revision of this
  file showed `connect p90 = 14.71 ms` because a redundant `getsockopt(SO_ERROR)`
  after `select` cost a full Windows timer tick on roughly a third of loopback
  connects. That call was removed, and the same split now reports ~0.3 ms.
* **a failed connect says why.** `connect_failures` and `worst_connect_ms` are
  reported separately from `failures`, because a refused connect and a slow one
  mean different things: a refusal is immediate, while one that burns the whole
  `connect_timeout` means the accept side stopped consuming. The first five
  failures also print their elapsed milliseconds.

The gate is `handles` growth, not an absolute count: absolute handle counts
differ per platform and per OS build, but a leak always grows with the work.

## Why one process

The quickstart uses `crpc::inproc_backend`, which connects two endpoints inside
one process with no sockets and no extra thread. That is the same transport the
test suite uses, and it makes the whole API deterministic and portable: the
server and the client are objects in `main()`.

To run the two roles in separate processes, keep the server code and the client
code and replace the backend and the endpoint: `crpc::iocp_backend` on
Windows, `crpc::io::epoll_backend` on Linux, and
`crpc::endpoint::tcp("127.0.0.1", port)` on both sides. The wire format and the
call/push API are unchanged.

## Where to go next

- [docs/tutorial.md](../docs/tutorial.md) builds the same program step by step.
- [docs/api/README.md](../docs/api/README.md) indexes the public headers.
