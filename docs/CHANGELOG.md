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
