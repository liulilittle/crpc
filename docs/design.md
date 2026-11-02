# CRPC Design (English)
[简体中文](design.zh-CN.md) | **English**

Version 1.0 / 2026-10-19

This document is the consolidated design record. It reconciles two independent design reviews that preceded it, cited in the text as **A** and **B**; neither is shipped with the repository, so a citation such as "A §3.5" records where a decision came from and is not a link. The §-numbered sections of this document are the authoritative statement of the design; [Architecture](architecture.md) records what the code implements today. Where this document and the code disagree, the code and its tests are authoritative. Decision identifiers such as R1, R5 or R6 tag numbered rules of that review record; the section that carries the tag is where the rule is stated in full.

The sketches below describe internal machinery rather than public API. `class epoll_connection` is a real internal class defined in `src/`; `class epoll_listener` and `class shard` are illustrative names this document uses for internal structure and do not exist in the headers at all. A qualified name that appears only here is therefore a design sketch, not a missing public symbol. Every name this document claims as *public* is checked against the headers by `tests/test_docs_lint.cpp`.
## Contents
| # | Section |
|---|---|
| [0](#s0) | Positioning and invariants |
| [1](#s1) | Terminology and authority matrix |
| [2](#s2) | Layering and directory structure |
| [3](#s3) | Transport backend contract |
| [4](#s4) | Thread and coroutine model |
| [5](#s5) | epoll ET-only design |
| [6](#s6) | IOCP design |
| [7](#s7) | Wire protocol |
| [8](#s8) | Message semantics |
| [9](#s9) | Error model |
| [10](#s10) | Serialization |
| [11](#s11) | Service API and stubs |
| [12](#s12) | HTTP/1.1, HTTP/2, HTTPS |
| [13](#s13) | MVC and RESTful |
| [14](#s14) | Compression |
| [15](#s15) | Resilience and governance |
| [16](#s16) | Observability |
| [17](#s17) | Memory and performance budgets |
| [18](#s18) | Concurrency correctness |
| [19](#s19) | Test strategy |
| [20](#s20) | Build and CI |
| [21](#s21) | v1 scope, v1.1, milestones |
| [22](#s22) | Open issues and risks |
## 0. Positioning and invariants <a id="s0"></a>
CRPC is a C++17 RPC framework with two first-class capabilities carried by one connection, one frame format, and one shard/coroutine model:

1. **Calls**: request/response, unary and streaming, synchronous-looking inside a coroutine, asynchronous through a `call<t>` handle.
2. **Push**: server-to-client messages through `push_hub`, subscriptions, SSE, and long polling.

The framework is not an HTTP wrapper. TCP carries a length-prefixed multiplexed frame stream; `crpc-binary` is the default codec. HTTP/1.1, HTTP/2, and HTTPS are additional adapters over the same routing table, `call_context`, resilience pipeline, and handler pool.
### 0.1 Three non-negotiable bottom lines
| Line | Mandatory mechanism | Verification | Failure mode |
|---|---|---|---|
| **Never deadlock** | Documented lock order (§18.1); no I/O, user callback, coroutine suspension, `sem_post`, log sink, or blocking allocation while holding a lock; cross-shard traffic only through MPSC plus doorbell; one reader and one writer per connection; in-coroutine waits only through `coro_rt_sem_wait`/`coro_rt_cond_wait`; self-dependent lane wait returns `protocol_error` instead of parking. | Debug lock-order assertion (thread-local held-lock stack aborts on inversion); TSan on Linux; 10-minute multi-shard cross-call stress with random cancel/close. | A shard stops forever; every connection on it times out. |
| **Never crash** | Validate every external input (frame length, field length, element count, enum value, UTF-8, header, path, body); no UB (byte-wise wire access, unsigned arithmetic with range checks, generation-checked handles, atomics for shared state); no exception crosses a coroutine or ABI boundary; `new` is `nothrow` or caught as `resource_exhausted`; recursion bounded. | ASan/UBSan; fuzzers for frame, HTTP, JSON/XML/TOML/INI, multipart, router; malformed-input fault injection; `_GLIBCXX_ASSERTIONS`. | Process abort; all connections drop; cascading failure. |
| **Never leak** | One-to-one pairing for every acquire/release, subscribe/unsubscribe, register/deregister, arm/cancel, wait/post, open/close; idempotent destroy; no delivery after close, in-flight completion exactly once as canceled (R9); late completions dropped by generation; no `shared_ptr` cycles; every slot/stream/lane/connection has a cap and a recycle point; timers removed when a call terminates. | ASan/LSan zero reports; `drain()` asserts `pending_calls == 0 && open_connections == 0 && armed_timers == 0 && temp_files == 0`; million-call long run with flat RSS; 1000 start/stop cycles. | Slow memory/fd growth; OOM or slot exhaustion after hours. |
### 0.2 Principles P1-P13
| # | Principle | Enforcement | Verification |
|---|---|---|---|
| P1 | Correctness before performance | Optimizations may not change observable semantics; batch/gather/pool paths run the same conformance suite. | Same assertions in Debug and Release, fake/inproc/TCP-loopback. |
| P2 | Explicit over implicit | Explicit LE wire encoding; fixed-width integers; no struct `memcpy`, bitfields, or unaligned `reinterpret_cast`; RAII ownership; `noexcept`/move/copy stated per API. | `static_assert` set (§7.7); UBSan alignment; review checklist. |
| P3 | Verifiable | Every guarantee maps to a named test (§19.2); every number is reproducible with toolchain and environment. | CI requires the test to exist and pass. |
| P4 | Convergent | CAS-only state transitions; monotonic `index`/`seq`; FIFO dedup; tombstones; order-independent terminal state. | Reorder/duplicate/delay injection; two-order watch tests. |
| P5 | Single-way authority | Exactly one writer per state variable (§1.2); non-authority writes rejected. | `test_single_writer`; TSan. |
| P6 | End-to-end correctness | Idempotency at the endpoint; transport ACK is only an optimization. | Retry three times, handler executes once. |
| P7 | Failure visible | Every drop has an `rpc_code`, a metric, and a log with trace id; no empty catch. | Drop accounting tests; static scan. |
| P8 | Portable | No host-endian, 64-bit, `long`-width, or signed-`char` assumptions; no compiler extensions without fallback. | x86+x64 MSVC; GCC/Clang; BE byte-stream unit tests. |
| P9 | Deterministic resources | Bounded pools and queues; idempotent close; generation checks. | LSan; active-count assertions; long run. |
| P10 | Simple | Smallest interface that is correct; a new abstraction must prove necessity. | Design review; two virtual functions in `io`. |
| P11 | Fail-closed on unknown input | Unknown frame type, version, capability bit, enum, wire type, content type: reject. | Fuzz + handcrafted illegal frames. |
| P12 | No wall-clock ordering | Deadlines are relative; ordering uses `stream_id`, `lane_seq`, registry `index`; breakers use monotonic time. | Clock-rollback test; static check for `system_clock::now()` in ordering paths. |
| P13 | Platform phases | Phase 1: x86/x64 Windows/Linux. Phase 2: ARM64/BE/Android/macOS. Protocol is platform-neutral from day one. | Phase-1 matrix; phase-2 reruns the same tests. |
## 1. Terminology and authority matrix <a id="s1"></a>
### 1.1 Terms
| Term | Definition |
|---|---|
| **connection** | A concrete transport object owning a socket or an in-process queue pair. It exposes `source`, `sink`, `close()`, `is_open()`, `remote()`, and `resume_read()`. |
| **channel** | A logical, independently flow-controlled byte stream. v1 has one channel per connection; HTTP/2 has one channel per h2 connection. `channel` is not a public type; the former channel abstraction was deleted (A §3.5). |
| **shard** | One OS thread plus one corocc LOOP runtime, one epoll/IOCP, one MPSC inbox, and one timer wheel. The unit of affinity and single-threaded ownership (R2). |
| **lane** | A logical serial domain identified by `lane_id`, scoped to `(client_id, target_service_id)`. Carries `lane_seq` and optional ordered execution (A §20). |
| **call_slot** | A pre-allocated, generation-tagged record for one in-flight call: state, refs, semaphore, result, stream id, response buffer, `then` storage. |
| **stream** | One RPC or streaming instance inside a connection, identified by the connection-local `stream_id` (31 bits, never reused). |
| **subscription** | A bounded record binding an `identity` and topic to a push queue, version, expiry, and optional lane. |
| **instance** | A registered service endpoint: `(service, instance_id, endpoint, weight, health, incarnation, index)`. |
| **node** | A process identity (`node_id` UUIDv4) hosting shards, clients, and optional registry authority. |
| **watch index** | The registry's monotonic revision. Snapshot and watch events both carry it; the only ordering anchor for registry state (A §28.9). |
### 1.2 Single-writer authority matrix
| State | Sole writer | Readers | Forbidden |
|---|---|---|---|
| Connection state (`open/closing/closed`) | Owning shard thread | Other shards via atomic load | Cross-shard write |
| `call_slot.state` | First successful CAS (shard, timer, canceller) | Any thread reads | Plain assignment |
| `stream_id` allocation | Per-connection shard writer, monotonic | None | Multiple writers, reuse |
| `lane_seq` | Lane owner shard at MPSC enqueue (`fetch_add`) | Server validates | Two allocation sites |
| Registry catalog | Authority shard | All shards read immutable snapshot | Direct multi-node writes |
| Breaker state | CAS winner per key | All callers | Non-CAS mutation |
| Health state | Health checker on authority shard | Clients read snapshot | Client-local health writes |
| Configuration snapshot | Reload thread (`atomic_store`) | Shards `atomic_load` | In-place mutation |
| Idempotency dedup table | Shard owning the handler | None | Shared multi-shard writes |
| Subscription table | Shard owning the identity mapping | Publish path through MPSC | Direct cross-shard mutation |
## 2. Layering and directory structure <a id="s2"></a>
### 2.1 Bounded contexts
| Context | Owns | May depend on |
|---|---|---|
| `core` | `span`, `buffer`, `rpc_code`, `rpc_error`, `rpc_result`, endpoint, config primitives | Nothing |
| `io` | `source`, `sink`, `completion`, `io_result`, `connection`, `backend_factory` | `core` |
| `transport` | epoll, IOCP, inproc, listener, timers, MPSC | `core`, `io` |
| `wire` | frame codec, varint, handshake, flow control, frame compression | `core` |
| `codec` | `codec_registry`, crpc-binary, RTTI format adapters | `core`, `wire` (frame metadata only) |
| `rpc` | `call<t>`, `call_context`, client, server, method registry, stubs, push hub | `core`, `io`, `wire`, `codec` |
| `http` | HTTP/1.1, HTTP/2, HPACK, TLS, router, binding, multipart, SSE, problem+json | `core`, `codec`, `rpc` (context only) |
| `cluster` | node, peer pool, registry, discovery, load balancing | `core`, `rpc` |
| `resilience` | pipeline, breaker, rate limit, bulkhead, retry, health | `core`, `rpc` |
| `observe` | AOP chain, trace, log, metrics | `core`, `rpc` |

Rules: `core` depends on nothing; `io` only on `core`; no context may include a header from a context that is not listed; `transport` never includes `rpc`; `http` never includes `cluster`; `observe` receives data through interfaces defined in `core`.
### 2.2 Directory layout
```
include/crpc/
  core/types.hpp            span, endpoint, duration, deadline, ids
  core/error.hpp            rpc_code, rpc_error, rpc_result
  core/buffer.hpp           buffer, small_vector
  core/config.hpp           config loading and validation
  io.hpp                    io::io_result, completion, source, sink
  backend.hpp               connection, listener, backend_factory, options
  wire/frame.hpp            frame_header, frame_type, flags, codec
  wire/varint.hpp           put_varint, get_varint, zigzag
  wire/handshake.hpp        client_hello, server_hello, settings TLV
  wire/flow.hpp             window accounting
  rpc/call.hpp              call<t>, call_slot, when_all
  rpc/call_context.hpp      call_context, call_options, metadata
  rpc/client.hpp            client, connection pool, retry driver
  rpc/server.hpp            server, shard, handler pool
  rpc/method.hpp            method registry, method_traits, invoke
  rpc/push.hpp              push_hub, push_channel, subscription
  http/http1.hpp            HTTP/1.1 parser and serializer
  http/http2.hpp            h2 frame layer, stream state machine
  http/hpack.hpp            static/dynamic table, Huffman
  http/tls.hpp              OpenSSL context, SNI, ALPN, session cache
  http/router.hpp           route table, matcher
  http/binding.hpp          DTO binding from route/query/header/form/body
  http/multipart.hpp        streaming multipart parser
  http/sse.hpp              event stream writer
  codec/registry.hpp        codec enable/canonical mapping
  codec/binary.hpp          crpc-binary encode/decode
  codec/rtti_bridge.hpp     JSON/XML/INI/TOML/protobuf/FlatBuffers adapters
  cluster/node.hpp          node identity and lifecycle
  cluster/registry.hpp      catalog, KV, watch, ACL
  cluster/balance.hpp       instance selector, SWRR
  resilience/pipeline.hpp   fixed six-stage pipeline
  observe/trace.hpp         trace_context, traceparent
  observe/log.hpp           SPSC logger
  observe/metrics.hpp       counters and histograms
src/
  transport/epoll_backend.cpp
  transport/iocp_backend.cpp
  transport/inproc_backend.cpp
  transport/timer_wheel.cpp
  transport/mpsc.cpp
  wire/frame.cpp
  wire/handshake.cpp
  rpc/server.cpp
  rpc/client.cpp
  rpc/push.cpp
  http/http1.cpp
  http/http2.cpp
  http/hpack.cpp
  http/tls.cpp
  http/router.cpp
  http/binding.cpp
  http/multipart.cpp
  codec/binary.cpp
  codec/rtti_bridge.cpp
  cluster/registry.cpp
  cluster/discovery.cpp
  resilience/pipeline.cpp
  observe/log.cpp
tests/
  unit/ integration/ conformance/ fuzz/ golden/ long_run/
scripts/
  build.ps1 build.sh test.ps1 test.sh ci.ps1 ci.sh
bench/
  bench_rpc.cpp bench_http.cpp bench_codec.cpp
```
## 3. Transport backend contract <a id="s3"></a>
### 3.1 Exact interfaces (R1)
```cpp
// include/crpc/io.hpp
#pragma once
#include <cstddef>
#include <system_error>
#include "crpc/core/types.hpp"

namespace crpc {
template <class t> class span;
using byte_span = span<const std::byte>;
class buffer;
struct service_id { std::uint32_t value = 0; };
template <class t> inline constexpr service_id service_id_of{0};
}

namespace crpc::io {
struct io_result {
    std::error_code ec;
    std::size_t bytes = 0;
    [[nodiscard]] bool ok() const noexcept { return !ec; }
};
class completion {
public:
    completion() = default;
    virtual ~completion() = default;
    completion(const completion&) = delete;
    completion& operator=(const completion&) = delete;
    virtual void complete(io_result r) noexcept = 0;   // exactly once, any thread, non-blocking, no throw
};
class source {
public:
    virtual ~source() = default;
    // Backend -> framework. Returns consumed bytes. 0 = pause reading.
    // The framework copies or parses every consumed byte before returning.
    virtual std::size_t on_data(span<const std::byte> data) noexcept = 0;
};
class sink {
public:
    virtual ~sink() = default;
    // Framework -> backend. The backend writes all bytes, then calls complete() exactly once.
    // The span stays valid until complete() returns.
    virtual void push(span<const std::byte> data, completion* c) noexcept = 0;
};
} // namespace crpc::io
```
`source::on_data` returns a credit, not a boolean: `n == data.size()` consumes everything, `0 < n < data.size()` consumes a prefix and pauses, `0` consumes nothing and pauses. This is the whole backpressure mechanism; there is no source completion object (R1).
### 3.2 Connection and factory
```cpp
// include/crpc/backend.hpp
namespace crpc {
struct endpoint {
    std::string host;                 // "127.0.0.1", "::1", or a unix path when scheme == uds
    std::uint16_t port = 0;
    enum class scheme : std::uint8_t { tcp, uds, inproc } kind = scheme::tcp;
};
struct connect_options {
    std::chrono::nanoseconds connect_timeout{std::chrono::seconds(3)};
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
    bool tcp_no_delay = true;
};
struct listen_options {
    std::size_t backlog = 1024;
    bool reuse_port = true;
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
};
class accept_sink {
public:
    virtual ~accept_sink() = default;
    virtual void on_accept(std::unique_ptr<class connection> c) noexcept = 0;
};
class connection {
public:
    virtual ~connection() = default;
    virtual io::source& source() noexcept = 0;
    virtual io::sink& sink() noexcept = 0;
    virtual void resume_read() noexcept = 0;   // required by R1 pause semantics; not part of source
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual endpoint remote() const = 0;
};
class listener {
public:
    virtual ~listener() = default;
    virtual void start(accept_sink* sink) = 0;
    virtual void stop() noexcept = 0;
};
class backend_factory {
public:
    virtual ~backend_factory() = default;
    virtual std::unique_ptr<connection> connect(const endpoint& ep, const connect_options& opt) = 0;
    virtual std::unique_ptr<listener> listen(const endpoint& ep, const listen_options& opt) = 0;
};
} // namespace crpc
```
Establishment, close, cancel, and options live on `connection`/`backend_factory`, never on `source`/`sink`. The core RPC layer accepts only `backend_factory&`; there is no `if (is_tcp)`.
### 3.3 Contract invariants
| # | Invariant | Verification |
|---|---|---|
| I1 | Every `completion::complete` runs exactly once, for success, failure, or cancellation. | Fake backend counts calls; 1000 random close races. |
| I2 | Completion may run on any thread. | Thread-id assertion allows submitter or another thread. |
| I3 | Completion must not block and must not throw. | `noexcept`; debug assertion on held locks. |
| I4 | After `close()`, no new source delivery and no new sink push accepted; in-flight sink completions still run exactly once with `operation_canceled` (R9). | Close-race test. |
| I5 | At most one sink `push` is in flight per direction. | Backend asserts `in_flight_sink <= 1`. |
| I6 | Partial writes are completed by the backend; the framework never re-submits a tail. | Backend test with a short-write socket. |
| I7 | Source span is valid until `on_data` returns; sink span is valid until `complete` returns. | ASan with poisoned buffers. |
| I8 | One reader and one writer per connection at any instant. | Debug atomic owner assertion. |
| I9 | `on_data` return value never exceeds `data.size()`. | Debug assertion; conformance test. |
| I10 | After `on_data` returns 0 or a short count, the backend retains the unconsumed tail and delivers it only after `resume_read()`. | Backend test with a 1-byte credit. |
### 3.4 Partial reads and the resume path
When `on_data` consumes a prefix, the backend must keep the remaining bytes in a stable buffer and must not overwrite them. It must not spin calling `on_data` with the same tail while the framework returns 0. `resume_read()` is the only resume signal. It is idempotent and callable from the shard thread only.
```cpp
void epoll_connection::resume_read() noexcept {
    if (!read_paused_) return;
    read_paused_ = false;
    post_local([this] { read_loop(); });   // self-post: ET gives no new edge for a retained tail
}
```
For IOCP, `resume_read()` re-posts `WSARecv` only when the retained tail is empty; otherwise it delivers the tail first, then posts.

Error propagation: `source` has no error return. A read error closes the connection; the framework observes close/EOF and fails every in-flight call with the mapped `rpc_code`. A `sink` error is delivered through `completion::complete({ec, bytes})`, where `bytes` is the count written before the error, and it also moves the connection to closing. No error is ever reported by dropping bytes silently.
### 3.5 Backpressure levels
| Level | Mechanism | Over-limit behavior |
|---|---|---|
| Kernel receive | `on_data` returns 0, backend stops reading | TCP window closes |
| Connection send ring | Bounded ring (default 256 KiB) | `sink::push` is not called until space exists; caller coroutine parks |
| Per-connection in-flight calls | Default 1024 | Immediate `rpc_code::overload` |
| Per-shard in-flight calls | Default 65536 | Immediate `overload` |
| Global in-flight calls | Default 1,000,000 | Immediate `overload` |
| MPSC inbox | Default 65536 entries | Immediate `overload` |
| Lane queue | Default 1024 entries | `overload` or park, per `lane_full_policy` |

Default policy is fast failure, not queuing. Queuing is opt-in and counts against the deadline.
### 3.6 inproc backend equivalence
```cpp
namespace crpc {
struct inproc_faults {
    std::uint32_t connect_reject_pct = 0;
    std::chrono::nanoseconds connect_delay{0};
    std::uint32_t drop_pct = 0;
    std::uint32_t reset_pct = 0;
    bool half_open = false;
    std::uint32_t reorder_pct = 0;
    std::uint32_t duplicate_pct = 0;
    std::uint32_t seed = 0;
};
struct inproc_options {
    std::size_t max_queued_bytes = 64 * 1024;
    bool serialize = true;
    inproc_faults faults;
};
class inproc_pair {
public:
    static std::pair<std::unique_ptr<connection>, std::unique_ptr<connection>>
    make(const inproc_options& opt);
};
} // namespace crpc
```
Requirements: bounded queue per direction; delivery posted to the target shard, never recursively on the caller stack; `serialize = true` by default so half frames, coalescing, EOF, and error propagation match TCP; every fault injection is reproducible from `seed`; `drop_pct` applies only to frames marked `UNRELIABLE`, never to reliable calls.
## 4. Thread and coroutine model <a id="s4"></a>
### 4.1 Shard layout
```
n_shards = clamp(physical_cores - 1, 1, 32)
per shard: 1 OS thread
         + 1 coro_rt_t created in LOOP mode (R2)
         + 1 epoll set or IOCP
         + 1 MPSC inbox
         + 1 timer wheel
         + 1 doorbell (eventfd / PostQueuedCompletionStatus)
         + 64 handler coroutines with 64 KiB stacks
threads = n_shards + 1 acceptor (Windows) + blocking pool (default 0)
```
Coroutines are M:N over threads, but a shard's runtime is pumped by exactly one thread. `coro_rt_pump` refuses a second concurrent pump (`pump_active`, `[READ:third_party/corocc/src\scheduler\rt_driver.c:458]`), and a LOOP runtime has exactly one worker (`[READ:third_party/corocc/src\scheduler\rt.c:804]`). Handler coroutines are long-lived workers, not one per request: this avoids the 256 KiB default stack (`[READ:third_party/corocc/include\corocc\coro.h:38]`) and the 256-entry default-size stack cache (`[READ:third_party/corocc/src\scheduler\rt.h:167]`).
### 4.2 Runtime construction
```cpp
struct shard_driver_ops {
    static std::uint64_t now(void* ctx) noexcept {
        return static_cast<shard*>(ctx)->monotonic_ns();
    }
    static void wake(void* ctx) noexcept {
        static_cast<shard*>(ctx)->wake_loop();
    }
    static int timer_arm(void* ctx, void* req, std::uint64_t deadline,
                         coro_driver_timer_fn fn, void* arg) noexcept {
        return static_cast<shard*>(ctx)->timers().arm(req, deadline, fn, arg) ? 0 : -1;
    }
    static int timer_cancel(void* ctx, void* req) noexcept {
        return static_cast<shard*>(ctx)->timers().cancel(req) ? 0 : -1;
    }
};
void shard::start() {
    coro_rt_driver_ops ops{};
    ops.struct_size = sizeof(ops);
    ops.flags = CORO_DRIVER_FLAG_LOOP;
    ops.now = &shard_driver_ops::now;
    ops.wake = &shard_driver_ops::wake;
    ops.timer_arm = &shard_driver_ops::timer_arm;
    ops.timer_cancel = &shard_driver_ops::timer_cancel;
    coro_rt_driver_register("crpc-shard", &ops, this, &driver_);
    coro_rt_driver_set_default(driver_);
    rt_ = coro_rt_create(1);
    for (std::uint32_t i = 0; i < handler_workers; ++i) {
        coro_attr_t attr = coro_attr_default();
        attr.stack_size = 64 * 1024;
        coro_rt_spawn(rt_, &attr, &shard::handler_main, this);
    }
}
```
Evidence for the API: `coro_rt_create` `[READ:third_party/corocc/include\corocc\coro_rt.h:299]`; LOOP flag `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:46]`; driver registration `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:146]`; default driver `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:185]`; pump `[READ:third_party/corocc/include\corocc\coro_rt_driver.h:258]`; `coro_attr_default` `[READ:third_party/corocc/include\corocc\coro.h:149]`; `coro_rt_spawn` `[READ:third_party/corocc/include\corocc\coro_rt.h:339]`; semaphore `[READ:third_party/corocc/include\corocc\coro_rt.h:631]`, `:639`, `:654`; destroy refuses live coroutines `[READ:third_party/corocc/include\corocc\coro_rt.h:304]`.
### 4.3 Loop body
```cpp
void shard::run() {
    while (!stopping_) {
        int n = poll_io(io_events_, 256, next_timeout_ns());
        for (int i = 0; i < n; ++i) dispatch_io(io_events_[i]);
        timers_.run_expired(monotonic_ns());
        drain_inbox();
        coro_rt_pump(rt_, 256);
        if (stopping_ && coro_rt_pending(rt_) == 0) break;
    }
}
```
`next_timeout_ns()` is the earliest of: timer-wheel deadline, keepalive scan, registry lease scan, and 1 second. On Linux the exact deadline is carried by a timerfd registered in the same epoll set; the `epoll_wait` timeout is infinite when a timerfd is armed. On Windows the exact deadline is carried by a high-resolution waitable timer that posts a completion to the IOCP (R3).
### 4.4 Handler pool
The reader coroutine only reads, frames, and dispatches complete messages. It never calls a user handler inline. A complete request is pushed to the shard's ready queue; an idle handler coroutine pops it and calls the user function. A suspended handler consumes one handler coroutine, not the reader and not the thread.
```cpp
void shard::handler_main(void* self) {
    auto& s = *static_cast<shard*>(self);
    for (;;) {
        handler_task task{};
        if (!s.ready_.try_pop(task)) {
            coro_rt_sem_wait(s.rt_, &s.ready_sem_);
            continue;
        }
        s.invoke(task);                       // user handler; may suspend
        s.finish(task);
    }
}
```
Handler concurrency is bounded by `handler_workers` (64 per shard, configurable). A CPU-bound handler that never suspends occupies one worker; this is documented, and the v1.1 `handler_executor` is the escape hatch.
### 4.5 Cross-thread wake path
The only cross-thread path is: producer pushes into the target shard's MPSC inbox, sets `wake_pending`, then rings the doorbell once. The target shard drains the inbox in its own loop and pumps its own runtime. No thread calls `coro_rt_spawn` or `coro_rt_pump` on another shard's runtime.
```cpp
bool shard::post(task t) noexcept {
    if (!inbox_.try_push(std::move(t))) return false;
    bool expected = false;
    if (wake_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        wake_loop();                          // eventfd write or PostQueuedCompletionStatus
    }
    return true;
}
```
`coro_rt_sem_post` is safe from any thread and signals activity through the driver wake callback (`[READ:third_party/corocc/src\scheduler\rt_sync.c:469]`, `[READ:third_party/corocc/src\scheduler\rt.c:208]`). A completion on shard A for a call owned by shard B must not touch B's runtime; it posts to B.
### 4.6 Shutdown order
1. `listener.stop()`: no new accepts.
2. Broadcast `GOAWAY`; stop admitting new calls (`shutting_down`).
3. Drain in-flight calls for `grace_period` (default 5 s).
4. Cancel remaining calls with `RST_STREAM`; in-flight completions get `operation_canceled` exactly once.
5. Close all connections: cancel I/O, wait for both in-flight operations, then close the handle.
6. Pump until `coro_rt_pending(rt) == 0`, with a hard deadline; then cancel scopes and pump again.
7. `coro_rt_destroy(rt)`; `CORO_EBUSY` means a coroutine is still alive, so repeat step 6.
8. Unregister the driver after the runtime is destroyed; unregister returns `CORO_EBUSY` while attached (`[READ:third_party/corocc/include\corocc\coro_rt_driver.h:151]`).
9. Release epoll/IOCP, timerfd, eventfd, and the MPSC storage.
### 4.7 Timer precision
```cpp
class timer_wheel {
public:
    using callback = void (*)(void* arg) noexcept;
    bool arm(void* key, std::uint64_t deadline_ns, callback fn, void* arg) noexcept;
    bool cancel(void* key) noexcept;
    [[nodiscard]] std::uint64_t next_deadline_ns() const noexcept;   // UINT64_MAX if empty
    void run_expired(std::uint64_t now_ns) noexcept;
};
```
Linux: `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)`, `timerfd_settime` with `TFD_TIMER_ABSTIME`, added to the epoll set. `epoll_pwait2` is an accepted alternative when available. Windows: `CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)`, `SetWaitableTimerEx` with an absolute `LARGE_INTEGER` in 100 ns units, and `RegisterWaitForSingleObject` whose callback posts a completion to the shard IOCP. The runtime's own 0.5 ms polling example must not be copied: LOOP mode does not expose its nearest deadline, which is why CRPC owns the wheel (A §8.1, D16).
## 5. epoll ET-only design <a id="s5"></a>
### 5.1 Registration
```cpp
epoll_event ev{};
ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
ev.data.u64 = make_token(slot_index, generation);
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
// listener
epoll_event lev{};
lev.events = EPOLLIN | EPOLLET;
lev.data.u64 = k_listener_token;
epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev);
```
Tokens are `u64`: `token = static_cast<std::uint64_t>(slot_index) * k_slot_stride + generation`. Never shift a 32-bit index by 32; that is UB on 32-bit targets (A §4.1). `EPOLLERR` and `EPOLLHUP` need no registration and are always reported.
### 5.2 Event dispatch order
For each event: validate the token and generation; handle `EPOLLERR`/`EPOLLHUP` first; mark `EPOLLRDHUP`; process `EPOLLIN`; process `EPOLLOUT`; if half-closed and read/write are both done, close gracefully. The order is fixed so an `IN` handler cannot consume an `OUT` edge without servicing it (E4).
```cpp
for (int i = 0; i < n; ++i) {
    const std::uint64_t tok = events[i].data.u64;
    connection* c = lookup_alive(tok);
    if (c == nullptr) continue;
    const std::uint32_t e = events[i].events;
    if (e & (EPOLLERR | EPOLLHUP)) { c->fail(std::errc::connection_aborted); continue; }
    if (e & EPOLLRDHUP) c->peer_half_closed_ = true;
    if (e & EPOLLIN) c->read_loop();
    if (e & EPOLLOUT) c->write_loop();
    if (c->peer_half_closed_ && c->read_done() && c->write_done()) c->close_graceful();
}
```
### 5.3 Read loop (R1 credit)
```cpp
void epoll_connection::read_loop() noexcept {
    if (read_paused_) return;
    for (;;) {
        const ssize_t k = ::recv(fd_, rx_, rx_cap_, 0);
        if (k > 0) {
            const std::size_t used = source().on_data({rx_, static_cast<std::size_t>(k)});
            if (used < static_cast<std::size_t>(k)) {
                const std::size_t tail = static_cast<std::size_t>(k) - used;
                std::memmove(rx_, rx_ + used, tail);
                rx_tail_ = tail;
                read_paused_ = true;
                return;                        // wait for resume_read()
            }
            continue;                          // consumed all; ET permits another recv
        }
        if (k == 0) { peer_eof_ = true; return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        fail(errno);
        return;
    }
}
```
`resume_read()` must first deliver `rx_tail_` to `source().on_data`; only when the tail is fully consumed may it call `recv` again. Returning to `epoll_wait` with a retained tail is safe only because `resume_read()` self-posts; ET will not report the retained bytes again.
### 5.4 Write loop (R4)
```cpp
void epoll_connection::write_loop() noexcept {
    std::size_t budget = k_write_budget_bytes;      // 256 KiB per turn
    while (!send_ring_.empty()) {
        auto slice = send_ring_.front_slice();
        if (budget == 0) {
            want_write_ = true;
            post_local([this] { write_loop(); });   // R4: self-post, do not rely on EPOLLOUT
            return;
        }
        const std::size_t n = std::min(budget, slice.size());
        const ssize_t k = ::send(fd_, slice.data(), n, MSG_NOSIGNAL);
        if (k > 0) {
            send_ring_.consume(static_cast<std::size_t>(k));
            budget -= static_cast<std::size_t>(k);
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            want_write_ = true;
            return;                                  // resume only on the next EPOLLOUT edge
        }
        if (k < 0 && errno == EINTR) continue;
        fail(errno);
        return;
    }
    want_write_ = false;
    complete_ready_outputs();                        // one completion per batch
}
```
`sink::push` enqueues into the send ring and then calls `write_loop()` optimistically if `want_write_` is false. This closes E3: an edge consumed while the ring was empty cannot strand later data.
### 5.5 Error and half-close handling
| Event | Meaning | Action |
|---|---|---|
| `EPOLLERR` | Socket error (RST and similar) | Read `SO_ERROR`, `fail()`, no read/write retry |
| `EPOLLHUP` | Both directions hung up | `fail()`; all in-flight completions get `connection_aborted` |
| `EPOLLRDHUP` | Peer shut down its write side | Set `peer_half_closed_`; drain to `recv == 0`; still flush queued writes |
| `EPOLLIN | EPOLLRDHUP` | Data plus FIN | Read first; evaluate EOF after the read loop |
| `recv == 0` | EOF | No more source delivery; finish writes; close |
### 5.6 Accept
```cpp
void epoll_listener::on_acceptable() noexcept {
    for (;;) {
        const int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) { assign_to_shard(fd); continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == ECONNABORTED) continue;
        if (errno == EMFILE || errno == ENFILE) { pause_accept_backoff(); return; }
        log_error(errno);
        return;
    }
}
```
Linux uses one listener per shard with `SO_REUSEPORT` (and `SO_REUSEADDR`), so the kernel hashes connections to shards without a cross-thread handoff. Shard-count changes require listener rebuild; rolling restart with `SO_REUSEPORT` smooths this.
### 5.7 ET lost-event checklist
| # | Scenario | Prevention |
|---|---|---|
| E1 | Read once and return with data left | Loop until `EAGAIN`; with R1 credit, deliver tail on `resume_read` |
| E2 | Resume without re-reading | `resume_read` self-posts the read loop |
| E3 | Write edge consumed while ring empty | Optimistic `write_loop` after enqueue |
| E4 | IN handling clears the ring before OUT | Fixed dispatch order; OUT always serviced |
| E5 | Accept only one connection | Accept loop to `EAGAIN` |
| E6 | fd reuse hits a stale event | Token with generation; `EPOLL_CTL_DEL` before `close` |
| E7 | `EPOLLRDHUP` not registered | Register it always |
| E8 | Handler returns early and skips later events | Event dispatch is `noexcept`; per-event try/catch |
| E9 | `dup`/`fork` shares the fd | Never `dup` a connection fd; `CLOEXEC` |
| E10 | eventfd edge lost | Read eventfd to `EAGAIN` |
| E11 | Close without `EPOLL_CTL_DEL` | DEL then close; generation check |
| E12 | Blocking `send` starves the shard | All fds non-blocking; per-turn byte budget |
## 6. IOCP design <a id="s6"></a>
### 6.1 Port and concurrency
```cpp
HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
// NumberOfConcurrentThreads = 1: one logical consumer per shard.
// The value limits threads that dequeue packets, not in-flight overlapped operations.
```
One IOCP and one thread per shard. Listener and connection sockets are bound with `CreateIoCompletionPort(sock, iocp, key, 0)`. The completion key is the same `(slot_index, generation)` token scheme used by epoll.
### 6.2 Per-connection operations
```cpp
struct io_op {
    OVERLAPPED ov{};                 // must be first
    enum class kind : std::uint8_t { read, write, accept, connect, disconnect } k;
    connection* conn;
    std::size_t want;
    std::size_t got;
    std::error_code ec;
    io::completion* c;
    std::byte* buf;
};
```
At most one read and one write per connection. Read completion with `got == 0` is EOF; otherwise deliver to `source::on_data`. If `on_data` consumes less than `got`, retain the tail and do not post the next `WSARecv` until `resume_read()`. Write completion with `sent < want` re-posts the remainder; when all bytes are written, call `complete({ec, total})` exactly once.
```cpp
OVERLAPPED_ENTRY entries[64];
ULONG n = 0;
if (GetQueuedCompletionStatusEx(iocp, entries, 64, &n, timeout_ms, FALSE)) {
    for (ULONG i = 0; i < n; ++i) dispatch(entries[i]);
}
```
Batch size is 64; per-turn completion budget is 1024 completions or 200 microseconds, whichever comes first, so timers and the inbox are not starved.
### 6.3 AcceptEx
```cpp
BOOL ok = AcceptEx(lfd, accept_sock, addr_buf, 0,
                   sizeof(sockaddr_storage) + 16, sizeof(sockaddr_storage) + 16,
                   &got, &op->ov);
```
Pre-post 64 to 256 accept operations. On completion: `GetAcceptExSockaddrs`, `setsockopt(SO_UPDATE_ACCEPT_CONTEXT)`, bind the new socket to the target shard's IOCP, then immediately pre-post the next `AcceptEx`. Sockets must be created with `WSA_FLAG_OVERLAPPED`. Windows has no `SO_REUSEPORT`: v1 uses a single acceptor that hands the accepted socket to the target shard with one `PostQueuedCompletionStatus`; the acceptor performs no per-connection I/O.
### 6.4 Graceful close
```cpp
shutdown(sock, SD_SEND);                                  // FIN, still readable
// wait for peer FIN or timeout
DisconnectEx(sock, &op->ov, TF_REUSE_SOCKET, 0);          // overlapped
// completion, then closesocket / reuse
```
Close order is fixed: `CancelIoEx(sock, nullptr)`; wait for both in-flight operations to complete exactly once; `shutdown`/`DisconnectEx`; `closesocket`; release the slot and increment the generation. Never `closesocket` while an operation is in flight: its `OVERLAPPED` may already be freed.
### 6.5 CancelIoEx races
| Race | Result | Handling |
|---|---|---|
| Operation already completed | `CancelIoEx` returns `ERROR_NOT_FOUND` | Normal completion path; do not call back twice |
| Cancel succeeded | Completion returns `ERROR_OPERATION_ABORTED` | Map to `operation_canceled`; exactly one callback |
| Data arrives after cancel | No further completion | Connection is closing; stop reading |
| `closesocket` concurrent with completion | `ERROR_OPERATION_ABORTED` or `ERROR_INVALID_HANDLE` | Operation holds a connection reference; close only at refcount zero |
| Repeated `CancelIoEx` | `ERROR_NOT_FOUND` | Idempotent |

Every `OVERLAPPED` has exactly one owner and the owner releases it in the completion handler. Cancellation is a request, never a guarantee that no completion will follow.
### 6.6 Error mapping
| Win32 / Winsock | Meaning | Mapping |
|---|---|---|
| `ERROR_OPERATION_ABORTED` (995) | Local cancel | `std::errc::operation_canceled` |
| `ERROR_CONNECTION_ABORTED` (1236) | Connection aborted | `std::std::errc::connection_aborted` |
| `ERROR_HANDLE_EOF` (38) | Peer closed | EOF |
| `WSAECONNRESET` (10054) | RST | `std::errc::connection_reset` |
| `WSAECONNREFUSED` (10061) | Refused | `std::errc::connection_refused` |
| `WSAENETUNREACH` / `WSAEHOSTUNREACH` | Unreachable | `std::errc::host_unreachable` |
| `WSAENOBUFS` | Kernel buffers exhausted | Bounded backoff or `no_buffer_space` |
| `ERROR_IO_PENDING` | Not an error | Keep waiting |
| `ERROR_NOT_FOUND` | Cancel target already done | Ignore |
### 6.7 ET / IOCP semantic alignment
| Semantics | epoll-ET | IOCP |
|---|---|---|
| Readable | `EPOLLIN` edge, read to `EAGAIN` | `WSARecv` completion |
| Writable | `EPOLLOUT` edge, write to `EAGAIN` | `WSASend` completion |
| Partial write | Short `send`, continue | Short completion, re-post remainder |
| EOF | `recv == 0` or `EPOLLRDHUP` | `got == 0` or `ERROR_HANDLE_EOF` |
| RST | `EPOLLERR` + `SO_ERROR` | `WSAECONNRESET` |
| Cancel | `close` plus generation drop | `CancelIoEx` plus aborted completion |
| Batch | `epoll_wait` array | `GetQueuedCompletionStatusEx` array |
| Backpressure | Stop `recv` | Do not post the next `WSASend`/`WSARecv` |

The RPC layer sees only `source`, `sink`, and `connection`; it cannot tell which backend is below.
## 7. Wire protocol <a id="s7"></a>
### 7.1 Frame header (16 bytes, explicit LE)
| Offset | Width | Field | Type | Rule |
|---|---|---|---|---|
| 0 | 4 | `payload_len` | u32 | LE; `<= max_frame` (default 16 MiB, hard cap 64 MiB) |
| 4 | 1 | `type` | u8 | Table 7.3; unknown is fail-closed |
| 5 | 1 | `flags` | u8 | Table 7.4 |
| 6 | 2 | `reserved` | u16 | LE; must be 0 |
| 8 | 4 | `stream_id` | u32 | LE; bit31 must be 0; 0 means connection-level |
| 12 | 4 | `ext` | u32 | LE; CRC32C when `flags.CRC32C`, else 0 |

Byte order is fixed by A §30.1, the single wire authority. Every multi-byte field is written and read byte-wise; no struct `memcpy`, no bitfield, no unaligned cast.
### 7.2 Ordered extension header (16 bytes, only when `flags.ORDERED = 1`)
| Offset (after header) | Width | Field | Type |
|---|---|---|---|
| 0 | 8 | `lane_id` | u64 LE |
| 8 | 8 | `lane_seq` | u64 LE |

`payload_len` counts only the payload after all extension headers. If `flags.ORDERED` is set and the payload is shorter than 16 bytes, the frame is rejected with `protocol_error`. If `flags.ORDERED` is clear, no extension bytes exist; any receiver that assumes otherwise is non-conformant.
### 7.3 Frame types
| Value | Name | Stream scope | Payload |
|---|---|---|---|
| 0x00 | `DATA` | non-zero | Streaming chunk or push body fragment |
| 0x01 | `REQUEST` | non-zero | Call envelope plus request body |
| 0x02 | `RESPONSE` | non-zero | Status byte plus response body or error envelope |
| 0x03 | `ONE_WAY` | non-zero | Call envelope plus body; `UNRELIABLE` must be set |
| 0x04 | `RST_STREAM` | non-zero | `rpc_code` u32 LE |
| 0x05 | `SETTINGS` | zero | Settings TLV |
| 0x06 | `SETTINGS_ACK` | zero | Empty |
| 0x07 | `PING` | zero | 8 opaque bytes |
| 0x08 | `PONG` | zero | 8 opaque bytes |
| 0x09 | `GOAWAY` | zero | `last_stream_id` u32 + `rpc_code` u32 |
| 0x0A | `WINDOW_UPDATE` | zero or non-zero | `increment` u32 |
| 0x0B | `PUSH` | non-zero | Push envelope plus body |
| 0x0C | `PUSH_ACK` | non-zero | `push_seq` u64 |

Numeric values are fixed by this document for the first time. They consolidate the union of A §9.2, A §23 and B §4.2.1; changing them is a wire break.
### 7.4 Flags
| Bit | Name | Meaning |
|---|---|---|
| 0 | `END_STREAM` | Last frame of this stream |
| 1 | `COMPRESSED` | Payload is compressed; algorithm in bits 5-7 |
| 2 | `CRC32C` | `ext` carries CRC32C of the payload |
| 3 | `ORDERED` | 16-byte lane extension follows the header |
| 4 | `UNRELIABLE` | Best-effort delivery; no retransmission |
| 5-7 | `ALGO` | 0 none, 1 lz4, 2 zstd, 3 gzip, 4-7 reserved |

A frame with `COMPRESSED = 0` must have `ALGO = 0`; otherwise `protocol_error`. A frame with `ALGO != 0` and `COMPRESSED = 0` is rejected. This resolves the A/B conflict in favor of A §30.1 plus §31.1; B §4.2.1's flag assignment is superseded.
### 7.5 Handshake
Client hello (12-byte prefix plus TLVs):

| Offset | Width | Field |
|---|---|---|
| 0 | 4 | magic `"CRPC"` |
| 4 | 2 | `version` u16 LE, v1 = 1 |
| 6 | 2 | `flags` u16 LE, must be 0 |
| 8 | 2 | `tlv_len` u16 LE |
| 10 | 2 | `reserved` u16 LE, must be 0 |
| 12 | `tlv_len` | TLVs |

Server hello has the same 12-byte prefix, with byte 8 = `status` u8 (0 ok, 1 version mismatch, 2 bad settings, 3 unauthorized), byte 9 reserved, and `tlv_len` at offset 10.

| TLV type | Value | Encoding |
|---|---|---|
| 0x01 | `max_frame` | u32 LE |
| 0x02 | `initial_window` | u32 LE |
| 0x03 | `codec` | u8: 0 binary, 1 json, 2 xml, 3 ini, 4 toml, 5 protobuf, 6 flatbuffers |
| 0x04 | `compress_bitmap` | u8: bit0 lz4, bit1 zstd, bit2 gzip |
| 0x05 | `compress_min` | u32 LE |
| 0x06 | `keepalive_ms` | u32 LE |
| 0x07 | `max_streams` | u32 LE |
| 0x08 | `identity` | u8 length plus bytes, `<= 128` |
| 0x09 | `resume_token` | u64 LE |
| 0x0A | `hmac` | 32 bytes |

Handshake limit is 4 KiB and timeout 3 s. Unknown TLV type, unknown codec, unknown capability bit, non-zero reserved, or version mismatch is fail-closed: send a server hello with a non-zero status where possible, then close (R9).
### 7.6 Payload envelopes
Call envelope (`REQUEST`, `ONE_WAY`, and the success path of `RESPONSE`):
```
service_id      u32 LE
method_id       u32 LE
call_flags      u8
remaining_ns    varint (0 = no deadline)
[trace_id]      16 bytes            if call_flags bit0
[span_id]       8 bytes             if call_flags bit0
[parent_span]   8 bytes             if call_flags bit1
[idem_len]      u8 + bytes          if call_flags bit2, <= 128
[config_ver]    u64 LE              if call_flags bit3
[route_ver]     u64 LE              if call_flags bit3
meta_len        u16 LE + meta TLV
body            remaining bytes
```
`call_flags`: bit0 `has_trace`, bit1 `has_parent`, bit2 `has_idem`, bit3 `has_versions`, bit4 reserved for HLC, bits 5-7 must be 0.

`RESPONSE` payload starts with `status` u8: 0 success, 1 error. Success carries the response body. Error carries the error envelope of §9.3.

`PUSH` payload: `topic_len` u16, topic bytes, `key_len` u16, key bytes, `push_seq` u64, `idem_len` u8, idempotency key bytes, body. `PUSH_ACK` payload: `push_seq` u64.

Meta TLV: repeated `key_len` u8, key bytes, `value_len` u16, value bytes. Keys are ASCII, values are opaque bytes. Total meta is at most 1 KiB.
### 7.7 Static assertions and primitives
```cpp
static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint8_t) == 1 && sizeof(std::uint16_t) == 2);
static_assert(sizeof(std::uint32_t) == 4 && sizeof(std::uint64_t) == 8);
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<double>::is_iec559);
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
namespace crpc::endian {
inline void put_le16(std::byte* p, std::uint16_t v) noexcept;
inline void put_le32(std::byte* p, std::uint32_t v) noexcept;
inline void put_le64(std::byte* p, std::uint64_t v) noexcept;
inline std::uint16_t get_le16(const std::byte* p) noexcept;
inline std::uint32_t get_le32(const std::byte* p) noexcept;
inline std::uint64_t get_le64(const std::byte* p) noexcept;
constexpr bool host_is_little() noexcept;
template <class t> constexpr t to_le(t v) noexcept;
template <class t> constexpr t from_le(t v) noexcept;
}
```
varint is LE base-128: low 7 bits first, bit 7 is continuation. `u64` uses at most 10 bytes; byte 10 may only be `0x00` or `0x01`. Zigzag is `(n << 1) ^ (n >> 63)` for encode and `(u >> 1) ^ -(u & 1)` for decode, with unsigned arithmetic. Floating point is `bit_cast` to `u32`/`u64`, then LE.
### 7.8 Length and overflow checks
| Check | Rule |
|---|---|
| Frame length | `payload_len <= max_frame`; reject before allocation |
| Accumulation | Check `len <= avail - off`; never compute `off + len` first |
| u64 to `size_t` | `if (v > SIZE_MAX) reject`; required on x86 |
| Element count | `count <= remaining / min_element_size`; prevents multiplication overflow |
| String | `len <= max_string_bytes` (1 MiB) |
| Allocation | Every `resize`/`reserve` is preceded by a cap check; failure is `resource_exhausted` |
| Nesting | Codec depth 32; HTTP header count 100; route segments 32 |
### 7.9 Golden bytes
Header for a `REQUEST` with `payload_len = 16`, `stream_id = 1`, no flags:
```
10 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00
```
Ordered variant, `lane_id = 7`, `lane_seq = 9`:
```
10 00 00 00 01 08 00 00 01 00 00 00 00 00 00 00
07 00 00 00 00 00 00 00 09 00 00 00 00 00 00 00
```
`PING` with payload `01 02 03 04 05 06 07 08`:
```
08 00 00 00 07 00 00 00 00 00 00 00 00 00 00 00
01 02 03 04 05 06 07 08
```
These byte strings are committed as `tests/golden/*.bin`; CI asserts byte equality on every platform and decodes them identically.
### 7.10 Fail-closed rules
| Condition | Action |
|---|---|
| Unknown `type` | `GOAWAY(protocol_error)`, close |
| `reserved != 0` | `GOAWAY(protocol_error)`, close |
| `stream_id` bit31 set | `GOAWAY(protocol_error)`, close |
| `stream_id == 0` for `REQUEST`/`RESPONSE`/`DATA`/`ONE_WAY`/`PUSH` | `GOAWAY(protocol_error)`, close |
| Non-zero `stream_id` for `SETTINGS`/`PING`/`PONG`/`GOAWAY` | `GOAWAY(protocol_error)`, close |
| `ORDERED` set but payload `< 16` | `GOAWAY(protocol_error)`, close |
| `COMPRESSED = 0` but `ALGO != 0` | `GOAWAY(protocol_error)`, close |
| `ONE_WAY` without `UNRELIABLE` | `GOAWAY(protocol_error)`, close |
| Duplicate `stream_id` | `RST_STREAM(protocol_error)` |
| Unknown `stream_id` data | Drop and count; no close |
| CRC mismatch | `RST_STREAM(protocol_error)` and count |
| `payload_len > max_frame` | `GOAWAY(protocol_error)`, no allocation |
## 8. Message semantics <a id="s8"></a>
### 8.1 Three message forms
| Form | Identifier | Has call slot | Completion signal |
|---|---|---|---|
| Call (unary) | `stream_id` | Yes | `RESPONSE` frame |
| Call (streaming) | `stream_id` | Yes | `RESPONSE` with `END_STREAM` |
| Push | `(topic, key, push_seq)` | No (subscription instead) | `PUSH_ACK` when reliable |

A call slot is created before the first byte is sent and released only after a terminal state. Push has no call slot; it has a subscription entry and a replay buffer.
### 8.2 Delivery × ordering matrix
| | Reliable | Unreliable |
|---|---|---|
| **Plain** | Default RPC: request/response, timeout, cancel, retry with idempotency key, explicit failure | Metrics, logs, heartbeats: `ONE_WAY` with `UNRELIABLE`; queue full drops and counts; no handle |
| **Ordered (lane)** | Command stream: strict enqueue/send/arrival order, server-side serial execution, in-place retry with the same `lane_seq` and idempotency key | Ordered send, may drop; a gap is reported as a counter; no retransmission |

Default is plain plus reliable. Ordered and unreliable are explicit opt-ins. There is no separate reliable one-way message type in v1: reliable one-way is a `REQUEST` whose response body is empty (A §19.1). `ONE_WAY` is always unreliable.
### 8.3 ACK, retransmission, dedup
TCP already provides ordered, reliable byte delivery within a connection. CRPC does not re-implement per-frame ACK, cumulative windows, or RTO for calls. Reliability across connection loss and handler failure is an RPC-layer concern:

| Case | Mechanism |
|---|---|
| Call success | `RESPONSE` frame is the application-level acknowledgment |
| Call retry | New attempt, new `stream_id`, same idempotency key; server dedup table returns the cached result |
| Reliable push | `PUSH` plus `PUSH_ACK`; server replay buffer of 256 entries per subscription; client dedups by `push_seq` |
| Unreliable message | No ACK, no retry, drop with counter `crpc_unreliable_dropped_total{reason}` |

Idempotency key (A §28.3):
```
idempotency_key = { client_id (16 bytes), client_epoch (u64), seq (u64) }
```
`client_id` is persisted per process; `client_epoch` increments on every start; `seq` increments per `(client_id, target_service)`. A natural business key may replace the triple. Random UUIDs are forbidden as idempotency keys. The triple is the monotonic dedup anchor: `seq` never decreases within an epoch, so the server can evict by FIFO and detect replays without any wall-clock comparison.

Server dedup table: per-client ring, default 256 entries, keyed by `(client_id, client_epoch, seq)`; FIFO eviction only; never cleared wholesale. `fresh` executes; `in_progress` parks the caller until completion or deadline; `done` returns the cached result without re-execution. The table is memory-only in v1, so exactly-once holds within one authority epoch (A §28.3).
### 8.4 Ordered lanes
- Lane scope is `(client_id, target_service_id)`; `lane_id` is client-assigned and stable across reconnects.
- `lane_seq` is allocated once, at MPSC enqueue, by `fetch_add`; it never wraps.
- Frames of one lane always use one connection at a time; on reconnect the lane pauses until the new connection is established.
- Server `ordered_execution = true` keeps a FIFO per lane with one handler in flight.
- Retry is in place: same lane, same `lane_seq`, same idempotency key. Unacknowledged later sequences are not sent while a retry is pending.
- Default failure policy is continue-after-failure; `fail_fast` cancels the remaining lane entries.
### 8.5 Deadline propagation
The wire carries `remaining_ns`, never an absolute timestamp (P12).
```
client:   deadline_abs = monotonic_now + timeout
          remaining_ns = deadline_abs - monotonic_now
server:   if remaining_ns <= min_remaining (1 ms) -> do not execute, return deadline_exceeded
          downstream: remaining_ns' = remaining_ns - elapsed - safety_margin (1 ms)
```
A handler must not continue after its deadline. `call_context::remaining()` is authoritative; `derive()` subtracts elapsed time and makes the current span the parent.
### 8.6 Cancellation
`call<t>::cancel()` and the deadline timer both call `call_slot::try_finish` with `canceled` or `deadline_exceeded`. The CAS winner sends `RST_STREAM`; the loser observes the terminal state and does nothing. A response that arrives after a terminal state is dropped and counted, never delivered to a recycled slot (generation check).
### 8.7 Push API
```cpp
enum class delivery_mode : std::uint8_t { reliable, unreliable };
enum class overflow_policy : std::uint8_t { drop_oldest, drop_newest, disconnect };
struct identity { std::array<std::byte, 16> bytes; };
struct subscribe_options {
    std::string key;
    std::size_t max_queued = 1024;
    overflow_policy on_overflow = overflow_policy::drop_oldest;
    delivery_mode mode = delivery_mode::reliable;
};
struct subscription_stats {
    std::uint64_t subscriptions = 0;
    std::uint64_t dropped = 0;
};
class subscription {
public:
    subscription() noexcept = default;
    subscription(subscription&&) noexcept;
    subscription& operator=(subscription&&) noexcept;
    subscription(const subscription&) = delete;
    ~subscription();                     // unsubscribe
    [[nodiscard]] bool valid() const noexcept;
    void unsubscribe() noexcept;
};
class push_hub {
public:
    void publish(std::string_view topic, std::string_view key,
                 byte_span payload, delivery_mode mode) noexcept;
    void publish_to_identity(const identity& who, std::string_view topic,
                             byte_span payload, delivery_mode mode) noexcept;
    [[nodiscard]] subscription_stats stats() const noexcept;
};
class push_channel {
public:
    [[nodiscard]] subscription subscribe(std::string_view topic,
                                         void (*on_msg)(byte_span, void*) noexcept,
                                         void* user, subscribe_options opt = {});
    [[nodiscard]] rpc_error unsubscribe(std::string_view topic) noexcept;
};
```
A push target is a logical identity, not a connection. The server maps `identity -> connection*` on the authority shard. A disconnected identity keeps its subscription for 30 s to allow resume. Reliable push uses `PUSH_ACK` plus a 256-entry replay buffer per subscription; unreliable push drops on overflow and counts. Slow clients never block a shard: the send-ring high-water policy applies.
### 8.8 Streaming
```cpp
class stream_reader {
public:
    virtual ~stream_reader() = default;
    [[nodiscard]] virtual rpc_result<std::size_t> read(byte_span dst) = 0;
};
class stream_writer {
public:
    virtual ~stream_writer() = default;
    [[nodiscard]] virtual status write(byte_span src) = 0;
    [[nodiscard]] virtual status finish() = 0;
};
```
A stream maps to `DATA` frames plus `END_STREAM`; HTTP/1.1 maps it to chunked or Content-Length; HTTP/2 maps it to DATA plus flow-control windows. `RST_STREAM` cancels, and cancellation releases the stream slot, the subscription, and any temporary file.
## 9. Error model <a id="s9"></a>
### 9.1 `rpc_code` (exactly R6)
```cpp
enum class rpc_code : std::uint16_t {
    ok = 0, canceled = 1, deadline_exceeded = 2, unavailable = 3,
    connection_refused = 4, connection_reset = 5, broken_pipe = 6, eof = 7,
    overload = 8, not_found = 9, no_such_method = 10, invalid_argument = 11,
    failed_precondition = 12, resource_exhausted = 13, protocol_error = 14,
    decode_error = 15, encode_error = 16, not_acknowledged = 17,
    unknown_side_effect = 18, internal = 19, shutting_down = 20
};
```
Numeric values are the declaration order and are stable forever. The larger table in B §14.1 is superseded by R6; no code may be added, renamed, or renumbered without a new design decision.
### 9.2 `rpc_error` and `rpc_result`
```cpp
using metadata_map = std::vector<std::pair<std::string, std::string>>;
struct rpc_error {
    rpc_code code = rpc_code::ok;
    std::string message;
    bool retryable = false;
    std::chrono::milliseconds retry_after{0};
    metadata_map metadata;
};
struct status {
    rpc_code code = rpc_code::ok;
    [[nodiscard]] bool ok() const noexcept { return code == rpc_code::ok; }
};
template <class t>
class rpc_result {
public:
    rpc_result(t v) : v_(std::move(v)) {}
    rpc_result(rpc_error e) : v_(std::move(e)) {}
    [[nodiscard]] bool ok() const noexcept { return v_.index() == 0; }
    [[nodiscard]] t& value() & { return std::get<0>(v_); }
    [[nodiscard]] const t& value() const& { return std::get<0>(v_); }
    [[nodiscard]] t&& value() && { return std::get<0>(std::move(v_)); }
    [[nodiscard]] rpc_error& error() & { return std::get<1>(v_); }
    [[nodiscard]] const rpc_error& error() const& { return std::get<1>(v_); }
private:
    std::variant<t, rpc_error> v_;
};
```
`rpc_result` does not require `t` to be default-constructible (D6). An enum is never used as an error object (R6). Error paths may allocate; success paths may not.
### 9.3 Error wire envelope
```
RESPONSE payload:
  status          u8 = 1
  code            varint
  retryable       varint (0 or 1)
  retry_after_ms  varint
  message_len     u16 LE + UTF-8 bytes (<= 512)
  meta_len        u16 LE + meta TLV (<= 1024)
```
Meta keys: `crpc.raw_code` (unknown future code), `trace_id`, `rtti_code`, `field`, `upstream_service`, `http.status`. `rtti::error_code` is a local detail and is never sent as the RPC error domain; it is mapped to `decode_error`/`encode_error` plus `rtti_code`.

Truncation: `message` is cut to 512 bytes with a trailing ellipsis; meta entries are dropped in key order beyond 1 KiB; a protocol frame is never truncated, it is rejected before send.
### 9.4 HTTP status mapping
| `rpc_code` | HTTP | Note |
|---|---|---|
| `ok` | 200 / 201 / 204 | Depends on verb and body |
| `canceled` | 499 | Non-standard; connection may simply close |
| `deadline_exceeded` | 504 | |
| `unavailable` | 503 | |
| `connection_refused` | 503 | |
| `connection_reset` | 503 | |
| `broken_pipe` | 503 | |
| `eof` | 503 | |
| `overload` | 429 | Include `Retry-After` |
| `not_found` | 404 | |
| `no_such_method` | 501 | |
| `invalid_argument` | 400 | |
| `failed_precondition` | 412 | |
| `resource_exhausted` | 429 | |
| `protocol_error` | 502 | Upstream protocol violation |
| `decode_error` | 400 request / 500 response | Direction-dependent |
| `encode_error` | 500 | |
| `not_acknowledged` | 503 | |
| `unknown_side_effect` | 504 | Never claim "not executed" |
| `internal` | 500 | Fixed title, no internal detail |
| `shutting_down` | 503 | |

Framework-level statuses without an `rpc_code`: 405 (path matched, verb not allowed, with `Allow`), 406 (no acceptable codec), 413 (body too large), 415 (unsupported content type or encoding), 431 (headers too large). Error body is RFC 7807 `application/problem+json` with `type`, `title`, `status`, `detail`, `instance`, and `crpc_code`.
## 10. Serialization <a id="s10"></a>
### 10.1 Codec registry
```cpp
namespace crpc::codec {
class registry {
public:
    static registry& instance();
    [[nodiscard]] status enable(std::string_view content_type, bool on);
    [[nodiscard]] bool enabled(std::string_view content_type) const noexcept;
    [[nodiscard]] std::string_view canonical(std::string_view ct) const noexcept;
};
template <class t>
struct binding {
    std::string_view content_type;
    rpc_code (*decode)(byte_span, t&);
    rpc_code (*encode)(const t&, buffer&);
};
template <class t>
inline constexpr binding<t> request_codecs[] = {
    {"application/crpc", &binary_decode<t>, &binary_encode<t>},
    {"application/json", &json_decode<t>, &json_encode<t>},
    {"application/xml", &xml_decode<t>, &xml_encode<t>},
    {"application/ini", &ini_decode<t>, &ini_encode<t>},
    {"application/toml", &toml_decode<t>, &toml_encode<t>},
    {"application/x-protobuf", &proto_decode<t>, &proto_encode<t>},
    {"application/x-flatbuffers", &flat_decode<t>, &flat_encode<t>}
};
} // namespace crpc::codec
```
The registry only toggles formats and canonicalizes content types; it does no type erasure. Each DTO instantiates its own binding array at compile time. Request selection is `Content-Type`; no match is 415. Response selection is `Accept` with q-values; no match is 406 unless `fallback_identity` is configured.
### 10.2 crpc-binary layout
Payload body is a sequence of fields:
```
key   = varint((field_id << 3) | wire_type)
value = wire_type-specific
```
| `wire_type` | Meaning | Encoding |
|---|---|---|
| 0 | varint | bool, int32/64, uint32/64, enum |
| 1 | fixed64 | double, fixed64 |
| 2 | length-delimited | string, bytes, nested message, packed repeated |
| 3 | zigzag varint | sint32/sint64 (CRPC extension) |
| 5 | fixed32 | float, fixed32 |

`field_id` is the declaration index plus one, matching `fields_of<t>::members` order, and is known at compile time. `4`, `6`, and `7` are invalid and produce `decode_error`.

Containers: numeric sequences are packed; other sequences are length-delimited with per-element encoding; maps are repeated entries of key field 1 and value field 2. Optional fields are written only when present. Unknown fields are skipped by wire type; an unknown wire type is rejected.
### 10.3 RTTI wiring
Hot-path encode uses member pointers directly; no virtual dispatch and no DOM:
```cpp
template <class t, class tuple_t, std::size_t... i>
rpc_code encode_members(const t& v, const tuple_t& m, buffer& out,
                        std::index_sequence<i...>) {
    rpc_code ec = rpc_code::ok;
    ((ec = ec == rpc_code::ok
               ? encode_field(static_cast<std::uint32_t>(i + 1), v.*std::get<i>(m), out)
               : ec), ...);
    return ec;
}
template <class t>
rpc_code binary_encode(const t& v, buffer& out) {
    using members_t = typename rtti::fields_of<t>::members_type;
    return encode_members(v, rtti::fields_of<t>::members, out,
                          std::make_index_sequence<std::tuple_size_v<members_t>>{});
}
```
`fields_of<t>::members` is a `std::tuple` of member pointers with constant initialization (`[READ:third_party/rtti/include\rtti\reflection.hpp:5449]`). Cold paths (HTTP binding, management, debugging) may use `field_access_base::set_value`/`get_value` (`[READ:third_party/rtti/include\rtti\reflection.hpp:451]`, `:461`, `:463`).

Derived types are rejected by crpc-binary with a `static_assert`: `RTTI_DEFINE_TYPE_DERIVED` places base fields outside `members` (D12, `[READ:third_party/rtti/include\rtti\reflection.hpp:5490]`). Text formats may still encode derived types through the virtual path.
### 10.4 Text and schema formats
| Format | Decode | Encode | Evidence |
|---|---|---|---|
| JSON | `json::parse` then `rtti::from_value` (preserves offset) | `rtti::to_value` then `json::serialize(document)` | `[READ:third_party/rtti/include\rtti\json.hpp:170]`, `:174`, `[READ:third_party/rtti/include\rtti\convert.hpp:1354]`, `:1414` |
| XML | `xml::parse_as<t>` | `rtti::to_value` then `xml::serialize(document)` | `[READ:third_party/rtti/include\rtti\xml.hpp:220]`, `:216` |
| INI | `ini::parse_as<t>` | `rtti::to_value` then `ini::serialize(document)` | `[READ:third_party/rtti/include\rtti\ini.hpp:346]`, `:320` |
| TOML | `toml::parse_as<t>` | `rtti::to_value` then `toml::serialize(document)` | `[READ:third_party/rtti/include\rtti\toml.hpp:297]`, `:271` |
| protobuf | `rtti::proto::parse_as<t>(bytes, cfg)` | `rtti::proto::serialize<t>(obj, out)` | `[READ:third_party/rtti/include\rtti\proto.hpp:2700]`, `:2679` |
| FlatBuffers | `rtti::flat::parse_as<t>(bytes, cfg)` | `rtti::flat::tree_to_wire(&doc, rtti::flat::schema_of<t>(), out, cfg)` | `[READ:third_party/rtti/include\rtti\flat.hpp:346]`, `:290` |

Two-phase encoding is mandatory for text formats because the convenience `serialize<T>` returns an empty string on conversion failure (D9, `[READ:third_party/rtti/include\rtti\json.hpp:222]`). Two-phase decoding is mandatory for JSON because `parse_as` loses the byte offset (D8, `[READ:third_party/rtti/include\rtti\json.hpp:193]`).

protobuf and FlatBuffers require an explicit user specialization of `rtti::proto::schema<t>` (`[READ:third_party/rtti/include\rtti\proto.hpp:432]`) or `rtti::flat::schema<t>` (`[READ:third_party/rtti/include\rtti\flat.hpp:198]`). The primary template is empty, so an unregistered type silently encodes to zero bytes. CRPC must therefore require a `schema_hash` and fail registration when the schema count is zero for a type used on those formats (D13). FlatBuffers typed encode goes through a DOM, so no zero-copy claim is made (D14).
### 10.5 Limits, endianness, error mapping
| Limit | Default | Over-limit |
|---|---|---|
| Binary nesting depth | 32 | `decode_error` |
| String/bytes | 1 MiB | `decode_error` |
| Frame payload | 16 MiB | `protocol_error` |
| Text document | 8 MiB | `resource_exhausted` |
| RTTI format depth | 32 (configured) | `decode_error` |

All integer and floating-point wire values are explicit LE; text formats are UTF-8 without BOM and locale-independent. The binary layout has no alignment or padding requirement: every field is encoded and decoded byte-wise, so a reader never performs an aligned load and a writer never inserts padding. `rtti::error_code` values (`[READ:third_party/rtti/include\rtti\error.hpp:15]`) map to `rpc_code::decode_error` on input and `encode_error` on output, with the original code in `metadata["rtti_code"]`. Schema mismatch is `failed_precondition` when `strict_schema = true`; otherwise decoding proceeds by field id and the mismatch is counted.
## 11. Service API and stubs <a id="s11"></a>
### 11.1 Declaration macros (R8)
```cpp
#define CRPC_ROUTE(path_literal) \
    inline static constexpr ::crpc::http::controller_meta crpc_route_meta{ path_literal }
#define CRPC_SERVICE(controller_t, service_number) \
    template <> inline constexpr ::crpc::service_id ::crpc::service_id_of<controller_t>{ service_number }
#define CRPC_METHOD(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_method_##controller_t##_##method{ ::crpc::rpc::kind::binary, path_literal, #method, __LINE__ }
#define CRPC_HTTP_GET(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_get_##controller_t##_##method{ ::crpc::http::verb::get, path_literal, #method, __LINE__ }
#define CRPC_HTTP_POST(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_post_##controller_t##_##method{ ::crpc::http::verb::post, path_literal, #method, __LINE__ }
#define CRPC_HTTP_PUT(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_put_##controller_t##_##method{ ::crpc::http::verb::put, path_literal, #method, __LINE__ }
#define CRPC_HTTP_PATCH(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_patch_##controller_t##_##method{ ::crpc::http::verb::patch, path_literal, #method, __LINE__ }
#define CRPC_HTTP_DELETE(controller_t, method, path_literal) \
    inline const ::crpc::rpc::method_registrar<controller_t, &controller_t::method> \
        crpc_delete_##controller_t##_##method{ ::crpc::http::verb::del, path_literal, #method, __LINE__ }
```
Every action macro carries the controller type and member, because C++17 cannot infer a member pointer from a following declaration. Return and parameter types are derived from `decltype(&controller_t::method)`, never written by hand.

Field binding attributes are RTTI attributes:
```cpp
#define CRPC_FROM_ROUTE(name)   RTTI_ATTR("FromRoute", name)
#define CRPC_FROM_QUERY(name)   RTTI_ATTR("FromQuery", name)
#define CRPC_FROM_BODY          RTTI_ATTR("FromBody")
#define CRPC_FROM_HEADER(name)  RTTI_ATTR("FromHeader", name)
#define CRPC_FROM_FORM(name)    RTTI_ATTR("FromForm", name)
#define CRPC_FROM_FILE(name)    RTTI_ATTR("FromFile", name)
```
They are used inside `RTTI_DEFINE_FIELD_ATTRS`, for example `RTTI_DEFINE_FIELD_ATTRS(get_user_req, RTTI_FIELD_ATTR_ENTRY(id, CRPC_FROM_ROUTE("id")))`. This uses only public RTTI APIs (`[READ:third_party/rtti/include\rtti\reflection.hpp:6791]`, `:6766`, `:1142`).
### 11.2 Method traits and invocation
```cpp
namespace crpc::rpc {
enum class kind : std::uint8_t { binary, http };
template <class sig> struct method_traits;
template <class ctl_t, class resp_t, class req_t>
struct method_traits<resp_t (ctl_t::*)(const req_t&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = resp_t;
    static constexpr bool has_context = false;
};
template <class ctl_t, class resp_t, class req_t>
struct method_traits<resp_t (ctl_t::*)(const req_t&, call_context&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = resp_t;
    static constexpr bool has_context = true;
};
template <class ctl_t, class resp_t>
struct method_traits<resp_t (ctl_t::*)()> {
    using controller_type = ctl_t;
    using request_type = void;
    using response_type = resp_t;
    static constexpr bool has_context = false;
};
template <class ctl_t, class req_t>
struct method_traits<void (ctl_t::*)(const req_t&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = void;
    static constexpr bool has_context = false;
};
template <class ctl_t, class req_t>
struct method_traits<void (ctl_t::*)(const req_t&, call_context&)> {
    using controller_type = ctl_t;
    using request_type = req_t;
    using response_type = void;
    static constexpr bool has_context = true;
};
template <class ctl_t>
struct method_traits<void (ctl_t::*)(call_context&)> {
    using controller_type = ctl_t;
    using request_type = void;
    using response_type = void;
    static constexpr bool has_context = true;
};
template <class ctl_t, auto method>
struct method_registrar {
    method_registrar(kind k, const char* path, const char* name, std::uint32_t ordinal) noexcept;
    method_registrar(http::verb v, const char* path, const char* name, std::uint32_t ordinal) noexcept;
};
template <class ctl_t, auto method>
status invoke_binary(ctl_t& self, span<const std::byte> in, buffer& out, call_context& ctx);
template <class ctl_t, auto method>
status invoke_http(ctl_t& self, const http::request& req, http::response& resp, call_context& ctx);
} // namespace crpc::rpc
```
`invoke_binary` decodes `request_type`, calls `(self.*method)(req, ctx)` or `(self.*method)(req)`, and encodes `response_type`; `void` returns an empty body. Exceptions at the handler boundary are caught and converted to `rpc_error{rpc_code::internal, "internal error"}`.
### 11.3 `call<t>`
```cpp
template <class t>
class call {
public:
    using value_type = t;
    call() noexcept = default;
    call(call&& other) noexcept;
    call& operator=(call&& other) noexcept;
    call(const call&) = delete;
    call& operator=(const call&) = delete;
    ~call();                                                   // unfinished: cancel and recycle
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] rpc_result<t> get();                         // parks in a coroutine; blocks elsewhere
    [[nodiscard]] bool try_get(rpc_result<t>& out) noexcept;
    template <class fn_t> void then(fn_t&& fn);
    void cancel() noexcept;
};
template <class... calls_t>
auto when_all(calls_t&&... calls)
    -> call<std::tuple<typename std::decay_t<calls_t>::value_type...>>;
```
The handle is 16 bytes: `call_slot*` plus `std::uint32_t generation`. Slots come from a per-shard freelist preallocated at startup. `then` stores a function pointer plus user pointer or a small buffer object of at most 32 bytes; it never uses `std::function` on the hot path (R7). The callback runs on the shard owning the connection, on a later pump turn, never inside `try_finish` and never on the registering stack.

`get()` behavior: inside a coroutine it parks on `coro_rt_sem_wait`; on a non-shard thread it blocks on a condition variable; on a shard thread without a coroutine context it returns `rpc_error{rpc_code::failed_precondition}` with `metadata["reason"] = "would_block"` and never blocks (A §18.6).
### 11.4 `call_context`
```cpp
struct trace_id { std::array<std::byte, 16> bytes; };
struct span_id { std::array<std::byte, 8> bytes; };
struct lane_token { std::uint64_t lane_id = 0; std::uint64_t seq = 0; };
class call_context {
public:
    [[nodiscard]] std::chrono::nanoseconds remaining() const noexcept;
    [[nodiscard]] bool expired() const noexcept;
    [[nodiscard]] trace_id trace() const noexcept;
    [[nodiscard]] span_id span() const noexcept;
    [[nodiscard]] span_id parent_span() const noexcept;
    [[nodiscard]] std::uint64_t config_version() const noexcept;
    [[nodiscard]] std::uint64_t route_version() const noexcept;
    [[nodiscard]] std::string_view idempotency_key() const noexcept;
    [[nodiscard]] std::optional<lane_token> lane() const noexcept;
    [[nodiscard]] std::string_view metadata(std::string_view key) const noexcept;
    [[nodiscard]] std::uint32_t attempt() const noexcept;
    [[nodiscard]] bool canceled() const noexcept;
    void set_response_metadata(std::string_view key, std::string_view value);
    [[nodiscard]] call_context derive() const;
};
```
`call_context` is per call, move-only, passed by reference to the handler, and invalid after the handler returns. It is never `thread_local`, because coroutines migrate between execution contexts (B §4.5).
## 12. HTTP/1.1, HTTP/2, HTTPS <a id="s12"></a>
### 12.1 Same-port dispatch
```
accept -> peek first bytes ->
  TLS: ALPN "h2" -> HTTP/2; ALPN "http/1.1" -> HTTP/1.1
  cleartext "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" -> h2c
  otherwise -> HTTP/1.1
```
Both protocols share the router, binding layer, handler pool, `call_context`, and resilience pipeline. ALPN is mandatory for TLS: a client that offers only an unsupported protocol is rejected, never downgraded to cleartext.
### 12.2 HTTP/1.1
| Item | v1 rule |
|---|---|
| Keep-alive | Default on; `Connection: close` disables; HTTP/1.0 requires explicit `keep-alive` |
| Pipelining | Accepted, processed serially; response order equals request order |
| Chunked | Decoded; encoded when length is unknown |
| Content-Length | Used when present; absent and not chunked is 400 |
| CL + TE together | 400 and close (request-smuggling defense, no TE precedence) |
| Multiple or invalid CL | 400 and close |
| `Transfer-Encoding: chunked, gzip` | 400; TE chains are unsupported |
| 100-continue | Send `100 Continue` only after routing and size checks; over-limit is 413 without 100 |
| Trailers | Parsed and exposed through `call_context`; optional |
| Header limit | 16 KiB; over-limit is 431 |
| Body limit | 8 MiB default; over-limit is 413 |
| Host | Required in HTTP/1.1; absent is 400 |
| Parsing | Strict RFC 9112; reject bare LF and whitespace before colon |
| HEAD | Executes the GET handler and discards the body |
### 12.3 HTTPS
TLS 1.2 and 1.3 via OpenSSL. ALPN advertises `h2` and `http/1.1`. SNI selects the certificate context; certificate reload atomically replaces `shared_ptr<SSL_CTX>`, old connections finish on the old context, new connections use the new one. Server-side session cache is enabled. OCSP stapling and client certificates are out of v1 scope. TLS-level compression is disabled.
### 12.4 HTTP/2
The minimum correct set is mandatory (R5); there is no "simplified h2".

| Area | Requirement |
|---|---|
| Framing | 9-byte header, frame types DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PUSH_PROMISE/PING/GOAWAY/WINDOW_UPDATE/CONTINUATION |
| HPACK | Static table (61 entries), dynamic table with size updates, Huffman coding, integer/string primitives |
| Flow control | Connection and stream windows, `WINDOW_UPDATE`, initial 65535, `SETTINGS_INITIAL_WINDOW_SIZE` |
| Stream state | idle, reserved, open, half-closed local, half-closed remote, closed; illegal transitions are connection errors |
| Settings | `HEADER_TABLE_SIZE`, `ENABLE_PUSH`, `MAX_CONCURRENT_STREAMS`, `INITIAL_WINDOW_SIZE`, `MAX_FRAME_SIZE`, `MAX_HEADER_LIST_SIZE`; ACK required |
| Errors | `RST_STREAM` for stream errors, `GOAWAY` for connection errors, correct error codes |
| Continuation | `CONTINUATION` frames must follow `HEADERS`/`PUSH_PROMISE` without interleaving |
| Limits | `MAX_FRAME_SIZE` 16384 default, `MAX_CONCURRENT_STREAMS` 100 default, header list 16 KiB |
| Interop | h2spec must pass the mandatory sections before release |

Two independent flow-control layers exist. The h2 layer owns connection and stream windows; the CRPC layer owns its own connection and stream windows for the binary protocol. They are not shared. h2 `stream_id` and CRPC `stream_id` are separate spaces; the adapter keeps `h2_stream_id -> call_slot*`.
```
h2 HEADERS :method POST, :path /svc/method, content-type application/crpc
  -> router lookup -> call_slot created with an internal stream_id
h2 DATA frames -> REQUEST body bytes
RESPONSE bytes -> h2 HEADERS (:status) + DATA frames
h2 RST_STREAM -> call_slot try_finish(canceled)
h2 GOAWAY -> no new streams; existing streams drain or cancel
```
HTTP/2 header names are lowercase. `x-crpc-*` fields are ordinary headers, never pseudo-headers. The same `x-crpc-trace-id`, `x-crpc-timeout-ms`, `x-crpc-idempotency-key`, `x-crpc-attempt`, `x-crpc-lane-id`, and `x-crpc-lane-seq` semantics apply on both HTTP versions.
### 12.5 Status and error mapping
HTTP status selection uses §9.4. `problem+json` is the only error body for 4xx/5xx generated by CRPC. A gateway or interceptor may set `metadata["http.status"]` to produce 401, 403, 409, or 422, because R6 does not define those codes; this bridge is recorded as an open issue in §22.
## 13. MVC and RESTful <a id="s13"></a>
### 13.1 Route table
```cpp
namespace crpc::http {
enum class verb : std::uint8_t { get, post, put, patch, del, head, options };
struct controller_meta {
    const char* prefix;
    constexpr explicit controller_meta(const char* p) noexcept : prefix(p) {}
};
struct request {
    verb method = verb::get;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    byte_span body;
};
struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    buffer body;
};
struct route_entry {
    const char* path;
    const char* name;
    verb method;
    std::uint32_t ordinal;
};
struct action_record {
    std::string pattern;
    std::string name;
    verb method;
    void* controller;
    const std::type_info* controller_type;
    status (*invoke_binary)(void*, span<const std::byte>, buffer&, call_context&);
    status (*invoke_http)(void*, const request&, response&, call_context&);
    std::uint64_t schema_hash;
};
struct match_result;

class http_router {
public:
    static http_router& instance();
    [[nodiscard]] status build();
    [[nodiscard]] match_result match(verb, std::string_view path, std::string* allow) const noexcept;
};
} // namespace crpc::http
```
`route_entry` is constant-initialized and lives at static storage. `build()` runs after `main`, detects conflicts, binds controller instances, and builds the exact and parameter tables. Two entries with the same `(verb, normalized_pattern)` are a `route_conflict` and abort startup with both names and ordinals. A controller with routes but no registered instance also aborts startup.
### 13.2 Matching and binding
The route tree is represented at v1 scale as an exact-match table plus a pre-sorted parameter list; a trie is an implementation option only if it preserves the same priority and conflict rules.

| Step | Rule |
|---|---|
| Normalize | Strip query, fold one trailing slash (`/users/` to `/users`; `/` stays) |
| Exact table | `unordered_map<string, action_record*>` for paths without `:` or `*` |
| Parameter table | Linear scan, pre-sorted by segment count then static-segment count |
| Priority | Static segment > `:param` > `*` |
| Case | Case-sensitive by default; `case_insensitive_routes` opt-in |
| Percent decoding | Per segment; reject `%2F` and `%00`; malformed escape is 400 |
| Query | `&` separated, `+` is space, percent-decoded, repeated keys become a list |
| Verb mismatch | 405 with `Allow` |
| No match | 404 |

DTO binding walks `rtti::fields<req>()` (`[READ:third_party/rtti/include\rtti\reflection.hpp:884]`) and reads field attributes. `FromRoute`, `FromQuery`, `FromHeader`, `FromForm` take the named source; `FromBody` or no attribute takes the body document. A missing required field is 400 with the field name; a missing `std::optional` is left empty; unknown body keys are ignored unless `strict_body = true`. Integer parsing uses `std::from_chars` and must consume every character; enums use `rtti::enum_table<t>` labels first, then numeric values (`[READ:third_party/rtti/include\rtti\reflection.hpp:1837]`). Conversion failure is 400 with `crpc_code = invalid_argument`.
### 13.3 Responses
| Handler result | Default |
|---|---|
| `t` | 200 with body; POST 201, DELETE 200 |
| `void` | 204, no body |
| `rpc_result<t>` | 200 on ok; error mapped by §9.4 |
| `created<t>(location, body)` | 201 with `Location` |
| `no_content()` | 204 |
| Thrown `rpc_exception` | Its `rpc_code`; other exceptions become 500 `internal` |

`Accept` negotiation selects the response codec; no match is 406. `HEAD` executes the GET handler and drops the body. Error bodies use `problem+json` with `instance` set to the request path.
### 13.4 Upload
`multipart/form-data` is parsed incrementally; the body is never fully buffered.

| Limit | Default |
|---|---|
| Parts | 32 |
| Single part | 64 MiB |
| Total upload | 256 MiB |
| Part headers | 8 KiB |
| Spill threshold | 1 MiB |

Parser states: `preamble`, `part_header`, `part_body`, `epilogue`. The body scanner keeps `boundary_len + 4` bytes of rollback so a boundary prefix inside content is not mis-split. Malformed input (missing boundary, unterminated part, illegal CRLF) is 400 and closes the connection. Files above the spill threshold are written to an `O_EXCL` temporary file under the configured root; `file_part` removes it on destruction, cancellation, or error. Client filenames are sanitized and used only as metadata; the on-disk name is random. `Expect: 100-continue` is honored only after routing and size checks; over-limit or unsupported requests get 413/415 without a 100 response.
```cpp
struct file_part {
    std::string name;
    std::string filename;
    std::string content_type;
    std::uint64_t size = 0;
    std::filesystem::path temp_path;
    std::shared_ptr<stream_reader> reader;
};
```
### 13.5 Download and SSE
Download uses `file_response{path, content_type, allow_range}`. It sends `Content-Length` and `Accept-Ranges: bytes`, streams in 64 KiB chunks, and never reads the whole file. `Range: bytes=a-b` yields 206 with `Content-Range`; invalid or unsatisfiable ranges yield 416. Range is ignored when the response is compressed (§14.4).

SSE responses use `text/event-stream`, `Cache-Control: no-cache`, and `X-Accel-Buffering: no`. Event format is `id: <index>`, `event: <name>`, `data: <json>`, blank line; heartbeat is `: ping` every 15 s. Reconnect uses `Last-Event-ID`; the server replays from a bounded buffer of 1024 entries, and beyond that sends `event: reset` followed by a full snapshot. Slow clients are disconnected after a 30 s write stall and their subscription is released.
## 14. Compression <a id="s14"></a>
### 14.1 HTTP content encoding
| Item | v1 |
|---|---|
| Request `Content-Encoding` | gzip, deflate; anything else is 415 |
| Response negotiation | `Accept-Encoding` with q-values; gzip preferred, deflate accepted |
| Minimum size | 1 KiB; smaller responses are not compressed |
| Headers | Correct `Content-Encoding`; `Vary: Accept-Encoding` |
| Chunked | Compress first, then chunk; order is fixed |
| Nesting | Exactly one encoding layer; multiple layers are 415 |
| `identity;q=0` and no alternative | 406 unless `fallback_identity` is set |

Compression and decompression are streaming `stream_reader`/`stream_writer` decorators; nothing is fully buffered. Backpressure propagates from the downstream write to the upstream read.
### 14.2 CRPC frame compression
`flags.COMPRESSED` plus `flags.ALGO` select lz4 block (1); gzip (3) is the compatibility codec; zstd (2) is reserved and not implemented (a decoder rejects it with `protocol_error`). The handshake `compress_bitmap` is the intersection of both sides. `compress_min` defaults to 512 bytes; smaller payloads are sent uncompressed. Compression is per frame payload; a batch is not compressed across frames. A compression failure is fail-closed: the connection closes, never a silent plaintext fallback.

Flow control counts compressed bytes on the wire. `WINDOW_UPDATE` returns compressed bytes, so both sides agree on window accounting.
### 14.3 Large messages
Payloads above `max_frame` are split into frames with `END_STREAM = 0` except the last. Each fragment is compressed independently and is at most 64 KiB before compression. The receiver decompresses and delivers each fragment incrementally.
### 14.4 Bomb protection
| Limit | CRPC frame | HTTP |
|---|---|---|
| Decompressed total | 64 MiB | 64 MiB |
| Single frame | 16 MiB | 8 MiB body |
| Compression ratio | 1000:1 | 100:1 |
| CPU time | 5 ms soft limit per frame | Counted against the request deadline |
| Nested encodings | 1 | 1 |

Over-limit is `resource_exhausted` or `protocol_error` and closes the connection (CRPC) or returns 413/400 and closes (HTTP). Range requests are ignored when `Content-Encoding` is present: the response is 200 with the full compressed entity and no `Accept-Ranges`.
## 15. Resilience and governance <a id="s15"></a>
### 15.1 Pipeline
Six fixed stages, assembled at startup, read-only at runtime:
```cpp
enum class resilience_stage : std::uint8_t {
    rate_limit, bulkhead, breaker, load_balance, retry, deadline
};
class resilience_policy {
public:
    virtual ~resilience_policy() = default;
    [[nodiscard]] virtual rpc_error admit(call_context& ctx) noexcept = 0;
    virtual void on_result(call_context& ctx, const rpc_error& e) noexcept = 0;
};
struct rate_limit_config {
    std::uint32_t rate_per_second = 10000;
    std::uint32_t burst = 1000;
};
struct bulkhead_config {
    std::uint32_t max_concurrent = 1024;
};
struct breaker_config {
    std::chrono::seconds window{10};
    std::uint32_t min_requests = 50;
    double failure_ratio = 0.5;
    double slow_ratio = 0.5;
    std::chrono::nanoseconds slow_threshold{std::chrono::microseconds(200)};
    std::uint32_t half_open_max_probes = 3;
    std::uint32_t recovery_successes = 5;
    std::chrono::seconds open_duration{5};
};
struct retry_config {
    std::uint32_t max_attempts = 3;
    std::chrono::nanoseconds base_backoff{std::chrono::milliseconds(2)};
    std::chrono::nanoseconds max_backoff{std::chrono::milliseconds(200)};
    double jitter = 0.2;
    double budget_ratio = 0.1;
    bool retry_on_connection_error = true;
    bool retry_on_timeout = false;
};
using fallback_fn = rpc_error (*)(call_context&, const rpc_error&) noexcept;
struct resilience_config {
    rate_limit_config rate;
    bulkhead_config bulkhead;
    breaker_config breaker;
    retry_config retry;
    fallback_fn fallback;
    std::chrono::nanoseconds default_deadline{std::chrono::seconds(1)};
    bool fail_fast_on_open = true;
};
```
`admit` runs `rate_limit -> bulkhead -> breaker -> load_balance -> deadline`; `on_result` runs in reverse. `retry` wraps a single attempt and re-runs `load_balance` before each attempt. Total success-path overhead is budgeted at 35-65 ns (A §27.8).
### 15.2 Circuit breaker
```
closed --(failure_ratio >= threshold and requests >= min_requests)--> open
open --(open_duration elapsed)--> half_open
half_open --(recovery_successes consecutive successes)--> closed
half_open --(any probe failure)--> open (reset duration, exponential backoff)
```
| Parameter | Default |
|---|---|
| Window | 10 s, ten 1 s buckets |
| `min_requests` | 50 |
| `failure_ratio` | 0.5 |
| `slow_ratio` | 0.5 |
| `slow_threshold` | 200 microseconds |
| `half_open_max_probes` | 3 |
| `recovery_successes` | 5 |
| `open_duration` | 5 s, exponential to 60 s |
| Granularity | Method and instance enabled; service and connection disabled |

Statistics are per-shard `alignas(64)` buckets with relaxed atomic increments; bucket rotation uses monotonic time only (P12). State transitions are CAS; the CAS winner performs the side effect. `probes_left` is an atomic token count, so only three half-open probes can run. Breaker-open skips retries. Breaker failure accounting uses the logical call result, not each retry attempt.
### 15.3 Rate limit, bulkhead, fallback, retry
| Policy | Default | Over-limit |
|---|---|---|
| Client token bucket | Per `(target, method)`, capacity and rate configurable | `resource_exhausted` with `retry_after` |
| Server token bucket | Per method and per connection | `resource_exhausted` |
| Bulkhead | Per service, per shard | Park the coroutine up to the remaining deadline, then `overload` |
| Overload guard | Per connection 1024, per shard 65536, global 1,000,000 in-flight | Immediate `overload` |
| Fallback | One function per method, called only after final failure | Result marked `degraded=true` |
| Retry | Max 3 attempts, base 2 ms, max 200 ms, jitter 20%, budget 10% | Stop retrying; return the last error |

Retry is allowed only for idempotent calls (explicit flag or idempotency key). `deadline_exceeded` is not retried by default because the side effect may have occurred. A retry attempt must fit within the remaining deadline. A fallback returns `ok` after writing its cached value into the call's response buffer; any other code replaces the original error.
### 15.4 Health, load balancing, node interop
Active health checks run every 5 s (TCP connect, HTTP `/health`, or RPC `Ping`); passive health comes from breaker and connection failures. A recovering instance ramps weight from 1 to its configured value by consecutive successes.

Selection order: same datacenter, healthy, breaker closed, then smooth weighted round-robin. Instance-level breaker open removes the instance from the endpoint set by atomic snapshot replacement.

Nodes use lazy peer connections; there is no full mesh. Inter-node calls are ordinary CRPC calls to the `crpc.registry` service, not a second protocol. Node identity is a persisted UUIDv4.
### 15.5 Registry (Consul-shaped, single authority)
| Operation | HTTP | CRPC method |
|---|---|---|
| Register/renew | `PUT /v1/agent/service/register` | `register` |
| Deregister | `PUT /v1/agent/service/deregister/:id` | `deregister` |
| Health pass/fail | `PUT /v1/agent/check/pass/:check_id` | `heartbeat` |
| Catalog | `GET /v1/catalog/services`, `GET /v1/catalog/service/:name` | `query` |
| Health list | `GET /v1/health/service/:name?passing=true` | `query` |
| KV get/put/CAS/delete | `GET/PUT/DELETE /v1/kv/:key` | `kv_get`, `kv_put`, `kv_cas` |
| Watch | `GET /v1/watch/kv/:key?index=N&wait=5m` | `watch` |

Data model: `service_instance{node, endpoint, service, instance_id, tags, version, weight, incarnation, index, health, lease_expiry}`. `index` increments on every mutation and is the only ordering anchor. TTL leases renew at `ttl/3`; expiry marks `critical`, then removes after another TTL, leaving a tombstone for 2×TTL.

Write authority is a single node. A non-authority write returns `unavailable` with `metadata["leader"]`; R6 has no `not_leader` code, which is recorded in §22. Reads use an immutable snapshot published with `atomic_load`/`atomic_store` of `shared_ptr<const catalog_snapshot>`; an unavailable authority leaves reads on the last snapshot (fail-static) and writes failing (fail-closed).

KV values are base64 on both HTTP and wire. `cas=N` succeeds only when `modify_index == N`; `cas=0` means create-only; conflict is 409. ACL uses a static token table with read/write prefix policies; a missing token when `require_token=true` is 401, insufficient permission is 403. The same check runs on the binary channel.

Watch uses long polling: `index == current` parks the handler coroutine until a change or the wait timeout. A timeout returns 200 with the current value and unchanged index, not 504. One watch per HTTP/1.1 connection; global watch count is capped at 10k.

Two-order convergence: snapshot index `S`; event index `E`. `E <= S` is dropped; `E == S + 1` is applied; `E > S + 1` is buffered with a bound and triggers a snapshot fetch. State never rolls back.
## 16. Observability <a id="s16"></a>
### 16.1 AOP
```cpp
enum class hook_point : std::uint8_t {
    client_before_call, client_after_call, client_encode_before, client_encode_after,
    server_before_call, server_after_call, server_decode_before, server_decode_after,
    conn_open, conn_close, conn_error
};
class interceptor {
public:
    virtual ~interceptor() = default;
    [[nodiscard]] virtual bool on_hook(hook_point p, call_context& ctx) noexcept = 0;
};
class aop_chain {
public:
    void add(hook_point p, interceptor* i) noexcept;
    void freeze() noexcept;
    [[nodiscard]] bool run(hook_point p, call_context& ctx) noexcept;
};
```
`run` begins with `if (hooks_[p].empty()) return true;`, so an unused hook costs about 1 ns. The chain is frozen before serving; interceptors are constructed at startup and must not allocate or block in a hook. An exception inside a hook is caught and converted to `internal`.
### 16.2 Tracing
Trace context is 16-byte `trace_id`, 8-byte `span_id`, 8-byte `parent_span_id`, plus flags. HTTP uses W3C `traceparent` (`00-<32 hex>-<16 hex>-<2 hex>`); CRPC carries the same fields in the call envelope. Parent/child is established by the send/receive pair, never by timestamps. Head sampling defaults to 1% and is decided once at call initiation. Baggage is at most 8 KiB and never affects routing. Exporters are asynchronous with a bounded queue of 8192; overflow drops and counts. With no exporter configured, tracing has zero allocations.
### 16.3 Logging
Hot path: stack formatting plus a per-shard SPSC ring of 8192 entries; no `malloc`, no lock, no syscall, no `std::string`. Format is `{}` style. Level filtering is compile-time (`CRPC_LOG_MAX_LEVEL`) plus a runtime atomic. A single background thread batches entries to `writev`/`WriteFile`, rotates by size or time, and writes UTF-8. Queue overflow drops and increments `crpc_log_dropped_total`. Structured fields include trace id, span id, service, method. Fields whose names contain `token`, `secret`, `password`, or `key` are refused by default. Target cost is at most 150 ns per enabled entry.
### 16.4 Metrics
Counters: `crpc_calls_total{service,method,code}`, `crpc_call_duration_ns` histogram, `crpc_unreliable_dropped_total{reason}`, `crpc_log_dropped_total`, `crpc_retry_attempts_total`, `crpc_breaker_state{key}`, `crpc_breaker_open_total`, `crpc_rate_limited_total`, `crpc_bulkhead_rejected_total`, `crpc_registry_watch_total`, `crpc_push_dropped_total{policy}`. Histograms use fixed buckets and no allocation.
## 17. Memory and performance budgets <a id="s17"></a>
### 17.1 Memory
| Item | Budget | Composition |
|---|---|---|
| Idle connection | <= 2 KiB | Connection object 256 B, lazy read buffer 1 KiB, no write buffer, table entry 64 B |
| Active connection | <= 160 KiB | Read 64 KiB, write 64 KiB, call table pointer |
| Call slot | 128 B | State 8, semaphore 32, result 16, buffer pointers 16, stream/lane 16, callback 40 |
| Shard fixed | <= 8 MiB | 64 handler stacks × 64 KiB = 4 MiB, timer wheel, MPSC, statistics |
| 100k in-flight calls | <= 64 MiB | 100k × 128 B plus mostly empty buffers |
| 10k idle connections | <= 20 MiB | 2 KiB each |
| Subscription table | <= 5 MiB | 100k entries × 48 B |
| Per-lane queue | <= 16 KiB | 1024 entries × 16 B, bounded ring |
| Send ring per connection | <= 256 KiB | Bounded, high-water reclaim |

All queues are bounded. Pools have caps and release memory at low water; a pool may not grow forever. Coroutine stacks are 64 KiB with a guard page; the handler pool is long-lived so the 256-entry default-stack cache is not on the request path.
### 17.2 Performance targets and measurement
| Metric | Target | Status |
|---|---|---|
| Aggregate throughput | 5M RPC/s | Target, not a gate (R10) |
| Per-connection pair, batched | >= 1M RPC/s | Measured 1.05M on the audit host |
| p99 latency | < 100 microseconds | Target, must be measured |
| Cross-shard handoff | 15-30 ns per message when batched | Budget |
| Codec, 64-byte message | < 100 ns encode and decode | Budget |
| Resilience pipeline | 35-65 ns per successful call | Budget |

Benchmarks must report message size, connection count, thread count, batching, toolchain, CPU model, and p50/p99/p999. Results must use an independent request counter. Converting "64 frames per RTT" into RPC/s is forbidden (R10). Measurement uses `bench/bench_rpc.cpp`, a 10 s warmup, five 30 s runs, and the median of the five.
## 18. Concurrency correctness <a id="s18"></a>
### 18.1 Lock order
| Level | Lock | May acquire while held |
|---|---|---|
| 1 | `registry_catalog_lock` (authority) | None |
| 2 | `config_lock` (reload) | None |
| 3 | `shard::inbox_lock` (MPSC internal) | None |
| 4 | `connection::send_ring_lock` | None (single writer; usually absent) |
| 5 | `dedup_table_lock` | None |
| 6 | breaker state (atomic, not a lock) | Not applicable |

Only increasing levels may be acquired. A shard lock is never held while acquiring another shard's lock. No I/O, user code, coroutine suspension, `sem_post`, log sink, or blocking allocation occurs while a lock is held. Paths that cannot be proven acyclic use `try_lock` with a timeout and an explicit recovery path.
### 18.2 One-to-one pairing
| Acquire | Release | Recycle point |
|---|---|---|
| `call_slot` from freelist | Return to freelist | Terminal state and refcount zero |
| Connection slot | Slot free, generation++ | Close complete |
| Subscription entry | Table erase | Unsubscribe, expiry, or disconnect TTL |
| Timer node | Heap remove | Call terminal state |
| `sem_wait` | `sem_post` | Exactly once via `try_finish` CAS |
| Sink `push` | `completion::complete` | Write complete, error, or cancel |
| Temp upload file | `std::filesystem::remove` | `file_part` destruction or cancel |
| Registry instance | Deregister or TTL tombstone | Lease expiry |
### 18.3 State machine convergence
```cpp
enum class call_state : std::uint8_t { idle, pending, completed, canceled, timed_out, conn_lost, recycled };
bool try_finish(call_slot& s, call_state terminal, rpc_error e) noexcept {
    std::uint32_t expected = static_cast<std::uint32_t>(call_state::pending);
    if (!s.state.compare_exchange_strong(expected, static_cast<std::uint32_t>(terminal),
                                         std::memory_order_acq_rel)) {
        return false;
    }
    s.result = std::move(e);
    coro_rt_sem_post(s.rt, &s.sem);
    return true;
}
```
Every terminal path (response, timeout, cancel, connection loss, rejection) calls `try_finish`. The first CAS wins; all later paths observe a terminal state and drop. Because `stream_id` is never reused, a late response can only hit a live slot or a terminal slot, never a recycled call with a different generation. This gives order-independent convergence: arrival order does not affect the terminal state.
### 18.4 D1-D20 defect mapping
D1-D15 are the RTTI integration defects from B §8; D16-D20 are the corocc integration defects A §1.1 D1-D5.

| ID | Defect | Mitigation | Test |
|---|---|---|---|
| D1 | No method attributes | CRPC POD route table | `test_router_conflict` |
| D2 | Per-TU duplicate method registration | Dedup by `(controller type, method name)` | `test_method_registry` |
| D3 | Attribute tables are dynamically initialized | Build/freeze after `main` | `test_static_init_order` |
| D4 | `fields<T>()` accessor dynamic init | Hot path uses `fields_of<T>::members` | `test_binary_roundtrip` |
| D5 | `reflect_method` is synchronous and has no context | Call `&T::method` directly | `test_invoke` |
| D6 | `result<T>` requires default construction | CRPC `rpc_result` | `test_move_only_response` |
| D7 | `error_info` has no message text | CRPC `rpc_error` | `test_error_mapping` |
| D8 | JSON `parse_as` loses offset | Two-phase parse | `test_json_offset` |
| D9 | Text `serialize<T>` swallows errors | Two-phase `to_value` + `serialize(document)` | `test_codec_errors` |
| D10 | XML zero-copy views outlive the body | Copy into arena or keep body alive | `test_xml_lifetime` |
| D11 | `u64 >= 2^63` loses precision in text | Binary native; JSON as string | `test_u64_text` |
| D12 | Derived `members` exclude base fields | crpc-binary rejects derived types | `test_derived_rejected` |
| D13 | protobuf/FlatBuffers need a hand-written schema | Require `schema<t>`; reject empty schema | `test_schema_required` |
| D14 | FlatBuffers typed path builds a DOM | No zero-copy claim | benchmark report |
| D15 | First `method_table<t>()` call allocates | Prewarm in `runtime::init` | `test_prewarm` |
| D16 | LOOP mode exposes no nearest deadline | CRPC timer wheel | `test_timer_precision` |
| D17 | Driver wake is only a doorbell | MPSC plus eventfd/IOCP post | `test_cross_shard_wake` |
| D18 | LOOP runtime has only `worker[0]` | One runtime per shard | `test_shard_isolation` |
| D19 | A runtime cannot be pumped concurrently | `pump_active`; single shard thread | `test_pump_exclusive` |
| D20 | corocc has no I/O completion reaping | `epoll_wait` / `GQCSEx` batch arrays | `test_batch_completion` |
## 19. Test strategy <a id="s19"></a>
### 19.1 Layers
| Layer | Content | Gate |
|---|---|---|
| Unit | Frame codec, varint, byte order, router, HTTP parser, multipart, codecs, error mapping, breaker, token bucket, dedup | Every test green, x86 and x64 |
| Conformance | Same assertions on fake, inproc, and TCP-loopback backends | No semantic divergence |
| Integration | Two-process RPC, HTTP/1.1, h2, TLS, registry, upload/download, SSE, push | Green on Windows and Linux |
| Fault injection | Drop, duplicate, reorder, delay, reset, half-open, short write, short read | Reproducible by seed |
| Fuzz | Frame, HTTP/1.1, h2 HPACK, JSON/XML/TOML/INI, multipart, router | No crash, no over-limit allocation |
| Sanitizer | ASan/LSan, UBSan, TSan, optional MSan | Zero reports |
| Long run | 1M calls, 1000 start/stop, RSS plateau, active counts zero | No growth |
### 19.2 Law-to-verification matrix
| Law | Verification | Test asset |
|---|---|---|
| No real-time global state | Reorder injection plus terminal-state assertion | `test_inproc_reorder` |
| Half-open is normal | Timeout then check `unknown_side_effect` | `test_timeout_semantics` |
| Peer writes diverge | Two writers, one succeeds | `test_single_writer` |
| External effects are irrevocable | Retry, handler executes once | `test_idempotency` |
| Consensus is last resort | Authority down: writes fail, reads continue | `test_registry_failover` |
| Synchronized retry storm | 1000 clients retry, server QPS bounded | `test_retry_storm` |
| Monotonic anchor | `lane_seq` monotonic, dedup hits | `test_dedup_window` |
| Rule accumulation | Unknown transition rejected | `test_state_machine` |
| Asymmetric safety | Illegal frame/version/cert rejected | `test_fail_closed` |
| Event before data | Both arrival orders converge | `test_watch_two_order` |
| NULL cannot pass through | Deregister, stale heartbeat does not revive | `test_tombstone` |
### 19.3 PIN protocol
Every defect found during implementation is pinned before it is fixed:

1. **Pin**: write a failing test that reproduces the defect exactly, with the observed error, not a paraphrase.
2. **Prove red**: run it and record the failing output in the task report.
3. **Fix**: minimal change; no unrelated refactoring.
4. **Flip to green**: the same test passes; keep it as a permanent regression guard.
5. **Never delete or weaken** the pinned test; extend it if the defect has more branches.

The project rule is that a failure must be proven before the fix, and that the test must not be removed afterwards.
### 19.4 Sanitizer matrix
| Tool | Targets | Job |
|---|---|---|
| ASan + LSan | Frame parsing, buffer reuse, slot recycling, close races, multipart, upload cancel | Linux Clang Debug |
| UBSan | Endian codec, varint, length math, alignment, shifts, 32-bit truncation | Linux Clang, `-fno-sanitize-recover=all` |
| TSan | Multi-shard, MPSC, call-slot state, breaker stats, registry watch | Linux Clang `-O1 -g` |
| MSan (optional) | Uninitialized reads on decode | Linux Clang |

corocc already provides fiber hooks: `COROCC_ASAN` and `COROCC_TSAN` build options, ASan context switching, and TSan fiber annotations. CRPC builds the dependency with those options; it does not add its own fiber annotations.
## 20. Build and CI <a id="s20"></a>
### 20.1 CMake targets
One library target is built, from the whole of `src/`:

| Target | Content |
|---|---|
| `crpc` | the library: core, io, wire, rpc, http, http2, cluster, mvc, codecs, files, push, db, mq, redis, ws, ipc, fs, orm, game, algorithm, crypto, text |
| `crpc::crpc`, `crpc_core` | aliases of `crpc`, for `target_link_libraries` and for tests |
| `crpc_mpmc` | INTERFACE target carrying the vendored mpmc include directories |

Everything else is one executable per source file, all written to the build
root: `crpc_test_<file>` (and `crpc_test_<dir>_<file>` for nested test
directories), `crpc_example_<stem>`, and `crpc_bench_<file>`. Subsystem
libraries were considered and are not used: they would not change the
dependency graph (the umbrella links all of it anyway) and would only add
per-target link lines to maintain.

Options: `CRPC_BUILD_TESTS`, `CRPC_BUILD_BENCH`, `CRPC_BUILD_EXAMPLES`,
`CRPC_BUILD_SHARED`, `CRPC_WITH_OPENSSL`, `CRPC_WITH_ODBC`,
`CRPC_WARNINGS_AS_ERRORS`, `CRPC_ENABLE_LTO`,
`CRPC_SANITIZER=none|asan|ubsan|tsan`, `CRPC_RTTI_DIR`, `CRPC_COROCC_DIR`.
Anything not in that list does not exist. In particular the compressors are
built in rather than optional, the 32-bit frame cap is a compile-time constant,
and there are no fuzz targets -- so there is nothing to switch on for them.

Zero-warning policy: MSVC `/W4 /WX`; GCC/Clang `-Wall -Wextra -Wpedantic -Werror`; additionally `-Wconversion -Wsign-conversion` where practical. Warnings are never disabled for CRPC code; third-party suppressions must name the library and the reason.
### 20.2 Scripts and CI
```
scripts/build.ps1 -Config Debug|Release|Both -Arch x86|x64 -Sanitizer none|asan|ubsan|tsan -Jobs N -Clean
scripts/build.sh  --config debug|release|both --arch x64 --sanitizer none|asan|ubsan|tsan --jobs N --clean
scripts/test.ps1  -Config Debug|Release -Arch x86|x64 -Sanitizer none|all -Filter <substring> -Repeat N
scripts/test.sh   --config debug|release --sanitizer none|all --filter <substring> --repeat N
```
CI jobs: `linux` (GCC Debug/Release, Clang Debug, and GCC/Clang shared),
`linux-sanitizers` (ASan + UBSan + TSan), `windows` (MSVC x64 and x86,
Debug/Release), `shared` (MSVC shared with the consumer link gate and the
export gate), `linux-bench-compile` and `bench-compile` (compile `bench/` on
both platforms, never run it), and `examples`. There is no fuzz job and no
golden-bytes job: byte-exact expectations live in the unit tests
(`test_frame.cpp`, `test_protocol*`, `test_redis_resp.cpp`, ...) rather than in
a separate comparison step. CI calls the same scripts developers use; it does not duplicate build logic. Logs are UTF-8; failures exit non-zero; builds are idempotent.
## 21. v1 scope, v1.1, milestones <a id="s21"></a>
### 21.1 v1 (R5, authoritative)
| Area | v1 content |
|---|---|
| Transport | `source`/`sink` (R1), epoll ET, IOCP, inproc |
| Concurrency | Shard-per-core, one LOOP runtime per shard, MPSC plus doorbell, timer wheel |
| Coroutines | Reader coroutine, 64 handler coroutines per shard with 64 KiB stacks |
| Wire | 16-byte header, varint, handshake, flow control, lanes, reliable/unreliable, ordered/plain, compression |
| RPC | `call<t>` sync/async, deadline propagation, cancel, idempotency key, retry, breaker, rate limit, bulkhead, fallback, health checks |
| Registry | Consul-shaped register/deregister/health/catalog/KV+CAS/watch/ACL, single authority, monotonic index, two-order convergence |
| Cluster | Load balancing, node interop |
| HTTP | HTTP/1.1, HTTP/2 (h2 and h2c), HTTPS via OpenSSL |
| MVC | GET/POST/PUT/DELETE/PATCH, parameter binding, problem+json |
| Codecs | crpc-binary, JSON, XML, INI, TOML, protobuf, FlatBuffers (all wired; protobuf/FlatBuffers require user `schema<t>`) |
| Files | Streaming multipart upload, temp-file RAII, download with Range/206 |
| Compression | HTTP gzip/deflate; CRPC lz4 block (+gzip compatibility in a later slice); zstd reserved, not implemented |
| Push | `push_hub`, subscriptions, streaming, SSE, long polling |
| Observability | AOP, W3C traceparent, high-performance logging |
| Engineering | One-command scripts, sanitizer matrix, zero warnings |
### 21.2 v1.1 (R5)
Gossip/Raft strong consistency, dynamic policy distribution, io_uring/shm/RDMA, ARM64/BE/Android/macOS. WebSocket is not in v1 or v1.1: SSE plus push covers the unidirectional requirement (A §32, B §19.2).
### 21.3 Milestones and acceptance gates
| Milestone | Deliverable | Exit gate |
|---|---|---|
| M0 | Skeleton, build scripts, core types, CI | Zero warnings x86/x64; core unit tests |
| M1 | source/sink, epoll, IOCP, inproc, timers | Conformance across three backends; close-race tests |
| M2 | Wire, handshake, calls, slots, deadline, cancel | Golden bytes; exactly-once terminal state; no slot leak |
| M3 | HTTP/1.1, MVC, binding, upload/download, SSE | HTTP parser fuzz; multipart cancel cleanup |
| M4 | HTTP/2, HPACK, TLS | h2spec mandatory sections; TLS interop |
| M5 | Resilience, registry, cluster, push | Retry storm; registry two-order; push replay |
| M6 | All codecs, observability, hardening | Sanitizer zero; long run; benchmark report |

Release gates (R11): zero errors and zero warnings on x86 and x64; ASan/LSan/UBSan/TSan zero reports; fuzz no crashes; all tests pass on both architectures; scripts reproduce the build; performance measured and reported honestly.
## 22. Open issues and risks <a id="s22"></a>
| # | Issue | Impact | Blocking point / plan |
|---|---|---|---|
| O1 | R6 has no `permission_denied`, `unauthenticated`, `already_exists`, `out_of_range`, `unimplemented`, `data_loss`, or `not_leader` | HTTP 401/403/409/422/501 and registry leader hints cannot be expressed as `rpc_code` | Bridge: HTTP layer sets the status directly and records `metadata["http.status"]`; registry returns `unavailable` plus `metadata["leader"]`. Requires ruling confirmation before M3/M5 freeze. |
| O2 | R1 defines `source::on_data` returning a credit but names no resume entry point | Without a resume path, ET retains a tail forever | `connection::resume_read()` sits outside `source`/`sink`, which keeps the two-interface rule intact. |
| O3 | A §30.1 flags differ from B §4.2.1 flags | Reusing B's constants would corrupt the wire | A is adopted; B's bit assignment is superseded. Any existing B-based code must be rewritten. |
| O4 | A §31.1 puts the compression algorithm in flags bits 5-7; B §21.6 puts it in the first payload byte | Interoperability break | A is adopted; B's layout is superseded. |
| O5 | Frame type numeric values were never fixed by an earlier document | Independent implementations could disagree | Values are fixed in §7.3 and committed as golden bytes; any change is a wire break. |
| O6 | `max_frame` default differs: A 16 MiB, B 1 MiB | Memory per connection versus large-message support | A §30.4 adopted: 16 MiB default, 64 MiB hard cap, 8 MiB default on 32-bit builds. Revisit after memory measurements. |
| O7 | Idempotency table is memory-only | Exactly-once holds only within one process epoch | v1 documents the boundary; persistence is v1.1. |
| O8 | Registry is single-authority | Authority failure stops writes | v1 accepts fail-closed writes and fail-static reads; gossip/Raft is v1.1 (R5). |
| O9 | HTTP/2 plus seven codecs are in v1 (R5) while audit B estimated 15-25 person-days for h2 alone | Schedule risk, not a design risk | M4 is a dedicated milestone; h2spec is a release gate. |
| O10 | protobuf and FlatBuffers require user `schema<t>` specializations | An unregistered type encodes to zero bytes | Registration fails when schema count is zero; this is documented in §10.4. |
| O11 | Derived types are unsupported in crpc-binary | A derived DTO fails to compile | `static_assert` with a clear message; text formats still support derived types. |
| O12 | Windows high-resolution timers require Windows 10 1803 or later | Older hosts fall back to coarser timers and miss the p99 target | Detect `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` failure and report it; do not silently claim the target. |
| O13 | Performance targets are unverified on the build host | 5M RPC/s and p99 < 100 microseconds may not be reached | R10 requires honest measurement; targets are not gates. |
| O14 | TSan is not available with MSVC | Windows concurrency races may escape detection | Linux Clang TSan is the authoritative job; MSVC relies on ASan plus stress tests. |
| O15 | `then` uses a 32-byte small buffer | Larger captures require user allocation | Documented; `std::function` is allowed only outside the hot path. |
| O16 | corocc's default stack cache holds only default-size stacks | 64 KiB handler stacks are not cached, but workers are long-lived | No per-request spawn; pool size is fixed at startup. |
| O17 | HTTP/1.1 pipelining is serial | A pipelined client cannot exceed one in-flight request | Documented; HTTP/2 is the multiplexed path. |
| O18 | Range and compression are mutually exclusive | A compressed download ignores `Range` | Documented; `allow_range` is false when compression is active. |
| O19 | No WebSocket | Browser bidirectional push is unavailable | SSE plus CRPC push covers unidirectional; v1.1 does not add WebSocket. |
| O20 | Registry ACL is static tokens | No dynamic policy or roles | v1 scope; dynamic policy is v1.1 (R5). |
