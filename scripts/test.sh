#!/usr/bin/env bash
# test.sh -- one-click test for CRPC (Linux/macOS).
# Location in the final repo: <repo>/scripts/test.sh
#
# Usage:
#   scripts/test.sh [--config Debug|Release|Both] [--arch x86|x64]
#                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan|all]
#                   [--filter REGEX] [--repeat N]
#                   [--source DIR] [--build-root DIR]
#
# Runs CTest per build dir, then each test binary for per-case output.
# Detects sanitizer reports. Non-zero exit on any failure.

set -euo pipefail

CONFIG="Debug"
ARCH="x64"
COMPILER="gcc"
SANITIZER="none"
FILTER=""
REPEAT=1
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --compiler) COMPILER="$2"; shift 2 ;;
    --sanitizer) SANITIZER="$2"; shift 2 ;;
    --filter) FILTER="$2"; shift 2 ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --source) SOURCE_DIR="$2"; shift 2 ;;
    --build-root) BUILD_ROOT="$2"; shift 2 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -z "$BUILD_ROOT" ]] && BUILD_ROOT="$SOURCE_DIR/out"
LOG_DIR="$BUILD_ROOT/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/test-$ARCH-$CONFIG-$SANITIZER-$(date +%Y%m%d-%H%M%S).log"

log() {
  local level="$1"; shift
  local line="[$(date '+%Y-%m-%d %H:%M:%S')] [$level] $*"
  printf '%s\n' "$line" | tee -a "$LOG"
}

CMAKE="$(command -v cmake || true)"
CTEST="$(command -v ctest || true)"
[[ -z "$CMAKE" ]] && { log ERROR "cmake not found"; exit 2; }
[[ -z "$CTEST" ]] && { log ERROR "ctest not found"; exit 2; }

BUILD_SH="$SCRIPT_DIR/build.sh"
FAILURES=0

case "$SANITIZER" in
  all) SAN_LIST=(asan ubsan tsan) ;;
  none|asan|ubsan|tsan) SAN_LIST=("$SANITIZER") ;;
  *) log ERROR "bad sanitizer: $SANITIZER"; exit 2 ;;
esac

case "$CONFIG" in
  Both) CFG_LIST=(Debug Release) ;;
  *) CFG_LIST=("$CONFIG") ;;
esac

for san in "${SAN_LIST[@]}"; do
  for cfg in "${CFG_LIST[@]}"; do
    log INFO "=== build $ARCH/$cfg/$san ==="
    if ! "$BUILD_SH" --config "$cfg" --arch "$ARCH" --compiler "$COMPILER" \
         --sanitizer "$san" --source "$SOURCE_DIR" --build-root "$BUILD_ROOT"; then
      log ERROR "build failed $ARCH/$cfg/$san"
      FAILURES=$((FAILURES + 1))
      continue
    fi

    suffix=""
    [[ "$san" != "none" ]] && suffix="-$san"
    BUILD_DIR="$BUILD_ROOT/build/$ARCH-$cfg$suffix"

    for ((r = 1; r <= REPEAT; ++r)); do
      log INFO "--- ctest $ARCH/$cfg/$san run $r/$REPEAT ---"
      CTEST_ARGS=(--test-dir "$BUILD_DIR" --output-on-failure)
      [[ -n "$FILTER" ]] && CTEST_ARGS+=(-R "$FILTER")
      if ! "$CTEST" "${CTEST_ARGS[@]}" 2>&1 | tee -a "$LOG"; then
        log ERROR "ctest failed $ARCH/$cfg/$san run $r"
        FAILURES=$((FAILURES + 1))
      fi
      if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer)|runtime error:|SUMMARY: (AddressSanitizer|ThreadSanitizer|LeakSanitizer)' "$LOG"; then
        log ERROR "sanitizer report detected"
        FAILURES=$((FAILURES + 1))
      fi
    done
  done
done

log INFO "done: failures=$FAILURES log=$LOG"
[[ $FAILURES -gt 0 ]] && exit 1
exit 0
