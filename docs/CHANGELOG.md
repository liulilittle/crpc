# Changelog

[简体中文](CHANGELOG.zh-CN.md) | **English**

## Unreleased

### MySQL: transaction state follows the server, not a local guess

- `in_transaction()` now reports the server's `SERVER_STATUS_IN_TRANS` from the last completed command instead of a local flag that only `begin()`/`commit()`/`rollback()` maintained. Consequences for a session running with `autocommit = 0`: the pool rolls back the implicit transaction instead of handing it to the next borrower; `commit()` commits it instead of returning `failed_precondition`; and a DDL statement that implicitly commits no longer leaves a phantom transaction behind. Two flags keep the two questions apart: `explicit_txn_` (did *we* begin one) gates `begin()`, `server_txn_` (did the server say so) answers the pool. An `ERR` packet carries no status word, so after a failed statement the previous status stands; when the session runs with `autocommit = 0` the transaction survives the error, so the connection is treated as having uncommitted work and the pool rolls it back instead of handing it on.

### MySQL: remove the private, uncalled `set_autocommit`

- Removed the private, uncalled `mysql::connection::set_autocommit`. Three private helpers (`apply_status`, `apply_error`, `has_uncommitted_work`) take its place. `__declspec(dllexport)` on a class exports its private members too, inline definitions included, so the net effect on a shared build is one fewer and three more private symbols. No public API changes: none of them can be named by a consumer.

### Test timing gates: keep the assertions that can fail, drop the ones that cannot

- `client_cancel_during_backoff` (`tests/test_server_client.cpp`) now measures
  the wake-up inside the waiter thread: `t_done` is taken immediately after
  `get()` returns, so `join()` latency is no longer charged to the interval.
  The retry backoff was raised from 100 ms to 1000 ms and the bound is 250 ms,
  which puts an immediate wake (microseconds) and a full backoff (one second)
  four times apart from the midpoint. The interval is also asserted
  non-negative: a negative value means the measurement is broken, and an
  upper-bound-only check would silently pass it. The case is documented as
  plain-thread-only: calling `get()` from a shard thread that is not inside a
  coroutine fails with `failed_precondition`, so the case would never reach the
  path it exists to pin. The plain-thread path is the one that waits on the
  call's condition variable; a shard wait is sliced into 5 ms steps instead.
- `iocp_timer_fires_and_cancels` (`tests/test_io_iocp.cpp`) no longer asserts
  `elapsed < 20 ms`. The pre-1803 fallback timer's ~15.6 ms tick passes that
  bound while the high-resolution path's loaded median (23.7-25.8 ms) fails it,
  so the gate was inverted; the 5 s `wait_for` stays as the hang guard. The new
  capability `crpc::iocp_backend::high_resolution_timer()` is the observable
  that replaces it, and the case skips (never fakes a pass) on a host running
  the fallback timer. ABI: one new MINOR symbol, Windows-only, and no data
  member in the class (the flag lives in the out-of-line `impl`), so
  `sizeof(iocp_backend)` is unchanged.
- `graceful_shutdown_drains_and_rejects` (`tests/test_server_client.cpp`)
  asserted `elapsed < 2 s` while `cfg.drain` is 2 s, so the bound was exactly
  the legal path and a drain that used its budget was red. It is now
  `cfg.drain * 4`.
- `net_accept_cancelled_by_close` (`tests/test_net_transport_edge.cpp`) waited
  4000 ms for the wake and then required `elapsed < 3 s`, so a slow but correct
  wake failed. The redundant wall-clock assertion is gone, and the wait is a
  pure hang guard at 4000 ms -- widening it costs nothing now that nothing reads
  the elapsed time; the `shutting_down` code it returns proves the semantics.
- The absolute LZ4 microsecond budgets in `perf_budget`
  (`tests/test_frame_compression.cpp`) moved to
  `bench/bench_compression.cpp` as soft PASS/WARN thresholds, together with the
  CRC32C ratios; an absolute budget on a shared runner measures the runner, and
  the Debug medians already sat inside the old ceilings. The self-referential
  size-scaling guard stays in CI: 32 KiB is 128x the 256-byte input, so linear
  work lands far below the `128 * 4` ceiling and only a superlinear regression
  trips it, and because both sides are the same codec in one process the
  instrumentation is symmetric, which keeps the ratio stable under /Od and the
  sanitizers. The CRC32C ratios stay in bench because `crc32c` uses the
  `_mm_crc32_u64` intrinsics on x64, which instrumentation touches very
  differently from the software LZ4 encoder -- measured headroom was 2.4-2.9x,
  below the 3x a behavioural assertion needs. The test keeps the correctness
  assertions too (a compressible 32 KiB payload shrinks and both sizes
  round-trip byte-for-byte).
- Evidence: `tests/test_server_client.cpp`, `tests/test_io_iocp.cpp`,
  `tests/test_net_transport_edge.cpp`, `tests/test_frame_compression.cpp`,
  `bench/bench_compression.cpp`, and a full `ctest -j 4` run.

### Flow control wired into the client call path

- The adaptive flow-control primitives (`flow::rtt_estimator`, `flow::pacer`,
  `flow::congestion_window`, `flow::rtt_tracker`) are now wired into
  `rpc::client` through a connection-level `rpc::flow_controller`
  (`include/crpc/rpc/flow_control.hpp`, `src/rpc/flow_control.cpp`). It is off by
  default; `rpc::client::set_flow_control` installs it, and the facade exposes
  the same call and applies it to every pooled connection, including ones opened
  later.
- Admission is a byte window over encoded frames. An empty window admits one
  call unconditionally; a `ONE_WAY` call is never charged (it completes as soon
  as it is published, so nothing could acknowledge it); a refused call carries
  `crpc.flow_rejected` in its error metadata, which the facade classifies as a
  local gate (never retried, never charged to the endpoint breaker).
- Round-trip samples come from PING/PONG probes: the client sends a PING whose
  8-byte payload is the send timestamp and the server echoes it as a PONG on
  stream 0. At most one probe is outstanding, so probe traffic is bounded by the
  round trip. A probe bypasses the byte window but not `max_pending_write_bytes`.
  Flow control adds no clock read to the send path.
- Congestion control: `theta = baseline * growth_per_mille / 1000`; a sample
  above it is withheld from the estimator and the window is reduced at most once
  per baseline; a floor-bound window accepts `slow_confirmations` consecutive
  over-threshold samples as the new baseline so the loop can escape. A sample
  below the baseline needs confirmation and the *slowest* sample of the window is
  accepted, so one artifact cannot poison the baseline.
- Per-lane byte quotas with idempotent `(stream_id, generation)` release on every
  terminal path (completion, cancel, handle destroy, `fail_all` fallback). The
  facade checks admission *before* consuming a lane sequence number, so a local
  refusal leaves no hole in the lane. The pacer is connection-level and its rate
  follows `window / baseline`.
- Fixed: `rtt_tracker` and `pacer` used zero as their "not initialised"
  sentinel, but zero is a legal timestamp, so the first probe of a clock that
  starts at zero was unusable and a zero-anchored pacer never gained credit. The
  sentinel is now all-ones. `pacer::set_rate` now rescales the stored credit, so
  a rate increase takes effect on the next send instead of two refill periods
  later.
- Fixed: `rpc::client::start_call` rebuilt a failed `status` from its code and
  message, dropping the metadata (including `crpc.flow_rejected`); it now
  forwards the whole `rpc_error`.
- ABI: `rpc::client` grew by one pointer-sized member (MAJOR); `rpc::client_options`
  and `client::options` are unchanged. New MINOR symbols: `set_flow_control`,
  `can_admit`, `flow_snapshot`, `rpc_client_size_probe`, and the
  `rpc::flow_controller` / `flow_control_options` / `flow_state` types. The
  server side is unchanged (PONG is a stateless echo). Evidence:
  `tests/test_flow_wiring.cpp`, `tests/test_flow_clock_base.cpp`,
  `tests/test_flow_lane_quota.cpp`.

### Server-side idempotency dedup table

- The RPC server now deduplicates replayed calls. A request whose envelope
  carries a non-empty `call_id` (the R6 triple `(client_id, client_epoch,
  seq)`, reused unchanged by every client retry) is claimed in a bounded table
  before the handler runs: a fresh key executes; a key that is still in flight
  *suspends* the duplicate (never drops it) until the leader publishes and then
  resends its exact encoded response, so concurrent duplicates execute the
  handler exactly once; a completed key replays the cached response byte for
  byte without running the handler. This closes requirement 12's "response lost
  + retry = handler executed twice" correctness incident for non-idempotent
  operations, and requirement 16's retry budget now stands on a real server
  guarantee. Evidence: `tests/test_rpc_dedup.cpp` (13 cases / 97 checks).
- New `rpc::dedup_table` (`include/crpc/rpc/dedup.hpp`,
  `src/rpc/dedup.cpp`), owned by `rpc::dispatcher` and shared by every shard of
  one server process. Configuration through `rpc::dedup_options` (consumed by
  the inline `dispatcher(registry, dedup_options)` constructor, so the record
  never crosses the shared-library boundary — [abi.md](abi.md) §4): capacity
  4096 entries with LRU eviction (in-flight entries pinned; a table saturated
  with in-flight entries degrades to at-least-once bypass, never a hang and
  never unbounded memory — the design sketch's per-client FIFO ring is
  unbounded in the number of clients, which is why LRU was chosen); a
  monotonic-clock TTL of 5 minutes defines the dedup window (never a wall
  clock); a 16 KiB per-entry cap on cached responses (larger responses are
  handed to the duplicates suspended at completion but not retained for
  replays); a 30 s cap — always intersected with the request's own wire
  deadline — on suspending a duplicate, with `max_wait <= 0` disabling
  suspension entirely (in-flight duplicates are refused immediately with a
  retryable error); capacity 0 disables the table. Counters (`claims_leader`,
  `claims_replay`, `claims_waiter`, `claims_bypass`, `waiters_woken`,
  `waiters_abandoned`, `waiters_expired`, `evictions`, `entries`) are readable
  through `dedup_table::stats()`.
- Only successful responses are cached and replayed. A failed completion is an
  uncached tombstone: its concurrent duplicates receive the leader's error, but
  a later replay re-executes, which keeps the client retry policy meaningful
  (`retry_idempotent_only` in `test_server_client.cpp` depends on it). A
  replayed `ONE_WAY` duplicate is dropped silently.
- A suspended duplicate is interruptible: the server passes the connection
  pointer to `dispatch` as the wait key, and connection teardown
  (`on_terminal`, send failure, connection destruction) calls
  `dedup_table::abandon`, ending the duplicate with an explicit `canceled`
  error instead of stranding a shard thread.
- The table's key is `(call_id, service_id, method_id)`: the design allocates
  `seq` per `(client_id, target_service)`, so scoping by the envelope ids rules
  out cross-method false replays.
- Documented limits ([architecture.md](architecture.md) §14): the guarantee
  holds inside `min(TTL, LRU lifetime)` — a replay after eviction or expiry
  re-executes, so the client retry budget and the server window must be sized
  together; `client_epoch` changes (client restart) create fresh keys and old
  entries age out through the same bounds (the epoch is part of the key, so a
  stale entry can never shadow a new-epoch call); the table is per process —
  multi-shard servers share it through the one dispatcher, but independent
  processes/nodes each dedup on their own, and routing a `call_id` to one node
  is a load-balancer concern not solved here.
- **ABI:** additive. New exported classes `rpc::dedup_table` (with nested
  `lease`) and a `dispatcher` constructor overload plus a new six-argument
  `dispatch` overload; `dispatcher` gained a `dedup_table` data member, so a
  consumer and the library must be rebuilt from the same headers (already
  required by this MAJOR cycle). No existing record changes layout; no existing
  exported symbol changes signature.

### HTTP/2 client: ALPN negotiation, h2c prior knowledge, multiplexing

- `http::client` now speaks HTTP/2 when the connection negotiates it. Over TLS
  the ALPN result decides: the default offer is `{"h2", "http/1.1"}`, so an
  origin that selects h2 gets h2 (`response::version` is
  `http_version::http_2`) and an origin that selects http/1.1 -- or no ALPN at
  all -- keeps the HTTP/1.1 client. Over cleartext, h2c with prior knowledge
  (RFC 9113 3.4) is available through the new `client_options::h2c` flag, off
  by default because no fallback exists after the preface.
- **ABI breaking:** `http::client_options::tls_alpn` changed from
  `std::string` to `std::vector<std::string>` (the preference list), and
  `client_options` gained `bool h2c`. Both are layout changes ([abi.md](abi.md)
  §1); a consumer must be rebuilt. The unreleased cycle is already MAJOR.
- New `http/h2_client.hpp`: `http::h2_client` keeps one connection open and
  multiplexes streams -- `begin` puts a whole request on the wire without
  waiting, `finish` pumps frames until that stream's response is complete
  (other streams' responses and failures are parked and returned by their own
  `finish`), and `send`/`send_text` cover the one-shot case. Pseudo-headers
  are emitted first (`:method`/`:scheme`/`:path`/`:authority`), field names
  are lower-cased, and connection-specific fields (`Connection`, `Keep-Alive`,
  `Proxy-Connection`, `Transfer-Encoding`, `Upgrade`), `Host`, and `TE`
  values other than `trailers` are dropped. `make_h2_client` builds one over
  TCP (h2c prior knowledge) or TLS (requires a negotiated "h2"; anything else
  is reported as `failed_precondition` so the caller can fall back to
  `http::make_client`). Response side: `:status` selects the code, trailers
  land in `response::trailers`, and a `content-length` that disagrees with the
  received body fails with `protocol_error` instead of truncating.
- **Not implemented:** server push (the client advertises
  `SETTINGS_ENABLE_PUSH=0` and a server that pushes anyway fails the
  connection) and CONNECT. Both are recorded here rather than silent.
- `http::http_version` gained `http_2` (appended at the end, per
  [abi.md](abi.md) §1).
- `db::stream_channel::wait_handshake` waits (bounded) for the TLS handshake
  to settle; the ALPN result only exists after it, so the factories consult it
  before the first byte of the selected protocol goes out.
- Two HTTP/2 connection-layer fixes surfaced by exercising the client role
  against the library's own server (`src/http2/connection.cpp`):
  - a client-role connection no longer treats a peer's
    `SETTINGS_ENABLE_PUSH=0` as a protocol error (RFC 7540 6.5.2 bounds the
    value, not the direction; the value is recorded either way);
  - a `WINDOW_UPDATE` received for a closed stream is ignored instead of
    answered with `STREAM_CLOSED` (RFC 7540 5.1, closed state).

### ABI breaking: ADO-style parameter direction and ORM column metadata

- `db::parameter` gained `std::string name` and `parameter_direction direction`
  (`input`, `output`, `input_output`, `return_value`), plus `is_output()`,
  `sends_value()` and a `parameter::out()` factory. A stored procedure writes
  back through OUT/INOUT, and MySQL binds those through session variables, so
  the direction must be part of the parameter rather than inferred per call.
- `db::column_info` gained `schema`, `table`, `original_name` and `flags`. An ORM
  binds a struct field to a column, which needs the original column name (an
  alias would bind the wrong field) and the owning table, not just the alias and
  the abstract type.
- Both are public aggregate layouts, so this is MAJOR ([abi.md](abi.md) §1): a
  consumer must be rebuilt. Positional initialisation of the first fields keeps
  compiling; the new members are appended.

### ABI breaking: the Redis transport moved to `crpc::net`

- The byte-level `connection`, `connection_config`, `connection_pool` and
  `pool_config` were protocol-agnostic all along, so they moved from
  `include/crpc/redis/connection.hpp` / `src/redis/connection.cpp` to
  `include/crpc/net/connection.hpp` / `src/net/connection.cpp`. memcached, the
  database adapters and the message-queue clients share one transport and one
  pool instead of each growing a copy.
- `crpc::redis::connection` and friends remain available as `using` aliases in
  `<crpc/redis/connection.hpp>`, so source that spells the old names still
  compiles with no edits.
- The linked symbols changed, which is MAJOR by ([abi.md](abi.md) §1):
  - `ABI breaking: crpc::redis::connection::connection(crpc::connection_config, crpc::backend_factory&, crpc::runtime*)` changed from `??0connection@redis@crpc@@QEAA@Uconnection_config@12@AEAVbackend_factory@2@PEAVruntime@2@@Z` to `??0connection@net@crpc@@QEAA@Uconnection_config@12@AEAVbackend_factory@2@PEAVruntime@2@@Z`.
  - `ABI breaking: crpc::redis::connection::write(crpc::span<std::byte const>)` changed from `?write@connection@redis@crpc@@QEAA?AVstatus@3@V?$span@$$CBW4byte@std@@@3@@Z` to `?write@connection@net@crpc@@QEAA?AVstatus@3@V?$span@$$CBW4byte@std@@@3@@Z`.
  - The same namespace change applies to every member of `connection` and
    `connection_pool`, and to `crpc::redis::pool_config`. A consumer must be
    rebuilt against the new headers.
- Two forward declarations became includes: `redis::pipeline.hpp` and
  `redis::sentinel.hpp` now include the alias header, because an alias cannot be
  forward-declared.
- Impact: MAJOR for ABI; no source change for consumers. Verified: full suite
  74/74 in a static x64 Debug build, plus the shared-build consumer gate
  (`crpc_test_shared_link`) passing in a `CRPC_BUILD_SHARED=ON` build.

### Reliability: the reliable pipelined path no longer deadlocks

- `rpc::client` had two mutually inverted lock orders on `completion_mutex_` and
  `map_mutex_`: the completion path took completion then map, while
  `release_slot_under_lock` (the handle-destruction path) took map then
  completion. A pipelined workload on the in-process backend (concurrency > 1,
  batch > 1) hit it in roughly a third of runs. There is now one order
  everywhere: `completion_mutex_` outer, `map_mutex_` inner.
- `poll()` and `fail_all()` no longer snapshot raw `call_slot*` pointers. A
  `then` callback runs under the completion lock and may legally destroy a
  *sibling* handle, which returns that sibling's slot to the pool; a
  pre-collected snapshot could therefore finish a slot that was already
  recycled, writing a bogus `timed_out` into it (and, after a re-acquire, into a
  fresh never-sent call). Both functions now pick one still-live victim per
  round and finish it immediately.
- `rpc::client::start_call` gained a trailing `bool flush = true` parameter. The
  default keeps the documented "start_call sends" behaviour; `client_facade`
  passes `false` and flushes through the new `flush_pending()` outside its
  connection-slot lock, which is what makes the synchronous in-process drain
  safe. Source compatible; templates are instantiated in the consumer, so no
  library symbol changed.
- Impact: MINOR. New regression `tests/test_poll_recycle.cpp` (2 cases) covers
  both recycle windows; it fails 200/200 checks against the old shape.


### Error diagnostics

- `rpc_error` now carries where a terminal failure originated and how many
  attempts the logical call made: `failure_origin origin` (default
  `local_lifecycle`) and `std::uint32_t attempts` (default `0`), appended after
  `metadata` (`include/crpc/error.hpp:179-183`). Aggregate and positional
  initialisers that name the first five members keep compiling.
- `crpc::to_string(const rpc_error&)` renders the code, message, origin and
  attempt count as one line. Seven classification predicates (`is_local`,
  `is_network`, `is_timeout`, `is_circuit_breaker`, `is_rate_limiter`,
  `is_fallback`, `is_server`) map `origin` to the subsystem that failed, so a
  caller asks the question it cares about instead of switching on one enum.
- A process-wide completion hook (`diagnostics::set_completion_hook` /
  `on_completion`) observes every terminal `response<Resp>::get()` outcome
  exactly once, outside any library lock. When no user hook is installed it runs
  the process-wide `obs::interceptor_chain`
  (`obs::set_default_interceptor_chain`) at `after_call`, so an installed
  interceptor sees the terminal error; with no observer installed the path is a
  lock- and allocation-free no-op.
- `response<Resp>::get()` attaches the call's W3C trace id to the terminal
  error metadata under `trace_id` when the call carried a valid trace context,
  so a failure can be correlated with its trace without re-deriving the context.
- Impact: MINOR for the additive behaviour; the ABI section below is MAJOR.

### ABI breaking: `rpc_error` layout and the `failure_origin` move

- `rpc_error` grew from five to seven data members (`origin`, `attempts`). A
  public struct layout change is MAJOR ([abi.md](abi.md) §1). The two fields
  were appended at the end with defaults, so a consumer that *initialises* the
  first five members keeps compiling, but one that allocated or copied an old
  `rpc_error` must be rebuilt from the same headers.
- `failure_origin` moved from `client_detail` into `crpc` (the core error
  header) so `rpc_error` can carry it without a dependency cycle. This renamed
  two exported symbols: the enum type now mangles as `crpc::failure_origin`
  instead of `crpc::client_detail::failure_origin`.
  - `ABI breaking: crpc::client_detail::breaker_record_for(crpc::client_detail::failure_origin, crpc::rpc_code)` changed from `?breaker_record_for@client_detail@crpc@@YA?AW4breaker_record@12@W4failure_origin@12@W4rpc_code@2@@Z` to `?breaker_record_for@client_detail@crpc@@YA?AW4breaker_record@12@W4failure_origin@2@W4rpc_code@2@@Z`.
  - `ABI breaking: crpc::client_detail::result_origin(const crpc::rpc_error&)` changed from `?result_origin@client_detail@crpc@@YA?AW4failure_origin@12@AEBUrpc_error@2@@Z` to `?result_origin@client_detail@crpc@@YA?AW4failure_origin@2@AEBUrpc_error@2@@Z`.
  `client_detail::failure_origin` remains an alias, so existing spellings still
  compile, but the linked symbol changed.
- Impact: MAJOR. Rebuild every consumer from the same headers and relink. See
  [abi.md](abi.md) §1, §4 and §8.3, and [migration.md](migration.md).

## 2.0.0 - 2026-10-24

### Changed (MINOR, behaviour only)

- The server now writes the reserved response meta entry `crpc-degradable:
  true` on four pre-execution admission refusals: the per-service circuit
  breaker (`unavailable`), the per-service rate bucket when the deadline is
  still reachable (`resource_exhausted`), the per-service bulkhead (`overload`)
  and the per-service in-flight cap (`overload`). The entry is written only
  when the request's remaining wire deadline is at least 1 ms
  (`resilience::min_remaining_ns`, now shared with the dispatcher's own
  pre-execution deadline refusal instead of being a second literal). It lets a
  client tell "refused before the handler ran, so a fallback value is
  meaningful" from "the handler ran and then failed with the same code": an
  `overload` from a handler and an `overload` from the bulkhead are otherwise
  identical on the wire. Five error paths never carry it (the per-connection
  flood bucket, the per-connection in-flight cap, the rate bucket's
  deadline-miss exit, the dispatcher's deadline refusal, and every
  handler-produced error), and a one-way request cannot carry it because it has
  no response frame. `crpc-degraded` is still written by nobody on the server
  and `fallback_execute` is still never called from it.
- Impact: MINOR, and it is a **behaviour change, not an ABI change**. No
  exported symbol is added, removed or resignatured; no public record changes
  layout or size; no `server::options` field is added, so a consumer's stack
  allocation of `server::builder` is unaffected. Only the payload of four
  existing error responses grows by 23 bytes (the two-byte `meta_count` becomes
  `0x0001` and one TLV entry is appended). A client that ignores unknown meta
  keys is unaffected. See [protocol.md](protocol.md) §10 and
  [abi.md](abi.md) §4.

### Fixed (MINOR, symbols only)

- A `CRPC_BUILD_SHARED=ON` build now exports the whole documented public API.
  Before this change only 27 classes carried `CRPC_API`, so a consumer that
  linked `crpc.dll` / `crpc.so` the way the README shows failed at link time
  with unresolved externals for the builders, the in-process backend, every
  resilience primitive, the push hub, the MVC router, the logger and tracer,
  the HTTP message/coding/file layers, the flow-control estimators, the codec
  registry and the `obs` span type. 46 public classes (43 top level plus the
  nested `server::builder`, `client::builder` and `http::http_server::builder`)
  and 51 namespace-scope functions with out-of-line definitions now carry the
  decoration on their own declaration. Seven cross-DLL interface classes
  (`io::completion`, `io::source`, `io::sink`, `io::connection`,
  `io::accept_sink`, `io::listener`, `io::backend_factory`) are decorated as
  well: consumers inherit from them and pass objects back across the boundary,
  so their destructor vtable slot and RTTI must be exported.
- Impact: MINOR, and it is a **symbol-only change**. No symbol is removed or
  resignatured, no public record changes layout or size, and no behaviour
  changes. A shared build's export table grows from 586 to 1258 names on
  Windows x64; a static build is unaffected because an archive exports
  everything by definition.
- A new build-time gate, `tests/test_shared_link.cpp`, is built and run only
  when `CRPC_BUILD_SHARED=ON`. It is a consumer that links the import library
  and calls the documented API directly, including header-inline templates whose
  bodies reference out-of-line members, so a missing decoration fails the build
  instead of a downstream consumer. See [abi.md](abi.md) §3 and
  [building.md](building.md) §4.

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
  `crpc/resilience/fallback.hpp`, plus `client::call<Resp>::degraded()`, which
  reports whether the terminal result came from a fallback function. The
  primitives are header-only; `client::fallback_stats()` was the only new
  exported symbol of that slice; the return type and the `degraded()` accessor
  are header-only. See [abi.md](abi.md) §4.

### Usability deep improvements (MINOR, symbols only)

Nine gaps from the usability gap analysis are closed. Every change is additive:
the low-level forms keep their signatures and the new names are canonical.

- `client::set_fallback_owned` and `crpc::fallback_install`: the RAII fallback
  installer. `fallback_install` owns `self` in a `std::shared_ptr` and uninstalls
  the policy when it is destroyed or reset, so the five-argument
  `set_fallback(function, self, min_remaining, retain, release)` retain/release
  contract is no longer the only way to install a policy.
- `rpc::call_context::set_deadline` / `set_timeout` / `set_idempotent` and the
  `with_deadline` / `with_timeout` / `with_idempotent` builders: the deadline
  setter sets `has_deadline` together with the value, so a `deadline` assigned
  without the flag no longer silently means "no timeout".
- `client::call<Resp>::then`: the facade-level completion callback, mirroring
  `rpc::call<Resp>::then`. It fires exactly once when the call terminates,
  including a terminal reached locally.
- `crpc::delivery_mode`: one enum shared by `crpc::rpc` and `crpc::push`; the two
  namespace aliases are the same type, so an RPC mode and a push mode
  interoperate without a conversion.
- Builder naming: `max_inflight_per_connection` is the canonical per-connection
  in-flight cap on both the client and the server builder; the server's
  `per_connection_inflight` is a deprecated alias. `default_deadline` is
  canonical on the client builder and `default_timeout` is a deprecated alias.
- `crpc::service_id` and `crpc::method_id`: strong newtypes for the two ids. A
  `service_id` converts implicitly from a `u64` literal but never from a
  `method_id`, so a swapped argument list is a compile error.
- `result<T>::error_or` and `value_or` overloads for lvalue and const lvalue
  results; `call<Resp>::get()` answers `invalid_argument` for an invalid handle,
  distinct from the `failed_precondition` of a wait outside a coroutine.
- `CRPC_HTTP_GET("/path", lambda)`: the two-argument MVC form with no
  controller type; the route ordinal and object name come from `__COUNTER__`, so
  two registrations on one source line cannot collide on `__LINE__`.
- `crpc::publish_result` and `server::publish`: `publish` returns a
  distinguishable result (`ok`, `empty_topic`, `not_running`,
  `unsupported_configuration`, `no_push_hub`, `internal_error`). The conversion
  to `bool` is implicit, so an existing `if (server.publish(...))` keeps
  compiling and keeps meaning "ok".
- Impact: MINOR, symbols and behaviour only. No exported symbol is removed or
  resignatured, no public record changes layout or size (the deprecated builder
  aliases are methods, not fields), and no wire value changes. A consumer that
  does not use the new names is unaffected.

### Documentation and examples

- `examples/` is now a wired build target. `examples/CMakeLists.txt` builds
  `crpc_example_quickstart` — one in-process program that starts a server,
  registers a typed method, makes a call and handles a push subscription —
  under `CRPC_BUILD_EXAMPLES=ON` and registers it in CTest;
  `examples/README.md` explains how to build and run it. The directory had no
  `CMakeLists.txt` before, so `CRPC_BUILD_EXAMPLES` had to stay off.
- `docs/api/README.md` indexes the public headers and key types;
  `docs/tutorial.md` is a step-by-step build from a message type to a call and
  a push subscription. Both exist in English and Chinese, and `README.md`,
  `README.zh-CN.md`, `docs/README.md` and `docs/README.zh-CN.md` link them.
- Impact: documentation and examples only. No public header, symbol, layout or
  behaviour changes; the examples live outside `src/` and the library target.

### Fallback wiring and the injected server clock

- The client now runs the installed fallback after the last attempt of a call
  has been finalized and its breaker outcome recorded, at the two terminal
  failure exits of `call<Resp>::get()`. `client::set_fallback(function, self,
  min_remaining, retain, release)` installs a policy, and
  `client::run_fallback` / `client::record_fallbacks` are the library-side
  entry points. The reference that keeps `self` alive is taken once per
  installed policy and released once when that policy is replaced or the client
  is destroyed; a non-null `self` without the `noexcept` `retain`/`release` pair
  is rejected and installs nothing. `crpc-degradable` (written by the server on
  four pre-execution refusals) is the gate for a remote refusal; a sent,
  non-idempotent call is never degraded, because replacing its error could hide
  a side effect that already happened. A fallback value that arrives after the
  deadline is reported as `deadline_exceeded`, not as a degraded success.
  `crpc-degraded` is still written by nobody on the wire: the observable
  carriers are `call<Resp>::degraded()` and `fallback_stats().applied`.
- Impact: MINOR for the new symbols, **MAJOR for the layout change below**.
  No existing member offset changes, but a consumer must be rebuilt from the
  same headers.
- `server::options` gained a trailing `resilience::clock_source clock_source`
  (invalid by default; the library replaces it with
  `resilience::monotonic_clock()`) and `server::builder::clock()` sets it. The
  same clock now feeds the per-connection rate bucket, the per-service rate
  bucket, the per-service circuit breaker and the dispatcher's deadline check,
  so one request can no longer be judged against two different "now" values.
  `sizeof(server::options)` grows 304 -> **320** (x64) and 256 -> **264**
  (x86); `sizeof(server::builder)` grows with it. Adding a field to a public
  struct is MAJOR under [abi.md](abi.md) §1, so this cycle's MAJOR record now
  covers it as well. The default keeps the previous behaviour exactly: an
  options struct that leaves the field invalid reads `monotonic_clock()`, which
  is the same `std::chrono::steady_clock` the server used before.
- ABI breaking: `server::options` grew from 304 to 320 bytes (x64) and from 256
  to 264 bytes (x86); `server::builder` grew with it (376 -> 392 / 296 -> 304).
  No exported symbol changed signature; three were added
  (`client::set_fallback`, `client::run_fallback`,
  `client::record_fallbacks`).

### Compile-time layout guards

- The internal translation unit `src/abi_check.cpp` (in no header, exporting no
  symbol) now asserts the layout of a subset of the public records that cross the
  ABI boundary: standard-layout for 15 records; an exact member count for 15
  records (`CRPC_MEMBER_COUNT`), which catches an append, an insertion or a
  removal; relative member chains (`CRPC_STEP`, `CRPC_STEP_SAME`,
  `CRPC_STEP_MIXED`, `CRPC_AFTER`) that catch a reorder or a rename; the total
  width and last-field position of `http_statistics`; and, on x86 and x64 only,
  absolute `sizeof`/`offsetof` anchors (`CRPC_ABS_SIZE`, `CRPC_ABS_OFF`). The
  member count is what sees a tail append that x86 tail padding would hide from an
  absolute `sizeof`.
- [abi.md](abi.md) §4 now states the scope predicate for "crosses the ABI
  boundary" and lists the 36 in-scope records that still have no compile-time
  guard (the exact count confirmed by `crpc_test_abi_docs`).
  `crpc_test_abi_docs` rescans `include/crpc/**/*.hpp` and
  `src/abi_check.cpp`, so a new cross-boundary record must be guarded or added to
  that list, or the test fails.
- Documented blind spots: the member count does not validate names (a rename is
  caught by a relative chain where one exists; in `rpc_error` it is caught by
  the member-type assertions in `src/abi_check.cpp`); among the 15
  member-counted records, `rpc_error` is the only one without a relative chain,
  so a reorder inside it is not caught by this unit. Its constructors name only
  `code`, `message` and `retryable`, so they do not catch a rename of
  `retry_after` or `metadata`; the unit's seven `decltype` assertions name all
  seven members, so a rename of any of them fails there. The absolute `sizeof` of
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
