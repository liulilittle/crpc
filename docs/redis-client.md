# CRPC Redis client

[简体中文](redis-client.zh-CN.md) | **English**

Version 1.0 / 2026-10-26

This document describes the asynchronous Redis client under
[`include/crpc/redis`](../include/crpc/redis). It is an API map and a set of
runnable recipes, not a replacement for the headers: every public name below is
declared in the linked header, and the header is authoritative.

The client is transport-agnostic. It talks to Redis over the same
`crpc::io::source` / `crpc::io::sink` contract as the rest of CRPC, so the same
code runs over the in-process backend in tests, IOCP on Windows, and epoll on
Linux.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Quick start |
| [2](#s2) | API map |
| [3](#s3) | Execution and asynchronous model |
| [4](#s4) | Error handling |
| [5](#s5) | Recipes |
| [6](#s6) | Build and test |
| [7](#s7) | Known limitations |
| [8](#s8) | See also |

## 1. Quick start <a id="s1"></a>

`crpc::redis::client` composes a connection pool and a typed command facade. It
is the shortest path to a working client.

```cpp
#include <chrono>
#include <iostream>
#include <string>

#include <crpc/io/iocp_backend.hpp> // Windows; epoll_backend.hpp on Linux
#include <crpc/redis/client.hpp>

int main() {
    crpc::io::iocp_backend backend;

    crpc::redis::client::options options;
    options.pool.connection.ep = crpc::endpoint::tcp("127.0.0.1", 6379);
    options.pool.min_size = 1;
    options.pool.max_size = 8;
    options.pool.acquire_timeout = std::chrono::seconds(2);
    options.pool.connection.operation_timeout = std::chrono::seconds(2);
    options.protocol = crpc::redis::resp_protocol::resp3;

    crpc::redis::client redis(options, backend);
    if (const crpc::status connected = redis.connect(); !connected.ok()) {
        std::cerr << "connect failed: " << connected.message() << "\n";
        return 1;
    }

    const crpc::result<crpc::redis::resp_value> set =
        redis.set("greeting", "hello");
    if (!set.ok()) {
        std::cerr << "SET failed: " << set.error().message << "\n";
        return 1;
    }

    const crpc::result<crpc::redis::resp_value> get = redis.get("greeting");
    if (get.ok() && get.value().is_bulk_string()) {
        std::cout << get.value().as_bulk_string() << "\n"; // hello
    }

    redis.close();
    return 0;
}
```

On Linux the backend is created with
`crpc::io::epoll_backend::create(crpc::io::epoll_options{})` and moved into a
`std::unique_ptr`; the rest of the snippet is unchanged. The backend is a
`crpc::backend_factory&`, so the Redis code does not depend on which one it is.

`connect()` establishes one pooled connection and verifies it with `PING`.
`close()` closes idle connections and rejects further acquires with
`rpc_code::shutting_down`.

For tests and examples that already have a server in the same process, pass a
`crpc::inproc_backend` and an `endpoint::inproc(name)` instead. The Redis layer
does not care which backend produced the byte stream.

## 2. API map <a id="s2"></a>

| Header | Key types | Responsibility |
|---|---|---|
| [`resp.hpp`](../include/crpc/redis/resp.hpp) | `resp_value`, `resp_parser`, `resp_encoder`, `resp_protocol` | RESP2/RESP3 value model, incremental parser and encoder. No I/O. |
| [`connection.hpp`](../include/crpc/redis/connection.hpp) | `connection`, `connection_pool`, `connection_config`, `pool_config` | Byte transport over `io::source`/`io::sink`: async read/write, bounded buffering, reconnect/backoff and pooling. |
| [`pipeline.hpp`](../include/crpc/redis/pipeline.hpp) | `command`, `command_executor`, `message_channel`, `command_channel`, `pipeline` | One command stream: single request/reply, batches, and the transport-facing channel that owns the RESP codec. |
| [`pubsub.hpp`](../include/crpc/redis/pubsub.hpp) | `pubsub`, `pubsub_message`, `pubsub_kind` | Pub/Sub subscribe/unsubscribe, message decoding, `PUBLISH` and `PUBSUB` introspection. |
| [`sentinel.hpp`](../include/crpc/redis/sentinel.hpp) | `sentinel`, `sentinel_options`, `sentinel_status`, `sentinel_connection_executor` | Master discovery, subjective/objective down detection, leader election and automatic failover. |
| [`stream.hpp`](../include/crpc/redis/stream.hpp) | `stream`, `stream_id`, `stream_entry`, `stream_read` | Redis Streams: `XADD`/`XREAD`/`XRANGE`, consumer groups, pending/claim operations. |
| [`transaction.hpp`](../include/crpc/redis/transaction.hpp) | `transaction`, `transaction_exec_result` | `WATCH`/`MULTI`/`EXEC`/`DISCARD` with the queue-error and watch-conflict semantics. |
| [`types.hpp`](../include/crpc/redis/types.hpp) | `value`, `string_type`, `list_type`, `hash_type`, `set_type`, `zset_type`, `stream_type`, `redis_stream_entry`, `bitmap_type`, `hyperloglog_type`, `geo_type`, `decode_*`, iterators | Typed value model, command builders and reply projections. |
| [`client.hpp`](../include/crpc/redis/client.hpp) | `client`, `client::options` | Pool + typed facade: `execute`, `execute_batch`, `ping`/`get`/`set`/hash/list/set/zset helpers. |
| [`cluster.hpp`](../include/crpc/redis/cluster.hpp) | `cluster`, `cluster_topology`, `cluster_options`, `cluster_slot_executor`, `cluster_pool_provider` | 16384-slot routing, `CLUSTER SHARDS`/`SLOTS` discovery, `MOVED`/`ASK`, CROSSSLOT rejection and cluster-safe pinning. |
| [`rtti.hpp`](../include/crpc/redis/rtti.hpp) | `encode_to_value`, `decode_from_value`, `hash_set_command`, `hash_mget_command`, `decode_hash_reply`, `value_from_resp` | Reflection-driven DTO <-> `redis::value` and DTO <-> Redis hash binding. Requires `CRPC_HAVE_RTTI`. |

`types.hpp` and `pipeline.hpp` both name the command type `command`; it is the
same alias (`resp_command`), so the two headers can be used together.

## 3. Execution and asynchronous model <a id="s3"></a>

- **No hidden threads.** `connection::write`, `read_exact` and `read_until`
  park the calling coroutine through `crpc::runtime::park()` when it runs on a
  CRPC shard. On a plain thread, or on a non-coroutine task on the shard
  thread, they fall back to a condition-variable wait bounded by
  `connection_config::operation_timeout`.
- **One command stream per owner.** `command_channel`, `pipeline`,
  `transaction`, `pubsub` and `cluster` are single-owner. Do not call them
  concurrently from several coroutines. Use one channel per pooled connection;
  `connection_pool` itself is the shared, thread-safe component.
- **Pooling.** `client::execute` acquires a pooled connection, wraps it in a
  `command_channel`, runs the command and releases it. A caller that needs
  several commands on one connection calls `client::acquire()` and
  `client::release()` explicitly.
- **Backpressure.** `connection_config::max_buffered_bytes` bounds the
  application read buffer. When it is full the read side stops consuming from
  the backend until the application drains it.
- **Reconnect.** A terminal error marks the connection closed. The next
  operation lazily reconnects with exponential backoff
  (`reconnect_backoff`/`reconnect_max_backoff`). `max_reconnects` caps
  consecutive failed reconnects; zero means unlimited. `close()` is terminal;
  only an explicit `reconnect()` revives the object.
- **Timeouts.** `operation_timeout` bounds a plain-thread read/write;
  `pool_config::acquire_timeout` bounds a plain-thread pool wait. On the
  coroutine path the operation parks instead of timing out locally.

## 4. Error handling <a id="s4"></a>

There are two different failure channels, and they must not be confused:

| Failure | Representation |
|---|---|
| Redis error reply (`-ERR ...`, `-WRONGTYPE ...`) | A successful `result` whose `resp_value` has `type() == resp_type::error`. |
| Transport, framing, local limit or shutdown | The `result`/`status` itself fails with an `rpc_code` (`connection_reset`, `eof`, `deadline_exceeded`, `protocol_error`, `resource_exhausted`, `shutting_down`, ...). |

`client::connect()` and `client::health_check()` are stricter: they treat a
Redis error reply as an `rpc_code::application` failure because the health probe
must distinguish "the server answered PING" from "the server answered an
error".

The facade never throws. Two lower-level exceptions are deliberate and
documented:

- `resp_encoder::encode_to_string` throws `std::invalid_argument` when a simple
  string or error payload contains CR or LF, because that would be a frame the
  parser rejects.
- `redis::value` accessors (`as_*`) throw `std::bad_variant_access` on the
  wrong alternative, matching `std::get`; use the `if_*` accessors for a
  non-throwing shape test.

A transaction has two protocol-level outcomes that are not transport errors:
`transaction_exec_result::aborted` is a successful `EXEC` that returned null
because a watched key changed, and individual entries in `replies` may be RESP
error values because Redis executes every queued command.

## 5. Recipes <a id="s5"></a>

### 5.1 Pipeline

```cpp
crpc::redis::connection_pool pool(pool_config, backend);
auto connection = pool.acquire();
if (!connection) {
    return;
}
crpc::redis::command_channel channel(*connection.value());

crpc::redis::pipeline pipe(channel);
pipe.add("SET", "counter", "0")
    .add("INCR", "counter")
    .add("GET", "counter");

const crpc::result<std::vector<crpc::redis::resp_value>> replies = pipe.run();
// replies.value().size() == 3; the queued commands are consumed by run().
```

A pipeline is not a transaction: other clients' commands may interleave, and a
failing command does not stop the following ones.

### 5.2 Transaction

```cpp
crpc::redis::transaction tx(channel);
if (tx.watch({"balance"}).ok() && tx.multi().ok()) {
    (void)tx.queue(crpc::redis::command{"DECRBY", "balance", "10"});
    const crpc::result<crpc::redis::transaction_exec_result> exec = tx.exec();
    if (exec.ok()) {
        if (exec.value().aborted) {
            // WATCH conflict: retry the whole read-modify-write.
        } else {
            // exec.value().replies is in queue order.
        }
    }
}
```

If a command is rejected while queuing, the transaction becomes dirty and
`exec()` sends `DISCARD` and returns the original queue error instead of
committing. The destructor performs no I/O; close an active transaction
explicitly.

### 5.3 Pub/Sub

A subscribed connection leaves request/response mode, so give it its own
connection:

```cpp
crpc::redis::connection_pool sub_pool(sub_config, backend);
auto sub_connection = sub_pool.acquire();
crpc::redis::command_channel sub_channel(*sub_connection.value());
crpc::redis::pubsub subscription(sub_channel);

if (subscription.subscribe({"news"}).ok()) {
    const crpc::result<crpc::redis::pubsub_message> message =
        subscription.next();
    if (message.ok()) {
        // message.value().channel, .payload, .pattern, .kind
    }
}

// Publish on a different executor (normally a different pooled connection).
const crpc::result<std::int64_t> receivers =
    crpc::redis::pubsub::publish(channel, "news", "hello");
```

`pubsub::matches` implements the same glob matcher Redis uses for `PSUBSCRIBE`.
Acknowledgement pushes that arrive while subscribing are queued and drained by
`next()`.

### 5.4 Streams

```cpp
crpc::redis::stream streams(channel);

const crpc::result<std::string> id = streams.add(
    "events", {{"type", "login"}, {"user", "alice"}});
if (!id.ok()) {
    return;
}

const crpc::result<std::vector<crpc::redis::stream_entry>> entries =
    streams.range("events", "-", "+", 10);
if (entries.ok()) {
    for (const crpc::redis::stream_entry& entry : entries.value()) {
        // entry.id, entry.parsed_id, entry.fields
    }
}

(void)streams.group_create("events", "workers", "$", true);
crpc::redis::stream_read_options read_options;
read_options.count = 16;
read_options.block = std::chrono::milliseconds(1000);
const auto batch = streams.read_group(
    "workers", "worker-1", {"events"}, {">"}, read_options);
```

`stream_id::parse` rejects the range markers `-`, `+`, `$` and `*`; those are
range syntax, not entry IDs. `stream_entry::id` keeps the exact server spelling,
and `parsed_id` is the numeric form.

### 5.5 Cluster

```cpp
crpc::redis::cluster_options cluster_options;
cluster_options.seeds.push_back(
    crpc::redis::cluster_address{"127.0.0.1", 7000});
cluster_options.max_redirects = 5;

crpc::redis::cluster cluster(cluster_options, backend);

const crpc::result<crpc::redis::resp_value> reply =
    cluster.execute(crpc::redis::command{"GET", "user:{42}:name"});
if (!reply.ok()) {
    return;
}

// A transaction must stay on one node. pin() validates the key set once.
const auto pinned = cluster.pin({"user:{42}:name", "user:{42}:age"});
if (pinned.ok()) {
    crpc::redis::transaction tx(*pinned.value());
    // MULTI/EXEC and every queued command now target the same slot owner.
}
```

A single command whose keys span slots, and a pipeline whose commands span
slots, are rejected locally with `failed_precondition` and a `CROSSSLOT`
message before any I/O. `MOVED` updates the slot table; `ASK` sends `ASKING`
first and does not update it. The client always routes to the master; there is
no read-from-replica policy.

### 5.6 Sentinel

```cpp
crpc::redis::sentinel_options sentinel_options;
sentinel_options.master_name = "mymaster";
sentinel_options.sentinels.push_back(
    crpc::redis::sentinel_address{"127.0.0.1", 26379});
sentinel_options.quorum = 2;

crpc::redis::sentinel_connection_executor executor(backend);
crpc::redis::sentinel monitor(sentinel_options, executor);
monitor.set_event_handler([](const crpc::redis::sentinel_event& event) {
    // sdown / odown / promoted / master_switch / ...
});

if (monitor.start()) {
    const crpc::redis::sentinel_address master = monitor.master();
    // Point a pool or cluster seed provider at `master`.
    monitor.stop();
}
```

The monitor never owns a socket: it drives a `sentinel_command_executor`.
`poll_once_at()` is the deterministic entry point used by tests; `start()` runs
the same pass as a coroutine. Do not call `poll_once*()` concurrently with the
background loop. The executor is borrowed and must outlive the monitor's
coroutine.

### 5.7 RTTI binding

```cpp
#include <crpc/redis/rtti.hpp>

struct profile {
    std::string id;
    int age = 0;
};
RTTI_DEFINE_TYPE(profile, id, age)

profile value{"u1", 30};
const crpc::result<crpc::redis::command> hset =
    crpc::redis::hash_set_command("user:u1", value);
if (hset.ok()) {
    (void)redis.execute(hset.value());
}

const crpc::result<crpc::redis::resp_value> reply =
    redis.execute(crpc::redis::hash_get_command("user:u1"));
if (reply.ok()) {
    crpc::redis::value decoded;
    if (crpc::redis::value_from_resp(reply.value(), decoded).ok()) {
        profile out;
        const crpc::status status =
            crpc::redis::decode_hash_reply(decoded, out);
        // status.ok() and out.id == "u1"
    }
}
```

A reflected struct becomes a Redis hash: one hash field per reflected field,
base-class fields first. `bind_options` controls schema drift:
`on_unknown = reject` (default) or `ignore`, and `on_missing = keep` (default)
or `reject`. A container-valued field cannot be stored in one hash field;
`hash_set_command` returns `rpc_code::encode_error` for it. `std::string_view`
is not decodable (it would dangle) and is rejected at compile time.

### 5.8 RESP and typed helpers directly

```cpp
crpc::redis::resp_parser parser;
parser.feed(bytes); // bytes is span<const std::byte>

crpc::redis::resp_value value;
for (;;) {
    switch (parser.next(value)) {
    case crpc::redis::resp_parser::result::value_ready:
        // consume value
        break;
    case crpc::redis::resp_parser::result::need_more:
        return;
    case crpc::redis::resp_parser::result::error:
        return;
    }
}
```

`types.hpp` adds the RESP3-shaped `redis::value` plus builders such as
`crpc::redis::string_type::set("k", crpc::redis::value{"v"})` and decoders such
as `decode_int`, `decode_string_array`, `decode_scored_members` and
`decode_stream_entries`.

## 6. Build and test <a id="s6"></a>

The Redis client is part of the library; no extra CMake option is needed.

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
.\scripts\build.ps1 -Config Debug -Arch x64
.\scripts\test.ps1  -Config Debug -Arch x64 -Filter redis
```

`-Filter redis` runs the Redis suites: `resp`, `connection`, `pipeline`,
`transaction`, `pubsub`, `stream`, `types`, `cluster`, `sentinel`, `client`,
`rtti` and `header_conflict`. `rtti` is empty when the RTTI dependency is
absent, matching the other RTTI-dependent tests.

To build the benchmark as well:

```powershell
cmake -S . -B .opencode/tmp/out-redis -G Ninja `
      -DCMAKE_BUILD_TYPE=Debug -DCRPC_BUILD_TESTS=ON -DCRPC_BUILD_BENCH=ON `
      -DCRPC_RTTI_DIR=F:\RTTI -DCRPC_COROCC_DIR=F:\corocc
cmake --build .opencode/tmp/out-redis --parallel 2
ctest --test-dir .opencode/tmp/out-redis -R redis --output-on-failure
.\.opencode\tmp\out-redis\crpc_bench_redis.exe
```

The benchmark argument is a substring filter:
`crpc_bench_redis.exe redis/pipeline` runs only the pipeline cases. Numbers in
[performance.md §2.5](performance.md#s2-5) are Debug, single-process and
transport-isolated; they are regression baselines, not network capacity.

## 7. Known limitations <a id="s7"></a>

- **`stream_entry` naming.** `stream.hpp` provides `crpc::redis::stream_entry`
  (an entry with the parsed ID in `parsed_id`); `types.hpp` provides
  `crpc::redis::redis_stream_entry` (the raw reply projection returned by
  `decode_stream_entries`). The two records are distinct types, both headers can
  be included in one translation unit, and
  `tests/test_redis_header_conflict.cpp` pins that separation.
- **Shared-build export.** The shared build covers the Redis header exports.
  Under `CRPC_BUILD_SHARED=ON`, `ctest -R 'redis|abi_docs|shared_link'` passes;
  `scripts/export_gate.py` checks that every decorated Redis header really
  exports its symbols. The static archive remains the default build; a shared
  build is a supported configuration with the same API.
- **Benchmark scope.** `crpc_bench_redis` isolates the pool with a null
  transport and the pipeline with a wire-shaped executor. It does not measure a
  real Redis server or a network round trip. The recorded numbers are Debug.
- **Cluster.** No read-from-replica policy and no background topology refresh;
  refresh is lazy (unknown slot, `CLUSTERDOWN`, `TRYAGAIN`) or explicit
  (`refresh()`). Cross-slot commands and batches are rejected locally.
- **Sentinel.** The monitor is a state machine over a caller-supplied executor.
  It does not open sockets itself, and `poll_once*()` must not race the
  background loop.
- **Pub/Sub.** A subscribed connection accepts only the subscription command
  family; use a dedicated connection and a separate executor for `PUBLISH`.
- **RTTI.** Requires `CRPC_HAVE_RTTI`. DTO hash fields must encode to scalars.
  `std::string_view` cannot be decoded. The defaults reject unknown reply
  fields and keep missing DTO fields.
- **RESP limits.** The parser defaults to 512 MiB per bulk value / container
  element count and a nesting depth of 128; both are constructor parameters.
- **Typed facade coverage.** `client` exposes convenience methods for strings,
  hashes, lists, sets and sorted sets. Everything else goes through
  `execute(command)` or the `types.hpp` builders; no API is guessed from a
  command name.

## 8. See also <a id="s8"></a>

- [API reference](api/README.md) — the header index, including the Redis table.
- [Wire protocol §24](protocol.md#s24) — frame compression used by the same
  connection layer.
- [Performance report §2.5](performance.md#s2-5) — recorded compression, RPC and
  Redis benchmark numbers.
- [Distributed testing and benchmarks](distributed-testing.md) — how the
  multi-node and benchmark scenarios are run.
