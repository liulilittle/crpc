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
| Add a new public symbol, add a new enum value at the end, add an optional field at the end of an options struct that is passed by pointer plus a size | MINOR | Compatible |
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

Every public struct that crosses the ABI boundary is:

1. standard-layout and trivially copyable, or explicitly documented as opaque
   with only pointer/reference access,
2. covered by `static_assert` on `sizeof` and `alignof` in the header,
3. free of virtual functions (virtual dispatch is confined to the two transport
   interfaces, whose objects are never passed across a library boundary by
   value),
4. free of conditional members (no `#if` inside a public struct).

Options structs follow the size-tag pattern:

```cpp
struct connect_options {
    std::uint32_t struct_size;   // = sizeof(connect_options)
    // ... fields ...
};
```

A newer library reads `struct_size` and only touches fields the caller
guarantees to be present. This is how a MINOR release adds a field without
breaking older callers.

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
must be built with the same toolchain family and debug setting as its consumer;
this is stated in the build documentation and checked by a CMake configure-time
warning when a mismatch is detectable.

## 8. Change process

1. A change that breaks §2–§5 requires a MAJOR version bump.
2. The same commit updates this document, the migration guide
   (`docs/migration.md`), and adds a compatibility shim when feasible.
3. The changelog states explicitly: "ABI breaking: <symbol> changed from … to …".
4. CI runs the ABI check job (`scripts/abi_check.ps1` / `.sh`), which compares
   the exported symbol list and the layout assertions of the current build
   against the previous release. A mismatch fails the build unless the commit
   carries the MAJOR bump marker.

## 9. What is not guaranteed

- Template instantiations: `result<T>`, `span<T>` and the interceptor chain are
  header-only and inline into the consumer; their ABI is the consumer's.
- Inline functions in headers: a consumer compiled with different flags may
  inline a different version; this is why no behaviour that matters for
  correctness lives only in an inline function.
- The layout of `std::variant` inside `result<T>` when `T` is a user type: it
  is stable for a given toolchain and `T`, not across toolchains.
