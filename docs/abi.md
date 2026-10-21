# CRPC ABI policy

[简体中文](abi.zh-CN.md) | **English**

Version 1.0 / 2026-10-19

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
- bit-fields, anonymous unions, packed structs.

Where a container is genuinely needed, the API takes a `span` or a
`const char*` plus a length, or returns an owning CRPC type that is documented
as opaque.

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
the public records, an exact member count for every record that crosses the
boundary, a relative member chain over the counters of `http_statistics`,
`client::statistics`, `client::resilience_statistics` and `server::statistics`
(the chain follows each member's own width, so `server::statistics`, which mixes
`std::size_t` and `std::uint64_t`, is checked on both 32-bit and 64-bit
targets), the total width and last-field position of `http_statistics`, and, on
x86 and x64 only, absolute `sizeof` and key `offsetof` values for the records
listed in that file. It is compiled as part of the library, so a layout change
fails the build in the cases those guards cover.

The guards are not uniform across change kinds or across architectures, and the
difference matters when judging whether a given change is caught:

- A reorder, an insertion between two existing members, or a removal is caught
  by the relative chains on the counter records, and by the member count on
  every architecture. Neither guard depends on absolute numbers.
- A rename is caught by the relative chains where a chain exists. The
  member-count guard does **not** validate names: a structured binding only
  binds one local name per member in declaration order, so a rename in a record
  with no relative chain (`rpc_error`, `status`, `http2::*`) is not caught here.
  A rename in `rpc_error` is nevertheless a compile error in the header itself,
  because its constructors initialize the renamed member.
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
and `status` measure 80 bytes on x64 Release and 96 in Debug, and 56 versus 64
on x86, with the same headers. They are guarded by their member count, their
standard-layout precondition, and the `sizeof(status) == sizeof(rpc_error)`
relation. Member **type** changes in `rpc_error` and `status` are not caught by
those guards, because their absolute size is deliberately not pinned. The
declared types of the `rpc_error` members are asserted individually instead
(`code`, `message`, `retryable`, `retry_after`, `metadata`), which is
independent of the build type; `status` has no public data member and is
covered only by the `sizeof`/`alignof` relation.

There is no size-tag pattern today. No public header declares a `struct_size`
member; `crpc::connect_options` contains only the timeout, buffer and no-delay
fields (`include/crpc/io.hpp:186-191`). The `struct_size` assignment at
`src/runtime/runtime.cpp:101` is on a local `coro_rt_driver_ops` from the corocc
dependency and is not a CRPC ABI mechanism. A size-tagged options struct is
planned, not implemented; while it is absent, callers and the library must be
built from the same headers.

The last released layout is 1.0.0 (`bd006b2`), where `http_statistics` had 9
fields and `sizeof` was 72 bytes; it had no long-poll counters at all. In this
unreleased cycle the struct gained 19 fields: the whole long-poll block
(`longpoll_waiters`, `longpoll_subscribers`, `longpoll_replay_window`,
`longpoll_events`, `longpoll_timeouts`, `longpoll_ack_clamped`,
`longpoll_conflicts`, `longpoll_restarts`, `longpoll_overloaded`) plus ten
SSE/compression/download counters. `sizeof` grew from 72 (9 fields) to 224
(28 fields). The nine fields that 1.0.0 shipped keep their offsets
(`active_connections` 0 … `timed_out_connections` 64). Adding a field to a
public struct is a MAJOR change (§1), so this cycle is ABI breaking.

The move of `longpoll_replay_window` is internal to that unreleased cycle: it
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
`client::statistics` keeps its field order, size and meaning.

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

Compiler and standard library: CRPC does not promise ABI compatibility across
different standard libraries (MSVC STL vs libstdc++ vs libc++) or across
different `_ITERATOR_DEBUG_LEVEL` / `_GLIBCXX_DEBUG` settings. A shared library
must be built with the same toolchain family and debug setting as its consumer.
This is stated in the build documentation only: there is no CMake
configure-time check for such a mismatch, and adding one is not implemented.

## 8. Change process

1. A change that breaks §2–§5 requires a MAJOR version bump.
2. The same commit updates this document and the changelog
   ([CHANGELOG.md](CHANGELOG.md)). A migration guide (`docs/migration.md`) does
   not exist; a compatibility shim is added when feasible.
3. The changelog states explicitly: "ABI breaking: <symbol> changed from … to …".
4. No ABI checker is implemented: `scripts/abi_check.ps1` and
   `scripts/abi_check.sh` do not exist, and CI does not compare exported symbols
   or layout assertions. The internal `src/abi_check.cpp` translation unit
   guards the layout of the public records at compile time, but it is not a
   symbol-table comparison against the previous release. Until such a job
   exists, the comparison is a manual review of the diff against the previous
   release.

## 9. What is not guaranteed

- Template instantiations: `result<T>`, `span<T>` and the interceptor chain are
  header-only and inline into the consumer; their ABI is the consumer's.
- Inline functions in headers: a consumer compiled with different flags may
  inline a different version; this is why no behaviour that matters for
  correctness lives only in an inline function.
- The layout of `std::variant` inside `result<T>` when `T` is a user type: it
  is stable for a given toolchain and `T`, not across toolchains.
