#!/usr/bin/env bash
# build.sh -- one-click build for CRPC (Linux/macOS reference implementation).
# Location in the final repo: <repo>/scripts/build.sh
#
# Usage:
#   scripts/build.sh [--config Debug|Release|Both] [--arch x86|x64]
#                    [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan]
#                    [--cxx-standard 17|20] [--jobs N] [--clean] [--shared]
#                    [--examples] [--no-tests] [--bench]
#                    [--source DIR] [--build-root DIR]
#
# --shared selects CRPC_BUILD_SHARED=ON (a crpc.so/dll plus the
# crpc_test_shared_link consumer gate). Without it the library is a static
# archive, which is the default and needs no runtime deployment.
#
# Guarantees: toolchain auto-detect, -Wall -Wextra -Wpedantic -Werror,
# idempotent, UTF-8 logs, non-zero exit on failure.

set -euo pipefail

CONFIG="Debug"
ARCH="x64"
COMPILER="gcc"
SANITIZER="none"
CXX_STANDARD="17"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
CLEAN=0
SHARED=0
BENCH=0
EXAMPLES=0
TESTS=1
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --compiler) COMPILER="$2"; shift 2 ;;
    --sanitizer) SANITIZER="$2"; shift 2 ;;
    --cxx-standard) CXX_STANDARD="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --shared) SHARED=1; shift ;;
    --bench) BENCH=1; shift ;;
    --examples) EXAMPLES=1; shift ;;
    --no-tests) TESTS=0; shift ;;
    --source) SOURCE_DIR="$2"; shift 2 ;;
    --build-root) BUILD_ROOT="$2"; shift 2 ;;
    # Print the leading comment block instead of a hardcoded line range; '2,20p'
    # silently cut the usage text when the header grew.
    -h|--help) sed -n '2,/^[^#]/p' "$0" | sed '$d'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -z "$BUILD_ROOT" ]] && BUILD_ROOT="$SOURCE_DIR/.opencode/tmp/out"
LOG_DIR="$BUILD_ROOT/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/build-$ARCH-$CONFIG-$SANITIZER-$(date +%Y%m%d-%H%M%S).log"

log() {
  local level="$1"; shift
  local line="[$(date '+%Y-%m-%d %H:%M:%S')] [$level] $*"
  printf '%s\n' "$line" | tee -a "$LOG"
}

CMAKES=(
  "$(command -v cmake 2>/dev/null || true)"
  /usr/bin/cmake /usr/local/bin/cmake
)
CMAKE=""
for c in "${CMAKES[@]}"; do [[ -n "$c" && -x "$c" ]] && CMAKE="$c" && break; done
[[ -z "$CMAKE" ]] && { log ERROR "cmake not found"; exit 2; }

CXX_COMPILER=""
CC_COMPILER=""
case "$COMPILER" in
  gcc) CXX_COMPILER="$(command -v g++ || true)"; CC_COMPILER="$(command -v gcc || true)" ;;
  clang) CXX_COMPILER="$(command -v clang++ || true)"; CC_COMPILER="$(command -v clang || true)" ;;
  *) log ERROR "unsupported compiler: $COMPILER"; exit 2 ;;
esac
[[ -z "$CXX_COMPILER" ]] && { log ERROR "$COMPILER not found"; exit 2; }

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARGS=(-G Ninja)
fi

ARCH_ARGS=()
if [[ "$ARCH" == "x86" ]]; then
  ARCH_ARGS=(-DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32)
fi

SAN_ARGS=()
case "$SANITIZER" in
  none) ;;
  asan) SAN_ARGS=(-DCRPC_SANITIZER=address) ;;
  ubsan) SAN_ARGS=(-DCRPC_SANITIZER=undefined) ;;
  tsan) SAN_ARGS=(-DCRPC_SANITIZER=thread) ;;
  *) log ERROR "bad sanitizer: $SANITIZER"; exit 2 ;;
esac

SHARED_ARGS=(-DCRPC_BUILD_SHARED=OFF)
[[ $SHARED -eq 1 ]] && SHARED_ARGS=(-DCRPC_BUILD_SHARED=ON)
BENCH_ARGS=(-DCRPC_BUILD_BENCH=OFF)
[[ $BENCH -eq 1 ]] && BENCH_ARGS=(-DCRPC_BUILD_BENCH=ON)
EXAMPLES_ARGS=(-DCRPC_BUILD_EXAMPLES=OFF)
[[ $EXAMPLES -eq 1 ]] && EXAMPLES_ARGS=(-DCRPC_BUILD_EXAMPLES=ON)
# The two scripts must accept the same inputs: build.ps1 gained -Examples and
# -NoTests when CI needed the examples-only shape, and this file had neither, so
# a Linux contributor could not reproduce the job that guards examples/ or
# bench/ from rotting. -NoTests exists for the same reason it does there:
# enable_testing() used to be called only under CRPC_BUILD_TESTS, so
# EXAMPLES=ON with TESTS=OFF registered zero tests and no TESTS=ON job could
# have noticed.
TESTS_ARGS=(-DCRPC_BUILD_TESTS=ON)
[[ $TESTS -eq 0 ]] && TESTS_ARGS=(-DCRPC_BUILD_TESTS=OFF)

# Dependency checkouts are CMake cache variables (CRPC_RTTI_DIR,
# CRPC_COROCC_DIR). The top-level CMakeLists.txt also falls back to the
# environment, but forwarding them as -D arguments keeps the configure line
# self-documenting and independent of how cmake is started.
# This script always passes -DCRPC_BUILD_TESTS=ON, and tests that use the
# RTTI_DEFINE_TYPE/RTTI_DEFINE_ENUM macros cannot be compiled without the RTTI
# checkout. Report that here instead of letting the compiler emit a wall of
# unrelated errors in test_mvc.cpp.
#
# A checkout can come from either place: an explicit CRPC_RTTI_DIR (a shared
# worktree, a vendored copy) or the third_party submodule, which is where a
# fresh clone gets it. CMakeLists.txt already falls back to the submodule, so
# requiring the environment variable here made a plain
# `git clone --recursive && scripts/build.sh` fail on Linux while the same
# thing worked through build.ps1 -- the one-click promise is per-platform, so
# the two scripts must accept the same inputs.
DEP_ARGS=()
for dep in CRPC_RTTI_DIR CRPC_COROCC_DIR; do
  value="${!dep:-}"
  if [[ -n "$value" ]]; then
    DEP_ARGS+=("-D${dep}=$value")
    log INFO "$dep=$value (explicit override)"
  fi
done
if [[ -z "${CRPC_RTTI_DIR:-}" && ! -f "$SOURCE_DIR/third_party/rtti/CMakeLists.txt" ]]; then
  log ERROR "No RTTI checkout: neither CRPC_RTTI_DIR nor third_party/rtti exists. Run \"git submodule update --init --recursive\", or export CRPC_RTTI_DIR=/path/to/RTTI. Without it the tests that use RTTI_DEFINE_TYPE/RTTI_DEFINE_ENUM cannot be compiled."
  exit 3
fi

log INFO "source=$SOURCE_DIR arch=$ARCH config=$CONFIG sanitizer=$SANITIZER compiler=$COMPILER shared=$SHARED cxx=$CXX_STANDARD"
log INFO "cmake=$CMAKE cxx=$CXX_COMPILER log=$LOG"

FAILURES=0
IFS=',' read -r -a CONFIGS <<< "$( [[ "$CONFIG" == "Both" ]] && echo 'Debug,Release' || echo "$CONFIG" )"

for cfg in "${CONFIGS[@]}"; do
  suffix=""
  [[ "$SANITIZER" != "none" ]] && suffix="-$SANITIZER"
  # A shared build is a different configuration of the same sources, so it
  # gets its own directory: reusing the static one would leave a stale archive
  # in place and silently link the wrong library type.
  [[ $SHARED -eq 1 ]] && suffix="$suffix-shared"
  # The language standard changes what compiles, so it gets its own tree for
  # the same reason shared does.
  [[ "$CXX_STANDARD" != "17" ]] && suffix="$suffix-cxx$CXX_STANDARD"
  BUILD_DIR="$BUILD_ROOT/build/$ARCH-$cfg$suffix"
  if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
    log INFO "clean: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  fi
  mkdir -p "$BUILD_DIR"

  log INFO "configure: $cfg"
  if ! "$CMAKE" -S "$SOURCE_DIR" -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" \
      -DCMAKE_BUILD_TYPE="$cfg" \
      -DCMAKE_C_COMPILER="$CC_COMPILER" -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
      -DCRPC_WARNINGS_AS_ERRORS=ON "-DCMAKE_CXX_STANDARD=$CXX_STANDARD" \
      "${TESTS_ARGS[@]}" \
         "${EXAMPLES_ARGS[@]}" \
      "${SHARED_ARGS[@]}" "${BENCH_ARGS[@]}" \
      "${DEP_ARGS[@]}" \
      "${ARCH_ARGS[@]}" "${SAN_ARGS[@]}" >>"$LOG" 2>&1; then
    log ERROR "configure failed ($cfg), see $LOG"
    FAILURES=$((FAILURES + 1))
    continue
  fi

  log INFO "build: $cfg (jobs=$JOBS)"
  if ! "$CMAKE" --build "$BUILD_DIR" --parallel "$JOBS" >>"$LOG" 2>&1; then
    log ERROR "build failed ($cfg), see $LOG"
    tail -n 30 "$LOG" || true
    FAILURES=$((FAILURES + 1))
    continue
  fi

  if grep -qE 'warning:' "$LOG"; then
    log ERROR "warnings present with -Werror ($cfg)"
    grep -E 'warning:' "$LOG" | head -n 20 || true
    FAILURES=$((FAILURES + 1))
    continue
  fi
  log INFO "build OK ($cfg) -> $BUILD_DIR"
done

log INFO "done: failures=$FAILURES"
[[ $FAILURES -gt 0 ]] && exit 1
exit 0


