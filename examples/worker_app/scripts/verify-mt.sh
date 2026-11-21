#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STRESS_DIR="$SCRIPT_DIR/../mt_stress"
PIONC="${1:-${PIONC:-}}"
RUNS="${RUNS:-5}"
TIMEOUT_SEC="${TIMEOUT_SEC:-3}"

if [ -z "$PIONC" ] || [ ! -x "$PIONC" ]; then
  echo "usage: PIONC=/path/to/pionc2 $0 [pionc2]" >&2
  exit 2
fi

PIONC_DIR=$(CDPATH= cd -- "$(dirname -- "$PIONC")" && pwd)
if [ -z "${PION_QBE:-}" ] && [ -x "$PIONC_DIR/qbe" ]; then
  export PION_QBE="$PIONC_DIR/qbe"
fi
if [ -z "${PION_RT_LIB:-}" ] && [ -f "$PIONC_DIR/libpionrt.a" ]; then
  export PION_RT_LIB="$PIONC_DIR/libpionrt.a"
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

DEBUG_BIN="$TMP_DIR/worker_mt_stress_debug"
RELEASE_BIN="$TMP_DIR/worker_mt_stress_release"

echo "=== Building worker_app/mt_stress (debug & --release) ==="
"$PIONC" build -o "$DEBUG_BIN" "$STRESS_DIR"
"$PIONC" build --release -o "$RELEASE_BIN" "$STRESS_DIR"

run_suite() {
  mode="$1"
  bin="$2"
  echo "=== Running $mode x$RUNS (timeout=${TIMEOUT_SEC}s each) ==="
  i=1
  while [ "$i" -le "$RUNS" ]; do
    python3 - "$mode" "$i" "$RUNS" "$TIMEOUT_SEC" "$bin" <<'PY'
import subprocess
import sys
import time

mode, idx, total, timeout_s, bin_path = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5]
t0 = time.monotonic()
try:
    proc = subprocess.run([bin_path], capture_output=True, text=True, timeout=timeout_s)
except subprocess.TimeoutExpired:
    elapsed_ms = (time.monotonic() - t0) * 1000.0
    print(f"[{mode} {idx}/{total}] FAIL: timed out after {elapsed_ms:.1f} ms (>{timeout_s:.1f}s)", file=sys.stderr)
    sys.exit(1)

elapsed_ms = (time.monotonic() - t0) * 1000.0
out = proc.stdout.strip()
err = proc.stderr.strip()
if proc.returncode != 0 or not out.startswith("OK rounds=500 received=500 mismatches=0 wakes="):
    print(f"[{mode} {idx}/{total}] FAIL rc={proc.returncode} elapsed={elapsed_ms:.1f} ms stdout={out!r} stderr={err!r}", file=sys.stderr)
    sys.exit(1)

print(f"[{mode} {idx}/{total}] PASS ({elapsed_ms:.1f} ms): {out}")
PY
    i=$((i + 1))
  done
}

run_suite "debug" "$DEBUG_BIN"
run_suite "release" "$RELEASE_BIN"

echo "=== worker_app/mt_stress: ALL PASSED ($RUNS/$RUNS debug + $RUNS/$RUNS release) ==="
