# memcached client

[简体中文](memcached-client.zh-CN.md) | **English**

An asynchronous memcached client built on the CRPC byte transport. It speaks the
text protocol, the meta protocol and (for framing) the binary protocol, and it
routes across servers with a libketama-compatible ring.

## Quick start

```cpp
#include <crpc/memcached/client.hpp>
#include <crpc/runtime.hpp>

crpc::inproc_backend backend;           // or a tcp backend
crpc::memcached::client::options opts;
opts.pool.connection.ep = crpc::endpoint::tcp("127.0.0.1", 11211);
opts.pool.max_size = 4;

crpc::memcached::client cache(opts, backend);

// A store, then a fetch.
const auto stored = cache.store(crpc::memcached::store_kind::set, "user:1", "hello");
if (!stored) { /* stored.error() carries the code and the server's message */ }

const auto values = cache.get({"user:1", "user:2"});
if (values) {
    for (const auto& entry : values.value()) {
        // entry.key, entry.flags, entry.data, entry.cas (gets/gats only)
    }
}
```

Inside a coroutine every call parks the calling coroutine instead of blocking the
thread; on a plain thread it waits on a condition variable bounded by the pool's
configured timeouts. There are no hidden worker threads.

## Several servers

`client` owns one endpoint; `router` owns several and places keys with a
libketama ring, so a deployment that mixes this client with another memcached
client still routes each key to the same server.

```cpp
#include <crpc/memcached/router.hpp>

std::vector<crpc::memcached::router::server> servers;
for (const char* name : {"10.0.0.1:11211", "10.0.0.2:11211"}) {
    crpc::memcached::connection_transport_config cfg;
    cfg.pool.connection.ep = crpc::endpoint::tcp(name);
    servers.push_back({name,
                       std::make_shared<crpc::memcached::connection_transport>(cfg, backend),
                       1});
}
crpc::memcached::router cache(std::move(servers));

// Routed by key.
cache.store(crpc::memcached::store_kind::set, "user:1", "hello");

// Fanned out: one request per server that owns a key, results merged.
const auto values = cache.get({"user:1", "user:2", "user:3"});
```

The server `name` is the string the ring hashes. Every client in the deployment
must use the same spelling (`host:port`), or placement differs between clients.

## Outcomes that are not errors

memcached answers routine situations with a word that is not a failure. The
client reports those as values, because treating them as errors would force every
caller to special-case them:

| Command | Reply | Result |
| --- | --- | --- |
| `add` on an existing key | `NOT_STORED` | `store_status::not_stored` |
| `cas` with a stale token | `EXISTS` | `store_status::exists` |
| `append`/`prepend`/`cas` on a missing key | `NOT_FOUND` | `store_status::not_found` |
| `delete` on a missing key | `NOT_FOUND` | `store_status::not_found` |
| `touch` succeeded | `TOUCHED` | `store_status::touched` |
| `incr`/`decr` on a missing key | `NOT_FOUND` | **error** `not_found` |

A counter reply is a number, so "the counter does not exist" cannot be expressed
as a number and is reported as an error instead.

Genuine failures map like this, always keeping the server's own message:

| Reply | `rpc_code` |
| --- | --- |
| `CLIENT_ERROR <message>` | `invalid_argument` (the request is malformed) |
| `SERVER_ERROR <message>` | `internal` (the server failed, not the caller) |
| `ERROR` | `protocol_error` |

## Meta protocol

The meta commands are exposed with the flag list passed through verbatim, so a
flag this library does not model is still usable:

```cpp
const auto reply = cache.meta_get("user:1", {"v", "f", "t", "c"});
if (reply && reply.value().type == crpc::memcached::text_reply::kind::meta_hit) {
    reply.value().value;      // the payload
    reply.value().meta_cas;   // the c<cas> token, if asked for
}
```

`meta_set`, `meta_delete` and `meta_noop` (`mn`) return the parsed outcome. For
anything else, `client::execute()` writes a command verbatim and returns the
decoded reply.

## Limits and fail-closed behaviour

* A key longer than 250 bytes (including any configured prefix) is rejected
  before it is written, because the server would reject it anyway.
* A value larger than `options::max_value_size` (1 MiB by default) is rejected
  before it is written. Raise it only when the server's `-I` limit was raised.
* A reply line above `max_line_size` (64 KiB) or a value block above
  `max_value_size` fails the call with `resource_exhausted` instead of growing
  memory.
* A reply that is not the kind the command asked for (`get` answered by
  `STORED`) is `protocol_error`: the streams are desynchronised.
* A reply with trailing bytes on the wire is `protocol_error`, and the connection
  is closed and dropped rather than returned to the pool. A desynchronised
  stream is never handed to the next caller.

## What is not here

* **Retries.** Whether a retry is safe depends on the command and on whether the
  link failed before or after the request was written, so the policy belongs to
  the caller.
* **SASL authentication** and the binary protocol's extras layouts. The binary
  header and status mapping are available; the command encoders are not.
* **UDP transport.**
* **Cross-server multi-get atomicity.** A fan-out `get` fails as a whole if any
  server fails, but it is not a snapshot: another client may change a key between
  two servers' replies.
