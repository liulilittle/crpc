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

- `crpc::client::resilience_statistics` and `crpc::client::resilience_stats()`.
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
