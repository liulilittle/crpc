# Changelog

[简体中文](CHANGELOG.zh-CN.md) | **English**

## Unreleased

### Added (MINOR, symbols only)

- `client::resilience_statistics` and `client::resilience_stats()`: additive
  observability for the client resilience pipeline (`rate_limited`,
  `bulkhead_rejected`, `local_gate_rejected`, `endpoint_open`, `lb_unavailable`,
  `pre_send_retries`, `probe_admitted`, `retries_delayed_by_hint`,
  `bulkhead_in_use`). No existing structure changes: `client::statistics` keeps
  its field order, size and meaning, and no C ABI entry point changes. See
  [abi.md](abi.md) §4 and §8.
- `resilience::fallback_statistics` and `client::fallback_stats()`: the
  seven-counter snapshot (`ineligible`, `skipped_deadline`, `attempted`,
  `failed`, `canceled`, `overrun`, `applied`) for the fallback primitives in
  `crpc/resilience/fallback.hpp`. The primitives are header-only and are not
  wired into the call path in this slice, so every counter stays zero. The
  by-value return is the only new symbol. See [abi.md](abi.md) §4.

### Compile-time layout guards

- The internal translation unit `src/abi_check.cpp` (in no header, exporting no
  symbol) now asserts the layout of a subset of the public records that cross the
  ABI boundary: standard-layout for 13 records; an exact member count for 14
  records (`CRPC_MEMBER_COUNT`), which catches an append, an insertion or a
  removal; relative member chains (`CRPC_STEP`, `CRPC_STEP_SAME`,
  `CRPC_STEP_MIXED`, `CRPC_AFTER`) that catch a reorder or a rename; the total
  width and last-field position of `http_statistics`; and, on x86 and x64 only,
  absolute `sizeof`/`offsetof` anchors (`CRPC_ABS_SIZE`, `CRPC_ABS_OFF`). The
  member count is what sees a tail append that x86 tail padding would hide from an
  absolute `sizeof`.
- [abi.md](abi.md) §4 now states the scope predicate for "crosses the ABI
  boundary" and lists the 19 in-scope records that still have no compile-time
  guard. `crpc_test_abi_docs` rescans `include/crpc/**/*.hpp` and
  `src/abi_check.cpp`, so a new cross-boundary record must be guarded or added to
  that list, or the test fails.
- Documented blind spots: the member count does not validate names (a rename is
  caught by a relative chain where one exists; in `rpc_error` it is caught by
  the member-type assertions in `src/abi_check.cpp`); among the 14
  member-counted records, `rpc_error` is the only one without a relative chain,
  so a reorder inside it is not caught by this unit. Its constructors name only
  `code`, `message` and `retryable`, so they do not catch a rename of
  `retry_after` or `metadata`; the unit's five `decltype` assertions name all
  five members, so a rename of any of them fails there. The absolute `sizeof` of
  `rpc_error` and `status` is deliberately not pinned because it follows the
  standard-library configuration. See [abi.md](abi.md) §4.
- Impact: MINOR (build-time validation only). No ABI break: no public struct
  layout changes and no exported symbol is added. No runtime behaviour change;
  the build is tighter, because a layout change these guards cover now fails to
  compile. See [abi.md](abi.md) §4 and §8.

### Client resilience pipeline

- Per-endpoint rate limiter and bulkhead gates, configured with
  `builder::per_endpoint_rate()` / `builder::per_endpoint_bulkhead()` and
  disabled as a group with `builder::resilience_enabled(false)`.
- Executed order is `select + allow -> probe check -> rate -> bulkhead ->
  register -> pool/slot -> retry -> deadline`; a half-open probe skips the rate
  and bulkhead gates while the pool and in-flight caps still apply.
- Local gate rejections ignore the breaker permit, carry the limiter/bulkhead
  `retry_after`, and are never retried automatically.
- `retry_after` from a peer is a lower bound on the retry delay
  (`max(backoff, hint)`), capped by the call deadline; the retry budget is
  charged only after that check. `retries_delayed_by_hint` counts only retries
  whose delay the hint actually determined.
- The selection loop tracks tried candidates with a per-endpoint bitmap that
  grows beyond 512 endpoints instead of silently ignoring higher indexes.
- `call::cancel()` interrupts a retry wait immediately (condition variable on
  plain threads, 5 ms slices on a shard) and serializes access to the low-level
  call handle with the completion registration.

### Admission stages and HTTP Retry-After

- New header-only `crpc/resilience/admission.hpp` gives the admission vocabulary
  one home: `resilience::stage_kind` (10 stages),
  `resilience::admission_decision` (`admit`, `reject`, `probe_bypass`;
  `probe_bypass` is reserved and no stage consumes it yet),
  `reject_code(stage_kind, bool deadline_met = true)` - the single
  stage-to-`rpc_code` table, including both rate outcomes (`resource_exhausted`
  while the deadline is still reachable, `deadline_exceeded` otherwise) - and
  `fallback_eligible(rpc_code, stage_kind, bool one_way)`, which only answers
  whether a (code, stage) pair may be degraded; fallback execution is not
  implemented yet.
- The breaker, service/endpoint rate and bulkhead, service-capacity, deadline and
  pre-send dispatch rejection branches converted in this slice now take their wire
  code from that table; the per-connection flood bucket, the per-connection
  in-flight cap, the client connection-pool capacity and the "cannot track endpoint
  candidates" branch still carry their literals. For the converted branches the
  codes are unchanged, as are the messages, `retryable`, `retry_after` and the
  per-stage counters: this part is a refactor with zero behaviour change.
- HTTP MVC error responses now carry `Retry-After` when
  `rpc_error::retry_after > 0`: milliseconds rounded up to whole seconds with a
  floor of 1, and the header omitted for a zero hint. The status-code mapping is
  unchanged.
- Impact: MINOR (new header and inline API; no exported symbol). No ABI break:
  the header is header-only, `stage_kind`/`admission_decision` are not cross-ABI
  types, and no existing record changes layout. Behaviour change is limited to
  the additive HTTP `Retry-After` header; RPC wire codes and messages are
  unchanged.

### HTTP long polling

- A server-owned housekeeping timer reclaims idle subscribers even when no later
  poll arrives. It is armed by `start()`, cancelled by `stop()` before the hub is
  torn down, and re-armed by its own callback.
- The front replay bound is clamped to the queue bound at `start()`
  (`max_replay < max_messages` is raised, the reverse is rejected). The count
  bound and the per-subscription byte budget are independent; a cursor gap that
  exceeds either answers 409 Restart instead of silently dropping unacknowledged
  data. `http_statistics::longpoll_replay_window` reports the effective bound and
  is zero while the server is not running (including a failed `start()`).
- `stop()` keeps the listener configuration so a later `start()` rebinds the same
  endpoints. The stored `backend_factory` pointers are borrowed: a backend must
  outlive the server, including every `stop()` -> `start()` cycle (documented on
  `http_server::builder::add_listener()` and `server::builder::add_listener()`).
  This also fixes `rpc::server`, whose second `start()` used to return ok while
  binding nothing.

### Exception and compiler-feature contract

- In the supported no-exception consumer configuration (prebuilt exception-enabled
  library, `CRPC_HAVE_RTTI=0`), all 52 public headers compile with exceptions
  disabled (`-fno-exceptions`, MSVC `/EHs-c-`). With the reflection dependency
  (`CRPC_RTTI_DIR`) and `-fno-exceptions`, the header stage still fails (see
  [building.md](building.md) §3). Rebuilding the library without exceptions is
  still unsupported: 11 of the 46 `src/` translation units fail with Linux GCC,
  12 with MinGW GCC and 13 with MSVC.
- The exception contract is stated per boundary. A header-inline boundary
  (`obs::interceptor_chain::run`, `mvc::invoke_http`) follows the consumer's
  exception flags; with exceptions enabled a throw is caught and becomes
  `rpc_code::internal` / HTTP 500 `internal error`. A library translation-unit
  boundary (`mvc::router::handle`, `rpc::dispatcher::dispatch`) follows the
  library's flags; a library built with exceptions catches a throwing handler and
  turns it into 500/`internal`. Without exception support a throw is outside the
  library's contract and in practice terminates. See
  [architecture.md](architecture.md#s15).
- New macro `CRPC_CAN_USE_TYPEID`: it is derived from the compiler
  (`__cpp_rtti`, and `1` for MSVC without clang-cl), and defining it externally
  is a hard `#error`. Every `typeid` use is behind it; when compiler RTTI is
  unavailable the type-erased codec path returns `failed_precondition` (HTTP 409)
  instead of forming `std::type_info`. `-fno-rtti` support remains limited: with
  the reflection dependency (`CRPC_RTTI_DIR`) the build fails inside that
  dependency, and the guarded fallback is currently unreachable through a
  documented configuration; it is kept for a future toolchain. See
  [building.md](building.md) §3.
- Impact: MINOR (new macro and new supported consumer configuration: no
  exceptions, `CRPC_HAVE_RTTI=0`). No ABI break: no layout or symbol change; the
  macro only selects inline branches at compile time. Behaviour change: a consumer
  can now build its own translation units without exceptions; at a header-inline
  boundary (`run`, `invoke_http`) a throwing hook or handler leaves the call site
  instead of being caught, while a library translation-unit boundary still follows
  the library's flags.

### ABI breaking: `http_statistics` layout

- Relative to the released 1.0.0 (`bd006b2`), `http_statistics` gained 19
  fields in this unreleased cycle: the whole long-poll block
  (`longpoll_waiters`, `longpoll_subscribers`, `longpoll_replay_window`,
  `longpoll_events`, `longpoll_timeouts`, `longpoll_ack_clamped`,
  `longpoll_conflicts`, `longpoll_restarts`, `longpoll_overloaded`) plus ten
  SSE/compression/download counters. `sizeof` grew from 72 (9 fields) to 224
  (28 fields); the nine fields 1.0.0 shipped keep their offsets (0 … 64).
  Adding fields to a public struct is a MAJOR change ([abi.md](abi.md) §1/§4).
- Inside that cycle `longpoll_replay_window` was first placed at offset 168 and
  later moved to the end (offset 216); the six counters that followed it each
  moved 8 bytes earlier (`longpoll_events` 176→168 … `longpoll_overloaded`
  216→208). `sizeof` was already 224 before and after that move because the
  struct had no trailing padding then either. No released header ever carried
  the offset-168 layout (no tag, no 1.0.1), so the internal move is not itself a
  break for published consumers; the 72 → 224 growth is.

### Behaviour change: COMPRESSED frames are no longer delivered as plaintext

Frame-level compression is now implemented for the LZ4 block codec (`ALGO = 1`) and the frame layer
fails closed everywhere else. The previous behaviour was unsafe: `encode_frame` accepted a
`COMPRESSED` flag and wrote the payload uncompressed, `decode_frame` and `frame_decoder` returned the
compressed bytes as if they were the payload, and any algorithm id was accepted. A `COMPRESSED`
`PING` with an 8-byte payload could even be echoed back as a plaintext `PONG`.

What changed:

- `encode_frame` rejects a `COMPRESSED` flag or a non-zero `ALGO` with `invalid_argument`; a
  compressed frame is produced only by `encode_frame_compressed`.
- `encode_frame_compressed` applies the same header validation as `encode_frame` before it
  writes a byte (known type, stream scope, `stream_id` bit 31, `ONE_WAY`/`UNRELIABLE`,
  `ext`/`CRC32C`, `payload_len <= max_frame`) and takes `max_frame` as an argument; a
  violation is `invalid_argument` with nothing written on both the compressing and the
  identity branch. A non-zero caller-supplied `ext` without `CRC32C` is now rejected
  instead of being silently zeroed; with `CRC32C` set the field carries the payload
  checksum by design, on both the plain and the compressed form.
- `decode_frame` rejects a `COMPRESSED` frame with `protocol_error` (it has no output buffer);
  `frame_decoder` decompresses in copying mode and rejects in borrowing mode. A decoded frame's
  header is normalised: `COMPRESSED`/`ALGO` cleared, `payload_len` and `CRC32C` rewritten for the
  logical payload.
- zstd (2) and gzip (3) are recognised ids that fail closed in this build; `4`-`7` are rejected.
- `COMPRESSED` is only accepted on `REQUEST`, `RESPONSE`, `DATA`, `ONE_WAY` and `PUSH`. A
  `COMPRESSED` control frame - including `PUSH_ACK` - is `protocol_error`.
- A malformed compressed payload on a `ONE_WAY` frame is an explicit drop: the decoder returns
  `result::frame_dropped`, `error()` stays `ok`, and the reason is reported through
  `frame_compression_info::drop_reason`. The drop criterion is the frame type, not the `UNRELIABLE`
  flag, so a peer cannot downgrade a fatal violation on a `REQUEST` into a lost request.
- `frame_decoder::result` gained `frame_dropped` (appended, so existing values keep their numbers).
  Code that switches over `result` without a `default` must add the case: GCC/Clang report
  `-Werror=switch`; MSVC `/W4` does not warn.

Compatibility: no production path writes a `COMPRESSED` frame, and both production decoders are in
borrowing mode, so no existing peer changes behaviour. A peer that hand-crafts a compressed frame
now gets a protocol error instead of silent misdelivery - that is the intended tightening. See
`docs/protocol.md` §24.

## 1.0.0 - 2026-10-19

The first CRPC release. It covers the core primitives, the CRPC call/server
facade and the HTTP/1.1, HTTP/2 and HTTPS front ends. It is not a claim that
every design target is met; the measured gaps are listed under known
limitations.

### Capabilities

- **Transports**: in-process, edge-triggered epoll and IOCP backends behind the
  two-interface `source`/`sink` contract, with credit-based backpressure and
  exactly-once completions.
- **Coroutines and shards**: the `runtime` shard primitive (corocc LOOP runtime,
  bounded cross-shard inbox, bounded handler ready queue, 64-handler coroutine
  pool, monotonic timer wheel) and coroutine call syntax in `client_facade`;
  a nofiber fallback when corocc is absent.
- **Protocol**: multiplexed CRPC binary frames, crpc-binary codec, typed
  dispatcher and service registry.
- **HTTP/1.1 and HTTP/2**: message parser/serializer, chunked framing,
  HPACK, h2c and h2, HTTP server facade, keep-alive and request limits.
- **HTTPS**: OpenSSL TLS decorator with ALPN, enabled by `CRPC_WITH_OPENSSL`.
- **MVC**: attribute-declared routes, DTO binding and response mapping.
- **Serialization**: crpc-binary plus the RTTI-driven JSON, XML, INI, TOML and
  schema adapters.
- **Push primitives**: topics, subscriptions, bounded overflow policies,
  reliable replay and streaming shapes.
- **Resilience primitives**: circuit breaker, rate limiter, bulkhead, load
  balancing and retry budget, with load balancing, retry and breaker wired into
  the client facade.
- **Observability**: W3C trace context, interceptor primitives, structured
  asynchronous logger and a bounded tracer; AOP is not yet wired into calls.
- **Files and compression**: multipart parsing, upload spill and ranged
  download, plus an HTTP gzip/deflate streaming content-coding primitive that
  is not yet wired into the HTTP server path.

### Fixes in this release

- **Borrowed-source lifetime contract**: bound sources are tracked by
  `bound_guard`; a connection never delivers into a source that is being
  destroyed.
- **Server and client unbinding**: `server` and `client_facade` unbind and wait
  for in-flight delivery before destroying the source they borrowed.
- **Timer keep-alive**: timer callbacks hold a keep-alive reference so a
  callback cannot run against a freed context during pop/shutdown races.

### Verification status

- MSVC x64 Debug and x86 Debug: 26/26 CTest suites pass with zero warnings.
- TSan: 26 suites, 0 races (nofiber/Linux configuration).
- ASan/LSan: 26 suites, 0 sanitizer reports (nofiber/Linux configuration).
- Independent dual audit: signed off at the audited baseline with no open
  P0/P1 defect.
- Performance evidence and measured gaps are recorded in
  [performance.md](performance.md).

### Known limitations

- Aggregate throughput target of 5M RPC/s is not met: best measured is 0.275M
  RPC/s (in-process, MSVC, pipeline depth 64).
- IOCP loopback p99 is 170.2 us, above the 100 us target.
- The call slot is 440 bytes, above the 128-byte budget.
- IOCP is Windows-only and has no Linux runtime validation.
- Cross-platform and cross-endianness verification (ARM64, big-endian, macOS)
  has not been done.
- Not complete end to end: `PUSH`/`PUSH_ACK` and HTTP SSE/long polling, HTTP
  compression and download wiring into the server path, a server-side
  idempotency/dedup table, and a network registry service.
