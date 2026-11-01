#!/usr/bin/env bash
# test.sh -- one-click test for CRPC (Linux/macOS).
# Location in the final repo: <repo>/scripts/test.sh
#
# Usage:
#   scripts/test.sh [--config Debug|Release|Both] [--arch x86|x64]
#                   [--compiler gcc|clang] [--sanitizer none|asan|ubsan|tsan|all]
#                   [--filter REGEX] [--repeat N] [--shared]
#                   [--source DIR] [--build-root DIR]
#
# --shared builds and tests CRPC_BUILD_SHARED=ON. In that mode the run also
# executes crpc_test_shared_link directly: it is the consumer link gate, and
# running it explicitly keeps a filtered ctest invocation from skipping it.
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
    --filter) FILTER="$2"; shift 2 ;;
    --repeat) REPEAT="$2"; shift 2 ;;
    --shared) SHARED=1; shift ;;
    --source) SOURCE_DIR="$2"; shift 2 ;;
    --build-root) BUILD_ROOT="$2"; shift 2 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -z "$BUILD_ROOT" ]] && BUILD_ROOT="$SOURCE_DIR/.opencode/tmp/out"
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

SHARED_ARG=()
[[ $SHARED -eq 1 ]] && SHARED_ARG=(--shared)

for san in "${SAN_LIST[@]}"; do
  for cfg in "${CFG_LIST[@]}"; do
    log INFO "=== build $ARCH/$cfg/$san shared=$SHARED ==="
    # Invoke through bash rather than relying on the executable bit. Git stores the
    # mode per file, and a checkout that lost the bit -- a Windows working tree, an
    # archive export, a zip download -- would otherwise fail here with
    # "Permission denied" while the build itself is fine. CI calls this script as
    # `bash scripts/test.sh`, so the interpreter is already known to exist.
    if ! bash "$BUILD_SH" --config "$cfg" --arch "$ARCH" --compiler "$COMPILER" \
         --sanitizer "$san" --source "$SOURCE_DIR" --build-root "$BUILD_ROOT" \
         "${SHARED_ARG[@]}"; then
      log ERROR "build failed $ARCH/$cfg/$san"
      FAILURES=$((FAILURES + 1))
      continue
    fi

    suffix=""
    [[ "$san" != "none" ]] && suffix="-$san"
    [[ $SHARED -eq 1 ]] && suffix="$suffix-shared"
    BUILD_DIR="$BUILD_ROOT/build/$ARCH-$cfg$suffix"

    for ((r = 1; r <= REPEAT; ++r)); do
      log INFO "--- ctest $ARCH/$cfg/$san run $r/$REPEAT ---"
      CTEST_ARGS=(--test-dir "$BUILD_DIR" --output-on-failure)
      [[ -n "$FILTER" ]] && CTEST_ARGS+=(-R "$FILTER")
      # A test may deliberately request an impossible allocation to prove the
      # failure is reported as an error instead of crashing
      # (fs_buffer_reserve_failure_is_an_error asks for SIZE_MAX/2). ASan
      # aborts such a request by default, before the code under test can
      # observe a null return, so allow the allocator to return null.
      case "$san" in
        asan|all) export ASAN_OPTIONS="${ASAN_OPTIONS:-}:allocator_may_return_null=1" ;;
      esac
      # The same impossible-allocation test runs under TSan, where the
      # option that turns the abort into a null return lives on
      # TSAN_OPTIONS.
      case "$san" in
        tsan|all) export TSAN_OPTIONS="${TSAN_OPTIONS:-}:allocator_may_return_null=1" ;;
      esac
      if ! "$CTEST" "${CTEST_ARGS[@]}" 2>&1 | tee -a "$LOG"; then
        log ERROR "ctest failed $ARCH/$cfg/$san run $r"
        FAILURES=$((FAILURES + 1))
      fi
      # The shared-build consumer gate is part of the ctest set, but a
      # --filter run can exclude it. Run it unconditionally in shared mode so
      # a filtered invocation cannot skip the export check.
      #
      # Every executable is written to the build root (see
      # tests/CMakeLists.txt), beside crpc.so/crpc.dll, so the probe is there.
      if [[ $SHARED -eq 1 ]]; then
        PROBE="$BUILD_DIR/crpc_test_shared_link"
        if [[ ! -x "$PROBE" ]]; then
          log ERROR "shared link gate missing: $PROBE"
          FAILURES=$((FAILURES + 1))
        elif ! ( cd "$BUILD_DIR" && "$PROBE" ) 2>&1 | tee -a "$LOG"; then
          log ERROR "shared link gate FAILED"
          FAILURES=$((FAILURES + 1))
        else
          log INFO "shared link gate OK"
        fi

        # The probe proves only that the symbols it names are exported. A new
        # public class with a forgotten CRPC_API builds cleanly, exports
        # nothing and passes every suite, so the headers are also compared
        # against the export table. See scripts/export_gate.py.
        GATE="$SOURCE_DIR/scripts/export_gate.py"
        if [[ ! -f "$GATE" ]]; then
          log ERROR "export gate missing: $GATE"
          FAILURES=$((FAILURES + 1))
        elif command -v python3 >/dev/null 2>&1; then
          if ! python3 "$GATE" --root "$SOURCE_DIR" --build-dir "$BUILD_DIR" 2>&1 | tee -a "$LOG"; then
            log ERROR "export gate FAILED"
            FAILURES=$((FAILURES + 1))
          else
            log INFO "export gate OK"
          fi
        else
          log WARN "export gate skipped: python3 not on PATH"
        fi

        # The gate is only as good as its parser, and every historical
        # blindness bug in that parser made it print OK. Pin the parser's three
        # real shapes (ILT suffix, annotation suffix, a name wrapped across two
        # lines) so a regression in `read_exports` fails here rather than
        # silently disabling the export check.
        #
        # Interpreter lookup is deliberately lenient: `python` on Windows,
        # `python3` on Linux/macOS. A missing interpreter skips the self-test
        # with a WARN, never a failure -- the same policy the gate itself uses,
        # so one missing interpreter cannot make two checks fail differently.
        SELFTEST="$SOURCE_DIR/scripts/export_gate_selftest.py"
        SELFTEST_PY=""
        if command -v python3 >/dev/null 2>&1; then
          SELFTEST_PY="python3"
        elif command -v python >/dev/null 2>&1; then
          SELFTEST_PY="python"
        fi
        if [[ ! -f "$SELFTEST" ]]; then
          log ERROR "export gate selftest missing: $SELFTEST"
          FAILURES=$((FAILURES + 1))
        elif [[ -n "$SELFTEST_PY" ]]; then
          if ! "$SELFTEST_PY" "$SELFTEST" 2>&1 | tee -a "$LOG"; then
            log ERROR "export gate selftest FAILED"
            FAILURES=$((FAILURES + 1))
          else
            log INFO "export gate selftest OK"
          fi
        else
          log WARN "export gate selftest skipped: no python interpreter on PATH"
        fi
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


