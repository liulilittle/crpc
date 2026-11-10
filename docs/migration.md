# CRPC migration guide

[简体中文](migration.zh-CN.md) | **English**

Version 2.0 / 2026-10-24

This guide records the binary and behaviour changes between the last compatible
release (1.0.0, `bd006b2`) and 2.0.0. The ABI policy is normative; see
[abi.md](abi.md). When a change below is MAJOR, a consumer must be rebuilt from
the same headers; a source-only change is called out as such.

## 1. 1.0.0 -> 2.0.0: what breaks

| Change | Kind | Affected type / symbol | 1.0.0 | 2.0.0 |
|---|---|---|---|---|
| `http_statistics` grew | MAJOR (layout) | `crpc::http::http_statistics` | 72 bytes, 9 fields | 224 bytes, 28 fields |
| `server::options` grew | MAJOR (layout) | `crpc::server::options` | 304 bytes (x64), 256 bytes (x86) | 320 bytes (x64), 264 bytes (x86) |
| `server::builder` grew | MAJOR (follows options) | `crpc::server::builder` | 376 bytes (x64), 296 bytes (x86) | 392 bytes (x64), 304 bytes (x86) |

The nine fields 1.0.0 shipped keep their offsets in `http_statistics`
(`active_connections` at 0 ... `timed_out_connections` at 64), and every
existing `server::options` member keeps its offset (`send_goaway_on_stop` at 249
on x64 / 225 on x86, `push_subscribe` at 264 / 232). Only `sizeof` grows,
because the new fields are appended at the end. That does not make the change
compatible: the two sides disagree on the size of an object passed by value, and
the compiler-generated copy, the `offsetof` of any member after a new one, and
the stack frame of a consumer that allocates the struct are all sized from the
consumer's headers.

### Rebuild requirement

Every consumer must be rebuilt from the 2.0.0 headers and relinked against the
2.0.0 library. Mixing a 1.0.0 header with a 2.0.0 binary is not supported and is
not forward compatible, because the change is a different MAJOR version
([abi.md](abi.md) §7). Recompiling the consumer translation units is sufficient
when the library is static; a shared build must be redeployed together with the
consumer.

## 2. Symbol additions (MINOR, compatible within 2.x)

These are additive and do not by themselves break a consumer that is rebuilt:

- Flow control: `crpc::rpc::client::set_flow_control`,
  `crpc::rpc::client::can_admit`, `crpc::rpc::client::flow_snapshot`,
  `crpc::rpc::rpc_client_size_probe`, `crpc::client::set_flow_control`, and the
  `crpc::rpc::flow_controller` / `flow_control_options` / `flow_state` types.
  `rpc::client` itself grew by one pointer-sized member, which is a MAJOR layout
  change: rebuild every consumer (see `abi.md` §1).
- `crpc::client::resilience_statistics` and `crpc::client::resilience_stats()`.
- `crpc::iocp_backend::high_resolution_timer()` (Windows only): a
  `const noexcept` accessor that reports whether the backend created its timer
  with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` or fell back to the
  pre-Windows-10-1803 millisecond timer. It adds one exported symbol and no
  data member to the class (the flag lives in the out-of-line `impl`), so
  `sizeof(crpc::iocp_backend)` is unchanged.
- `crpc::resilience::fallback_statistics` and `crpc::client::fallback_stats()`.
- `crpc::client::call<Resp>::degraded()` (header-only accessor).
- `crpc::client::set_fallback`, `crpc::client::run_fallback` and
  `crpc::client::record_fallbacks`.
- `crpc::server::builder::clock(resilience::clock_source)`.
- The usability deep improvements (plan C): `crpc::client::set_fallback_owned`
  and `crpc::fallback_install` (RAII fallback installer),
  `crpc::rpc::call_context::set_deadline` / `set_timeout` / `set_idempotent`
  and the `with_deadline` / `with_timeout` / `with_idempotent` builders,
  `crpc::client::call<Resp>::then`, the shared `crpc::delivery_mode`,
  `crpc::service_id` / `crpc::method_id`, `crpc::result<T>::error_or` /
  `value_or`, `crpc::publish_result`, and the two-argument
  `CRPC_HTTP_GET("/path", lambda)` MVC form. The client builder gains
  `default_timeout` and `per_connection_inflight` as deprecated aliases and the
  server builder gains `max_inflight_per_connection`; the canonical names are
  `default_deadline` and `max_inflight_per_connection`.

`server::publish` now returns `publish_result` instead of `bool`. The conversion
to `bool` is implicit (`true` only for `ok`), so `if (server.publish(...))` and
`CRPC_CHECK(server.publish(...))` keep compiling and keep meaning "ok"; a caller
that spelled the result as a `bool` variable also keeps compiling. See
[CHANGELOG.md](CHANGELOG.md).

No exported symbol is removed or resignatured. The whole documented public API is
now exported from a shared build; see [CHANGELOG.md](CHANGELOG.md).

## 3. Behaviour changes that are not ABI breaks

- The server writes the reserved response meta entry `crpc-degradable: true` on
  four pre-execution admission refusals (the per-service breaker, the per-service
  rate bucket while the deadline is still reachable, the per-service bulkhead
  and the per-service in-flight cap). A client that ignores unknown meta keys is
  unaffected.
- HTTP MVC error responses carry `Retry-After` when `rpc_error::retry_after > 0`.
- `COMPRESSED` frames are no longer delivered as plaintext. `decode_frame`
  returns `protocol_error` for a `COMPRESSED` frame (it has no output buffer),
  and `frame_decoder` decompresses in copying mode and rejects in borrowing mode.
  A peer that hand-crafts a compressed frame now gets a protocol error.
- Frame-level compression supports the LZ4 block codec (`ALGO = 1`) and fails
  closed for every other algorithm id.

## 4. How to migrate

1. Rebuild the consumer from the 2.0.0 headers.
2. Re-allocate or value-initialize every `http_statistics`, `server::options` and
   `server::builder` from the new headers; do not deserialize a 1.0.0 blob.
3. Relink against the 2.0.0 library.
4. Re-run the test suite: the layout guards in `src/abi_check.cpp` assert
   `sizeof(server::options)` and `sizeof(http_statistics)` on x86 and x64.

## 5. Unreleased: `rpc_error` layout and the `failure_origin` move

The unreleased diagnostics work is a MAJOR change on top of 2.0.0:

- `rpc_error` gained two data members (`origin`, `attempts`), appended after
  `metadata`. Initialising the first five members still compiles; allocating or
  copying an old `rpc_error` does not.
- `failure_origin` moved from `client_detail` into `crpc`, which renamed two
  exported symbols (`breaker_record_for`, `result_origin`). The
  `client_detail::failure_origin` alias keeps source compatibility, but a
  consumer linked against the old symbol must relink.

Rebuild every consumer from the same headers and relink. The old and new mangled
names are listed in [CHANGELOG.md](CHANGELOG.md).

## 6. Unreleased: `registry_config::require_issued_fence` defaults to `true`

This is a **behaviour change, not an ABI break**. The field, its offset and
`sizeof(registry_config)` are unchanged, so no consumer has to be rebuilt for the
layout. What changes is that a default-constructed `cluster::registry` now
enforces issuance attribution (L-b) and the zero-fence rule (Z2).

### What breaks

A caller that constructs a registry and immediately writes without obtaining a
token from the authority:

```cpp
cluster::registry reg;                     // default config
auto r = reg.register_instance(instance, ctx);   // ctx.fence == 0
// before: accepted   now: failed_precondition / registry.reason=fence_required
```

A caller that forges its own token from a value it observed:

```cpp
ctx.fence = reg.fence_floor(service, node).value() + 1;   // not an issuance
// before: accepted (numeric contract)   now: registry.reason=not_issued
```

### How to migrate

1. **Preferred: obtain the token from the authority.** Every write of a key
   should present a token issued for that exact key, principal and lease:

   ```cpp
   const auto token = reg.next_fence(service, node_id);
   if (!token.ok()) { /* handle */ }
   cluster::write_context ctx;
   ctx.node_id = node_id;
   ctx.incarnation = incarnation;
   ctx.fence = token.value();
   ctx.writer_epoch = 0;  // external writer: epoch tracking not used
   reg.register_instance(instance, ctx);
   ```

   `writer_epoch` defaults to `UINT64_MAX`, which is the "caller did not set
   an epoch" sentinel and is **rejected** with
   `registry.reason=epoch_required`. Only epoch-tracked writers (a health bridge
   or an equivalent service-level writer) set it, from
   `registry::next_writer_epoch()`; an external writer that is not part of that
   regime must explicitly set `0`, which is the documented exemption and is never
   recorded as the key's epoch floor.

   `next_fence()` returns `max(global allocation, per-key high-water) + 1`, so
   one call clears any lease `fence_barrier` as well. A token is spent by the
   write it authorises: the next mutation of the same key needs a fresh
   issuance. For a lease, the token the grant published is already bound to the
   lease id, so pass `lease_id` to `register_instance`/`set_state`/`deregister`.

2. **The bridge does this for you.** `cluster::health_bridge::flush()` asks the
   registry for a token per key on every write; no caller change is needed.

3. **Only if the caller is genuinely single-writer and in-process**, set the
   switch off explicitly and record the risk:

   ```cpp
   cluster::registry_config config;
   config.require_issued_fence = false;   // L-b and Z2 are now inert
   cluster::registry reg(config);
   ```

   With it off, a peer that learnt the key high-water can write with
   `high_water+1`, and a zero fence can create a key. That is the historical
   behaviour and it is unsafe with more than one writer.

4. Turning `strict_fencing` off alone is **not** enough to restore the old
   unfenced shape: `require_issued_fence` owns Z2 independently. A legacy
   in-process caller that relied on unfenced writes needs both switches off.

### Verification

`tests/test_cluster_phase0.cpp` pins the default directly:
`default_config_rejects_unissued_token` (a never-issued token is rejected with
`not_issued`), `default_config_rejects_zero_fence` (`fence_required`),
`default_config_accepts_issued_token` (a token from `next_fence()` works and is
spent by the write), and `health_bridge_uses_authority_issued_token` (the bridge
presents an authority-issued token). `tests/test_registry_fence.cpp` pins the
semantics of the switch when it is set explicitly.

## 7. Unreleased: `cluster::health_bridge` must be rebuilt after a registry log restore

A `health_bridge` carries a `writer_epoch` obtained from
`registry::next_writer_epoch()`. The epoch is allocated from the registry's
global fence allocator, which a log restore only rewinds to the highest fence
actually **recorded** in the log. Epochs that a bridge consumed without writing
(one per construction, one per takeover) are not in the log, so after a restore
the allocator can hand out an epoch lower than one a surviving bridge still
holds.

**Step 1 rule:** a registry that restores from a log must be followed by
**reconstructing every `health_bridge`** bound to it. In step 1 the bridge lives
inside the same process as the registry, so a process restart already rebuilds
it; this rule only matters if you replay a log into a registry that outlives the
bridges.

**Step 2:** cross-process epoch persistence is a required work item; see
`plan.md` step2. Until it lands, do not run bridges across a registry restore.

Residual risk (P-d): there is no runtime check that detects "the allocator
handed out an epoch below a surviving bridge". Step 1's deployment shape (one
process for both) is what keeps the rule true.

**Which harm the violation actually causes.** The direction is worth stating
precisely, because the intuitive guess is the wrong one. Replaying a log into a
registry that live bridges still point at does **not** let a deposed writer
overwrite newer state:

- A restore into a *new* registry rejects a pre-restart writer whose epoch is
  below the log's fence, because the recorded floor is seeded from that fence
  (measured: rejected with `registry.reason=stale_writer`).
- An in-place restore is refused outright when the log is not ahead of the
  registry (`registry.reason=stale_log`), so a log that would *lower* the floor
  cannot be replayed.
- When the log *is* ahead, its record for a key carries a fence above every
  surviving bridge's epoch, so the floor moves **up**, not down.

The harm is therefore **availability, not safety**: the surviving bridge's epoch
is now below the recorded floor, so it is refused with `stale_writer` and — by
the permanent-yield rule — stops writing for the rest of its lifetime, even
though it is the legitimate owner. A freshly constructed bridge is granted an
epoch above the new floor and writes normally. Measured on the accepted
in-place path: the live bridge was refused `stale_writer` while a newly
constructed one succeeded.

So the rule "reconstruct every `health_bridge` after a restore" is about not
freezing the live writer, not about stopping a stale one. A deployment that
skips the rule loses write availability on every key whose fence the log
raised, and does not gain any stale-writer protection problem.


## 8. Unreleased: registry service (step2) operational requirements

### 8.1 The ACL is a bearer-token authorisation layer, not authentication

A holder of a valid token can act as the principal that token was issued to.
There is no transport security: the RPC layer carries no peer identity
(`call_envelope`, `call_context` and `frame` have no identity field) and there is
no TLS. A token observed on the wire can be replayed until it expires.
Deployments that need authenticated peers must terminate TLS in front of the
service or run it on a trusted network. The token travels in the DTO body, never
in `metadata`, because the client's `metadata` does not reach the server.
The principal is injected by the service from the matched ACL rule; a client's
own claim is never trusted.

### 8.2 The token time window depends on clock synchronisation

A token carries its issue time (`registry_tag_token_issued_at`) and the service
rejects one whose absolute difference from the service's clock exceeds
`registry_token_window` (30 s). The comparison uses `system_clock` (Unix epoch)
because the window is cross-process and `steady_clock` has a per-process epoch.
A token with no issue time is rejected (fail closed).

This is a **time limit, not replay prevention**: a token observed inside the
window can still be replayed. It also **depends on the client and the service
agreeing on the wall clock (NTP)**; a client whose clock is ahead or behind by
more than the window is denied. Clock synchronisation is `UNVERIFIED`.

### 8.3 Epochs are issued by the service, not chosen by the client

`writer_epoch` is allocated from the service's epoch store, never taken from the
client's claim. The store is a separate file written atomically **before** the
number is returned; a persist failure refuses the write with
`registry.reason=epoch_not_persisted`. At startup the counter is seeded with
`max(persisted, registry.fence())`, with `fence()` read after the restore. A
client that presents `writer_epoch = 0` is refused with `epoch_required`.

### 8.4 Only one service per state directory

A service holds a single-instance lease over its `state_dir`: an in-process
registry plus a named kernel mutex (a Windows mutex is re-entrant for its owning
thread, so the kernel object alone cannot stop a second instance in the same
process). `WAIT_ABANDONED` is accepted -- refusing it would make a crashed
deployment permanently unrestartable -- and taking over does **not** reimplement
the torn-tail truncate: `file_registry_log::open()` already scans the file,
adopts the intact prefix and truncates a torn tail, and a corrupt record makes it
fail. A log behind the restored catalog is refused with `stale_log`.

### 8.5 The idempotency map has a capacity boundary

The service caches the last successful write per `client_request_id` in a map
bounded by `max_idempotency_entries` (default 4096) and `idempotency_ttl`
(default 300000 ms). Entries are evicted by TTL first and then oldest-first, one
at a time; the map is never cleared wholesale, which would drop every request's
protection at once.

**A request outside the map has no protection.** A replay of an evicted request
takes the normal path and may be rejected as a stale incarnation or a stale
anchor. Size the deployment so that `max_idempotency_entries` multiplied by
`idempotency_ttl` is at least the client's maximum retry window. Only successful
writes are cached; a failed first attempt stays retryable.

### 8.6 `permission_denied` does not round-trip

The response codec fails closed on codes above `application` (21), so a client
observes `internal` with the original code in the `crpc.raw_code` metadata entry.
The service still emits `permission_denied` (22) per the reason table; a client
that must distinguish an ACL denial reads `crpc.raw_code`.
