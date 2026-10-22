#!/usr/bin/env bash
# build.sh -- one-click build for CRPC (Linux/macOS reference implementation).
# Location in the final repo: <repo>/scripts/build.sh
#
# Usage:
#   scripts/build.sh [--config Debug|Release|Both] [--arch x86|x64]
#                    [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan]
#                    [--jobs N] [--clean] [--shared]
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
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
CLEAN=0
SHARED=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --compiler) COMPILER="$2"; shift 2 ;;
    --sanitizer) SANITIZER="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --shared) SHARED=1; shift ;;
    --source) SOURCE_DIR="$2"; shift 2 ;;
    --build-root) BUILD_ROOT="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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

log INFO "source=$SOURCE_DIR arch=$ARCH config=$CONFIG sanitizer=$SANITIZER compiler=$COMPILER shared=$SHARED"
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
      -DCRPC_WARNINGS_AS_ERRORS=ON -DCRPC_BUILD_TESTS=ON \
      "${SHARED_ARGS[@]}" \
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


