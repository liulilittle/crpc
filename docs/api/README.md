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
| [`server.hpp`](../../include/crpc/server.hpp) | `server`, `server::builder`, `server::options`, `publish`, `publish_result` | The server facade: listeners, shards, handler pool, graceful stop and topic publish. |
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
| [`observability/tracer.hpp`](../../include/crpc/observability/tracer.hpp) | `obs::tracer` | Spans, head sampling and asynchronous export. |

## Cluster and flow control

| Header | Key types | What it is |
|---|---|---|
| [`cluster/registry.hpp`](../../include/crpc/cluster/registry.hpp) | `cluster::registry` | Service catalog with immutable snapshots and a monotonic index. |
| [`cluster/lease.hpp`](../../include/crpc/cluster/lease.hpp) | `cluster::lease_manager` | Lease/session manager. |
| [`cluster/election.hpp`](../../include/crpc/cluster/election.hpp) | `cluster::election` | Leader election primitives. |
| [`cluster/health.hpp`](../../include/crpc/cluster/health.hpp) | `cluster::health_checker` | Health checkers. |
| [`cluster/watch.hpp`](../../include/crpc/cluster/watch.hpp) | `cluster::watch_hub` | Index-based watch/subscription over the registry. |
| [`flow/estimator.hpp`](../../include/crpc/flow/estimator.hpp) | `flow::estimator` | Adaptive flow control. |

## Codec and serialization

| Header | Key types | What it is |
|---|---|---|
| [`codec.hpp`](../../include/crpc/codec.hpp) | `codec_registry`, `encode_into`, `decode_from` | The codec registry and the typed serialization interface. |
| [`codec_id.hpp`](../../include/crpc/codec_id.hpp) | `codec_id` | Codec identifiers. |

## C ABI

| Header | Key types | What it is |
|---|---|---|
| [`c_api.h`](../../include/crpc/c_api.h) | `crpc_*` functions | The stable C entry points. See [abi.md](../abi.md). |

## See also

- [Tutorial](../tutorial.md) — a step-by-step build of a call and a push subscription.
- [Architecture overview](../architecture.md) — the subsystem map and status matrix.
- [Wire protocol](../protocol.md) — the frame format and codecs.
- [ABI policy](../abi.md) — layout and export rules.
- [Build and test](../building.md) — CMake options and the one-click scripts.
