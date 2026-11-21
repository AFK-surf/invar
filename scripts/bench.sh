#!/usr/bin/env bash
# Unified Invar Benchmark Runner + A/B Regression Comparator
# Usage:
#   PIONC=/path/to/pionc2 bash scripts/bench.sh [--out bench_report.json] [--compare baseline.json] [--cpu-only] [--filter <name>]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIONC="${PIONC:-$(command -v pionc2 || true)}"
if [ -z "$PIONC" ] || [ ! -x "$PIONC" ]; then
  echo "ERROR: pionc2 not found. Set PIONC=/path/to/pionc2" >&2
  exit 1
fi

# Auto-derive PION_QBE / PION_RT_LIB / PION_WASM_RT next to PIONC if unset
PIONC_DIR="$(cd "$(dirname "$PIONC")" && pwd)"
if [ -z "${PION_QBE:-}" ] && [ -x "$PIONC_DIR/qbe" ]; then
  export PION_QBE="$PIONC_DIR/qbe"
fi
if [ -z "${PION_RT_LIB:-}" ] && [ -f "$PIONC_DIR/libpionrt.a" ]; then
  export PION_RT_LIB="$PIONC_DIR/libpionrt.a"
fi
if [ -z "${PION_WASM_RT:-}" ] && [ -f "$PIONC_DIR/../runtime/pion_wasm_rt.mjs" ]; then
  export PION_WASM_RT="$PIONC_DIR/../runtime/pion_wasm_rt.mjs"
fi

OUT_FILE=""
COMPARE_FILE=""
CPU_ONLY=0
FILTER=""

while [ $# -gt 0 ]; do
  case "$1" in
    --out)
      OUT_FILE="$2"
      shift 2
      ;;
    --compare)
      COMPARE_FILE="$2"
      shift 2
      ;;
    --cpu-only)
      CPU_ONLY=1
      shift
      ;;
    --filter)
      FILTER="$2"
      shift 2
      ;;
    -h|--help)
      echo "Usage: PIONC=/path/to/pionc2 bash scripts/bench.sh [--out report.json] [--compare baseline.json] [--cpu-only] [--filter <substr>]"
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/invar_bench.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

CPU_BIN="$TMP_DIR/invar-bench"
GPU_BIN="$TMP_DIR/invar-bench-gpu"
WASM_OUT="$TMP_DIR/lg_web.wasm"

CPU_JSON="$TMP_DIR/cpu.json"
GPU_JSON="$TMP_DIR/gpu.json"
WASM_JSON="$TMP_DIR/wasm.json"
PERF_JSON="$TMP_DIR/perf.json"

OS_NAME="$(uname -s)"
ARCH_NAME="$(uname -m)"
if [ "$OS_NAME" = "Darwin" ]; then
  CPU_MODEL="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "Apple Silicon")"
elif [ -f /proc/cpuinfo ]; then
  CPU_MODEL="$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^[ \t]*//' || echo "unknown")"
else
  CPU_MODEL="unknown"
fi
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")"
TS_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

BENCH_ARGS=(--json)
if [ -n "$FILTER" ]; then
  BENCH_ARGS+=(--filter "$FILTER")
fi

echo "==> [1/3] Building & running Layer-1 CPU suite (packages/bench --release)..." >&2
"$PIONC" build --release -o "$CPU_BIN" packages/bench >&2
"$CPU_BIN" "${BENCH_ARGS[@]}" > "$CPU_JSON"

if [ "${PERF:-0}" = "1" ] && [ "$OS_NAME" = "Linux" ] && command -v perf >/dev/null 2>&1; then
  echo "==> Running Linux perf stat on packages/bench..." >&2
  PERF_RAW="$TMP_DIR/perf.txt"
  perf stat -x, -e instructions,cycles,cache-misses "$CPU_BIN" "${BENCH_ARGS[@]}" >/dev/null 2>"$PERF_RAW" || true
  python3 - "$PERF_RAW" "$PERF_JSON" <<'PY'
import json, sys
raw_path, out_path = sys.argv[1], sys.argv[2]
metrics = {}
with open(raw_path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
        parts = [p.strip() for p in line.strip().split(",")]
        if len(parts) >= 3 and parts[2]:
            try:
                metrics[parts[2]] = int(parts[0])
            except ValueError:
                pass
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(metrics, f)
PY
fi

RUN_GPU=0
if [ "$CPU_ONLY" -eq 0 ]; then
  if [ "$OS_NAME" = "Darwin" ]; then
    RUN_GPU=1
  elif [ "$OS_NAME" = "Linux" ] && command -v pkg-config >/dev/null 2>&1 && pkg-config --exists epoxy gtk4 2>/dev/null; then
    RUN_GPU=1
  fi
fi

if [ "$RUN_GPU" -eq 1 ]; then
  echo "==> [2/3] Building & running Layer-2 GPU suite (packages/bench_gpu --release)..." >&2
  "$PIONC" build --release -o "$GPU_BIN" packages/bench_gpu >&2
  "$GPU_BIN" "${BENCH_ARGS[@]}" > "$GPU_JSON"
else
  echo "==> [2/3] Skipping GPU suite (--cpu-only or unsupported host)." >&2
fi

if [ "$CPU_ONLY" -eq 0 ] && [ -z "$FILTER" ]; then
  echo "==> [3/3] Building packages/lg_web (--release --target wasm32)..." >&2
  python3 - "$PIONC" "$WASM_OUT" "$WASM_JSON" <<'PY'
import gzip, json, os, shutil, subprocess, sys, time
pionc, wasm_out, wasm_json = sys.argv[1], sys.argv[2], sys.argv[3]
t0 = time.monotonic()
proc = subprocess.run(
    [pionc, "build", "--release", "--target", "wasm32", "-o", wasm_out, "packages/lg_web"],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
)
elapsed_ms = (time.monotonic() - t0) * 1000.0
if proc.returncode == 0 and os.path.isfile(wasm_out):
    data = open(wasm_out, "rb").read()
    raw_bytes = len(data)
    gz_bytes = len(gzip.compress(data, compresslevel=9))
    br_bytes = None
    if shutil.which("brotli"):
        br_proc = subprocess.run(["brotli", "-q", "11", "-c", wasm_out], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if br_proc.returncode == 0:
            br_bytes = len(br_proc.stdout)
    info = {
        "package": "lg_web",
        "ok": True,
        "build_ms": round(elapsed_ms, 2),
        "raw_bytes": raw_bytes,
        "gzip_bytes": gz_bytes,
        "brotli_bytes": br_bytes,
    }
else:
    info = {
        "package": "lg_web",
        "ok": False,
        "build_ms": round(elapsed_ms, 2),
        "error": (proc.stderr or proc.stdout).strip()[:400],
    }
with open(wasm_json, "w", encoding="utf-8") as f:
    json.dump(info, f)
PY
fi

python3 - "$GIT_SHA" "$TS_UTC" "$OS_NAME" "$ARCH_NAME" "$CPU_MODEL" \
  "$CPU_JSON" "$GPU_JSON" "$WASM_JSON" "$PERF_JSON" "$OUT_FILE" "$COMPARE_FILE" <<'PY'
import json, os, sys

git_sha, ts_utc, os_name, arch_name, cpu_model = sys.argv[1:6]
cpu_path, gpu_path, wasm_path, perf_path, out_file, cmp_file = sys.argv[6:12]

def load_opt(p):
    if p and os.path.isfile(p) and os.path.getsize(p) > 0:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    return None

report = {
    "git_sha": git_sha,
    "timestamp_utc": ts_utc,
    "os": os_name,
    "arch": arch_name,
    "cpu_model": cpu_model,
    "cpu": load_opt(cpu_path),
    "gpu": load_opt(gpu_path),
    "wasm": load_opt(wasm_path),
}
perf_data = load_opt(perf_path)
if perf_data is not None:
    report["perf_stat"] = perf_data

if out_file:
    os.makedirs(os.path.dirname(os.path.abspath(out_file)), exist_ok=True)
    with open(out_file, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
        f.write("\n")

print(f"\n### Invar Benchmark Summary (`{git_sha}` · {os_name}/{arch_name} · {cpu_model})\n")

cpu = report.get("cpu")
if cpu and cpu.get("results"):
    print(f"#### Layer-1 CPU Suite (Peak RSS: `{cpu.get('rss_peak_kb', 0):,} KB`)\n")
    print("| Benchmark | Iters | P50 (ms) | P95 (ms) | P99 (ms) | Mean (ms) | Ops/s | Checksum |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for r in cpu["results"]:
        print(
            f"| `{r['name']}` | {r['iters']} | {r['p50_ms']:.3f} | {r['p95_ms']:.3f} | "
            f"{r['p99_ms']:.3f} | {r['mean_ms']:.3f} | {r['ops_per_sec']:.1f} | `{r['checksum']}` |"
        )
    print()

gpu = report.get("gpu")
if gpu and gpu.get("results"):
    print(f"#### Layer-2 GPU Suite ({gpu.get('renderer')} · {gpu.get('resolution')} · Peak RSS: `{gpu.get('rss_peak_kb', 0):,} KB`)\n")
    print("| Scenario | Frames | Frame P50/P95/P99/Max (ms) | Layout P50 | Paint P50 | Present P50/P95 | Inst Avg | Dirty Avg | 120fps / 60fps |")
    print("|---|---:|---|---:|---:|---|---:|---:|---:|")
    for r in gpu["results"]:
        fm, lm, pm, prm = r["frame_ms"], r["layout_ms"], r["paint_ms"], r["present_ms"]
        print(
            f"| `{r['name']}` | {r['frames']} | {fm['p50']:.2f} / {fm['p95']:.2f} / {fm['p99']:.2f} / {fm['max']:.2f} | "
            f"{lm['p50']:.2f} | {pm['p50']:.2f} | {prm['p50']:.2f} / {prm['p95']:.2f} | "
            f"{r['instances_avg']:.0f} | {r['dirty_rects_avg']:.2f} | "
            f"{r['budget_120fps_hit_pct']:.1f}% / {r['budget_60fps_hit_pct']:.1f}% |"
        )
    print()

wasm = report.get("wasm")
if wasm:
    if wasm.get("ok"):
        br_str = f"{wasm['brotli_bytes']:,} B ({wasm['brotli_bytes']/1024:.1f} KiB)" if wasm.get("brotli_bytes") is not None else "n/a"
        print(
            f"#### WASM Artifact (`lg_web.wasm`): raw `{wasm['raw_bytes']:,} B` ({wasm['raw_bytes']/1024:.1f} KiB) · "
            f"gzip `{wasm['gzip_bytes']:,} B` ({wasm['gzip_bytes']/1024:.1f} KiB) · brotli `{br_str}` · "
            f"build `{wasm['build_ms']:.0f} ms`\n"
        )
    else:
        print(f"#### WASM Artifact (`lg_web.wasm`): skipped/failed ({wasm.get('error', 'unknown')})\n")

if cmp_file and os.path.isfile(cmp_file):
    base = json.load(open(cmp_file, "r", encoding="utf-8"))
    print(f"### A/B Comparison vs `{cmp_file}` (`{base.get('git_sha', '?')}` -> `{git_sha}`)\n")
    print("| Suite | Benchmark | Base P50 (ms) | Now P50 (ms) | Delta (%) | Status |")
    print("|---|---|---:|---:|---:|---|")

    def emit_cmp(suite_name, base_map, now_map):
        for name, now_v in now_map.items():
            if name not in base_map:
                print(f"| {suite_name} | `{name}` | n/a | {now_v:.3f} | n/a | NEW |")
                continue
            b_v = base_map[name]
            if b_v > 1e-9:
                delta = (now_v - b_v) * 100.0 / b_v
            else:
                delta = 0.0
            status = "REGRESSION" if delta > 10.0 else ("FASTER" if delta < -10.0 else "OK")
            print(f"| {suite_name} | `{name}` | {b_v:.3f} | {now_v:.3f} | {delta:+.1f}% | **{status}** |")

    if cpu and base.get("cpu"):
        b_cpu = {r["name"]: r["p50_ms"] for r in base["cpu"].get("results", [])}
        n_cpu = {r["name"]: r["p50_ms"] for r in cpu.get("results", [])}
        emit_cmp("cpu", b_cpu, n_cpu)
    if gpu and base.get("gpu"):
        b_gpu = {r["name"]: r["frame_ms"]["p50"] for r in base["gpu"].get("results", [])}
        n_gpu = {r["name"]: r["frame_ms"]["p50"] for r in gpu.get("results", [])}
        emit_cmp("gpu", b_gpu, n_gpu)
    print()

if out_file:
    print(f"Saved JSON report to `{out_file}`")
PY
