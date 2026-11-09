# CRPC API reference

[简体中文](README.zh-CN.md) | **English**

A map of the public headers under [`include/crpc`](../../include/crpc) and the key
types each one defines. It is an index, not a tutorial: for the shape of a
program see [the tutorial](../tutorial.md), and for the wire/ABI rules see
[protocol.md](../protocol.md) and [abi.md](../abi.md).

The headers are grouped by subsystem. Every entry links to the header, which is
the authoritative declaration.

## Core primitives

| Header | Key types | What it is |
|---|---|---|
| [`config.hpp`](../../include/crpc/config.hpp) | `u8`…`u64`, `CRPC_API`, `CRPC_OS_*` | Platform/compiler detection and the explicit-width integer aliases. |
| [`span.hpp`](../../include/crpc/span.hpp) | `span<T>`, `as_bytes` | Minimal, constexpr, bounds-clamping span; the only borrowed-range type. |
| [`buffer.hpp`](../../include/crpc/buffer.hpp) | `buffer` | Growable byte buffer with `readable()`/`writable()` spans; no reallocation surprise on the hot path. |
| [`endian.hpp`](../../include/crpc/endian.hpp) | `load_le*`, `store_le*` | Explicit little-endian load/store, the wire's only byte order. |
| [`wire.hpp`](../../include/crpc/wire.hpp) | `wire_reader`, `wire_writer` | Bounds-checked byte reader/writer: varints, zigzag, fixed fields. |
| [`frame.hpp`](../../include/crpc/frame.hpp) | `frame`, `frame_header`, `frame_type`, `frame_decoder`, `encode_frame`, `decode_frame` | The multiplexed binary frame header and its codec. |
| [`frame_compression.hpp`](../../include/crpc/frame_compression.hpp) | `encode_frame_compressed`, `frame_compression_info` | LZ4 block frame compression (the only supported algorithm). |
| [`delivery_mode.hpp`](../../include/crpc/delivery_mode.hpp) | `delivery_mode` | `reliable` / `unreliable`, shared by RPC calls and push. |
| [`id.hpp`](../../include/crpc/id.hpp) | `service_id`, `method_id` | Strong wrappers over the wire `u64` ids, so the two cannot be swapped by mistake. |
| [`error.hpp`](../../include/crpc/error.hpp) | `rpc_code`, `rpc_error`, `result<T>`, `status`, `metadata_list` | The value-or-error model: every failure is a value, never a throw. |
| [`runtime.hpp`](../../include/crpc/runtime.hpp) | `runtime`, `runtime_options` | One shard: one thread, one coroutine runtime, one timer wheel, a bounded inbox and handler pool. |

## RPC — calls and the server facade

| Header | Key types | What it is |
|---|---|---|
| [`rpc/call.hpp`](../../include/crpc/rpc/call.hpp) | `call_state`, `call_context`, `call_slot`, `call_slot_pool`, `call<T>`, `call_codec<T>` | The call state machine, the per-call context and the move-only handle. |
| [`rpc/dispatcher.hpp`](../../include/crpc/rpc/dispatcher.hpp) | `service_registry`, `method_slot`, `method_handler`, `dispatcher`, `register_typed_method` | Server-side method table and dispatch; `call_codec` needs no RTTI for a typed method. |
| [`rpc/client.hpp`](../../include/crpc/rpc/client.hpp) | `rpc::client`, `client_options`, `start_call<Resp>`, `set_push_sink`, `subscribe_push` | One connection: a reader, a writer queue, typed calls and client-side push. |
| [`server.hpp`](../../include/crpc/server.hpp) | `server`, `server::builder`, `server::options`, `publish`, `publish_result`, `rpc::default_shard_count`, `builder::shards_auto`, `server::shard_count` | The server facade: listeners, shards, handler pool, graceful stop and topic publish. `shards_auto()` (opt-in) and `shard_count()` observe the automatic shard policy; the default shard count stays 1. |
| [`client_facade.hpp`](../../include/crpc/client_facade.hpp) | `client`, `client::builder`, `client::stub<Resp,Req>`, `client::call<Resp>` | The high-level client: pooled connections, load balancing, breaker, retry and typed stubs. |
| [`push/hub.hpp`](../../include/crpc/push/hub.hpp) | `push::hub`, `push::subscribe_options`, `push::push_message` | Server push hub: topics, bounded per-subscription queues and publish routing. |
| [`push/wire.hpp`](../../include/crpc/push/wire.hpp) | `push::push_envelope`, `push::push_message_view`, `push::push_subscription` | PUSH/PUSH_ACK payload codecs and the subscribe control contract. |
| [`push/stream.hpp`](../../include/crpc/push/stream.hpp) | `stream_transport`, `client_streaming`, `server_streaming` | DATA-frame streams: client/server/bidi shapes and flow control. |

## Transport

| Header | Key types | What it is |
|---|---|---|
| [`io.hpp`](../../include/crpc/io.hpp) | `io::source`, `io::sink`, `connection`, `backend_factory`, `endpoint`, `inproc_backend` | The two-interface transport contract, the endpoint type and the in-process backend. |
| [`io/iocp_backend.hpp`](../../include/crpc/io/iocp_backend.hpp) | `iocp_backend` | Windows IOCP TCP backend. |
| [`io/epoll_backend.hpp`](../../include/crpc/io/epoll_backend.hpp) | `epoll_backend` | Linux edge-triggered epoll TCP backend. |
| [`io/tls.hpp`](../../include/crpc/io/tls.hpp) | `tls_decorator`, `tls_errc` | TLS decorator over the two-interface contract; needs `CRPC_WITH_OPENSSL`. |

## HTTP and MVC

| Header | Key types | What it is |
|---|---|---|
| [`http/server.hpp`](../../include/crpc/http/server.hpp) | `http::http_server`, `http_server::builder` | HTTP/1.1 + HTTP/2 (h2c/h2) + HTTPS server facade over any backend listener. |
| [`http/client.hpp`](../../include/crpc/http/client.hpp) | `http::client`, `client_options` | HTTP client; HTTP/1.1 by default, HTTP/2 when ALPN negotiates h2 (or h2c prior knowledge). |
| [`http/h2_client.hpp`](../../include/crpc/http/h2_client.hpp) | `http::h2_client`, `h2_client_options` | Multiplexing HTTP/2 client: concurrent streams on one connection (h2c or TLS+ALPN h2). |
| [`http/message.hpp`](../../include/crpc/http/message.hpp) | `http::request`, `http::response` | HTTP/1.1 message model, incremental parser and serializer. |
| [`http2/frame.hpp`](../../include/crpc/http2/frame.hpp) | `h2::frame`, `h2::frame_header` | HTTP/2 frame layer. |
| [`http2/hpack.hpp`](../../include/crpc/http2/hpack.hpp) | `h2::hpack_encoder`, `h2::hpack_decoder` | HPACK header compression. |
| [`http2/connection.hpp`](../../include/crpc/http2/connection.hpp) | `h2::connection` | HTTP/2 connection state machine. |
| [`http/content_coding.hpp`](../../include/crpc/http/content_coding.hpp) | `http::content_coding` | Content-Encoding negotiation and streaming gzip/deflate. |
| [`http/multipart.hpp`](../../include/crpc/http/multipart.hpp) | `http::multipart_parser` | Streaming `multipart/form-data` parser. |
| [`http/file_transfer.hpp`](../../include/crpc/http/file_transfer.hpp) | `http::upload_sink`, `http::download_source` | HTTP upload sink and ranged download source. |
| [`http/sse.hpp`](../../include/crpc/http/sse.hpp) | `http::sse` | Server-Sent Events framing helpers. |
| [`mvc/router.hpp`](../../include/crpc/mvc/router.hpp) | `mvc::router`, `mvc::route` | MVC route table, matching and dispatch. |
| [`mvc/binding.hpp`](../../include/crpc/mvc/binding.hpp) | `mvc::bind_request` | Request parameter binding to handler arguments. |
| [`mvc/attributes.hpp`](../../include/crpc/mvc/attributes.hpp) | `CRPC_HTTP_*` macros | Attribute-declared routes and fields. |

## Resilience

| Header | Key types | What it is |
|---|---|---|
| [`resilience/retry.hpp`](../../include/crpc/resilience/retry.hpp) | `retry_sequence`, `retry_budget`, `retry_policy` | Per-call retry decisions and a shared token-bucket budget. |
| [`resilience/circuit_breaker.hpp`](../../include/crpc/resilience/circuit_breaker.hpp) | `breaker_config`, `breaker_permit` | Circuit breaker. |
| [`resilience/rate_limiter.hpp`](../../include/crpc/resilience/rate_limiter.hpp) | `rate_limit_config` | Token-bucket rate limiter. |
| [`resilience/bulkhead.hpp`](../../include/crpc/resilience/bulkhead.hpp) | `bulkhead_config`, `bulkhead_permit` | Bulkhead (bounded counting semaphore). |
| [`resilience/load_balancer.hpp`](../../include/crpc/resilience/load_balancer.hpp) | `lb_strategy` | Load balancing over service instances. |
| [`resilience/fallback.hpp`](../../include/crpc/resilience/fallback.hpp) | `fallback_fn`, `fallback_outcome` | Fallback vocabulary and the value entry point. |
| [`resilience/admission.hpp`](../../include/crpc/resilience/admission.hpp) | `stage_kind`, `reject_code` | The single admission-stage → `rpc_code` table. |

## Observability

| Header | Key types | What it is |
|---|---|---|
| [`observability/context.hpp`](../../include/crpc/observability/context.hpp) | `obs::trace_context`, `obs::span` | W3C `traceparent`/baggage trace context. |
| [`observability/interceptor.hpp`](../../include/crpc/observability/interceptor.hpp) | `obs::interceptor_chain` | AOP interceptor chain. |
| [`observability/log.hpp`](../../include/crpc/observability/log.hpp) | `obs::logger`, `log_record` | High-performance asynchronous structured logger. |
| [`observability/metrics.hpp`](../../include/crpc/observability/metrics.hpp) | `obs::metrics`, `obs::counter`, `obs::gauge`, `obs::histogram` | Counters, gauges and histograms with a zero-cost disabled path. |
| [`observability/tracer.hpp`](../../include/crpc/observability/tracer.hpp) | `obs::tracer` | Spans, head sampling and asynchronous export. |

## Cluster and flow control

| Header | Key types | What it is |
|---|---|---|
| [`cluster/registry.hpp`](../../include/crpc/cluster/registry.hpp) | `cluster::registry` | Service catalog with immutable snapshots and a monotonic index. |
| [`cluster/lease.hpp`](../../include/crpc/cluster/lease.hpp) | `cluster::lease_manager` | Lease/session manager. |
| [`cluster/election.hpp`](../../include/crpc/cluster/election.hpp) | `cluster::election` | Leader election primitives. |
| [`cluster/health.hpp`](../../include/crpc/cluster/health.hpp) | `cluster::health_checker` | Health checkers. |
| [`cluster/health_bridge.hpp`](../../include/crpc/cluster/health_bridge.hpp) | `cluster::health_bridge`, `cluster::health_instance_set` | Debounced health-to-registry writer and read-only load-balancer view. |
| [`cluster/registry_log.hpp`](../../include/crpc/cluster/registry_log.hpp) | `cluster::registry_log`, `cluster::file_registry_log`, `cluster::memory_registry_log` | Durable, replayable record log backing the registry's write-ahead log. |
| [`cluster/watch.hpp`](../../include/crpc/cluster/watch.hpp) | `cluster::watch_hub` | Index-based watch/subscription over the registry. |
| [`flow/estimator.hpp`](../../include/crpc/flow/estimator.hpp) | `flow::rtt_estimator`, `flow::pacer`, `flow::congestion_window`, `flow::rtt_tracker` | Adaptive flow-control primitives. |
| [`rpc/flow_control.hpp`](../../include/crpc/rpc/flow_control.hpp) | `rpc::flow_controller`, `rpc::flow_control_options`, `rpc::flow_state` | Connection-level flow control: byte window, PING/PONG probes, per-lane quotas, pacer. |

## Codec and serialization

| Header | Key types | What it is |
|---|---|---|
| [`codec.hpp`](../../include/crpc/codec.hpp) | `codec_registry`, `encode_into`, `decode_from` | The codec registry and the typed serialization interface. |
| [`codec_id.hpp`](../../include/crpc/codec_id.hpp) | `codec_id` | Codec identifiers. |

## Redis client

| Header | Key types | What it is |
|---|---|---|
| [`redis/resp.hpp`](../../include/crpc/redis/resp.hpp) | `resp_value`, `resp_parser`, `resp_encoder`, `resp_protocol` | RESP2/RESP3 value model, incremental parser and encoder. |
| [`redis/cache.hpp`](../../include/crpc/redis/cache.hpp) | `client_cache` | Bounded client-side LRU cache refreshed through RESP3 CLIENT TRACKING invalidation pushes. |
| [`redis/command_table.hpp`](../../include/crpc/redis/command_table.hpp) | `command_spec`, `k_command_table`, `lookup_command` | Checked-in metadata (arity, key positions) for every command the client emits, kept in sync with a live server. |
| [`redis/connection.hpp`](../../include/crpc/redis/connection.hpp) | `connection`, `connection_pool`, `connection_config`, `pool_config` | Byte transport over `io::source`/`io::sink`, reconnect/backoff and pooling. |
| [`redis/pipeline.hpp`](../../include/crpc/redis/pipeline.hpp) | `command`, `command_executor`, `message_channel`, `command_channel`, `pipeline` | One command stream, batches and the transport-facing RESP channel. |
| [`redis/pubsub.hpp`](../../include/crpc/redis/pubsub.hpp) | `pubsub`, `pubsub_message`, `pubsub_kind` | Pub/Sub subscribe/unsubscribe, message decoding and `PUBLISH`/`PUBSUB`. |
| [`redis/sentinel.hpp`](../../include/crpc/redis/sentinel.hpp) | `sentinel`, `sentinel_options`, `sentinel_status`, `sentinel_connection_executor` | Master discovery, down detection, election and failover. |
| [`redis/stream.hpp`](../../include/crpc/redis/stream.hpp) | `stream`, `stream_id`, `stream_entry`, `stream_read` | Redis Streams and consumer groups. |
| [`redis/transaction.hpp`](../../include/crpc/redis/transaction.hpp) | `transaction`, `transaction_exec_result` | `WATCH`/`MULTI`/`EXEC`/`DISCARD`. |
| [`redis/types.hpp`](../../include/crpc/redis/types.hpp) | `value`, `*_type` builders, `decode_*`, iterators | Typed value model, command builders and reply projections. |
| [`redis/client.hpp`](../../include/crpc/redis/client.hpp) | `client`, `client::options` | Pool + typed command facade. |
| [`redis/cluster.hpp`](../../include/crpc/redis/cluster.hpp) | `cluster`, `cluster_topology`, `cluster_slot_executor` | Slot routing, `CLUSTER SHARDS`/`SLOTS`, `MOVED`/`ASK` and pinning. |
| [`redis/rtti.hpp`](../../include/crpc/redis/rtti.hpp) | `encode_to_value`, `decode_from_value`, `hash_set_command`, `decode_hash_reply` | Reflection-driven DTO <-> value/hash binding. |

The API map, quick start, async model, error handling and known limitations are
in [Redis client](../redis-client.md).

## C ABI

| Header | Key types | What it is |
|---|---|---|
| [`c_api.h`](../../include/crpc/c_api.h) | `crpc_*` functions | The stable C entry points. See [abi.md](../abi.md). |

## Complete header index

The sections above pick out key types by purpose. This one lists **every**
public header under `include/crpc` by directory, for lookup by name. The
entries are links only; the authoritative declaration is the header.

`tests/test_docs_lint.cpp` guards this list
(`docs_api_index_lists_every_public_header`): adding a public header
without registering it here fails the suite.

### `crpc`

- [`buffer.hpp`](../../include/crpc/buffer.hpp)
- [`client_facade.hpp`](../../include/crpc/client_facade.hpp)
- [`codec.hpp`](../../include/crpc/codec.hpp)
- [`codec_id.hpp`](../../include/crpc/codec_id.hpp)
- [`config.hpp`](../../include/crpc/config.hpp)
- [`delivery_mode.hpp`](../../include/crpc/delivery_mode.hpp)
- [`diagnostics.hpp`](../../include/crpc/diagnostics.hpp)
- [`endian.hpp`](../../include/crpc/endian.hpp)
- [`error.hpp`](../../include/crpc/error.hpp)
- [`frame.hpp`](../../include/crpc/frame.hpp)
- [`frame_compression.hpp`](../../include/crpc/frame_compression.hpp)
- [`id.hpp`](../../include/crpc/id.hpp)
- [`io.hpp`](../../include/crpc/io.hpp)
- [`runtime.hpp`](../../include/crpc/runtime.hpp)
- [`server.hpp`](../../include/crpc/server.hpp)
- [`span.hpp`](../../include/crpc/span.hpp)
- [`wire.hpp`](../../include/crpc/wire.hpp)

### `crpc/algorithm`

- [`algorithm/astar.hpp`](../../include/crpc/algorithm/astar.hpp)
- [`algorithm/heuristic.hpp`](../../include/crpc/algorithm/heuristic.hpp)
- [`algorithm/pathfind.hpp`](../../include/crpc/algorithm/pathfind.hpp)

### `crpc/async`

- [`async/executor.hpp`](../../include/crpc/async/executor.hpp)

### `crpc/concurrency`

- [`concurrency/volatile.hpp`](../../include/crpc/concurrency/volatile.hpp) — `volatile_read`, `volatile_write`, `full_fence`, `signal_fence`: acquire/release access to ordinary 1/2/4/8-byte values.

### `crpc/crypto`

- [`crypto/base64.hpp`](../../include/crpc/crypto/base64.hpp)
- [`crypto/hash.hpp`](../../include/crpc/crypto/hash.hpp)
- [`crypto/rsa.hpp`](../../include/crpc/crypto/rsa.hpp)

### `crpc/db`

- [`db/bulk.hpp`](../../include/crpc/db/bulk.hpp)
- [`db/channel.hpp`](../../include/crpc/db/channel.hpp)
- [`db/client.hpp`](../../include/crpc/db/client.hpp)
- [`db/cluster.hpp`](../../include/crpc/db/cluster.hpp)
- [`db/connection.hpp`](../../include/crpc/db/connection.hpp)
- [`db/hbase/rest.hpp`](../../include/crpc/db/hbase/rest.hpp)
- [`db/mapping.hpp`](../../include/crpc/db/mapping.hpp)
- [`db/memory_result_set.hpp`](../../include/crpc/db/memory_result_set.hpp)
- [`db/mongo/auth.hpp`](../../include/crpc/db/mongo/auth.hpp)
- [`db/mongo/bson.hpp`](../../include/crpc/db/mongo/bson.hpp)
- [`db/mongo/connection.hpp`](../../include/crpc/db/mongo/connection.hpp)
- [`db/mongo/protocol.hpp`](../../include/crpc/db/mongo/protocol.hpp)
- [`db/mysql/auth.hpp`](../../include/crpc/db/mysql/auth.hpp)
- [`db/mysql/channel.hpp`](../../include/crpc/db/mysql/channel.hpp)
- [`db/mysql/cluster.hpp`](../../include/crpc/db/mysql/cluster.hpp)
- [`db/mysql/connection.hpp`](../../include/crpc/db/mysql/connection.hpp)
- [`db/mysql/protocol.hpp`](../../include/crpc/db/mysql/protocol.hpp)
- [`db/odbc/connection.hpp`](../../include/crpc/db/odbc/connection.hpp)
- [`db/oracle/tns.hpp`](../../include/crpc/db/oracle/tns.hpp)
- [`db/oracle/ttc.hpp`](../../include/crpc/db/oracle/ttc.hpp)
- [`db/result_set_list.hpp`](../../include/crpc/db/result_set_list.hpp)
- [`db/rtti_mapping.hpp`](../../include/crpc/db/rtti_mapping.hpp)
- [`db/stream_channel.hpp`](../../include/crpc/db/stream_channel.hpp)
- [`db/tds/cluster.hpp`](../../include/crpc/db/tds/cluster.hpp)
- [`db/tds/connection.hpp`](../../include/crpc/db/tds/connection.hpp)
- [`db/tds/protocol.hpp`](../../include/crpc/db/tds/protocol.hpp)
- [`db/tidb/connection.hpp`](../../include/crpc/db/tidb/connection.hpp)
- [`db/tidb/retry.hpp`](../../include/crpc/db/tidb/retry.hpp)
- [`db/types.hpp`](../../include/crpc/db/types.hpp)

### `crpc/detail`

- [`detail/atomic_shared_ptr.hpp`](../../include/crpc/detail/atomic_shared_ptr.hpp)

### `crpc/fs`

- [`fs/directory.hpp`](../../include/crpc/fs/directory.hpp)
- [`fs/file.hpp`](../../include/crpc/fs/file.hpp)
- [`fs/fs.hpp`](../../include/crpc/fs/fs.hpp)
- [`fs/memory.hpp`](../../include/crpc/fs/memory.hpp)
- [`fs/mmap.hpp`](../../include/crpc/fs/mmap.hpp)
- [`fs/path.hpp`](../../include/crpc/fs/path.hpp)
- [`fs/shared_memory.hpp`](../../include/crpc/fs/shared_memory.hpp)
- [`fs/temp.hpp`](../../include/crpc/fs/temp.hpp)

### `crpc/game`

- [`game/ai/adaboost.hpp`](../../include/crpc/game/ai/adaboost.hpp)
- [`game/ai/behavior_tree.hpp`](../../include/crpc/game/ai/behavior_tree.hpp)
- [`game/ai/decision_tree.hpp`](../../include/crpc/game/ai/decision_tree.hpp)
- [`game/aoi/aoi.hpp`](../../include/crpc/game/aoi/aoi.hpp)
- [`game/aoi/grid.hpp`](../../include/crpc/game/aoi/grid.hpp)
- [`game/aoi/world.hpp`](../../include/crpc/game/aoi/world.hpp)
- [`game/geom/attack.hpp`](../../include/crpc/game/geom/attack.hpp)
- [`game/geom/collide2d.hpp`](../../include/crpc/game/geom/collide2d.hpp)
- [`game/geom/collide3d.hpp`](../../include/crpc/game/geom/collide3d.hpp)
- [`game/geom/hitbox.hpp`](../../include/crpc/game/geom/hitbox.hpp)
- [`game/geom/shapes2d.hpp`](../../include/crpc/game/geom/shapes2d.hpp)
- [`game/geom/shapes3d.hpp`](../../include/crpc/game/geom/shapes3d.hpp)
- [`game/geom/transform.hpp`](../../include/crpc/game/geom/transform.hpp)
- [`game/geom/vec.hpp`](../../include/crpc/game/geom/vec.hpp)

### `crpc/http`

- [`http/client.hpp`](../../include/crpc/http/client.hpp)
- [`http/content_coding.hpp`](../../include/crpc/http/content_coding.hpp)
- [`http/file_transfer.hpp`](../../include/crpc/http/file_transfer.hpp)
- [`http/forwarded.hpp`](../../include/crpc/http/forwarded.hpp)
- [`http/h2_client.hpp`](../../include/crpc/http/h2_client.hpp)
- [`http/message.hpp`](../../include/crpc/http/message.hpp)
- [`http/multipart.hpp`](../../include/crpc/http/multipart.hpp)
- [`http/server.hpp`](../../include/crpc/http/server.hpp)
- [`http/sse.hpp`](../../include/crpc/http/sse.hpp)

### `crpc/http2`

- [`http2/connection.hpp`](../../include/crpc/http2/connection.hpp)
- [`http2/frame.hpp`](../../include/crpc/http2/frame.hpp)
- [`http2/hpack.hpp`](../../include/crpc/http2/hpack.hpp)

### `crpc/io`

- [`io/epoll_backend.hpp`](../../include/crpc/io/epoll_backend.hpp)
- [`io/iocp_backend.hpp`](../../include/crpc/io/iocp_backend.hpp)
- [`io/tls.hpp`](../../include/crpc/io/tls.hpp)

### `crpc/ipc`

- [`ipc/channel.hpp`](../../include/crpc/ipc/channel.hpp)
- [`ipc/ring_buffer.hpp`](../../include/crpc/ipc/ring_buffer.hpp)
- [`ipc/spsc_queue.hpp`](../../include/crpc/ipc/spsc_queue.hpp)

### `crpc/lock`

- [`lock/local_lock.hpp`](../../include/crpc/lock/local_lock.hpp)

### `crpc/memcached`

- [`memcached/client.hpp`](../../include/crpc/memcached/client.hpp)
- [`memcached/hash.hpp`](../../include/crpc/memcached/hash.hpp)
- [`memcached/protocol.hpp`](../../include/crpc/memcached/protocol.hpp)
- [`memcached/router.hpp`](../../include/crpc/memcached/router.hpp)

### `crpc/memory`

- [`memory/arena.hpp`](../../include/crpc/memory/arena.hpp)
- [`memory/object_pool.hpp`](../../include/crpc/memory/object_pool.hpp)
- [`memory/pool_allocator.hpp`](../../include/crpc/memory/pool_allocator.hpp)

### `crpc/mq`

- [`mq/amqp/connection.hpp`](../../include/crpc/mq/amqp/connection.hpp)
- [`mq/amqp/protocol.hpp`](../../include/crpc/mq/amqp/protocol.hpp)
- [`mq/amqp/types.hpp`](../../include/crpc/mq/amqp/types.hpp)
- [`mq/msmq/queue.hpp`](../../include/crpc/mq/msmq/queue.hpp)
- [`mq/msmq/transaction.hpp`](../../include/crpc/mq/msmq/transaction.hpp)
- [`mq/msmq/types.hpp`](../../include/crpc/mq/msmq/types.hpp)

### `crpc/mvc`

- [`mvc/attributes.hpp`](../../include/crpc/mvc/attributes.hpp)
- [`mvc/binding.hpp`](../../include/crpc/mvc/binding.hpp)
- [`mvc/router.hpp`](../../include/crpc/mvc/router.hpp)

### `crpc/net`

- [`net/address.hpp`](../../include/crpc/net/address.hpp)
- [`net/backoff.hpp`](../../include/crpc/net/backoff.hpp)
- [`net/channel.hpp`](../../include/crpc/net/channel.hpp)
- [`net/connection.hpp`](../../include/crpc/net/connection.hpp)
- [`net/connector.hpp`](../../include/crpc/net/connector.hpp)
- [`net/icmp.hpp`](../../include/crpc/net/icmp.hpp)
- [`net/ip.hpp`](../../include/crpc/net/ip.hpp)
- [`net/listener.hpp`](../../include/crpc/net/listener.hpp)
- [`net/ping.hpp`](../../include/crpc/net/ping.hpp)
- [`net/udp.hpp`](../../include/crpc/net/udp.hpp)

### `crpc/observability`

- [`observability/context.hpp`](../../include/crpc/observability/context.hpp)
- [`observability/interceptor.hpp`](../../include/crpc/observability/interceptor.hpp)
- [`observability/log.hpp`](../../include/crpc/observability/log.hpp)
- [`observability/metrics.hpp`](../../include/crpc/observability/metrics.hpp)
- [`observability/tracer.hpp`](../../include/crpc/observability/tracer.hpp)

### `crpc/orm`

- [`orm/annotate.hpp`](../../include/crpc/orm/annotate.hpp)
- [`orm/fwd.hpp`](../../include/crpc/orm/fwd.hpp)
- [`orm/metadata.hpp`](../../include/crpc/orm/metadata.hpp)
- [`orm/orm.hpp`](../../include/crpc/orm/orm.hpp)
- [`orm/session.hpp`](../../include/crpc/orm/session.hpp)
- [`orm/sql.hpp`](../../include/crpc/orm/sql.hpp)
- [`orm/value.hpp`](../../include/crpc/orm/value.hpp)

### `crpc/pattern`

- [`pattern/constraint.hpp`](../../include/crpc/pattern/constraint.hpp)
- [`pattern/memento.hpp`](../../include/crpc/pattern/memento.hpp)
- [`pattern/version.hpp`](../../include/crpc/pattern/version.hpp)

### `crpc/push`

- [`push/bridge.hpp`](../../include/crpc/push/bridge.hpp)
- [`push/hub.hpp`](../../include/crpc/push/hub.hpp)
- [`push/stream.hpp`](../../include/crpc/push/stream.hpp)
- [`push/wire.hpp`](../../include/crpc/push/wire.hpp)

### `crpc/redis`

- [`redis/cache.hpp`](../../include/crpc/redis/cache.hpp)
- [`redis/client.hpp`](../../include/crpc/redis/client.hpp)
- [`redis/cluster.hpp`](../../include/crpc/redis/cluster.hpp)
- [`redis/command_table.hpp`](../../include/crpc/redis/command_table.hpp)
- [`redis/connection.hpp`](../../include/crpc/redis/connection.hpp)
- [`redis/distributed_lock.hpp`](../../include/crpc/redis/distributed_lock.hpp)
- [`redis/keyspace.hpp`](../../include/crpc/redis/keyspace.hpp)
- [`redis/pipeline.hpp`](../../include/crpc/redis/pipeline.hpp)
- [`redis/pubsub.hpp`](../../include/crpc/redis/pubsub.hpp)
- [`redis/resp.hpp`](../../include/crpc/redis/resp.hpp)
- [`redis/rtti.hpp`](../../include/crpc/redis/rtti.hpp)
- [`redis/scan.hpp`](../../include/crpc/redis/scan.hpp)
- [`redis/scripting.hpp`](../../include/crpc/redis/scripting.hpp)
- [`redis/sentinel.hpp`](../../include/crpc/redis/sentinel.hpp)
- [`redis/stream.hpp`](../../include/crpc/redis/stream.hpp)
- [`redis/transaction.hpp`](../../include/crpc/redis/transaction.hpp)
- [`redis/types.hpp`](../../include/crpc/redis/types.hpp)

### `crpc/resilience`

- [`resilience/admission.hpp`](../../include/crpc/resilience/admission.hpp)
- [`resilience/bulkhead.hpp`](../../include/crpc/resilience/bulkhead.hpp)
- [`resilience/circuit_breaker.hpp`](../../include/crpc/resilience/circuit_breaker.hpp)
- [`resilience/fallback.hpp`](../../include/crpc/resilience/fallback.hpp)
- [`resilience/load_balancer.hpp`](../../include/crpc/resilience/load_balancer.hpp)
- [`resilience/rate_limiter.hpp`](../../include/crpc/resilience/rate_limiter.hpp)
- [`resilience/retry.hpp`](../../include/crpc/resilience/retry.hpp)
- [`resilience/smart_policy.hpp`](../../include/crpc/resilience/smart_policy.hpp)

### `crpc/rpc`

- [`rpc/call.hpp`](../../include/crpc/rpc/call.hpp)
- [`rpc/client.hpp`](../../include/crpc/rpc/client.hpp)
- [`rpc/dedup.hpp`](../../include/crpc/rpc/dedup.hpp)
- [`rpc/dispatcher.hpp`](../../include/crpc/rpc/dispatcher.hpp)
- [`rpc/flow_control.hpp`](../../include/crpc/rpc/flow_control.hpp)
- [`rpc/lane_queue.hpp`](../../include/crpc/rpc/lane_queue.hpp)
- [`rpc/method_macro.hpp`](../../include/crpc/rpc/method_macro.hpp)
- [`rpc/method_meta.hpp`](../../include/crpc/rpc/method_meta.hpp)

### `crpc/text`

- [`text/case.hpp`](../../include/crpc/text/case.hpp)
- [`text/number.hpp`](../../include/crpc/text/number.hpp)
- [`text/string_ops.hpp`](../../include/crpc/text/string_ops.hpp)
- [`text/utf.hpp`](../../include/crpc/text/utf.hpp)

### `crpc/ws`

- [`ws/client.hpp`](../../include/crpc/ws/client.hpp)
- [`ws/frame.hpp`](../../include/crpc/ws/frame.hpp)
- [`ws/handshake.hpp`](../../include/crpc/ws/handshake.hpp)
- [`ws/server.hpp`](../../include/crpc/ws/server.hpp)
- [`ws/stream.hpp`](../../include/crpc/ws/stream.hpp)

### `crpc/actor`

- [`actor/actor.hpp`](../../include/crpc/actor/actor.hpp) — `actor`, typed post/stop lifecycle, bounded mailbox backpressure.
- [`actor/mailbox.hpp`](../../include/crpc/actor/mailbox.hpp) — `mailbox<Message>`, lock-free SPSC with `resource_exhausted` on full.

### `crpc/aspect`

- [`aspect/aspect.hpp`](../../include/crpc/aspect/aspect.hpp) — `advice`, `pointcut`, `aspect_chain`: before/after/around/after-throwing with an allocation-free hot path.

### `crpc/cache_line`

- [`cache_line.hpp`](../../include/crpc/cache_line.hpp) — `cache_line_size`, `CRPC_CACHE_LINE_ALIGN`: 64-byte alignment for internal hot state.

### `crpc/crypto` additions

- [`crypto/md5.hpp`](../../include/crpc/crypto/md5.hpp) — `md5`, `md5_digest`: RFC 1321 (UUID v3 only, not for security).
- [`crypto/uuid.hpp`](../../include/crpc/crypto/uuid.hpp) — `uuid`, `uuid_v1`…`uuid_v7`, `parse_uuid`, `to_string`, `uuid_hash`.

### `crpc/db/sqlite`

- [`db/sqlite/connection.hpp`](../../include/crpc/db/sqlite/connection.hpp) — `sqlite::connection`, `connection_factory`: the embedded SQL backend over the sqlite3 C API.

### `crpc/domain` (DDD/CQRS)

- [`domain/entity.hpp`](../../include/crpc/domain/entity.hpp) — `entity<Id, Derived>`: identity-based equality.
- [`domain/value_object.hpp`](../../include/crpc/domain/value_object.hpp) — `value_object<Derived>`: value-based equality.
- [`domain/aggregate.hpp`](../../include/crpc/domain/aggregate.hpp) — `aggregate_root<Id, Derived>`: consistency boundary with uncommitted events.
- [`domain/domain_event.hpp`](../../include/crpc/domain/domain_event.hpp) — `domain_event`: name/payload/monotonic sequence.
- [`domain/repository.hpp`](../../include/crpc/domain/repository.hpp) — `repository<T, Id>`, `in_memory_repository`.
- [`domain/unit_of_work.hpp`](../../include/crpc/domain/unit_of_work.hpp) — `unit_of_work`: atomic commit, reverse-order rollback.
- [`domain/cqrs.hpp`](../../include/crpc/domain/cqrs.hpp) — `command`, `query`, `handler_registry`, `projection`.

### `crpc/ftp`

- [`ftp/client.hpp`](../../include/crpc/ftp/client.hpp) — `ftp::client`: RFC 959/2228/4217 client with PASV/EPSV, resume, explicit/implicit FTPS.
- [`ftp/command.hpp`](../../include/crpc/ftp/command.hpp) — command parsing and serialization.
- [`ftp/reply.hpp`](../../include/crpc/ftp/reply.hpp) — `ftp_reply`: single- and multi-line reply parsing.
- [`ftp/server.hpp`](../../include/crpc/ftp/server.hpp) — `ftp::server`: path-confined FTP/FTPS server.

### `crpc/i18n`

- [`i18n/catalog.hpp`](../../include/crpc/i18n/catalog.hpp) — `message_catalog`: locales, fallback chains, plural rules, interpolation.
- [`i18n/format.hpp`](../../include/crpc/i18n/format.hpp) — `format_number`, `parse_number`: locale-aware numeric formatting.

### `crpc/numeric`

- [`numeric/bigint.hpp`](../../include/crpc/numeric/bigint.hpp) — `uint_big<Bits>` template: wraparound arithmetic like built-in integers.
- [`numeric/uint128.hpp`](../../include/crpc/numeric/uint128.hpp) — `uint128`.
- [`numeric/uint192.hpp`](../../include/crpc/numeric/uint192.hpp) — `uint192`.
- [`numeric/uint256.hpp`](../../include/crpc/numeric/uint256.hpp) — `uint256`.
- [`numeric/uint384.hpp`](../../include/crpc/numeric/uint384.hpp) — `uint384`.
- [`numeric/uint512.hpp`](../../include/crpc/numeric/uint512.hpp) — `uint512`.

### `crpc/proactor`

- [`proactor/proactor.hpp`](../../include/crpc/proactor/proactor.hpp) — `proactor<Backend>`: completion-driven I/O over the `io::` contract, exactly one completion per submit.

### `crpc/reactor`

- [`reactor/reactor.hpp`](../../include/crpc/reactor/reactor.hpp) — `reactor<Demux>`: readiness-driven dispatch with demux adapter.

### `crpc/system`

- [`system/cpu.hpp`](../../include/crpc/system/cpu.hpp) — `logical_processor_count`, `physical_processor_count`, `get/set_current_thread_affinity`, `get/set_current_thread_priority`: query and control the calling thread's processor placement and scheduling priority.
- [`system/process.hpp`](../../include/crpc/system/process.hpp) — `process::run/spawn`, `list_processes`, `query_process`: subprocesses with stdin/stdout/stderr redirection.
- [`system/sysinfo.hpp`](../../include/crpc/system/sysinfo.hpp) — `cpu()`, `memory()`, `disks()`, `hostname()`, `uptime_seconds()`.

### `crpc/time`

- [`time/duration.hpp`](../../include/crpc/time/duration.hpp) — `timestamp`, `duration`: nanosecond base with second/millisecond/microsecond helpers.
- [`time/datetime.hpp`](../../include/crpc/time/datetime.hpp) — `date_time`: Gregorian calendar arithmetic, leap rules, RFC 3339.
- [`time/format.hpp`](../../include/crpc/time/format.hpp) — `%Y %m %d %H %M %S %f %z` formatting.
- [`time/timezone.hpp`](../../include/crpc/time/timezone.hpp) — `timezone_at`, `local_timezone`, `list_timezones`: IANA tzdata with fixed-offset fallback.
- [`time/clock.hpp`](../../include/crpc/time/clock.hpp) — `now_unix`, `utc_now`, `gmt_now`, `system_tick`, `process_tick`, `steady_now_ticks`, `steady_frequency`: wall-clock and monotonic tick readings.

### `crpc/text` additions

- [`text/encoding.hpp`](../../include/crpc/text/encoding.hpp) — base32/base32hex/base58/base85/ascii85/uuencode/quoted-printable/punycode/UTF-7/Latin-1/GBK.
- [`text/pinyin.hpp`](../../include/crpc/text/pinyin.hpp) — `to_pinyin`, `to_pinyin_initials`, `pinyin_candidates`: full-GBK pinyin with phrase disambiguation.
- [`text/regex.hpp`](../../include/crpc/text/regex.hpp) — `regex`: backtracking engine with a hard step limit against catastrophic backtracking.
- [`text/string_order.hpp`](../../include/crpc/text/string_order.hpp) — `string_order`, `string_comparison`: .NET StringComparer-style ordinal/invariant comparisons.
- [`text/unicode.hpp`](../../include/crpc/text/unicode.hpp) — `normalize` (NFC/NFD/NFKC/NFKD), `to_case`, category queries, grapheme clusters.

### `crpc/algorithm` additions

- [`algorithm/sort.hpp`](../../include/crpc/algorithm/sort.hpp) — `intro_sort`, `quick_sort`, `heap_sort`, `radix_sort`.
- [`algorithm/heap.hpp`](../../include/crpc/algorithm/heap.hpp) — `binary_heap`, `make_heap`, `push_heap`, `pop_heap`.
- [`algorithm/tree_traversal.hpp`](../../include/crpc/algorithm/tree_traversal.hpp) — non-recursive pre/in/post/level-order traversal.

### `crpc/cluster`

- [`cluster/election.hpp`](../../include/crpc/cluster/election.hpp)
- [`cluster/health.hpp`](../../include/crpc/cluster/health.hpp)
- [`cluster/health_bridge.hpp`](../../include/crpc/cluster/health_bridge.hpp)
- [`cluster/lease.hpp`](../../include/crpc/cluster/lease.hpp)
- [`cluster/registry.hpp`](../../include/crpc/cluster/registry.hpp)
- [`cluster/registry_log.hpp`](../../include/crpc/cluster/registry_log.hpp)
- [`cluster/watch.hpp`](../../include/crpc/cluster/watch.hpp)

### `crpc/flow`

- [`flow/estimator.hpp`](../../include/crpc/flow/estimator.hpp)


## See also

- [Tutorial](../tutorial.md) — a step-by-step build of a call and a push subscription.
- [Architecture](../architecture.md) — the subsystem map and status matrix.
- [Wire protocol](../protocol.md) — the frame format and codecs.
- [Redis client](../redis-client.md) — the Redis API map and recipes.
- [Distributed testing and benchmarks](../distributed-testing.md) — multi-node tests and benchmark commands.
- [ABI policy](../abi.md) — layout and export rules.
- [Build and test](../building.md) — CMake options and the one-click scripts.
