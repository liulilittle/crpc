# Building and testing CRPC

[简体中文](building.zh-CN.md) | **English**

Version 1.0 / 2026-10-19

This document describes how to configure, build, test and diagnose CRPC on
Windows and Linux. The one-click scripts are the supported entry point; CI is
designed to call the same scripts rather than reimplement their logic.

## Contents

| # | Section |
|---|---|
| [1](#s1) | Prerequisites |
| [2](#s2) | Dependency checkouts |
| [3](#s3) | CMake options |
| [4](#s4) | Static and shared builds |
| [5](#s5) | One-click scripts |
| [6](#s6) | Build layout and artifacts |
| [7](#s7) | Sanitizers |
| [8](#s8) | Unattended Windows tests |
| [9](#s9) | CI reuse |
| [10](#s10) | Troubleshooting |
| [11](#s11) | Verification |

## 1. Prerequisites <a id="s1"></a>

- CMake 3.16 or newer.
- A C++17 compiler: MSVC 2022, GCC 9+, or Clang 10+. The build enables
  `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang) or `/W4 /WX` (MSVC); warnings
  are errors.
- Ninja is preferred; without it the scripts fall back to NMake Makefiles
  (Windows) or the default Unix generator. The compiler is auto-detected:
  `vswhere`/`vcvars` for MSVC, `PATH` for GCC/Clang.
- A filesystem with room for two build trees when `Both` is requested. All
  artifacts go under `.opencode/tmp` by default; no build output is placed in
  the source tree or on `C:` by the scripts.

Windows x86/x64 and Linux x64 are the tested targets. `config.hpp` also accepts
macOS, but only the in-process backend is available there; the epoll backend is
Linux-only and the IOCP backend is Windows-only.

## 2. Dependency checkouts <a id="s2"></a>

Two optional dependencies are consumed through their public headers and CMake
targets:

- **RTTI** — reflection and serialization (`include/rtti/reflect.hpp`).
- **corocc** — stackful coroutines (`include/corocc/coro.h`).

Point CMake at their checkout roots with cache variables or environment
variables:

```powershell
$env:CRPC_RTTI_DIR   = 'F:\RTTI'
$env:CRPC_COROCC_DIR = 'F:\corocc'
.\scripts\build.ps1 -Config Debug -Arch x64
```

```sh
export CRPC_RTTI_DIR=/path/to/RTTI
export CRPC_COROCC_DIR=/path/to/corocc
./scripts/build.sh --config Debug --arch x64
```

The variables are read at configure time only when the cache value is empty;
the scripts forward the environment and need no extra arguments. Each root is
validated (`include/rtti/reflect.hpp`, `include/corocc/coro.h`) and added with
`add_subdirectory(... EXCLUDE_FROM_ALL)` under the build tree's `_deps`
directory. RTTI's own test entries are excluded by name from CTest because they
are not built under `EXCLUDE_FROM_ALL`. `COROCC_BUILD_TESTS` is forced off.

The strict warning policy (`-Wall -Wextra -Wpedantic -Werror` on GCC/Clang,
`/W4 /WX` on MSVC) is attached to crpc's own targets with
`target_compile_options`, not to the directory, so it never reaches these
vendored checkouts: a dependency's warnings cannot fail the crpc build, and
crpc's `-Wpedantic` is not imposed on third-party code.

If a dependency is absent, the corresponding `CRPC_HAVE_*` macro is off:
RTTI-dependent codecs are not registered, the MVC field-attribute macros are not
defined, and coroutine-backed components are not compiled. The current CRPC sources do
not call `coro_rt_*`; corocc is linked but its runtime is not yet used. This is
stated so a build without corocc is not mistaken for a broken build.

## 3. CMake options <a id="s3"></a>

| Option | Default | Meaning |
|---|---|---|
| `CRPC_BUILD_TESTS` | `ON` | build `tests/` and register CTest entries |
| `CRPC_BUILD_BENCH` | `OFF` | add `bench/`; the directory currently has no `CMakeLists.txt`, so keep it off |
| `CRPC_BUILD_EXAMPLES` | `OFF` | add `examples/`; same caveat |
| `CRPC_WARNINGS_AS_ERRORS` | `ON` | `/W4 /WX` or `-Werror` |
| `CRPC_ENABLE_LTO` | `ON` | IPO/LTO for Release when supported and no sanitizer is active |
| `CRPC_BUILD_SHARED` | `OFF` | build a shared library instead of a static archive |
| `CRPC_WITH_OPENSSL` | `OFF` | find and link OpenSSL, define `CRPC_HAVE_OPENSSL` |
| `CRPC_SANITIZER` | empty | `address`, `undefined`, or `thread` |
| `CRPC_RTTI_DIR` | empty | RTTI checkout root (env fallback) |
| `CRPC_COROCC_DIR` | empty | corocc checkout root (env fallback) |

`CRPC_WITH_OPENSSL` links `OpenSSL::SSL` and `OpenSSL::Crypto` and defines
`CRPC_HAVE_OPENSSL`. The TLS decorator is implemented in
`src/io/tls_openssl.cpp` and exposed through `include/crpc/io/tls.hpp` and
`http_server::listen_tls`; public headers never include an OpenSSL header. With
the option off, the same entry points return `tls_errc::ssl_not_available` and
`test_tls` prints its documented skip line.

Verification status: the guarded branch was built and run under WSL with
OpenSSL 3.0.13 (8 cases / 80 assertions green, `CRPC_WITH_OPENSSL=ON`). On
Windows only the unguarded build has been exercised: the OpenSSL branch has not
been linked or run there, and the source records that it was code-reviewed
rather than compiled in that environment. Treat Windows HTTPS as unverified.
Evidence: `.opencode/plan.md` P10, `.opencode/process.md:108-110`,
`src/io/tls_openssl.cpp:9-11`.

### Compiler feature requirements

**Exceptions.** Headers build without them; the library does not.

- *Prebuilt library, `CRPC_HAVE_RTTI=0`:* all 52 public headers compile with `-fno-exceptions` and with MSVC `/EHs-c-`. The library itself must be the default exception-enabled build; only the consumer switches exceptions off.
- *Rebuilding the library without exceptions is not supported:* 11 of the 46 `src/` TUs keep `try`/`catch` (GCC names the `catch` at `src/observability/log.cpp:270`, `src/c_api.cpp:291`, `src/rpc/dispatcher.cpp:368`; MSVC names the `try` two lines earlier): GCC fails, MSVC reports C4530 (C2220 under `/WX`). CMake has no switch that removes exception handling.
- *`CRPC_RTTI_DIR` + `-fno-exceptions`:* fails at the header stage, before any `src/` file: MSVC reports C4530 (C2220 under `/WX`) at the `try` on `rtti/convert.hpp:2597`, GCC reports "exception handling disabled" at the `catch` on `:2599`.

Throw semantics per boundary: [architecture §15](architecture.md#s15). These flags decide what builds, not how a throw is recovered.

**RTTI.** `-fno-rtti` / `/GR-` work only with `CRPC_HAVE_RTTI=0`; with the reflection library the failure is inside the dependency.

- MSVC `/GR-` + `CRPC_RTTI_DIR`: C4541 at `rtti/proto.hpp:2527,2529` (`dynamic_cast` is UB under `/GR-`); without `/WX` it exits 0 with two C4541 warnings (compiled, but UB), with `/WX` that becomes C2220.
- GCC/Clang `-fno-rtti` + `CRPC_RTTI_DIR`: the first error is the dependency's `rtti/reflection.hpp:2183` (its own `typeid`), not a CRPC header.
- Without the reflection library the 52 headers build: text/schema codecs are not registered and MVC binding returns `failed_precondition` (HTTP 409) "binding requires the RTTI dependency" (`mvc/binding.hpp:533`). Separately, under `-fno-rtti` the type-erased codec dispatch is compiled out behind the compiler-RTTI guard in `codec.hpp`, and `encode_into`/`decode_from` return `encode_error`/`decode_error` instead of reaching `typeid`.

`CRPC_HAVE_RTTI` means "the rtti reflection library is available", not compiler RTTI; the two are orthogonal. With no reflection checkout it is `0`: text/schema codecs are not registered, MVC field-attribute macros are undefined, and the binary codec is a stub reporting encode/decode errors.

```bat
rem MSVC: 52 headers, no exceptions, no compiler RTTI, warnings are errors.
for /r include %f in (*.hpp *.h) do @cl /nologo /std:c++17 /EHs-c- /GR- /W4 /WX /TP /Zs /I include %f
```

```sh
# GCC: 52 headers, no exceptions, warnings are errors.
find include -name '*.hpp' -o -name '*.h' | while read -r f; do
  g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Werror \
      -fno-exceptions -I include "$f" || echo "FAIL $f"
done
# Rebuilding the library without exceptions must fail (11 of 46 TUs):
g++ -std=c++17 -I include -fno-exceptions -c src/observability/log.cpp -o /tmp/log.o
```

The library is one target, `crpc` (alias `crpc::crpc` and `crpc_core`).
Sources are globbed recursively from `src` with `CONFIGURE_DEPENDS`, so adding a
file does not require editing `CMakeLists.txt`.

## 4. Static and shared builds <a id="s4"></a>

The default is a static archive. `-DCRPC_BUILD_SHARED=ON` builds a shared
library and defines `CRPC_SHARED` (public) and `CRPC_BUILDING` (private). The
shared build sets hidden visibility, hides inline symbols, disables automatic
export of all symbols on Windows, and on non-MSVC adds
`-Wl,--no-undefined`. On MSVC, C4251 is suppressed because private STL members
never appear in a signature; the layout rules in [ABI policy](abi.md) still
apply, and a shared library must use the same toolchain family and debug setting
as its consumer.

## 5. One-click scripts <a id="s5"></a>

Windows:

```powershell
.\scripts\build.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
                    [-Sanitizer none|asan|ubsan|tsan] [-Jobs N] [-Clean]
                    [-SourceDir <path>] [-BuildRoot <path>]
.\scripts\test.ps1  [-Config Debug|Release|Both] [-Arch x86|x64]
                    [-Sanitizer none|asan|ubsan|tsan|all] [-Filter <regex>]
                    [-Repeat N] [-SourceDir <path>] [-BuildRoot <path>]
```

Linux/macOS:

```sh
./scripts/build.sh [--config Debug|Release|Both] [--arch x86|x64]
                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan]
                   [--jobs N] [--clean] [--source DIR] [--build-root DIR]
./scripts/test.sh  [--config Debug|Release|Both] [--arch x86|x64]
                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan|all]
                   [--filter REGEX] [--repeat N] [--source DIR] [--build-root DIR]
```

Build behavior: `Config Both` builds Debug and Release in separate directories.
`-Jobs`/`--jobs` defaults to the processor count. `-Clean`/`--clean` removes
only the selected build directory, never the source or the whole artifact root.
The Windows script runs CMake inside the detected `vcvars` environment when one
is available; the POSIX script selects `g++`/`clang++` explicitly and passes
`-m32` for `--arch x86`.

Test behavior: `test.ps1`/`test.sh` build first (calling the build script), then
run CTest in each build directory and inspect the output for sanitizer reports.
`-Filter`/`--filter` is a CTest regex. `-Repeat`/`--repeat` runs the whole CTest
set N times. `-Sanitizer all` builds and runs ASan, UBSan and TSan in separate
directories; this is a Linux/Clang workflow, because MSVC has no TSan.

Every script writes a timestamped UTF-8 log and exits non-zero on the first
failure summary. PowerShell uses `[System.IO.File]::AppendAllText` with UTF-8
rather than `Tee-Object`, which writes UTF-16 on PowerShell 5.1.

## 6. Build layout and artifacts <a id="s6"></a>

The default artifact root is `<source>/.opencode/tmp/out`. A build directory is
`build/<arch>-<config>[-<sanitizer>]`, for example
`.opencode/tmp/out/build/x64-Debug-asan`. Logs are
`.opencode/tmp/out/logs/build-<arch>-<config>-<sanitizer>-<timestamp>.log` and
`test-...log`. Minidumps go to `.opencode/tmp/out/dumps` during tests.

The library is `crpc.lib`/`crpc.dll` on Windows or `libcrpc.a`/`libcrpc.so` on
Linux, inside the selected build directory. Test executables are
`crpc_test_<area>` and are registered one per source by `tests/CMakeLists.txt`.
Dependency builds live under `<build>/_deps`. There are no `install()` rules, so
`cmake --install` is not a supported deployment path yet.

## 7. Sanitizers <a id="s7"></a>

The scripts map their names to CMake values:

| Script name | CMake `CRPC_SANITIZER` | Compiler flag |
|---|---|---|
| `asan` | `address` | `-fsanitize=address` / `/fsanitize=address` |
| `ubsan` | `undefined` | `-fsanitize=undefined` / `/fsanitize=undefined` |
| `tsan` | `thread` | `-fsanitize=thread` (GCC/Clang only) |

On GCC/Clang the build adds `-fno-omit-frame-pointer` and the sanitizer to the
link line. A sanitizer build disables LTO. `test.ps1`/`test.sh` detect
`AddressSanitizer`, `LeakSanitizer`, `UndefinedBehaviorSanitizer` and
`ThreadSanitizer` reports, plus `runtime error:`, and count a hit as a failure
even if the process exited zero.

The intended merge matrix is ASan+LSan (frame parsing, buffers, slot recycling,
close races), UBSan (`-fno-sanitize-recover=all` on Linux), and TSan (multi-shard
concurrency, Linux Clang `-O1 -g`). The current code has no shard, so TSan
coverage is limited to the implemented thread-safe primitives; this is a
coverage gap, not a passing claim. MSVC supports ASan/UBSan flags but not TSan;
use the POSIX script for the TSan gate.

## 8. Unattended Windows tests <a id="s8"></a>

A Debug build that hits a CRT assertion, invalid parameter, pure virtual call or
access violation can open a modal dialog and hang an unattended run. Three
layers prevent that:

1. `test.ps1` sets `HKCU\Software\Microsoft\Windows\Windows Error Reporting`
   `DontShowUI=1` and `Disabled=1`, and exports `CRPC_TEST_MINIDUMP_DIR` pointing
   at the dump directory.
2. `tests/test_framework.hpp` redirects `_CRT_WARN`/`_CRT_ERROR`/`_CRT_ASSERT`
   to stderr, disables report-fault on abort, installs handlers for invalid
   parameter, pure virtual call and `std::terminate`, sets `SetErrorMode` and
   installs an unhandled-exception filter. The filter prints the exception code,
   address and thread, writes a minidump (`MiniDumpWithDataSegs`,
   `MiniDumpWithHandleData`, `MiniDumpWithThreadInfo`, unloaded modules,
   indirectly referenced memory) and exits non-zero.
3. `test.ps1` gives every CTest entry a 300-second timeout and kills leftover
   `crpc_test_*` processes after each run, so a wedged child cannot poison the
   next iteration. **`test.sh` does not add this per-test timeout or the
   leftover-process cleanup;** CTest's default is used on POSIX.

The guards are Debug-only where they depend on the MSVC CRT. A Release crash is
still expected to fail the process; it simply may not leave a minidump.

## 9. CI reuse <a id="s9"></a>

The design's CI matrix is MSVC x64 Debug/Release, MSVC x86 Debug/Release, GCC x64, Clang x64, ASan, UBSan, TSan, fuzz smoke, golden bytes and a benchmark report. CI is intended to invoke `scripts/build.*` and `scripts/test.*` exactly as a developer does; no CI YAML is checked in yet, and no build logic is duplicated outside the scripts. The local equivalents are `.\scripts\test.ps1 -Config Both -Arch x64 -Sanitizer none` and `./scripts/test.sh --config Debug --arch x64 --sanitizer all --compiler clang`.

A "zero warnings" claim requires the build log to contain no `warning:` line;
the scripts treat a warning as a failure even when the compiler emitted no
error. The 2026-10-19 status recorded in `.opencode/process.md` is 16 suites
green on x64 Debug and x86 Debug; rerun the scripts to confirm the current tree.

## 10. Troubleshooting <a id="s10"></a>

| Symptom | Cause / action |
|---|---|
| `CRPC_RTTI_DIR does not look like an RTTI checkout` | point it at the checkout root, not `include/` |
| `CRPC_COROCC_DIR does not look like a corocc checkout` | point it at the root containing `include/corocc/coro.h` |
| configure fails on `bench`/`examples` | keep `CRPC_BUILD_BENCH`/`CRPC_BUILD_EXAMPLES` off; the trees are empty |
| a sanitizer is requested on MSVC | TSan is unavailable; use Linux/Clang for the TSan job |
| `cmake` or `ninja` not found | the scripts search `PATH`, Program Files and the VS bundle; install CMake/Ninja or add them to `PATH` |
| a test hangs on Windows | inspect `.opencode/tmp/out/dumps`, the test log, and the 300 s CTest timeout |

## 11. Verification <a id="s11"></a>

A build is verified when: configure succeeds; every target compiles with zero
warnings and zero errors; `ctest` reports all registered tests passed; the
sanitizer run reports no ASan/LSan/UBSan/TSan diagnostics; and the resulting
library links into the test executables for the requested architecture. Record
the command, build directory, log path and result in the task report rather
than asserting success without output.
