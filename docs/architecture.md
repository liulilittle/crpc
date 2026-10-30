# CRPC architecture

[绠€浣撲腑鏂嘳(architecture.zh-CN.md) | **English**

Version 2.0 / 2026-10-24

This document describes the layers, the transport contract, the execution model, the call state machine, and where each requirement lives. It separates what the repository implements today from what the design freezes but the code does not yet provide. **Specified, not implemented** marks a design commitment, not current behaviour.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Scope and status |
| [2](#s2) | Two first-class capabilities |
| [3](#s3) | Layer map |
| [4](#s4) | Dependency graph |
| [5](#s5) | Base layer |
| [6](#s6) | Transport contract |
| [7](#s7) | Transport backends |
| [8](#s8) | Shard-per-core thread model |
| [9](#s9) | RPC core |
| [10](#s10) | Call state machine |
| [11](#s11) | Codec layer |
| [12](#s12) | HTTP and MVC |
| [13](#s13) | Cluster layer |
| [14](#s14) | Resilience layer |
| [15](#s15) | Observability |
| [16](#s16) | Push and streaming |
| [17](#s17) | Flow control |
| [18](#s18) | Requirement map |
| [19](#s19) | Status matrix |
| [20](#s20) | Verification map |

## 1. Scope and status <a id="s1"></a>

CRPC is a C++17 framework for large server systems. Its public surface is header-first: `include/crpc/**` defines every type a consumer names, and `src/**` contains the out-of-line implementation. The project is at v1.0: the core modules and the CRPC/HTTP server facades are implemented and tested; several distribution and end-to-end features remain primitive-level (搂18). There is no install target and no checked-in CI configuration yet.

- **implemented** 鈥?code exists, compiles, and has a test suite in `tests/`.
- **partial** 鈥?code exists and is tested, but a documented part is missing.
- **specified, not implemented** 鈥?the design freezes the behaviour, but no code path provides it.

The authoritative target is [the design specification](superpowers/specs/2026-10-19-crpc-design.md). If this document and the specification disagree, the code wins for a factual statement about today and the specification wins for the target contract.

## 2. Two first-class capabilities <a id="s2"></a>

CRPC carries two capabilities on one connection, one frame format and one execution model:

1. **Calls.** One request produces exactly one response. Inside a coroutine the call is intended to look synchronous; outside one it is a handle (`rpc::call<T>`) that can be waited on, polled, cancelled or given a callback. Both forms share one state machine.
2. **Push.** A server publishes to a logical subscriber identity. The subscriber may reconnect and resume; reliable delivery uses a replay buffer and acknowledgements, unreliable delivery may drop and is counted.

They share `source`/`sink`, framing, error values, deadlines, trace context and backpressure. They do not share a call slot: push has a subscription, not an in-flight call. That is why `include/crpc/rpc/**` and `include/crpc/push/**` have separate state machines.

The call path is implemented through `rpc::client` and `rpc::dispatcher`; the push path through `push::hub` and `push::stream`. Both are wired to the same connection at the frame level: `rpc::client::handle_frame` decodes `RESPONSE`, `PING`, `GOAWAY`, `PUSH` and `PUSH_ACK` (`src/rpc/client.cpp:129-149`), delivers a decoded `PUSH` message to the installed `push_sink_` and answers a reliable message with `PUSH_ACK`; the server's push pump encodes `PUSH` frames and handles the peer's `PUSH_ACK` (`src/server/server.cpp:591-609,741-747,1389`).

## 3. Layer map <a id="s3"></a>

The tree is flatter than the design's illustrative layout: public headers live directly under `include/crpc/` (`frame.hpp`, not `wire/frame.hpp`), while `src/` mirrors the module names. The layers remain distinct:

| Layer | Public headers | Implementation | Responsibility |
|---|---|---|---|
| base | `config.hpp`, `span.hpp`, `buffer.hpp`, `error.hpp`, `endian.hpp` | `src/error.cpp` | fixed widths, platform checks, borrowed views, buffer, errors, byte order |
| protocol | `wire.hpp`, `frame.hpp` | `src/frame/frame.cpp` | varint/zigzag, bounds-checked I/O, 16-byte frame codec, call envelope, decoder |
| transport | `io.hpp`, `io/epoll_backend.hpp`, `io/iocp_backend.hpp` | `src/io/*` | two-interface contract, connection/factory, inproc, epoll ET, IOCP |
| rpc | `rpc/call.hpp`, `rpc/client.hpp`, `rpc/dispatcher.hpp` | `src/rpc/*` | slots, state machine, deadlines, cancel, request/response, dispatch |
| codec | `codec.hpp` | `src/codec/codec.cpp` | crpc-binary, RTTI text/schema adapters, registry |
| http | `http/message.hpp`, `http/multipart.hpp`, `http/file_transfer.hpp` | `src/http/*` | HTTP/1.1 messages, parser, multipart, upload sink, download/range |
| http2 | `http2/frame.hpp`, `http2/hpack.hpp`, `http2/connection.hpp` | `src/http2/*` | framing, HPACK, stream and flow-control state |
| mvc | `mvc/attributes.hpp`, `mvc/router.hpp`, `mvc/binding.hpp` | `src/mvc/*` | routes, DTO binding, response mapping |
| cluster | `cluster/registry.hpp`, `cluster/lease.hpp`, `cluster/election.hpp`, `cluster/health.hpp`, `cluster/watch.hpp` | `src/cluster/*` | catalog, leases, election, health, watches |
| resilience | `resilience/*.hpp` | `src/resilience/*` | breaker, rate limit, bulkhead, load balancing, retry |
| observability | `observability/context.hpp`, `observability/interceptor.hpp`, `observability/log.hpp`, `observability/tracer.hpp` | `src/observability/*` | trace context, AOP, logger, spans |
| push | `push/hub.hpp`, `push/stream.hpp` | `src/push/*` | topics, subscriptions, replay, streams |
| flow | `flow/estimator.hpp` | `src/flow/estimator.cpp` | RTT baseline, queue signal, pacer, window |

The design's `transport` and `wire` contexts map to `io` and `frame`/`wire` in the tree. The names differ; the dependency direction does not.

## 4. Dependency graph <a id="s4"></a>

The intended direction is strictly downward, derived from the `#include <crpc/...>` lines in the public headers:

```
                    rpc          mvc
                  /  |  \       /   \
              codec frame  io  http  codec
                 \   |     |    |    /
                  \  |     |   http2
                   \ |     |   /
                    base + wire
                         |
                 config / span / buffer / error / endian

  cluster ---- resilience ---- observability ---- flow
      \__________  all depend on base only  __________/
```

Rules that hold today:

- `base` includes nothing from a higher layer.
- `io` depends only on `base`; the RPC layer accepts a `backend_factory&` and never tests whether the transport is TCP or in-process.
- `rpc/client.hpp` includes `frame.hpp`, `io.hpp`, `rpc/call.hpp` and `rpc/dispatcher.hpp`; `rpc/dispatcher.hpp` includes `frame.hpp` and `rpc/call.hpp`.
- `mvc/router.hpp` includes `http/message.hpp`, `codec.hpp` and `mvc/binding.hpp`; `http2/connection.hpp` includes `http2/frame.hpp`, `http2/hpack.hpp` and `http/message.hpp`.
- `cluster`, `push` and `flow` are independent of `rpc` today. `resilience` is partly integrated: `client_facade` wires load balancing, per-endpoint rate/bulkhead/breaker, retry and the deadline; `rpc::server` wires the admission half of the pipeline (per-connection rate, per-service rate, bulkhead, breaker, cap). The fallback primitives are implemented: the server advertises `crpc-degradable` at four pre-execution rejection points, but `fallback_execute` is still never called from the server; the client facade calls it from `call<Resp>::get()` after the last attempt was finalized, so a refusal the server marked degradable can be answered by an installed fallback value. The observability interceptor chain is wired into the server call path (`before_call`/`after_call` around admission and dispatch; the client terminal outcome is observed through the process-wide default chain). Flow control is still not wired into the call path.
- No script enforces these directions. The design's context-dependency rule is a review rule; a build-time dependency check is **specified, not implemented**.

## 5. Base layer <a id="s5"></a>

`config.hpp` rejects an unsupported target at compile time: 8-bit bytes, exactly 8/16/32/64-bit integers, IEEE-754 float and double, and a known host byte order. It defines `u8`鈥u64`, `i8`鈥i64`, `CRPC_API`, and the platform macros used by the backends.

`span<T>` is the C++17 stand-in for `std::span`: two words, trivially copyable, borrowed, bounds-clamping on `subspan`. `as_bytes`/`as_writable_bytes` are the only reinterpretation helpers and require a trivially copyable element type.

`buffer` is the only dynamic byte container. It grows geometrically (1.5x), has a caller-supplied ceiling (64 MiB by default), keeps capacity across `clear()`, and reports allocation failure through `failed()` instead of throwing.

`error.hpp` defines `rpc_code` (0鈥?1), `rpc_error` (code, message, retryable, retry-after, ordered metadata), `status` for void results, and `result<T>` for value-or-error. Nothing in the library throws across a coroutine, thread or ABI boundary; `result<T>` does not require `T` to be default-constructible.

`endian.hpp` and `wire.hpp` are the only sanctioned way to touch bytes. Multi-byte integers use explicit little-endian load/store, never a struct `memcpy`, bit-field or unaligned `reinterpret_cast`. `byte_reader` and `byte_writer` are total: every read returns a value or `rpc_code::protocol_error`, every write succeeds or reports `rpc_code::encode_error`.

## 6. Transport contract <a id="s6"></a>

The transport is two interfaces wide, plus lifecycle objects:

- `io::source` 鈥?backend to framework: `on_data(span) -> size_t`, `on_eof()`, `on_error(std::error_code)`.
- `io::sink` 鈥?framework to backend: `push(span, completion*)`.
- `io::completion` 鈥?exactly one `complete(io_result)` per push.
- `connection` / `listener` / `backend_factory` 鈥?establishment, close, cancellation, options, `bind_source`, `resume_read`.

`on_data` returns a **credit**, not a boolean: `n == data.size()` consumes everything; `0 < n < data.size()` consumes a prefix, retains the tail and delivers it only after `resume_read()`; `0` pauses reading. This is the whole backpressure mechanism, and there is no source-side completion object. `resume_read()` is on `connection`, outside the two interfaces; the design records that placement as open issue O2, and the code follows it.

| # | Invariant enforced by the backends |
|---|---|
| I1 | Every `completion::complete` runs exactly once (success, failure, cancel). |
| I2 | A completion may run on any thread. |
| I3 | A completion must not block and must not throw (`noexcept`). |
| I4 | After `close()`, no new source delivery or sink push starts; in-flight completions still run once, cancelled. |
| I5 | At most one sink push is in flight per direction. |
| I6 | The backend completes partial writes; the framework never re-submits a tail. |
| I7 | The source span is valid until `on_data` returns; the sink span until `complete` returns. |
| I8 | One reader and one writer per connection at any instant. |
| I9 | `on_data` never returns more than `data.size()`; a violation is clamped. |
| I10 | After a short credit, the tail is retained and delivered only after `resume_read()`. |

Establishment, close, cancel and options live on `connection` and `backend_factory`, never on `source`/`sink`. Above this header there is no `if (is_tcp)`.

Backpressure is layered: the kernel receive window closes by returning zero credit; the send ring is bounded; the client rejects a frame that would exceed `max_pending_write_bytes` rather than blocking; push subscriptions have per-subscription message/byte bounds. The default policy is fast failure or explicit parking, not unbounded queuing.

## 7. Transport backends <a id="s7"></a>

| Backend | Header | Model | Bounds |
|---|---|---|---|
| in-process | `io.hpp` (`inproc_pair`, `inproc_backend`) | bounded queue pair, serialized delivery, deterministic faults | `max_queued_bytes` 64 KiB per direction |
| epoll | `io/epoll_backend.hpp` | one loop thread, epoll + eventfd doorbell + timerfd, ET only | send ring 64 KiB, write budget 256 KiB/turn, 4096 connections, 256 events/batch |
| IOCP | `io/iocp_backend.hpp` | one completion port, one pump thread, AcceptEx, one recv/one send per connection, high-resolution timer thread | send queue 256 KiB, 64 accept posts, 64 completions/batch |

The in-process backend is a peer, not a mock: it implements the same `connection`, `sink`, `source` and `completion` contracts, serializes delivery, propagates EOF and errors, honours the credit rule, and exposes deterministic fault hooks (`drop_bytes`, `delay_chunks`, `reorder_pairs`, `inject_write_error`, `inject_read_error`). Most contract tests use it.

The epoll backend registers every fd with `EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP`; there is no level-triggered path and no millisecond fallback. The event token is a slot index plus a generation, never a raw pointer, so a stale event cannot touch freed memory. The write loop has a per-turn byte budget and self-posts a wake when the budget runs out while the socket is still writable (R4); otherwise an edge consumed while the ring was empty could strand later data. `timerfd` carries sub-millisecond deadlines in the same epoll set.

The IOCP backend uses one completion port per backend and one pump thread with `GetQueuedCompletionStatusEx`. Each connection has at most one outstanding `WSARecv` and one `WSASend`, records come from a pool, `AcceptEx` is pre-posted, and `CancelIoEx` is a request: the cancelled operation still produces exactly one completion, mapped to `operation_canceled`. A separate high-resolution waitable-timer thread posts completion packets so the pump never sleeps in milliseconds.

Both network backends own their loop/pump thread. Source callbacks are serialized per connection. `close`, `resume_read`, `is_open` and `remote` are callable from other threads; `push` is safe from one other thread at a time and must not be issued twice concurrently on the same connection.

## 8. Shard-per-core thread model <a id="s8"></a>

The runtime implements the shard primitive. One `runtime` is:

```
1 OS thread running one corocc LOOP runtime     (CRPC_HAVE_COROCC)
a bounded cross-shard inbox                     (inbox_capacity, default 65536)
a bounded handler ready queue                   (ready_capacity, default 65536)
64 long-lived handler coroutines, 64 KiB stacks (handler_workers / handler_stack_bytes)
a monotonic timer wheel                         (arm_timer / run_timers)
a condition-variable doorbell                   (wake / idle wait)
```

`runtime::start_thread()` creates the thread and `runtime::run()` drives the loop: drain the inbox, expire timers, pump the corocc runtime, then wait for the next deadline or the doorbell. `post()` and `spawn()` enqueue to the inbox; `submit()` enqueues to the handler ready queue; `arm_timer()` stores a timer keyed by monotonic nanoseconds. A completion owned by another shard is posted to that shard's inbox; no thread pumps another shard's runtime.

When `CRPC_HAVE_COROCC` is undefined the same API runs on the shard thread without coroutines: `post`/`submit` execute the task as a plain call and `spawn` invokes the function directly. This is the nofiber fallback, not a separate API.

`rpc::server` builds on the primitive: `start()` creates `options.shards` runtimes (default 1) and starts each thread; listeners feed an accept sink; accepted connections are assigned round-robin to a shard; the transport source decodes frames and posts a `request_job` through `runtime::submit`; handler coroutines run `rpc::dispatcher` and enqueue responses on a per-connection send queue with at most one backend push in flight.

Still specified, not implemented: the automatic `clamp(physical_cores - 1, 1, 32)` shard count (the caller sets `options.shards`), per-shard ownership of the epoll/IOCP loop (each network backend still owns its own loop/pump thread), and a lock-free inbox/doorbell (the inbox is mutex-protected and woken through a condition variable, not an eventfd or `PostQueuedCompletionStatus`).

Evidence: `include/crpc/runtime.hpp:55-69`, `src/runtime/runtime.cpp:108-118,246,387-440,455-463`, `src/server/server.cpp:214-239,546-586,714-738`.

## 9. RPC core <a id="s9"></a>

`rpc::client` owns one `connection`, one `frame_decoder`, a fixed `call_slot_pool`, an in-flight map keyed by `stream_id`, and a deadline multimap. It is the single writer of stream ids and slot states on that connection. It allocates scratch buffers once and reuses them; a call handle never allocates.

`rpc::dispatcher` is the server half. A `service_registry` maps numeric service ids to services and method ids to type-erased handlers. Registration is single-threaded before `freeze()`; after freeze, lookup reads immutable maps without a lock. `dispatch()` decodes the call envelope, rejects an expired deadline before invoking the handler, admits the method's bulkhead, catches any exception at the handler boundary, and encodes a response payload.

`rpc::method_traits` derives request/response types from a member pointer; the typed trampoline decodes with `call_codec<T>` and encodes the response. `call_codec` has no primary definition for an unsupported type, so a missing specialization is a compile error rather than a silent empty payload. The typed registration path is implemented (`service_registry::register_service`/`register_method` and `register_typed_method`), and the RTTI-driven declaration macros (`CRPC_ROUTE`, `CRPC_HTTP_*`, `CRPC_FROM_*`) live in `mvc/attributes.hpp` for the MVC/HTTP side. They are macros over an explicitly named controller and member, not a separate code-generation step. Evidence: `include/crpc/rpc/dispatcher.hpp:167-174,226-289,316-322`; `include/crpc/mvc/attributes.hpp:45-90`.

The server connection loop is implemented in `src/server/server.cpp`. `server::start()` freezes the registry, builds an immutable per-service guard table, creates the shard runtimes and starts every listener; the accept sink wraps each accepted `connection` in a `reader_source` and binds it; `reader_source::on_data` feeds the `frame_decoder` and submits `REQUEST`/`ONE_WAY` frames to the shard's handler pool. `server::impl::handle_request` validates the service against the frozen registry, applies the server admission pipeline described in 搂14, calls `rpc::dispatcher::dispatch`, and queues the `RESPONSE` or error frame on the connection's bounded send path. The client-side pipeline is wired per endpoint. The fallback primitives are implemented: the server advertises `crpc-degradable` at four pre-execution rejection points, but `fallback_execute` is still never called from the server. Evidence: `src/server/server.cpp:317-403,905-1168,1362-1408`; `include/crpc/server.hpp:78-217`.

## 10. Call state machine <a id="s10"></a>

The implemented states are:

```
idle --(acquire)--> queued --(request written)--> sent
                                              /  |  \       \
                                             /   |   \       \
                                     completed timed_out canceled failed
```

Only the owning client thread performs `idle -> queued -> sent`. The terminal transition may be attempted by four racing parties: a response, a timeout, a cancel and a connection loss. `try_claim_terminal` is one compare-exchange on the state word; exactly one winner writes `code`, `error` and `response`. A loser observes a terminal state and must not touch the result fields.

`result_ready` is published with release ordering after the result fields are written; `ready()`/`get()` acquire-load it. A late response for an unknown or terminal stream is dropped and never delivered to a recycled slot. Slots are generation-tagged; `call<T>` carries the slot pointer, generation and pool pointer, and returns the slot on destruction.

Terminal paths: a response calls `decode_response` then `finish(completed, ...)`; `client::poll()` walks the deadline multimap and claims `timed_out` with `deadline_exceeded`, adding `metadata["unknown_side_effect"] = "1"` for a written non-idempotent call; `call<T>::cancel()` claims `canceled` and sends `RST_STREAM(canceled)` if the request was written; `fail_all()` claims `failed` for every in-flight slot on loss, EOF or protocol failure. A one-way call is a `ONE_WAY` frame with `UNRELIABLE`; the client claims `completed` as soon as the frame is queued.

The design's `call_slot` budget is 128 bytes. The current slot contains a mutex, a condition variable, a buffer, an `rpc_error`, callback storage and the state word; a compile-time probe on MSVC x64 gives `sizeof(call_slot) = 440` bytes. The budget is not met. A coroutine-park design can remove the mutex/condition variable and shrink it.

## 11. Codec layer <a id="s11"></a>

`codec.hpp` defines the erased `codec` boundary and the typed `encode_into<T>` / `decode_from<T>` helpers. Typed helpers perform one `id()` check and call the concrete codec's member template directly; there is no `std::function`, per-field virtual dispatch or intermediate document tree.

`crpc-binary` is the default and complete codec. Every field is `varint field_id, varint payload_len, payload`; field ids are the declaration index plus one, base fields first for derived types. Signed integers are zigzag, floats are fixed-width little-endian IEEE-754, containers are packed or length-delimited, maps are repeated key/value entries, optionals carry a presence byte. Missing fields leave the existing value untouched unless the type is registered with `RTTI_DEFINE_TYPE_WITH_DEFAULT`; duplicate ids, id 0, truncation, length overrun and depth over 32 are rejected. Unknown ids are skipped, which makes the format forward-compatible.

The text/schema adapters (`json_codec`, `xml_codec`, `ini_codec`, `toml_codec`, `protobuf_codec`, `flatbuffers_codec`) are conditionally compiled behind `CRPC_HAVE_RTTI_<FORMAT>`. protobuf and FlatBuffers require an explicit user `schema<T>` specialization and report a clear error when it is absent. Without RTTI, the binary codec is a stub that reports encode/decode errors rather than pretending to succeed. `codec_registry` adds, looks up and freezes codecs; the typed path does not need it.

## 12. HTTP and MVC <a id="s12"></a>

HTTP/1.1 is implemented at the message layer: an incremental parser and serializer, a strict chunked decoder/encoder, and a method/target/header model. The parser rejects bare CR inside a line, obs-fold, `Content-Length` plus `Transfer-Encoding`, repeated `Content-Length`, and a transfer-encoding chain that does not end in `chunked`. It accepts CRLF and bare LF for start lines and headers, and strict CRLF for chunk framing. Limits: 8 KiB request line, 64 KiB header block, 100 headers, 8 MiB body, 64 KiB trailers.

`http/multipart.hpp` is an incremental `multipart/form-data` parser with bounded parts, headers and total bytes, filename sanitization, and a rollback tail of `boundary_len + 4` bytes. `http/file_transfer.hpp` adds `upload_sink` (in-memory until 1 MiB, then exclusive-create spill to a random temporary file, RAII deletion) and `download_source` (streaming reads, `Range`/206/416, single range only).

HTTP/2 is implemented at the protocol layer: a 9-byte big-endian frame codec, HPACK with the static table, dynamic table and Huffman coding, and a connection state machine with settings, CONTINUATION rules, stream states, connection and stream windows, parked sends, GOAWAY and RST_STREAM. It does not own a socket, TLS, ALPN or the CRPC call adapter.

MVC is a route table plus binding: `CRPC_ROUTE` and `CRPC_HTTP_*` register entries at static initialization; `router::freeze` compiles patterns, rejects malformed patterns and conflicts, and builds an exact-match table plus a priority-sorted parameter list. Binding fills a reflected DTO from route/query/header/form/file/body sources in a fixed order, with explicit 400 mapping. `router::handle` performs match, Accept negotiation, bind, invoke and response writing.

Implemented: the `http/content_coding.hpp` gzip/deflate primitive is wired into the `http_server` response path (content negotiation, `Content-Encoding`, request-body decoding; `src/http/server.cpp`, `test_http_coding.cpp`), and SSE plus HTTP long polling are wired (`http/sse.hpp`, `longpoll_sweep.hpp`, `src/http/server.cpp`, `test_http_sse.cpp`). Still missing: the CRPC-call-over-HTTP adapter, **specified, not implemented** (`src/http/server.cpp` dispatches HTTP requests through the MVC router, not through `rpc::dispatcher`).

`http/server.hpp` implements the server facade that ties those layers together. `http_server` binds one or more `io::backend_factory` listeners, assigns each accepted connection to a shard `runtime`, and touches all protocol state on that single thread: the transport source only copies bytes into a bounded ingress buffer and posts work, so no per-connection thread is created and a full ingress buffer pauses reading (the R1 credit) instead of dropping. Cleartext connections are sniffed -- the HTTP/2 connection preface selects h2c, anything else is HTTP/1.1. A request is dispatched through the MVC router (HEAD runs the GET handler and drops the body) and the serialized response is queued; a per-connection write queue keeps at most one backend push in flight and backpressures the reader when it grows. HTTP/1.1 handles keep-alive, pipelining, `100-continue`, chunked bodies and the 400/404/405/411/413/431/500 paths, and rejects request smuggling through the message layer. HTTP/2 reuses the frame/HPACK/connection layers unchanged: it honours connection and stream windows, parks a response that exceeds the peer window and resumes it on `WINDOW_UPDATE` (with a periodic PING probe when a peer stays at zero, R18), answers PING/SETTINGS/GOAWAY, and maps errors to RST_STREAM for a stream error and GOAWAY for a connection error. `listen_tls` wraps a listener in `io::tls_stream` and routes on the ALPN result (`h2` -> HTTP/2, `http/1.1` -> HTTP/1.1); the public header stays free of OpenSSL types and `listen_tls` returns a clear error when the library is built without `CRPC_HAVE_OPENSSL`. `stop()` stops accepting, sends GOAWAY on HTTP/2, drains in-flight requests up to `drain_timeout`, then closes; it is idempotent. It also keeps the builder listener configuration so a later `start()` rebinds the same endpoints; the stored `backend_factory` pointers are borrowed, so a backend must outlive the server including every `stop()` -> `start()` cycle (the same contract as the MVC router). Resource bounds are explicit and per server: `max_connections` (excess accepts are closed and counted, never queued), `idle_timeout` (a silent connection is closed) and `request_timeout` (an incomplete request is answered with 408), so neither a socket flood nor a slowloris client can hold memory or a connection indefinitely. The egress queue is bounded in practice by stopping the HTTP/1.1 parse loop and returning unparsed bytes to the bounded ingress buffer once the cap is crossed, and by parking HTTP/2 responses in the per-stream queue whose cap is enforced. DTO binding needs reflection: a build without RTTI serves routes whose handlers take no reflected request type, and document codecs fall back from JSON to another registered format rather than answering 406 for every request.

## 13. Cluster layer <a id="s13"></a>

`cluster::registry` is the single authority for the catalog. An instance is keyed by `(service, node_id)`; every accepted mutation bumps a global monotonic `index`. Readers load an immutable `registry_snapshot` atomically; the writer serializes mutations with one mutex and publishes a new snapshot. A write is accepted only from the owning `node_id` or with the admin token. A stale incarnation is rejected; a tombstone rejects every incarnation that is not strictly greater. Every accepted mutation returns a fence derived from the monotonic index, never from a wall clock.

`cluster::lease_manager` grants TTL leases, renews them, and applies expiry through the registry authority path: critical first, deregistered after a grace period. `cluster::election` provides PreVote, Vote and lease-backed leadership with a fencing epoch; it is explicitly not Raft and provides no log, persistence or membership changes. `cluster::health` provides TTL, TCP-connect and callback checks with consecutive-success/failure thresholds. `cluster::watch_hub` turns registry sink events into snapshot-plus-delta streams ordered by the monotonic index; a gap marks the subscriber for resync, and a slow subscriber is dropped with an observable event.

Missing, all **specified, not implemented**: the network registry service and its HTTP/CRPC methods, node identity/peer pool, discovery, Consul-compatible HTTP endpoints, KV+CAS+ACL storage, gossip/Raft, and multi-node failover. The primitives are present; the service is not.

## 14. Resilience layer <a id="s14"></a>

Five components are implemented independently:

- `circuit_breaker`: sliding-window counters in `alignas(64)` buckets, CAS transitions, half-open probe tokens, monotonic time only.
- `rate_limiter`: token bucket with explicit refill, non-blocking acquire, bounded `retry_after`.
- `bulkhead`: lock-free counting semaphore with an RAII permit.
- `load_balancer`: round-robin, random, least-inflight, consistent hash and smooth weighted round-robin over an immutable `instance_set`.
- `retry_sequence` / `retry_budget`: per-call attempts with exponential
  backoff and jitter, plus a shared token-bucket budget; `unknown_side_effect`
  is retryable only when idempotent.

The design names six logical stages, `rate_limit -> bulkhead -> load_balance -> breaker -> retry -> deadline`. The server admission half is wired; the client wires the same stages per endpoint, but not in that literal order: it has to select an endpoint (load balance) and admit it through the breaker before it can apply that endpoint's rate and bulkhead, so the executed order is `select + allow -> probe check -> rate -> bulkhead -> register -> pool/slot -> retry -> deadline`. The fallback primitives are implemented: the server advertises `crpc-degradable` at four pre-execution rejection points, but `fallback_execute` is still never called from the server; the client facade calls it at the terminal failure exits of `get()`, after `finish_attempt` recorded the breaker outcome. The global rate stage belongs to slice 2c.

On the client, one logical call runs:

1. A selection loop asks the load balancer for a candidate and admits it through that endpoint's breaker. `allow()` hands out the half-open probe and promotes an expired open endpoint, so a rejected candidate makes the loop try the next one instead of failing the call.
2. A half-open probe skips the per-endpoint rate and bulkhead gates. The connection-pool and in-flight caps still apply to probes, and a probe refused by a cap records no endpoint failure.
3. The closed path takes the per-endpoint rate limiter (deadline-aware; a wait that would cross the call deadline answers `deadline_exceeded`), then the per-endpoint bulkhead (full answers `overload`), then the pooled connection and the low-level call.
4. Local gate rejections call `ignore()` and are never retried automatically: the caller owns that quota. Pool saturation, an exhausted slot pool, an open candidate after the selection loop, and connect failures are pre-send failures and may be retried even when the call is not idempotent; sent/remote failures require idempotency or an idempotency key. `retry_after` is a lower bound on the delay (`max(backoff, hint)`), capped by the deadline, and the retry budget is charged only after that deadline check.
5. Every gate is `nullopt` by default (one predictable branch; no object, lock or allocation). `resilience_enabled=false` creates no gates and skips them; the failure-origin classification remains active. Close/cancel finalize the active attempt and release its bulkhead and breaker permits exactly once.
6. `call::cancel()` sets the cancel flag whether or not a low-level call is in flight. A retry wait is interrupted by it: the plain-thread path waits on a condition variable the cancel notifies, and the shard path sleeps in 5 ms slices, so the observed latency is about one slice plus scheduling (bounded by the configured backoff only when a shard sleep cannot be cut). A cancelled call issues no further attempt.

`server::start()` builds a frozen, read-only `service_guard` table after `registry.freeze()` (one entry per registered service: optional rate limiter, optional bulkhead, breaker, lock-free in-flight counter). An unknown service id has no entry and creates no state, so wire-chosen ids cannot grow memory. The handler path takes no guard lock and does not allocate.

Per request:

1. `submit` (connection reader) applies an optional **per-connection** token bucket before the per-connection in-flight cap and before any business allocation. Rejection is `resource_exhausted` with `retry_after` (HTTP 429); only the small forced-reject job is allocated. This is connection-level flood protection, not the per-service rate stage.
2. `handle_request` checks the breaker **first**. Open answers `unavailable` (503) with its `retry_after`. A half-open probe token skips the per-service rate and bulkhead stages, bounded by `half_open_max_probes`, so a saturated rate bucket or bulkhead cannot starve the breaker. The per-service cap is a real capacity bound and still applies to probes: a probe that finds the cap full is refused with `overload`. A probe is also subject to the connection-level bucket, so cross-connection probe supply is not guaranteed.
3. Closed path: per-service rate (optional, deadline-aware; when waiting would cross the call deadline the answer is `deadline_exceeded`, HTTP 504) -> per-service bulkhead (full answers `overload`, 429) -> the existing per-service in-flight cap -> `rpc::dispatcher::dispatch`.
   `mvc::write_error` projects a positive `rpc_error.retry_after` into a `Retry-After` header, rounded up to whole seconds with a minimum of 1, and omits the header when the hint is zero. The HTTP long-poll 503 path is separate: it always sends a fixed `Retry-After: 1` and does not project `rpc_error.retry_after`.
4. Outcome classification: local admission (rate, bulkhead, pool, cap) and application errors call `ignore()`; `internal`, `unavailable` and `not_acknowledged` call `failure()`; `deadline_exceeded` is `slow()` when the handler ran and `ignore()` when the dispatcher refused before execution; `ok` becomes `success` or `slow` by the permit's measured duration.

Both per-service stages are `nullopt` by default (one predictable branch; no object, lock or allocation). A bulkhead needs an explicit `per_service_bulkhead`; a zero capacity derives from `per_service_inflight`. `resilience_enabled=false` skips the new rate/bulkhead stages and keeps the breaker and cap, matching the pre-pipeline behaviour; there is no probe special case in that mode, so the cap applies to probes as well. `stop()` stops accepts, drains, closes connections and joins every shard before releasing the guard table; Debug builds assert that each cap and bulkhead returned to zero. 64-bit counters report `rate_limited_connection`, `rate_limited_service`, `bulkhead_rejected`, `breaker_open`, `deadline_rejected` and `probe_admitted`; `rejected_overload` counts the per-connection cap, the per-service cap and the per-method limit (rate, bulkhead and breaker rejections stay in their own counters).

The client path passes through the same stages per endpoint: load balancing with breaker-aware re-selection, per-endpoint rate/bulkhead gates (probes skip them), retry with a shared budget and an honoured `retry_after`, and a per-endpoint breaker (`src/client/client_facade.cpp`). Evidence for the server pipeline: `src/server/server.cpp:295-380,882-1145,1339-1385`; `include/crpc/server.hpp:78-180`.

### Server-side idempotency dedup

The retry budget above assumes the server never executes a replayed call twice. That is the server-side dedup table (`include/crpc/rpc/dedup.hpp`, `src/rpc/dedup.cpp`), owned by the dispatcher and shared by every shard of one server process. A request whose envelope carries a non-empty `call_id` (the R6 triple) claims the table before the handler runs:

- **fresh key** -> the caller executes (`claims_leader`) and publishes the encoded response at dispatch exit;
- **key still in flight** -> the duplicate is *suspended* (never dropped) inside `claim` until the leader publishes, then resends the leader's exact response (`claims_waiter`/`waiters_woken`); concurrent duplicates therefore execute the handler exactly once;
- **key completed and cached** -> the cached response is replayed byte-for-byte without running the handler (`claims_replay`);
- **key completed but uncached** -> the entry is re-claimed and the handler runs again.

Only successful responses are cached. A failed completion is an uncached tombstone: its concurrent duplicates still receive the leader's error, but a later replay re-executes, which is what keeps the client retry policy meaningful. A replayed `ONE_WAY` duplicate is dropped silently (no response exists to wait for).

Bounds and eviction (all configurable through `dedup_options`): capacity 4096 entries with LRU eviction (the least recently used *completed* entry yields; in-flight entries are pinned; a table saturated with in-flight entries degrades to at-least-once bypass, never a hang and never unbounded memory 鈥?the per-client FIFO ring of the original sketch is unbounded in the number of clients, which is why LRU was chosen); a monotonic (steady-clock) TTL of 5 minutes from the dispatch instant defines the dedup window, never a wall clock; a 16 KiB per-entry cap on cached responses (larger responses are handed to the duplicates suspended at completion but not retained for replays); a 30 s cap (and always the request's own wire deadline) on suspending a duplicate, so a shard thread is never held indefinitely; `max_wait <= 0` disables suspension entirely (in-flight duplicates are refused immediately with a retryable error). An entry whose `client_epoch` belongs to a restarted client can never shadow a new-epoch call 鈥?the epoch is part of the key 鈥?and ages out through the same TTL/LRU bounds.

Waiters are interruptible: the server hands the connection pointer to `dispatch` as the wait key, and connection teardown (`on_terminal`, send failure, connection destruction) calls `dedup_table::abandon`, ending suspended duplicates with an explicit `canceled` error instead of stranding them.

Documented limits: the guarantee holds inside the window `min(TTL, LRU lifetime)` 鈥?a replay after eviction/expiry re-executes, so the client retry budget and the server window must be sized together; the table is per process (multi-shard servers share it through the one dispatcher, so the guarantee is process-wide), and independent server processes/nodes each dedup on their own 鈥?routing a `call_id` to one node is a load-balancer concern, not solved here. Evidence: `tests/test_rpc_dedup.cpp` (13 cases: exactly-once replay, concurrent duplicates, independence, no-key regression, expiry re-execution, oversized non-cached, abandon on disconnect, capacity bound, failure tombstone, one-way duplicate, bypass, two end-to-end server cases).

## 15. Observability <a id="s15"></a>

`obs::trace_context` carries a 16-byte trace id, an 8-byte span id, an 8-byte parent and W3C flags; `parse_traceparent`/`format_traceparent` implement the W3C header, and `baggage` is ordered and bounded. `obs::deadline` is relative (`after(duration)`), as required for cross-machine propagation.

`obs::interceptor_chain` is a fixed table of hooks per hook point. An empty chain costs one predictable branch; hooks are registered at startup and frozen. The server call path is now wired: `server::builder::interceptor_chain` installs a chain that `handle_request` runs at `before_call` (after envelope decode, before admission; a hook returning false, or throwing, ends the request with the chain verdict) and at `after_call` (after dispatch; an early stop replaces the response with the chain verdict), and the interceptor-written metadata reaches the handler's `rpc::call_context` through `rpc::dispatcher::dispatch`'s context-injection overload. On the empty path the only cost is one `count(point)` branch per hook point: no lock, no allocation, no virtual call, no per-call context. The client terminal outcome is observed through the process-wide chain (`obs::set_default_interceptor_chain`) that `diagnostics::on_completion` runs at `after_call` from `response<>::get()`; `before_serialize`/`after_deserialize`/`on_retry`/`on_breaker_event`/`on_connect`/`on_close` and a per-attempt client `before_call` remain **specified, not implemented**. The server facade also has a separate `server_interceptor` hook that is called around `dispatch` (`src/server/server.cpp:1332-1336` for `on_request`, `:1517-1521` and `:1790-1794` for `on_response`), and it is not the observability chain: the two mechanisms coexist in the pinned order documented in `include/crpc/server.hpp` (legacy request observers, then `before_call`, then admission and dispatch, then `after_call`, then the legacy response observers).

**Exception contract.** The HTTP and RPC paths cross two different boundaries, and they do not behave alike. A header-inline boundary is compiled into the consumer and follows the consumer's exception flags; a library translation unit is prebuilt and keeps the library's flags.

- **Header-inline boundary:** `obs::interceptor_chain::run` (`include/crpc/observability/interceptor.hpp:194-231`) and the MVC handler adapter `mvc::invoke_http` (`include/crpc/mvc/router.hpp:385-399`). With C++ exceptions (`__cpp_exceptions` or `_CPPUNWIND` defined) a throw is caught by the layer and converted to `rpc_code::internal`: `run()` stops and returns false, and `invoke_http` writes the HTTP 500 `internal error` response, so nothing escapes into the RPC or HTTP path. Without exception support (`-fno-exceptions`, or MSVC with no `/EH` option or with `/EHc-`, `/EHs-`, `/EHa-`, `/EHs-c-`, `/EHa-c-`) there is no `try`/`catch` at all: a throwing hook or handler leaves the call site, and the outcome is not defined by this library (see the paragraph after the list). In practice a caller that has its own `try`/`catch` may still catch it; otherwise the process calls `std::terminate`.
- **Library translation-unit boundary:** `mvc::router::handle` (`src/mvc/router.cpp:617-659`, declared `noexcept`) and `rpc::dispatcher::dispatch` (`src/rpc/dispatcher.cpp:366-372`). These frames are prebuilt and follow the *library's* flags, not the consumer's. A library built with exceptions surrounds route invocation and the handler call with `try`/`catch (...)`, so a throwing handler is caught inside the library and becomes HTTP 500 `internal error` or `rpc_code::internal`; it does not terminate, and the consumer cannot observe the throw. A library built without exception support has no such frames: the same throw is undefined behaviour and in practice terminates.

This is a limitation of `-fno-exceptions`, not a guarantee of this library: the standard does not define a `throw` compiled without unwind information. In practice it may end in `std::terminate`, or a caller-side `try`/`catch` may still catch it, but stack unwinding and destruction do not run normally 鈥?frames compiled without exception support carry no cleanups, so their local objects (and any lock or permit they own) are not destroyed. A caller-side `try`/`catch` around a no-exception boundary is therefore not a supported recovery mechanism.

The contract therefore requires hook and handler implementations never to throw: callbacks are `noexcept` by policy ([principles](principles.md#s8), "noexcept on callbacks").

`obs::logger` formats into a stack buffer, copies a fixed-size record into a per-thread SPSC ring (8192 records by default), and drains it on one background thread with batched `WriteFile`/`writev`, size/time rotation, redaction and an optional compression hook. Disabled levels cost an atomic load and a compare; above `CRPC_LOG_MAX_LEVEL` the macro expands to nothing.

`obs::tracer` uses deterministic head sampling; with no exporter, starting a span reads no clock, generates no id and allocates nothing. With an exporter, spans go through a bounded queue and a background exporter; overflow is counted. Parent/child is established by the send/receive pair, never by timestamps. Metrics are **specified, not implemented**: there is no counter/histogram registry in the tree.

## 16. Push and streaming <a id="s16"></a>

`push::hub` is the single writer for a push shard. A topic maps to subscribers; a subscriber id is a logical identity that outlives a connection. `publish` copies the payload once and shares it across subscribers, never blocks on a subscriber, and applies the subscription's overflow policy (`drop_oldest`, `drop_newest`, `disconnect_subscriber`) at a queue bound. Reliable messages carry `(publisher_id, epoch, seq)` and stay unacknowledged until `ack`; reconnect replays unacknowledged messages from a bounded replay buffer. Ordered subscriptions use a per-topic monotonic `topic_seq`; a gap or duplicate produces an observable event.

`push::stream_state`, `stream_reader` and `stream_writer` implement client-streaming, server-streaming and bidirectional shapes. A write accepts at most the transport's current credit and returns the accepted count; zero means backpressure, not an error. Reset is idempotent.

Missing: a shared publish path between CRPC and HTTP. The `PUSH`/`PUSH_ACK` frame path in `rpc::client`, HTTP SSE and long polling are implemented.

## 17. Flow control <a id="s17"></a>

`flow::rtt_estimator` keeps one baseline, not a process model. A lower sample moves the baseline down immediately (queueing cannot cause a lower sample); an upward sample moves it up by a bounded fraction (12.2% by default) capped at the observation, and a sustained shift needs confirmations before it is treated as a path change. Samples above the ceiling are ignored as measurement artifacts. `queue_delay_ns(observed)` is the only congestion signal.

`flow::pacer` is a byte token bucket with a 2 ms burst; `congestion_window` grows only on acknowledgements (self-clocked) and shrinks on a queue signal; `rtt_tracker` maps send timestamps to samples and rejects stale or backwards tokens. All clocks are injected monotonic nanosecond functions.

The estimator, pacer and window are not yet wired into `io` or `rpc`. The integration, adaptive backpressure and per-lane quotas are **specified, not implemented**.

## 18. Requirement map <a id="s18"></a>

| Requirement | Where it lives | Status |
|---|---|---|
| Two-interface transport | `io.hpp` (`source`, `sink`, `completion`) | implemented, tested |
| In-memory backend | `io.hpp`, `src/io/inproc.cpp` | implemented, tested |
| epoll ET-only | `io/epoll_backend.hpp`, `src/io/epoll_backend.cpp` | implemented, Linux-tested |
| IOCP | `io/iocp_backend.hpp`, `src/io/iocp_backend.cpp` | implemented, Windows-tested |
| Multiplexed binary frames | `frame.hpp`, `src/frame/frame.cpp` | implemented, tested |
| crpc-binary codec | `codec.hpp`, `src/codec/codec.cpp` | implemented, tested |
| RTTI text/schema codecs | `codec.hpp` adapters | implemented, conditionally compiled |
| Calls and typed stubs | `rpc/call.hpp`, `rpc/client.hpp`, `rpc/dispatcher.hpp`, `client_facade.hpp` | implemented: callback/poll core plus coroutine `client_facade`; no generated stub macros |
| Deadline and cancel | `rpc/client.cpp`, `rpc/call.cpp` | implemented, tested |
| Idempotency key | `rpc/call.hpp` (`call_id`), `rpc/dedup.hpp` (`dedup_table`), call envelope | implemented: bounded server-side dedup table (LRU + monotonic TTL window, in-flight suspension with abandon, response replay); per-process only |
| Ordered lanes | `lane_token` in `rpc/call.hpp`, `rpc/lane_queue.hpp`, `src/server/server.cpp`, `client_facade.hpp` | implemented: one serial queue per `(connection, lane_id)`; client opt-in `client::lane(lane_id)` with per-lane monotonic seq; out-of-order buffering with a bounded gap wait (`CRPC_LANE_GAP_TIMEOUT_MS`, default 5 s) and skip; duplicate rejection with a consumed watermark; queued calls failed explicitly on close/shutdown; per-lane entry released when drained |
| HTTP/1.1 | `http/message.hpp`, `http/server.hpp` | implemented: message layer + server facade |
| HTTP/2 and HPACK | `http2/*`, `http/server.hpp` | implemented: protocol layer + h2c/h2 server |
| HTTPS/TLS | `http/server.hpp`, `io/tls.hpp` | implemented with `CRPC_WITH_OPENSSL`; clear error without it |
| MVC/REST | `mvc/*` | implemented, wired through `http_server` |
| Multipart and files | `http/multipart.hpp`, `http/file_transfer.hpp` | implemented, tested (`test_multipart.cpp`, `test_file_transfer.cpp`); multipart parsing plus upload/download with Range/206 |
| Registry | `cluster/registry.hpp`, `src/cluster/registry.cpp` | implemented, tested |
| Lease/election/health/watch | `cluster/*` | implemented primitives |
| Resilience policies | `resilience/*` | implemented primitives; server admission pipeline and per-endpoint client pipeline wired; fallback primitives implemented and wired into the client facade; the server advertises `crpc-degradable` at four pre-execution rejection points, `fallback_execute` still never called from the server |
| Push hub/streams | `push/*` | implemented; PUSH/PUSH_ACK wired to `rpc::client` and the server push pump, SSE and long polling wired in `http_server` |
| Tracing/logging/AOP | `observability/*` | implemented; server call path wired (`before_call`/`after_call` with short-circuit, exception boundary and zero-cost empty chain); client terminal outcome observed via the diagnostics default chain; serialize/retry/breaker/connect hook points not wired |
| Flow control | `flow/estimator.hpp` | implemented primitives, not wired into the call path |
| Shard/coroutines/MPSC inbox | `runtime.hpp`, `src/runtime/runtime.cpp` | implemented primitive; automatic shard-count policy and per-shard I/O ownership not implemented |

## 19. Status matrix <a id="s19"></a>

| Area | Status | Evidence / gap |
|---|---|---|
| base, wire, frame | implemented | `test_endian.cpp`, `test_wire.cpp`, `test_frame.cpp` |
| inproc / epoll / IOCP | implemented | `test_io_inproc.cpp`, `test_io_epoll.cpp`, `test_io_iocp.cpp` |
| RPC core | implemented | `test_rpc_core.cpp`, `test_server_client.cpp`, `test_push_e2e.cpp`; low-level client is callback/poll based; PUSH/PUSH_ACK frame integration is wired |
| codec | implemented | `test_codec.cpp`; text formats depend on RTTI being present |
| HTTP/1.1, multipart, files | implemented | `test_http_message.cpp`, `test_http_server.cpp`, `test_multipart.cpp`, `test_file_transfer.cpp`; multipart parsing plus upload/download with Range/206 |
| HTTP/2 + HPACK | implemented | `test_http2.cpp`; `test_http_server.cpp` covers h2c over inproc |
| MVC | implemented | `test_mvc.cpp`; `test_http_server.cpp` dispatches through the router over inproc |
| cluster primitives | implemented | `test_cluster.cpp`; no network registry service |
| resilience primitives | implemented | `test_resilience.cpp`, `test_server_client.cpp`; server admission pipeline and per-endpoint client pipeline wired; fallback primitives implemented and wired into the client facade; the server advertises `crpc-degradable` at four pre-execution rejection points, `fallback_execute` still never called from the server |
| observability | partial | `test_observability.cpp`, `test_aop_wiring.cpp`; server call path wired (`before_call`/`after_call`, short-circuit, exception boundary, zero-cost empty chain; metadata injected into the handler context), client terminal outcome observed via the diagnostics default chain, no metrics |
| push | implemented | `test_push.cpp`, `test_push_e2e.cpp`, `test_http_sse.cpp`; PUSH/PUSH_ACK wired to `rpc::client` and the server push pump; SSE and long polling wired in `http_server` |
| flow | partial | `test_flow.cpp`; not wired to transport |
| shard/coroutines | implemented | `test_server_client.cpp`; `src/runtime/runtime.cpp` calls `coro_rt_*`; automatic `clamp(cores-1,1,32)` shard count not implemented |
| compression | implemented | HTTP gzip/deflate streaming codec implemented, tested (`test_content_coding.cpp`) and wired into `http_server` (`test_http_coding.cpp`); CRPC frame compression: none/lz4/gzip/deflate/lz77/zip implemented and tested (`test_frame_compression.cpp`, 61 cases), zstd reserved and fail-closed; a `COMPRESSED` frame is decompressed or rejected, never delivered as plaintext (it used to be delivered verbatim) |
| Redis client | implemented (shared build covers the Redis header exports; under `CRPC_BUILD_SHARED=ON`, `ctest -R 'redis\|abi_docs\|shared_link'` passes) | `test_redis_*.cpp` covers RESP2/3, connection pool, pipeline, transaction, Pub/Sub, Streams, cluster, Sentinel, typed helpers and RTTI binding; `test_redis_header_conflict.cpp` pins `stream_entry` vs `redis_stream_entry`. Static build is the default tested configuration; the shared build is supported with the same API and its Redis header exports are covered. |
| SSE / long polling | implemented | `test_http_sse.cpp`; SSE writer and long-poll routes wired in `src/http/server.cpp` |

## 20. Verification map <a id="s20"></a>

The test tree discovers one executable per `tests/test_<area>.cpp` and registers it with CTest. The suites behind the claims above:

| Suite | What it proves |
|---|---|
| `test_endian.cpp` | explicit little-endian load/store, float bit patterns, byte swaps |
| `test_wire.cpp` | varint/zigzag boundaries, truncation, overlong rejection |
| `test_frame.cpp` | golden bytes, ordered extension, CRC32C, fail-closed rules, envelopes |
| `test_io_inproc.cpp` | credit, close races, bounded queues, faults, completion exactly once |
| `test_io_epoll.cpp` | ET readiness, pause/resume, half-close, 100 connections, 10k messages, sub-ms timer |
| `test_io_iocp.cpp` | AcceptEx, pause/resume, cancel, 500 accepts, 10k messages, timer |
| `test_rpc_core.cpp` | four-way terminal race, slot recycle, round trip, timeout/cancel, dispatch errors |
| `test_codec.cpp` | binary golden bytes, containers, depth, derived fields, text formats |
| `test_http_message.cpp` | incremental parsing, chunking, smuggling defenses, serialize round trip |
| `test_http2.cpp` | framing, HPACK vectors, settings/stream state, flow control, fuzz never-crash |
| `test_mvc.cpp` | route priority/conflict/405/404, binding sources, dispatch, multipart upload |
| `test_cluster.cpp` | registry authority/fence/tombstone, lease expiry, PreVote, health, watch gaps |
| `test_resilience.cpp` | breaker, limiter, bulkhead, retry budget, load balancers |
| `test_observability.cpp` | traceparent, baggage, interceptor chain, logger, tracer |
| `test_aop_wiring.cpp` | AOP chain wired into the server call path: pinned order with the legacy observer, short-circuit (handler skipped, chain verdict on the wire, chain default `canceled`), exception boundary at both hook points (`internal`, connection survives), context flow between hooks and into the handler metadata, response-side-only chain, eight-interceptor registration order, concurrent exact counts, empty-chain and no-chain regressions |
| `test_push.cpp` | fan-out, overflow, replay/dedup, ordered gaps, slow subscribers, streams |
| `test_flow.cpp` | directional RTT baseline, queue signal, pacer, window, stale samples |
| `test_frame_compression.cpp` | frame compression container, none/lz4/gzip/deflate/lz77/zip codecs, fail-closed table, unreliable drop, limits, never-inflate and mutation fuzz |
| `test_redis_*.cpp` | RESP2/3, pool/connection, pipeline, transaction, Pub/Sub, Streams, cluster, Sentinel, typed helpers, RTTI binding and header coexistence |
| `test_distributed_rpc.cpp`, `test_distributed_push.cpp` | discovery/failover, partition replay, registry/watch recovery and multi-hop push |

These are functional tests, not a substitute for the sanitizer matrix. `scripts/test.ps1` and `scripts/test.sh` build and run them; ASan/LSan/UBSan/TSan are the merge gates described in [Build and test](building.md). Fuzzing exists only as in-suite never-crash cases for the HTTP/2 frame and connection paths; standalone fuzz targets are **specified, not implemented**.
