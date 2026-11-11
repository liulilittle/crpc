# CRPC ABI policy

[简体中文](abi.zh-CN.md) | **English**

Version 2.0 / 2026-10-24

This document defines what CRPC guarantees at the binary level, what it does not,
and how a change that breaks binary compatibility is handled. It is normative:
a release that violates it is a release defect.

## 1. Versioning

CRPC uses semantic versioning with an ABI suffix:

```
MAJOR.MINOR.PATCH
```

| Change | Version bump | ABI impact |
|---|---|---|
| Remove or rename a public symbol, change a function signature, change a struct layout, change an enum value that crosses the wire | MAJOR | Breaking |
| Add a new public symbol, add a new enum value at the end | MINOR | Compatible |
| Change the layout of any public struct, including adding a field | MAJOR | Breaking (no size-tagged options struct exists yet, §4) |
| Bug fix with no interface change | PATCH | Compatible |

The wire protocol has its own version (`protocol_version`, currently `1`). It is
independent of the library version: a library patch release never changes the
wire format, and a wire change always requires a protocol version bump plus a
documented migration.

The 2.0.0 release is a MAJOR change: the layout of `http::http_statistics`
grew (§4), and `server::options` grew as well (§4). The 1.0.0 layout
(`bd006b2`) is the last compatible release; 2.0.0 is not forward-compatible
with a 1.0.x binary.

The unreleased diagnostics work adds a further MAJOR break on top of 2.0.0:
`rpc_error` grew from five to seven data members (`origin`, `attempts`), and
`failure_origin` moved from `client_detail` into `crpc`, which renamed two
exported symbols (`breaker_record_for`, `result_origin`). The old layout is not
forward-compatible. The change, the old and new mangled names, and the rebuild
requirement are recorded in [CHANGELOG.md](CHANGELOG.md) and [migration.md](migration.md),
as §8.3 requires.

The unreleased flow-control wiring adds exactly one more MAJOR break:
`rpc::client` grew by one pointer-sized member (an owned flow controller), so
its `sizeof` changed and every consumer must be rebuilt. It is the only MAJOR in
this change: `rpc::client_options` and `client::options` keep every field and
every offset, so the four `rpc::client_options` guards in `src/abi_check.cpp`
are unchanged. The new symbols are MINOR additions:

- `rpc::client::set_flow_control(const rpc::flow_control_options&)`
- `rpc::client::can_admit(u64, std::size_t)`
- `rpc::client::flow_snapshot()`
- `rpc::rpc_client_size_probe()`
- `client::set_flow_control(const rpc::flow_control_options&)`
- `rpc::flow_controller`, `rpc::flow_control_options`, `rpc::flow_state`

The unreleased G08 handshake batch 1 adds exactly one more MINOR symbol and no
layout change: `rpc::encode_frame_compressed_ex(span<std::byte>, const frame&,
compression_algorithm, const compression_limits&, std::size_t&,
compression_algorithm*, compression_statistics*, compression_fallback_reason*,
u32)`. It is the single counting point that tells a sender-side compression
fallback apart (`ratio_exceeded` from `not_smaller` from `below_min`); the
existing `encode_frame_compressed` keeps its signature and delegates to it with a
null reason. `compression_statistics` is unchanged, so this is a MINOR addition.

`rpc::client` cannot carry a header-side `static_assert(sizeof(client) == N)`:
it owns standard-library members, so its width is a property of the standard
library configuration rather than of this library (MSVC Debug and Release
differ; the same reasoning already applies to `rpc_error`, `status`, `buffer`
and `endpoint` above). The tripwire is therefore the out-of-line
`rpc::rpc_client_size_probe()`, which reports the size the library was built
with, plus a consumer-side assertion that the size the consumer compiled against
matches it. An accidental layout change in either direction fails that assertion
instead of corrupting memory.

The unreleased test-timing work adds one more MINOR symbol, Windows-only:
`crpc::iocp_backend::high_resolution_timer()`, a `const noexcept` capability
accessor that reports whether the backend created its timer with
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` or fell back to the pre-Windows-10-1803
millisecond timer. It adds a symbol and no layout: `iocp_backend` is an exported
class that already had out-of-line members, and the flag it reads lives in the
out-of-line `impl` behind the existing `std::unique_ptr`, so
`sizeof(iocp_backend)` is unchanged.

The unreleased database-pool session-reset work adds one more MAJOR break:
`db::connection` gained a virtual member function, `reset_session()`. Inserting it
after `rollback()` moves every later vtable slot, so the vtable layout of
`db::connection` and of every derived class changed, and all consumers must be
recompiled. It has to be virtual on the base class because the pool only ever
holds a `db::connection*` and cannot know whether the connection behind it keeps
session state at all. The default implementation returns `status{}` and touches
nothing, so the mongo, ODBC and TDS adapters pay nothing for it
(`mysql::connection` overrides it, because `autocommit` is a session variable
that `ROLLBACK` does not restore; TiDB inherits the override). The default also
keeps this a MAJOR rather than a source break: no existing subclass has to
change.

The unreleased MongoDB transaction work adds one more MAJOR break:
`db::mongo::connection` gained two data members (`in_transaction_`,
`txn_started_`) to track the server-side transaction it now starts, commits and
aborts, so its `sizeof` changed and every consumer must be recompiled. The new
behaviour is delivered through the existing `db::connection` verbs
(`begin()`/`commit()`/`rollback()`/`in_transaction()`), which no longer return
`no_such_method` for MongoDB; no vtable slot moved because none of the base
virtuals were added or removed.

The unreleased G19 Phase 0 cluster-primitive hardening adds one more MAJOR
break plus MINOR symbols. Four consumer-visible cluster types changed layout,
so every consumer must be rebuilt from the same headers (sizes measured with
MSVC 19.51, x64 Debug, against the previous headers):

- `cluster::registry_config` grew from 88 to 104 bytes: four trailing fields
  (`separate_state_writers`, `strict_fencing`, `strict_incarnation`,
  `max_fence_lead`). The `registry` constructor takes it by value, so this is
  an exported signature change as well as a layout change (§1, §4).
- `cluster::registry` grew from 368 to 432 bytes: private tombstone
  incarnation high-water and fenced-key maps. Consumers allocate a registry, so
  its width is part of the contract.
- `cluster::watch_hub` grew from 144 to 192 bytes (bounded replay ring and
  capacity) and gained a second constructor.
- `cluster::watch_stream` grew from 312 to 320 bytes (cancel-barrier state:
  the dispatching thread id and an in-callback flag).
- `cluster::write_context` grew from 96 to 104 bytes (x64 Debug; 80 to 88
  Release): one trailing `u64 writer_epoch`. This is a **second MAJOR break in
  the same unreleased slice**, accepted deliberately because the field cannot be
  added any other way: it is the only thing that lets a writer prove it is the
  current owner of a key rather than merely the holder of a fresh fence token.
  See the writer-epoch paragraph below for the semantics and for the one
  default that keeps the guard from failing open.

`cluster::lease_manager` (200 bytes), `cluster::lease` (120),
`cluster::lease_config` (32), `cluster::health_monitor` (64),
`cluster::registry_snapshot` (40), `cluster::service_instance` (192),
`cluster::write_result` (24) and `cluster::watch_event` (72) keep their layout.
`cluster::write_context` (104) does not: it took the trailing epoch field
described above.
The new symbols are MINOR
additions: `cluster::registry::fence_barrier(const std::string&, const
std::string&, u64, const write_context&)`, `cluster::watch_stream::park()`,
`cluster::watch_stream::cancel()` and
`cluster::watch_hub::watch_hub(registry&, std::size_t, std::size_t)`.
No enum value, wire field or protocol byte changed; the semantic fixes
(grace-zero purge, tombstone high-water, bounded caller fencing, expired-lease
renewal rejection, purge index/remove events, inclusive TTL boundary, watch
cancel barrier/park/replay) are behaviour changes only. The four
`registry_config` switches are **on by default**:
`separate_state_writers=true`, `strict_fencing=true`,
`strict_incarnation=true`, `require_issued_fence=true`. A pre-existing
in-process deployment that relied on the historical overwrite semantics must set
them to `false` explicitly. That legacy configuration is unsafe for any
deployment with more than one writer: registration can clobber a health/lease
verdict, an update of an externally fenced key can silently drop its fence, and
a same-incarnation payload change overwrites instead of reporting the conflict.

`registry_config::require_issued_fence` changed its default from `false` to
`true` in this slice. The historical default of `false` was not a working
configuration: with it off, L-b (issuance attribution) and Z2 (a zero fence is
never a token) are both inert, so a peer that learnt the key high-water from any
rejection could write with `high_water+1`, and an unfenced create could occupy a
key. Turning the switch off is now the only way to get the historical numeric
contract, and it means opting into both holes. An in-process caller that
constructs a registry and immediately writes without calling
`next_fence()`/`fence_barrier()` must either issue a token first (the supported
shape) or set the switch to `false` and accept that the deployment has no
issuance attribution. This is a behaviour change, not an ABI one: the field,
its offset and `sizeof(registry_config)` are unchanged (x64 Debug 104, Release
96; `sizeof(registry)` 432 / 384).

Two further behaviour-only details belong to this section. A successful write
reports the key's effective fence high-water in `write_result::fence` (it is
never below a lease `fence_barrier`, so it can be presented to the next write of
the same key); a `stale_fence` rejection carries that high-water in the additive
`registry.fence` error-metadata key, which is how `health_bridge` re-anchors.
Under the default `require_issued_fence`, a zero fence is rejected on every
path, including a create of a key that has never existed (Z2). A key whose
tombstone watermark or fence high-water survived a purge is likewise not fresh:
a zero-fence create is rejected with `registry.reason=fence_required`, and an
unissued token above the high-water is rejected with
`registry.reason=not_issued`.

**Writer epoch (the second MAJOR break in this slice).** `write_context` gained
a trailing `u64 writer_epoch`, so the type is 104 bytes on x64 Debug and 88 on
Release. Every consumer must be rebuilt; a consumer that only ever writes from
one process and does not care about ownership can leave the field alone, because
the default is the "unset" sentinel described next.

The field answers a question the fence token cannot: *may this caller write at
all, or has it been deposed?* A fence token proves the authority issued a
number; it does not prove the holder is still the owner, because a replacement
writer can always obtain a larger token. The epoch is the per-key high-water of
the epochs that have actually committed, and a write whose epoch is below that
high-water is rejected with `registry.reason=stale_writer` — a verdict the bridge
treats differently from `stale_fence`: `stale_fence` means "ask for a fresh
token and try again", while `stale_writer` means "you have been deposed, stop
writing".

Three values carry distinct meanings, and the default matters:

- `UINT64_MAX` (the default) means *the caller did not set an epoch*. It is
  rejected with `registry.reason=epoch_required`. The default is deliberately
  the reject value: defaulting to zero would have silently promoted every
  forgetful caller to the exempt case below, i.e. the guard would have failed
  open instead of closed. `cluster::registry::next_writer_epoch()` returns a
  value that is always strictly greater than every epoch handed out before, and
  never returns the sentinel.
- `0` means *an external writer that is not epoch-tracked*. It is accepted and
  **does not update** the recorded high-water, so an external write can never
  lower the floor that fences an epoch-tracked writer out.
- Any other value is an epoch-tracked writer. It is accepted when it is greater
  than or equal to the key's recorded high-water (an equal epoch is the same
  writer continuing) and rejected with `stale_writer` when it is lower.

`next_writer_epoch()` is a **monotonic number, not a capability**: it is handed
to any caller with no identity or liveness check. Because acquiring a larger
epoch is always possible, "I hold a larger epoch" is not evidence that the
previous owner is gone. That is why `cluster::health_bridge` treats a
`stale_writer` rejection as a permanent yield for that bridge instance: only a
freshly constructed bridge may take the key back.

**The epoch itself is not persisted.** It is allocated from the fence
allocator's counter, and `restore_from_log` seeds each key's recorded epoch (and
the global counter) from the fences the log actually records. Epochs a bridge
consumed without writing — one per construction — are therefore not in the log.
Measured consequences, by deployment shape:

- Restoring into a **new** registry: a writer that predates the restore has an
  epoch at or below the recorded fence and is rejected with `stale_writer`; the
  direction is safe (`DEPOSED_CAN_OVERWRITE=0`).
- Restoring **in place**, with a log that is not ahead of the registry: refused
  outright with `registry.reason=stale_log`, so the floor cannot be lowered.
- Restoring **in place**, with a log that is ahead: accepted, and if the log's
  fence for a key exceeds a live bridge's epoch, that **legitimate** bridge is
  rejected with `stale_writer` and — under the permanent-yield rule — stops
  writing for the rest of its lifetime. A newly constructed bridge is granted an
  epoch above the new floor and writes normally (`LIVE_BRIDGE_FROZEN_OUT=1`,
  `fresh_epoch=902` accepted).

So the residual gap is an **availability** one, not a safety one: replaying a
log under live bridges can freeze a legitimate writer, and it never lets a
deposed one overwrite. A deployment that replays must reconstruct its bridges;
cross-process epoch persistence is a step-2 work item.

The G36 auto-sharding work adds **two MINOR exported symbols** plus
**two header-inline `shards_auto()` overloads (no exported symbol)**, with no
layout change: the free function `crpc::rpc::default_shard_count()` and
`crpc::server::shard_count()` are the two exported symbols;
`crpc::server::builder::shards_auto()` (both the parameterless and the
injected-core-count overload) is header-inline and adds no export.
`server::options` and `server::builder` keep every field, offset and size (x64:
`sizeof(options) == 320`, `sizeof(builder) == 400`; x86: `264` / `304`), and
the default `options::shards == 1` and the `builder::shards(0) -> 1` mapping are
unchanged. Automatic sharding is opt-in only (it is mutually exclusive with
PUSH), so the default behaviour is identical and this is a MINOR addition, not
a MAJOR one.

The unreleased build-gate work adds **exported symbols only, with no layout or
vtable change**, which is a MINOR addition: the value types
`crpc::time::date_time`, `crpc::time::duration` and `crpc::time::timestamp` gain
a class-level `CRPC_API`, so the DLL now exposes their existing out-of-line
members (`date_time::is_valid`/`validate`, `duration::absolute`/`checked_*` and
the `from_*` factories); `date_time::validate()` keeps its signature and moves
from a member-level to the class-level decoration (MSVC rejects both spellings
together as C2487). The abstract interface `crpc::resilience::instance_set` also
gains `CRPC_API` because the exported class
`crpc::cluster::health_instance_set` derives from it (rule E), which is what
removes the MSVC C4275 warning. No virtual slot is added or moved and no field,
offset or `sizeof` changes: only the export table grows, so consumers keep the
same layout.

## 2. What crosses the ABI boundary

Only these may appear in an exported signature:

- fundamental types with fixed width (`crpc::u8` … `crpc::u64`, `crpc::i8` …
  `crpc::i64`, `float`, `double`, `bool`),
- `std::byte`, `std::size_t`, `std::ptrdiff_t`,
- `crpc::span<T>`, `crpc::buffer`, `crpc::rpc_error`, `crpc::result<T>`,
  `crpc::status`,
- `std::chrono::duration` / `time_point` with a fixed period,
- pointers and references to CRPC types,
- `std::string_view` (a two-word trivially copyable view).

Everything else is forbidden in an exported signature:

- standard containers (`std::string`, `std::vector`, `std::map`, …) — their
  layout is implementation-defined and differs between MSVC, libstdc++ and
  libc++,
- `std::function`, `std::any`, `std::variant` with non-trivial alternatives,
- exceptions thrown across the boundary (the library never throws),
- `long`, `unsigned long`, `wchar_t`, `size_t` spelled without a fixed width,
- bit-fields, anonymous unions, packed structs,
- anything under an implementation-internal namespace (`client_detail`,
  `rpc_detail`, or any other `detail` namespace) as a *consumer-facing* type:
  these may appear only as parameters the library itself produces.

Where a container is genuinely needed, the API takes a `span` or a
`const char*` plus a length, or returns an owning CRPC type that is documented
as opaque.

Types in the `crpc::client_detail` and `crpc::rpc_detail` namespaces, and in
the anonymous or nested `detail` namespaces of the other headers, are
**implementation-internal**: they do not appear in a *consumer-facing* exported
signature. They are not, however, all invisible to the consumer's compiler.
`client_facade.hpp:898` instantiates `client_detail::call_state<Resp>` inside
the consumer's own translation unit
(`std::make_shared<client_detail::call_state<Resp>>()` in a header-inline
template), and `call_state` holds `call_core` **by value**. A layout change in
`call_core` or `call_state` therefore requires the consumer and the library to
be rebuilt from the same headers (§4, "callers and the library must be built
from the same headers"). Under the §1 table ("Change the layout of any public
struct, including adding a field | MAJOR") such a change is a layout change and
is MAJOR; this cycle is already MAJOR (§4), so this slice adds no new
compatibility gap.

Concretely, `client::finish_attempt(client_detail::call_core&, ...)` and
`client::begin_attempt(client_detail::call_core&, ...)` mention `call_core` in
an exported signature, and the only producer of that reference is `client`
itself. That does not make the layout irrelevant: adding a field to `call_core`
(for example the `degraded_` flag that records a fallback-answered attempt)
moves every field after it. Measured on MSVC x64 (`call_core` 664 -> 672 bytes,
`call_state<Resp>` 760 -> 768), a consumer built from the old header and linked
against the new library writes the new field over the first byte of the old
`inner_mutex`, because the field lands at absolute offset 8 + old
`sizeof(call_core)` — 672 under `/MD`, 696 under `/MDd`. On x86 the write
happens to fall into four bytes of internal tail padding inside `call_core`
(`retry` ends at 484 while `sizeof(call_core)` stays 488), so `sizeof` is
unchanged and no overwrite is observed today. That is luck, not a guarantee:
the padding is only four bytes and three remain after `degraded_`, so a fourth
one-byte field already pushes `sizeof(call_core)` to 496, and a single field
wider than the three remaining bytes crosses immediately. Either way the x64
failure mode returns on x86.

`call_core` is decorated `CRPC_API` (`client_facade.hpp:295`) and its members
therefore appear in the shared library's export table. That decoration is a
**link-time requirement, not an ABI promise**: header-inline templates
(`client_detail::wake_functor`, `call<Resp>::get()`, `start_encoded`) compile
into the consumer's translation unit and call `call_core::signal`,
`mark_degraded`, `clear_degraded`, `degraded`, `reset_for_attempt`, its
constructor and its destructor, so a shared build that hid them would fail to
link any consumer that instantiates a call. Their presence in the export table
does **not** put them in the §2 whitelist, does not make their layout stable
across a header change, and does not allow a consumer to name them (§2 already
forbids `detail` types as consumer-facing).

The other internal types do not all cross the boundary the same way, and the
difference matters. Only `call_state_base` and `call_state<Resp>` are
polymorphic; `connection_slot` and `client_liveness` are plain structs with
no vptr, and `rpc_detail::always_false` is stateless.
- `call_state_base` is the polymorphic base of `call_state<Resp>`; a consumer
  translation unit allocates the derived object, so a layout change to either
  is a change to a consumer-allocated size.
- `connection_slot` and `client_liveness` are plain structs with no vptr, and
  they are **allocated only by the library** (`src/client/client_facade.cpp:1146`
  and `:396`; there is no construction site in any header) and reached through
  `std::shared_ptr<connection_slot>` and `std::weak_ptr<client_liveness>`.
  Their size therefore never crosses the boundary as a consumer-allocated
  object. Their **member offsets are still baked into consumer translation
  units** by header-inline code, though: `live_owner` reads
  `client_liveness::alive` (`client_facade.hpp:399`) and `client::issue`
  reads `connection_slot::mutex`, `healthy` and `conn`
  (`client_facade.hpp:1079-1081`), both compiled in the consumer's own
  translation unit. Adding a field to either type still breaks a consumer built
  from older headers; for `client_liveness::alive` the failure is a missed
  death check that hands out a dangling `client*`. They are listed here
  because a consumer must still not *name* or *depend on* them (§2), and
  because a layout change to them is not invisible either.
- `rpc_detail::always_false` (`rpc/call.hpp`) is a stateless trait, so it has no
  layout to change.

`client::call<Resp>` holds exactly one member, a
`std::shared_ptr<client_detail::call_state<Resp>>`, so its size is always
`sizeof(std::shared_ptr<T>)` for some `T` — 16 bytes on x64 and 8 bytes on x86 —
whatever `call_state` contains. That stability covers the handle, not the heap
object it points at: the object's size still follows the headers it was
allocated with, which is the case measured above.

`rpc::lane_queue` (`rpc/lane_queue.hpp`) is the same kind of citizen: a
header-only class with no `CRPC_API`, wired into the server request path
and directly includable by tests. It carries no `CRPC_API` signature, so
its nested records never enter the cross-boundary list above; its layout
is fixed by the headers a consumer compiles against, like every other
header-only component.

## 3. Symbol visibility

- Every exported symbol is declared `CRPC_API` (see `include/crpc/config.hpp`).
- Windows: a shared build exports only `CRPC_API` symbols; everything else is
  hidden by the default linker behaviour.
- Linux/macOS: the shared build is compiled with
  `-fvisibility=hidden -fvisibility-inlines-hidden`; only `CRPC_API` symbols
  are default-visibility.
- The static library exports everything by definition; consumers that link it
  statically are not subject to the visibility rules but are still subject to
  the layout rules in §2.

Two rules follow from the first bullet, and both are easy to get wrong:

1. **Every public class that has an out-of-line member must carry `CRPC_API`
   itself.** Exporting a member function requires the decoration on that
   class's own declaration; a base class, a namespace or a sibling declaration
   does not export it. A class with only inline members needs nothing: the
   consumer compiles those into its own object file.
2. **The macro on an outer class does not cover a nested class.** `class
   CRPC_API outer { class nested { void f(); }; };` exports `outer`'s members
   but not `outer::nested::f`. Each nested class with out-of-line members must
   be decorated on its own declaration, as `server::builder`,
   `client::builder` and `http::http_server::builder` are.

The same applies to namespace-scope functions: a free function that is
declared in a public header and defined in `src/` must carry `CRPC_API` on its
declaration. A friend declaration inside a class introduces the name into the
enclosing namespace, so when a function is both a friend and a namespace-scope
entry point, both declarations must agree on the decoration (MSVC reports
C2375 otherwise).

`tests/test_shared_link.cpp`, built only under `CRPC_BUILD_SHARED=ON`, is the
gate for these rules. It is a consumer: it links against the import library and
calls the documented API directly, including two header-inline templates
(`client_detail::wake_functor` and `mvc::bind_request`) whose bodies reference
out-of-line members. A missing decoration therefore fails the build instead of
a downstream consumer. See [building.md](building.md) §4.

## 4. Struct layout

The rule for a public struct that crosses the ABI boundary is:

1. standard-layout and trivially copyable, or explicitly documented as opaque
   with only pointer/reference access,
2. free of virtual functions (virtual dispatch is confined to the transport
   interfaces, whose objects are never passed across a library boundary by
   value),
3. free of conditional members (no `#if` inside a public struct).

The headers do **not** currently enforce item 1 with layout assertions. None of
the public record and option structs (for example `frame_header`,
`crpc::connect_options`, `rpc::client_options`, `server::options`) has a
`static_assert(sizeof(...))` or `static_assert(alignof(...))`. The assertions
that do exist cover other contracts:

- `config.hpp:96-102` fixes the integer widths and the IEEE-754 formats;
- `observability/log.hpp:327` bounds `log_record` to 512 bytes;
- `rpc/call.hpp:559-566` bounds the inline callback storage.

Several public types (`rpc_error`, `buffer`, `endpoint`) contain owning
standard-library members and are therefore not trivially copyable. They are
passed by reference, returned as owning CRPC types, or documented as opaque;
they do not cross the C ABI by value. Adding per-struct `sizeof`/`alignof`
assertions to the headers is planned, not implemented. Until that exists, a
change to any public struct layout is a MAJOR change (§7), not a MINOR
extension.

The layout invariants that are checkable without header changes live in one
internal translation unit, `src/abi_check.cpp`: it asserts standard-layout for
15 of the public records, an exact member count for the 16 records listed in
that file, a relative member chain over the counters of `http_statistics`,
`client::statistics`, `client::resilience_statistics`,
`resilience::fallback_statistics` and `server::statistics`
(the chain follows each member's own width, so `server::statistics`, which mixes
`std::size_t` and `std::uint64_t`, is checked on both 32-bit and 64-bit
targets), the total width and last-field position of `http_statistics`, and, on
x86 and x64 only, absolute `sizeof` and key `offsetof` values for the records
listed in that file. It is compiled as part of the library, so a layout change
fails the build in the cases those guards cover.

The guards do **not** cover every type that crosses the ABI boundary, and the
set of records that do cross it is defined precisely rather than left open:

> **Scope.** A plain aggregate record that appears **by value** in a `CRPC_API`
> function signature, or that a virtual interface passes by value, is in scope.
> A record that appears **by reference or as an out-parameter** is also in scope
> **when the library writes to it or reads fields from it across the boundary**;
> a `const&` parameter is in scope as well when the callee compiles the field
> offsets into the library. Option structs that are only ever handed to inline
> constructors or builder methods are outside this scope, because they never
> cross a shared-library boundary: the caller compiles the constructor into its
> own object file, so both sides always agree on the layout.

The option structs that reach the library through an **out-of-line** function are
in scope, not excluded, because the caller builds the argument from its own
headers and the library reads it from its own: `http_server_options` and
`io::tls_config` (by value into `http_server::http_server` / `listen_tls`, and
`tls_config` returned by value from `tls_context::config`), `rpc::method_options`
(by value into `method_slot` / `service_registry::register_method`),
`push::subscribe_options` (by value into `hub::subscribe`),
`db::mysql::connection_options` (by value into the exported `mysql::connection`
constructor; its nested `tls` struct carries the TLS transport policy the
handshake reads), and
`resilience::rate_limit_config` / `breaker_config` / `bulkhead_config` /
`retry_policy` (by reference into the corresponding constructors). The structs
that are genuinely outside this scope are the ones with **no** out-of-line entry
point at all; the list below is maintained by `crpc_test_abi_docs`, not by hand.
`rpc::dedup_options` and `rpc::dedup_statistics` are the current examples of
that exclusion: the dedup table is configured only through the INLINE
`dispatcher(registry, dedup_options)` constructor and read only by the inline
`dedup_table::stats()`, so both records are consumed in the caller's own
translation unit and never cross the shared-library boundary. If those
constructors ever move out of line, `crpc_test_abi_docs` fails until the
records are guarded or listed.

`resilience::fallback_context` is in scope for the same reason even though no
signature spells its name: the library builds it and calls the consumer's
`fallback_fn` with it, so both sides must agree on the field offsets. It is
hand-anchored in `src/abi_check.cpp` (32 bytes on x86 and x64, offsets
`0/2/8/16/17/18/19/24`) and is therefore **not** in the unguarded list below.

Records **in scope** that have **no compile-time layout guard at all** are listed
below, fully qualified. This list is generated by `crpc_test_abi_docs`, which
rescans `include/crpc/**/*.hpp` and `src/abi_check.cpp`; the test fails if a new
cross-boundary record is added without updating this list. Names are fully
qualified because two different records are both called `frame_header`
(`::crpc::frame_header` and `::crpc::http2::frame_header`), and only one of them
is guarded.

- `::crpc::call_envelope`
- `::crpc::cluster::prevote_result`
- `::crpc::cluster::vote_result`
- `::crpc::cluster::registry_anchor`
- `::crpc::cluster::registry_deregister_request`
- `::crpc::cluster::registry_next_fence_request`
- `::crpc::cluster::registry_ping_request`
- `::crpc::cluster::registry_query_request`
- `::crpc::cluster::registry_query_response`
- `::crpc::cluster::registry_register_request`
- `::crpc::cluster::registry_set_state_request`
- `::crpc::cluster::registry_tag`
- `::crpc::cluster::registry_write_response`
- `::crpc::cluster::kv_get_request`
- `::crpc::cluster::kv_get_response`
- `::crpc::cluster::kv_put_request`
- `::crpc::cluster::kv_put_response`
- `::crpc::cluster::kv_delete_request`
- `::crpc::cluster::kv_delete_response`
- `::crpc::cluster::anchor_store`
- `::crpc::cluster::service_instance`
- `::crpc::cluster::raft_persistent_state`
- `::crpc::cluster::raft_state_store`
- `::crpc::cluster::raft_fence_authority`
- `::crpc::cluster::consul_error`

`::crpc::cluster::registry_query_row` is deliberately absent from the list: it is
nested inside `::crpc::cluster::registry_query_response` and never appears by
value in an exported signature of its own, so its layout changes are not listed
separately -- they surface through the parent record's serialisation.
- `::crpc::compression_limits`
- `::crpc::compression_statistics`
- `::crpc::crypto::uuid`
- `::crpc::crypto::uuid_v1_context`
- `::crpc::db::bulk_options`
- `::crpc::db::column_info`
- `::crpc::db::field_binding`
- `::crpc::db::hbase::endpoint`
- `::crpc::db::hbase::endpoint_health`
- `::crpc::db::hbase::row`
- `::crpc::db::hbase::scan_filter`
- `::crpc::db::hbase::scan_options`
- `::crpc::db::insert_target`
- `::crpc::db::mapping_options`
- `::crpc::db::mongo::aggregate_request`
- `::crpc::db::mongo::find_request`
- `::crpc::db::mongo::message_header`
- `::crpc::db::mongo::op_msg_reply`
- `::crpc::db::mongo::replica_set_topology`
- `::crpc::db::mongo::topology_member`
- `::crpc::db::mongo::write_concern_options`
- `::crpc::db::mysql::binary_row`
- `::crpc::db::mysql::cluster_config`
- `::crpc::db::mysql::column_definition`
- `::crpc::db::mysql::connection_options`
- `::crpc::db::mysql::discovered_replica`
- `::crpc::db::mysql::eof_packet`
- `::crpc::db::mysql::err_packet`
- `::crpc::db::mysql::handshake`
- `::crpc::db::mysql::handshake_response_options`
- `::crpc::db::mysql::ok_packet`
- `::crpc::db::mysql::prepare_ok`
- `::crpc::db::mysql::text_row`
- `::crpc::db::oracle::tns_accept`
- `::crpc::db::oracle::tns_connect`
- `::crpc::db::oracle::tns_header`
- `::crpc::db::oracle::tns_refuse`
- `::crpc::db::oracle::ttc_column`
- `::crpc::db::oracle::ttc_execute`
- `::crpc::db::oracle::ttc_fetch`
- `::crpc::db::oracle::ttc_value`
- `::crpc::db::parameter`
- `::crpc::db::replica_health`
- `::crpc::db::tds::cluster_config`
- `::crpc::db::tds::discovered_replica`
- `::crpc::db::tds::login7`
- `::crpc::db::tds::packet_header`
- `::crpc::db::tds::prelogin`
- `::crpc::db::tds::rpc_parameter`
- `::crpc::db::tds::rpc_request`
- `::crpc::flow::rtt_tracker`
- `::crpc::frame`
- `::crpc::fs::directory_entry`
- `::crpc::fs::file_info`
- `::crpc::ftp::client_options`
- `::crpc::ftp::ftp_command`
- `::crpc::ftp::ftp_reply`
- `::crpc::ftp::reply_parse_result`
- `::crpc::ftp::server_options`
- `::crpc::game::geom::aabb`
- `::crpc::game::geom::aabb3`
- `::crpc::game::geom::attack_profile`
- `::crpc::game::geom::attack_target2`
- `::crpc::game::geom::attack_target3`
- `::crpc::game::geom::circle`
- `::crpc::game::geom::cone`
- `::crpc::game::geom::convex_polyhedron`
- `::crpc::game::geom::hit_info2`
- `::crpc::game::geom::hit_info3`
- `::crpc::game::geom::line`
- `::crpc::game::geom::line3`
- `::crpc::game::geom::obb`
- `::crpc::game::geom::obb3`
- `::crpc::game::geom::plane3`
- `::crpc::game::geom::polygon`
- `::crpc::game::geom::ray`
- `::crpc::game::geom::ray3`
- `::crpc::game::geom::sector`
- `::crpc::game::geom::segment`
- `::crpc::game::geom::segment3`
- `::crpc::game::geom::sphere`
- `::crpc::game::geom::spread_config`
- `::crpc::game::geom::transform2d`
- `::crpc::game::geom::umbrella`
- `::crpc::game::geom::umbrella3`
- `::crpc::http2::frame_header`
- `::crpc::http2::goaway_payload`
- `::crpc::http2::header_field`
- `::crpc::http2::headers_payload`
- `::crpc::http2::priority_payload`
- `::crpc::http2::push_promise_payload`
- `::crpc::http2::setting`
- `::crpc::http::client_options`
- `::crpc::http::client_pool_options`
- `::crpc::http::coding_limits`
- `::crpc::http::forwarded_client`
- `::crpc::http::h2_client_options`
- `::crpc::http::multipart_limits`
- `::crpc::http::negotiation`
- `::crpc::http::range_parse_result`
- `::crpc::http::request`
- `::crpc::http::request_encoding_plan`
- `::crpc::http::response`
- `::crpc::http::serialize_options`
- `::crpc::http::sse::event_id`
- `::crpc::http::trusted_proxies`
- `::crpc::i18n::locale_format_options`
- `::crpc::inproc_pair`
- `::crpc::io::io_result`
- `::crpc::memcached::binary_header`
- `::crpc::memcached::connection_transport_config`
- `::crpc::memcached::failover_options`
- `::crpc::memcached::node_health`
- `::crpc::memcached::text_reply`
- `::crpc::mq::amqp::frame`
- `::crpc::mq::amqp::frame_header`
- `::crpc::mq::amqp::message_properties`
- `::crpc::mvc::binding_limits`
- `::crpc::mvc::file_part`
- `::crpc::mvc::route_entry`
- `::crpc::mvc::route_params`
- `::crpc::net::backoff_config`
- `::crpc::net::connect_options`
- `::crpc::net::icmp_echo_reply`
- `::crpc::net::icmp_options`
- `::crpc::net::icmp_ping_result`
- `::crpc::net::icmp_ping_statistics`
- `::crpc::net::ping_reply`
- `::crpc::net::resolve_options`
- `::crpc::net::retry_report`
- `::crpc::obs::log_record`
- `::crpc::obs::trace_context`
- `::crpc::obs::traceparent_result`
- `::crpc::orm::column_meta`
- `::crpc::orm::entity_meta`
- `::crpc::orm::sql::condition`
- `::crpc::orm::sql::order_term`
- `::crpc::orm::sql::statement`
- `::crpc::push::push_envelope`
- `::crpc::push::push_subscription`
- `::crpc::push::subscribe_request`
- `::crpc::redis::bitmap_type`
- `::crpc::redis::geo_type`
- `::crpc::redis::hash_type`
- `::crpc::redis::hyperloglog_type`
- `::crpc::redis::list_type`
- `::crpc::redis::notification`
- `::crpc::redis::redis_stream_entry`
- `::crpc::redis::resp_array_value`
- `::crpc::redis::resp_big_number`
- `::crpc::redis::resp_blob_error`
- `::crpc::redis::resp_boolean`
- `::crpc::redis::resp_bulk_string`
- `::crpc::redis::resp_double`
- `::crpc::redis::resp_error`
- `::crpc::redis::resp_integer`
- `::crpc::redis::resp_map_value`
- `::crpc::redis::resp_null`
- `::crpc::redis::resp_push_value`
- `::crpc::redis::resp_set_value`
- `::crpc::redis::resp_simple_string`
- `::crpc::redis::resp_verbatim_string`
- `::crpc::redis::scored_member`
- `::crpc::redis::sentinel_reply`
- `::crpc::redis::set_type`
- `::crpc::redis::stream_type`
- `::crpc::redis::string_type`
- `::crpc::redis::value`
- `::crpc::redis::zset_type`
- `::crpc::resilience::call_traits`
- `::crpc::resilience::instance`
- `::crpc::resilience::retry_policy`
- `::crpc::resilience::smart_policy`
- `::crpc::system::cpu_affinity`
- `::crpc::system::cpu_info`
- `::crpc::system::disk_info`
- `::crpc::system::memory_info`
- `::crpc::system::process_info`
- `::crpc::system::process_options`
- `::crpc::system::process_result`
- `::crpc::text::pinyin_options`
- `::crpc::time::date_time`
- `::crpc::time::local_time_options`
- `::crpc::time::local_time_result`
- `::crpc::time::timezone_database_info`
- `::crpc::time::timezone_info`
- `::crpc::ws::client_options`
- `::crpc::ws::frame_header`

These records are governed only by the policy in section 7 (any change to a
public struct layout is a MAJOR change), not by a build-time check. Most of them
could be added to the member-count guard; `::crpc::endpoint` cannot, because its
members are private, and it is outside the scope above for the same reason (it is
returned by value from a virtual method, but it is an encapsulated class rather
than a plain aggregate).

`server::builder` is a second encapsulated exported class that needs an explicit
statement. It is decorated (`class CRPC_API builder`) and its out-of-line member
functions read the private `options_` member of a consumer-allocated object, so
its layout is ABI-relevant; it is not in the list above because it is not a plain
aggregate record. The member-count and `offsetof` guards cannot be applied to its
private data members, and its width is not a fixed number: it is
`sizeof(server::options)` plus its three `std::vector` members and the borrowed
`obs::interceptor_chain*` observability-chain pointer, rounded to its
alignment, and the vector width changes with the standard library's debug
configuration (MSVC 19.44: x64 is 400 bytes in Release and 424 in Debug;
x86 is 304 and 320).
`src/abi_check.cpp` therefore pins the structural relation rather than an
absolute size, and requires the builder to keep the options alignment; that
catches an added, removed or re-typed builder member on every target and
configuration. A reorder of the three same-width vectors, like any
other change the guards cannot see, remains MAJOR under §7.

The guards are not uniform across change kinds or across architectures, and the
difference matters when judging whether a given change is caught:

- A reorder, an insertion between two existing members, or a removal is caught
  by the relative chains, on every architecture, but only on records that have
  a chain. The member count does **not** help here: a reorder leaves the count
  unchanged, so it provides no coverage for a reorder at all. `rpc_error` has no
  relative chain, so a reorder inside `rpc_error` is not caught.
- A rename is caught by a relative chain where one exists; in `rpc_error` it is
  caught by the member-type assertions in `src/abi_check.cpp`. The member-count
  guard does **not** validate names: a structured binding only binds one local
  name per member in declaration order. The only cross-ABI record with no
  relative chain is `rpc_error` among the 16 member-counted records (the
  unguarded records in §4 have no chain at all, and `status` has no public data
  member, so a rename cannot occur there), so a rename in `rpc_error` is not
  caught by a chain. The four HTTP/2 value types `http2::h2_error`,
  `http2::connection_options`, `http2::stream_snapshot` and
  `http2::window_credit` **do** have `CRPC_AFTER` chains, so their renames are
  caught. For `rpc_error`, the unit names all seven members (`code`, `message`,
  `retryable`, `retry_after`, `metadata`, `origin`, `attempts`) in `decltype`
  type assertions, so a rename fails that translation unit; its constructors name
  only `code`, `message` and `retryable`, so they do not catch a rename of
  `retry_after`, `metadata`, `origin` or `attempts`.
- A field appended at the end is caught by the member count on every
  architecture, and additionally by the absolute `sizeof` anchor on x86 and x64.
  The member count is what makes the x86 case work: on x86
  `server::statistics` is 104 bytes while its last field ends at 100, so an
  appended `std::size_t` is absorbed by the 4 bytes of tail padding and
  `sizeof` does not change. An absolute `sizeof` assertion alone is therefore
  blind to a tail append on x86, which is why the member count exists.
- On architectures other than x86 and x64 (for example ARM64) no absolute
  `sizeof` or `offsetof` value is maintained. A tail append is still caught by
  the member count, but a change that preserves both the member count and the
  relative order while altering the absolute width is **not** caught at compile
  time on those targets.

The unit does not pin the size of types that own standard-library members
(`rpc_error`, `status`, `buffer`, `endpoint`), because their width is a property
of the standard library configuration rather than of this library: `rpc_error`
and `status` measure 88 bytes on x64 Release and 104 in Debug, and 64 versus 72
on x86, with the same headers. What actually guards each of them differs, and
only the first is fully guarded:

- `rpc_error` has public members, so it is covered by its member count and by an
  assertion on the declared type of every member (`code`, `message`,
  `retryable`, `retry_after`, `metadata`, `origin`, `attempts`). Both are
  independent of the build type.
- `status` has no public data member, so neither a member count nor a member
  type can be asserted. It is covered only by the
  `sizeof(status) == sizeof(rpc_error)` and `alignof(status) ==
  alignof(rpc_error)` relations, which catch added storage but not a change
  inside the wrapped `rpc_error` that keeps its size.
- `buffer` and `endpoint` have **no layout guard beyond a minimum alignment**
  (`alignof` at least `alignof(void*)`). Their size and members are unguarded
  and are governed only by the section 7 policy.

None of the four is covered by a standard-layout assertion, and none carries an
absolute `sizeof`. Member **type** changes in `status` are therefore not caught,
because its absolute size is deliberately not pinned; `rpc_error` is the only
one of the four whose member types are pinned.

These four are not asserted to be standard-layout either, although `rpc_error`,
`status` and `endpoint` in fact are on the toolchains audited here (MSVC 19.51,
GCC 13, Clang 18), and `buffer` is on MSVC and GCC but not on Clang. The reason
is not that `offsetof` would be ill-formed — it is well-formed and yields the
expected values — but that the offsets are a property of the standard-library
configuration, the same dependency that makes the absolute size unpinnable. An
`is_standard_layout` assertion would also fire on `buffer` under Clang for a
reason unrelated to any CRPC layout change.

There is no size-tag pattern today. No public header declares a `struct_size`
member; `crpc::connect_options` contains only the timeout, buffer and no-delay
fields (`include/crpc/io.hpp:186-191`). The `struct_size` assignment at
`src/runtime/runtime.cpp:101` is on a local `coro_rt_driver_ops` from the corocc
dependency and is not a CRPC ABI mechanism. A size-tagged options struct is
planned, not implemented; while it is absent, callers and the library must be
built from the same headers.

The 1.0.0 layout (`bd006b2`), the last compatible release, had 9 fields and
`sizeof` was 72 bytes; it had no long-poll counters at all. In the 2.0.0
release the struct gained 19 fields: the whole long-poll block
(`longpoll_waiters`, `longpoll_subscribers`, `longpoll_replay_window`,
`longpoll_events`, `longpoll_timeouts`, `longpoll_ack_clamped`,
`longpoll_conflicts`, `longpoll_restarts`, `longpoll_overloaded`) plus ten
SSE/compression/download counters. `sizeof` grew from 72 (9 fields) to 224
(28 fields). The nine fields that 1.0.0 shipped keep their offsets
(`active_connections` 0 … `timed_out_connections` 64). Adding a field to a
public struct is a MAJOR change (§1), so 2.0.0 is ABI breaking.

The move of `longpoll_replay_window` is internal to the 2.0.0 release: it
was first placed at offset 168, before the six counters, and later moved to the
end (offset 216). The counters that followed it then each moved one
`std::size_t` earlier (8 bytes on the audited 64-bit builds):

| member | before the move (`4adf359`) | new offset |
|---|---|---|
| `longpoll_events` | 176 | 168 |
| `longpoll_timeouts` | 184 | 176 |
| `longpoll_ack_clamped` | 192 | 184 |
| `longpoll_conflicts` | 200 | 192 |
| `longpoll_restarts` | 208 | 200 |
| `longpoll_overloaded` | 216 | 208 |

`sizeof` stays 224 across that move because no field was added or removed: in
`4adf359` the six counters followed `longpoll_replay_window` (which ended at
offset 176) and already filled the struct to 224, so there was no trailing
padding then and none now. No released header ever carried the offset-168
layout (there is no tag and no 1.0.1), so the internal move is not itself a
compatibility break for published consumers; the 72 → 224 growth is. The change
is ABI breaking (MAJOR) unless a size-tagged options struct exists (§7).

Additive types do not change existing layouts. `client::resilience_statistics`
(a standard-layout, trivially copyable value type, §4) and
`client::resilience_stats()` are MINOR additions; they add symbols only.
`client::statistics` keeps its field order, size and meaning. Likewise,
`resilience::fallback_statistics` (a standard-layout, trivially copyable
seven-counter value type) and `client::fallback_stats()` are MINOR additions.
The fallback primitives in `crpc/resilience/fallback.hpp` are header-only and
add no exported symbol; only the by-value return adds one.

`server::options` gained a trailing `resilience::clock_source clock_source`
field. Every existing member keeps its offset (`send_goaway_on_stop` stays 249
on x64 and 225 on x86, `push_subscribe` stays 264 / 232), so a consumer that
only reads those members keeps working **within one header set**, but
`sizeof(server::options)` grows from 304 to **320** on x64 and from 256 to
**264** on x86, and `sizeof(server::builder)` grows with it (376 → **400** on
x64, 296 → **304** on x86 in Release) because the builder holds the options
by value. Passing an
options struct by value into the library constructor is an exported signature,
so this is a **MAJOR** change under §1, not a MINOR extension: a consumer must
be rebuilt from the same headers. The same field is the one injection point for
the server clock: the per-connection rate bucket, the per-service rate bucket,
the per-service circuit breaker and the dispatcher's deadline check all read it,
so a partially injected server would interpret one request's deadline against
two different "now" values.

`client::set_fallback`, `client::run_fallback` and `client::record_fallbacks`
are new exported symbols and therefore MINOR additions; they change no layout.

The AOP call-path wiring (requirement 17) changes two exported surfaces. First,
`server::builder` gained a trailing private `obs::interceptor_chain* aop_chain_`
data member installed by the new `builder::interceptor_chain(obs::interceptor_chain&)`
method; the encapsulated exported class's layout changes, so this is a **MAJOR**
change under §1: `sizeof(server::builder)` grows by one pointer and the structural
relation in `src/abi_check.cpp` was extended to the options-plus-three-vectors-
plus-pointer form. Second, `rpc::dispatcher` gained a new `dispatch` overload
taking a trailing `const metadata_list* context_metadata` pointer; a new overload
is a new exported symbol (a **MINOR** addition) and the existing overloads keep
their mangled names and behaviour, with `nullptr` meaning "no injection", which
is what every call site without an interceptor chain passes. Neither change
touches `server::options`, any wire format, or any cross-boundary record:
`obs::interceptor_chain` and `obs::call_context` remain header-inline types with
no exported data layout, and the response/error encoding is unchanged.
`set_fallback` takes a `void*` plus a `noexcept` `retain`/`release` pair rather
than a standard-library owning type, so the reference it keeps is created and
destroyed by the caller's own code and no consumer-side control block crosses
the boundary.

`resilience::min_remaining_ns` (1'000'000 ns) is the single authority for the
server's admission floor: the dispatcher's pre-execution deadline refusal and
the server's four `crpc-degradable` decisions both read it. The client has **no**
equivalent floor — its deadline checks only ask whether the call already expired
locally, which is deliberate asymmetry, not a defect. Any future client-side
"give up early" floor must reuse this constant; a second `1'000'000` or `1ms`
literal would be a second deadline policy. `fallback_policy::min_remaining` is a
different, caller-configured floor ("is the degraded value still deliverable");
`set_fallback` documents that callers pass this constant for it, and the
signature carries no default argument, so no literal can hide there.

## 5. Enum values

- Enum values that are written to the wire are fixed forever; they are never
  renumbered or reused.
- New values are appended at the end.
- A decoder that receives an unknown value must fail closed (`protocol_error`)
  rather than map it to a default.

## 6. C ABI layer

The C++ API is the primary interface. A C ABI (`include/crpc/c_api.h`) is
provided for language bindings and plugin authors:

- opaque handles (`crpc_client*`, `crpc_server*`),
- no C++ types in signatures,
- all strings as `(const char*, size_t)` pairs,
- status returned as `int` with a documented enumeration,
- callbacks carry a `void* user` context and never throw.

The export decoration is selected at compile time and mirrors `CRPC_API`:

| Build | Macros | Windows | GCC/Clang |
|---|---|---|---|
| static library (default) | neither | none | none |
| shared library, building | `CRPC_SHARED` + `CRPC_BUILDING` | `dllexport` | default visibility |
| shared library, consuming | `CRPC_SHARED` | `dllimport` | none |

The C ABI is versioned separately (`CRPC_C_ABI_VERSION`) and follows the same
MAJOR/MINOR rules.

Every function is implemented in `src/c_api.cpp` over the C++ facade; there are
no declaration-only symbols. `tests/test_c_api.c` is compiled as a C
translation unit (the CMake target sets `LINKER_LANGUAGE CXX`) so a broken
decoration or a missing definition fails the build rather than a downstream
consumer.

## 7. Compatibility matrix

A binary built against version X must link and run against version Y when:

| X → Y | Guarantee |
|---|---|
| same MAJOR, Y.MINOR ≥ X.MINOR | Yes (forward compatible) |
| same MAJOR, Y.MINOR < X.MINOR | No (a newer build may use newer symbols) |
| different MAJOR | No |

Every row of this table holds only when the consumer and the library are built
from the same headers. A forward-compatible pairing does not license mixing an
old header with a newer binary: §2 shows that the header-inline templates
instantiate `client_detail` types in the consumer's translation unit, so a
layout change in those types breaks that mix even inside one MAJOR version.

Compiler and standard library: CRPC does not promise ABI compatibility across
different standard libraries (MSVC STL vs libstdc++ vs libc++) or across
different `_ITERATOR_DEBUG_LEVEL` / `_GLIBCXX_DEBUG` settings. A shared library
must be built with the same toolchain family and debug setting as its consumer.
This is stated in the build documentation only: there is no CMake
configure-time check for such a mismatch, and adding one is not implemented.

## 8. Change process

1. A change that breaks §2–§5 requires a MAJOR version bump.
2. The same commit updates this document and the changelog
   ([CHANGELOG.md](CHANGELOG.md)), and records the breaking change in the migration
   guide ([migration.md](migration.md)) when the MAJOR bump is a layout or
   signature change; a compatibility shim is added when feasible.
3. The changelog states explicitly: "ABI breaking: <symbol> changed from … to …".
4. The export gate `scripts/export_gate.py` is run by `scripts/test.ps1` and
   `scripts/test.sh` whenever `CRPC_BUILD_SHARED=ON`. It statically scans
   `include/crpc/**` and requires: (A) every non-template class with
   out-of-line members carries `CRPC_API`; (B) every decorated class actually
   exports a symbol, checked against `dumpbin /exports` or `nm -D -C`; (C)
   every namespace-scope declaration without a body carries `CRPC_API`; (D) no
   class template carries `CRPC_API`; and (E) an exported class does not derive
   from an undecorated non-template base. Reviewed implementation-detail
   exceptions live in `scripts/export_baseline.txt`; a missing or stale entry
   fails the gate. The internal `src/abi_check.cpp` translation unit guards
   the layout of the public records at compile time. Neither compares the
   export table against the previous release: that comparison remains a manual
   review of the diff against the previous release.

## 9. What is not guaranteed

- Template instantiations: `result<T>`, `span<T>` and the interceptor chain are
  header-only and inline into the consumer; their ABI is the consumer's.
- Inline functions in headers: a consumer compiled with different flags may
  inline a different version; this is why no behaviour that matters for
  correctness lives only in an inline function.
- The layout of `std::variant` inside `result<T>` when `T` is a user type: it
  is stable for a given toolchain and `T`, not across toolchains.
- Exception behaviour at the hook and handler boundary is not part of the ABI;
  the contract is in [architecture §15](architecture.md#s15).
