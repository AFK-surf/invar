#!/usr/bin/env bash
# End-to-end regression test suite for the unified `invar` CLI (`scripts/invar`) and `scripts/install.sh`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INVAR="$ROOT/scripts/invar"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/invar_cli_test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

assert_py() {
  local code="$1"
  shift
  python3 -c "$code" "$@"
}

echo "=== 1. invar --help ==="
"$INVAR" --help >/dev/null
echo "OK: --help"

echo "=== 2. invar doctor --json ==="
DOC_JSON="$("$INVAR" doctor --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'doctor not ok: {d}'
assert d['pionc']['found'] is True
assert d['web']['available'] is True
print('OK: doctor (os=%s, pionc=%s)' % (d['os'], d['pionc']['path']))
" "$DOC_JSON"

echo "=== 3. invar audit --json ==="
if [ -n "${INVAR_CLI_TEST_AUDIT_JSON:-}" ] && [ -f "$INVAR_CLI_TEST_AUDIT_JSON" ]; then
  AUD_JSON="$(cat "$INVAR_CLI_TEST_AUDIT_JSON")"
else
  AUD_JSON="$("$INVAR" audit --json)"
fi
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'audit failed: {d}'
assert d['error_count'] == 0, f'audit errors: {d}'
print('OK: audit (0 errors, %d warnings)' % d['warning_count'])
" "$AUD_JSON"

echo "=== 4. invar check --pkg view --json ==="
CHK_JSON="$("$INVAR" check --pkg view --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'check view failed: {d}'
assert d['fmt_checked'] is False, f'expected fmt_checked=False by default: {d}'
assert len(d['packages']) == 1 and d['packages'][0]['ok'] is True
print('OK: check view (%d ms)' % d['elapsed_ms'])
" "$CHK_JSON"

echo "=== 5. invar test --pkg damage --json ==="
TST_JSON="$("$INVAR" test --pkg damage --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'test damage failed: {d}'
assert d['total_passed'] > 0 and d['total_failed'] == 0
print('OK: test damage (%d passed, %d ms)' % (d['total_passed'], d['elapsed_ms']))
" "$TST_JSON"

echo "=== 5b. invar check (workspace) + parallel multi-pkg --only-pkg + filter + fail reporting ==="
if [ -n "${INVAR_CLI_TEST_WS_CHECK_JSON:-}" ] && [ -f "$INVAR_CLI_TEST_WS_CHECK_JSON" ]; then
  WS_CHK_JSON="$(cat "$INVAR_CLI_TEST_WS_CHECK_JSON")"
else
  WS_CHK_JSON="$("$INVAR" check --json)"
fi
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and len(d['packages']) >= 40, f'workspace check failed: {d}'
print('OK: workspace check (%d packages, %d ms)' % (len(d['packages']), d['elapsed_ms']))
" "$WS_CHK_JSON"

if [ -n "${INVAR_CLI_TEST_WS_TEST_JSON:-}" ] && [ -f "$INVAR_CLI_TEST_WS_TEST_JSON" ]; then
  WS_TST_JSON="$(cat "$INVAR_CLI_TEST_WS_TEST_JSON")"
else
  WS_TST_JSON="$("$INVAR" test --json)"
fi
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['total_failed'] == 0 and len(d['packages']) >= 40, f'workspace test failed: {d}'
zero_pkgs = [p['pkg'] for p in d['packages'] if p['passed'] <= 0]
assert not zero_pkgs, f'workspace test package name mapping mismatch (0 passed): {zero_pkgs}'
print('OK: workspace test (%d packages, %d passed, %d ms)' % (len(d['packages']), d['total_passed'], d['elapsed_ms']))
" "$WS_TST_JSON"

MULTI_TST_JSON="$("$INVAR" test --pkg damage --pkg halt --json)"
assert_py "
import glob, json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['total_failed'] == 0, f'multi-pkg test failed: {d}'
assert [p['pkg'] for p in d['packages']] == ['damage', 'halt'], f'order mismatch: {d}'
# --only-pkg must run only each package's own tests without re-running transitive deps
own_damage = sum(1 for p in glob.glob('packages/damage/src/*.pion') for ln in open(p) if ln.startswith('test '))
assert d['packages'][0]['passed'] == own_damage and d['packages'][1]['passed'] >= 1, f'unexpected counts: {d} (own_damage={own_damage})'
print('OK: parallel multi-pkg --only-pkg (%d passed, %d ms)' % (d['total_passed'], d['elapsed_ms']))
" "$MULTI_TST_JSON"

ZERO_FLT_JSON="$("$INVAR" test --pkg damage --filter __no_such_test_xyz__ --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['total_passed'] == 0 and d['total_failed'] == 0, f'zero-match filter failed: {d}'
print('OK: zero-match filter')
" "$ZERO_FLT_JSON"

# Run independent heavy E2E groups (6: jsx-app, 7-8b: web, 9-12: diff/install/shot/compare) in parallel.
(
set -euo pipefail
SECONDS=0; trap 'rc=$?; [ -z "${RUNNER_TEMP:-}" ] || echo "cli-step6 ${SECONDS}s rc=$rc" >> "$RUNNER_TEMP/timings.txt"' EXIT
echo "=== 6. invar new jsx-app + check + test + audit + no-overwrite ==="
JSX_DIR="$TMP/my_jsx_app"
NEW_JSX="$("$INVAR" new --template jsx-app --json "$JSX_DIR")"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['template'] == 'jsx-app', f'new jsx-app failed: {d}'
print('OK: new jsx-app (%d files)' % len(d['files']))
" "$NEW_JSX"

# Refuse overwrite on second call
if "$INVAR" new --template jsx-app --json "$JSX_DIR" >/dev/null 2>&1; then
  echo "FAIL: invar new should refuse to overwrite existing files" >&2
  exit 1
fi
echo "OK: new refuses overwrite"

JSX_CHK="$("$INVAR" check --pkg "$JSX_DIR" --fmt --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['fmt_checked'] is True, f'jsx-app check --fmt failed: {d}'
print('OK: jsx-app check --fmt')
" "$JSX_CHK"

JSX_TST="$("$INVAR" test --pkg "$JSX_DIR" --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['total_passed'] >= 1, f'jsx-app test failed: {d}'
print('OK: jsx-app test (%d passed)' % d['total_passed'])
" "$JSX_TST"

JSX_AUD="$("$INVAR" audit --json "$JSX_DIR")"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['error_count'] == 0 and d['warning_count'] == 0, f'jsx-app audit failed: {d}'
print('OK: jsx-app audit (0 errors, 0 warnings)')
" "$JSX_AUD"

# Verify failing test propagation in JSON output
printf '\ntest "intentional_cli_fail" {\n  debug_assert(false, "cli_fail_sentinel")\n}\n' >> "$JSX_DIR/src/main_test.pion"
FAIL_RC=0
FAIL_TST="$("$INVAR" test --pkg "$JSX_DIR" --json)" || FAIL_RC=$?
if [ "$FAIL_RC" -eq 0 ]; then
  echo "FAIL: invar test should exit non-zero on failing test" >&2
  exit 1
fi
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is False and d['total_failed'] == 1, f'expected 1 failure: {d}'
err = d['packages'][0]['error']
assert 'intentional_cli_fail' in err and 'cli_fail_sentinel' in err, f'missing fail details: {err}'
print('OK: jsx-app failing test properly reported')
" "$FAIL_TST"
) > "$TMP/log_step6.txt" 2>&1 &
PID_STEP6=$!

(
set -euo pipefail
WEB_DIR="$TMP/my_web_app"
WEB_OUT="$TMP/web_dist"
LG_WASM_OUT="$TMP/lg_web_step8.wasm"

(
  set -euo pipefail
  SECONDS=0; trap 'rc=$?; [ -z "${RUNNER_TEMP:-}" ] || echo "cli-step7 ${SECONDS}s rc=$rc" >> "$RUNNER_TEMP/timings.txt"' EXIT
  echo "=== 7. invar new web-app + invar web build ==="
  "$INVAR" new --template web-app --json "$WEB_DIR" >/dev/null
  WEB_BLD="$("$INVAR" web build --pkg "$WEB_DIR" --out "$WEB_OUT" --json)"
  assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'web-app build failed: {d}'
assert d['raw_bytes'] > 0 and d['gzip_bytes'] > 0
assert d['imports_missing'] == [], f'missing imports: {d}'
assert os.path.isfile(os.path.join(d['out_dir'], 'index.html'))
for lf in ('LICENSE', 'NOTICE', 'THIRD_PARTY_NOTICES.md', 'LICENSES/Apache-2.0.txt', 'LICENSES/OFL-1.1.txt', 'LICENSES/Unicode-3.0.txt'):
    assert os.path.isfile(os.path.join(d['out_dir'], lf)), f'missing license file in web dist: {lf}'
print('OK: web-app build (%d B raw, %d B gzip, %d imports, 0 missing)' % (d['raw_bytes'], d['gzip_bytes'], d['imports_total']))
" "$WEB_BLD"
) > "$TMP/log_step7.txt" 2>&1 &
PID_S7=$!

(
  set -euo pipefail
  SECONDS=0; trap 'rc=$?; [ -z "${RUNNER_TEMP:-}" ] || echo "cli-step8 ${SECONDS}s rc=$rc" >> "$RUNNER_TEMP/timings.txt"' EXIT
  echo "=== 8. invar web check --json (packages/lg_web) ==="
  if [ -n "${INVAR_CLI_TEST_WEB_CHECK_JSON:-}" ] && [ -f "$INVAR_CLI_TEST_WEB_CHECK_JSON" ]; then
    WEB_CHK="$(cat "$INVAR_CLI_TEST_WEB_CHECK_JSON")"
    if [ -z "${INVAR_CLI_TEST_LG_WASM:-}" ] || [ ! -f "$INVAR_CLI_TEST_LG_WASM" ]; then
      echo "FAIL: INVAR_CLI_TEST_LG_WASM must point to an existing file when INVAR_CLI_TEST_WEB_CHECK_JSON is set" >&2
      exit 1
    fi
    cp "$INVAR_CLI_TEST_LG_WASM" "$LG_WASM_OUT"
  else
    WEB_CHK="$("$INVAR" web check --out-wasm "$LG_WASM_OUT" --json)"
    if [ ! -f "$LG_WASM_OUT" ]; then
      echo "FAIL: invar web check --out-wasm did not create $LG_WASM_OUT" >&2
      exit 1
    fi
  fi
  assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'web check lg_web failed: {d}'
assert d['imports_total'] > 100 and d['imports_missing'] == []
print('OK: web check lg_web (%d B raw, %d imports, 0 missing)' % (d['raw_bytes'], d['imports_total']))
" "$WEB_CHK"
) > "$TMP/log_step8.txt" 2>&1 &
PID_S8=$!

W78_FAIL=0
wait "$PID_S7" || W78_FAIL=1
wait "$PID_S8" || W78_FAIL=1
cat "$TMP/log_step7.txt" "$TMP/log_step8.txt"
if [ "$W78_FAIL" -ne 0 ]; then
  exit 1
fi

echo "=== 8b. invar web build cleans stale _headers/_redirects/wasm across packages ==="
# Steps 7 and 8 already exercise full wasm32 compilation + brotli -q 11 compression on both
# my_web_app and lg_web; step 8b only verifies _web_dist_reconcile stale-file cleanup across
# 6 rebuilds by reusing those freshly compiled .wasm binaries via --prebuilt-wasm.
NO_BROTLI_BIN="$TMP/no_brotli_bin"
mkdir -p "$NO_BROTLI_BIN"
printf '#!/bin/sh\nexit 1\n' > "$NO_BROTLI_BIN/brotli"
chmod +x "$NO_BROTLI_BIN/brotli"

(
  set -euo pipefail
  SECONDS=0; trap 'rc=$?; [ -z "${RUNNER_TEMP:-}" ] || echo "cli-step8b ${SECONDS}s rc=$rc" >> "$RUNNER_TEMP/timings.txt"' EXIT
  export PATH="$NO_BROTLI_BIN:$PATH"
  SHARED_OUT="$TMP/shared_web_out"
  MY_WEB_WASM="$WEB_OUT/my_web_app.wasm"

  if "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$TMP/missing_prebuilt.wasm" --json >/dev/null 2>&1; then
    echo "FAIL: invar web build should reject missing --prebuilt-wasm" >&2
    exit 1
  fi
  if "$INVAR" web dev --pkg "$WEB_DIR" --prebuilt-wasm "$MY_WEB_WASM" --json >/dev/null 2>&1; then
    echo "FAIL: invar web dev should reject --prebuilt-wasm" >&2
    exit 1
  fi

  "$INVAR" web build --pkg "$ROOT/packages/lg_web" --out "$SHARED_OUT" --prebuilt-wasm "$LG_WASM_OUT" --json >/dev/null
  assert_py "
import os, sys
out = sys.argv[1]
assert os.path.isfile(os.path.join(out, '_headers')), 'lg_web must emit _headers'
assert os.path.isfile(os.path.join(out, '_redirects')), 'lg_web must emit _redirects'
assert os.path.isfile(os.path.join(out, 'lg_web.wasm')), 'lg_web must emit lg_web.wasm'
" "$SHARED_OUT"
  # Truncated prebuilt wasm must fail before touching existing files in SHARED_OUT.
  printf '\0asm\1\0\0\0\2\x10' > "$TMP/truncated.wasm"
  IDX_BEFORE="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/index.html")"
  if "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$TMP/truncated.wasm" --json >/dev/null 2>&1; then
    echo "FAIL: invar web build should reject truncated --prebuilt-wasm" >&2
    exit 1
  fi
  IDX_AFTER="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/index.html")"
  if [ "$IDX_BEFORE" != "$IDX_AFTER" ] || [ ! -f "$SHARED_OUT/lg_web.wasm" ]; then
    echo "FAIL: truncated --prebuilt-wasm modified existing output dir" >&2
    exit 1
  fi
  # Valid WASM structure with unknown host import (env.f) must fail before touching SHARED_OUT.
  python3 -c '
import sys
# Valid WASM module: type ()->(), import env.f : func 0
wasm = b"\x00asm\x01\x00\x00\x00" + b"\x01\x04\x01\x60\x00\x00" + b"\x02\x09\x01\x03env\x01f\x00\x00"
open(sys.argv[1], "wb").write(wasm)
' "$TMP/bad_import.wasm"
  LG_BEFORE="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/lg_web.wasm")"
  if "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$TMP/bad_import.wasm" --json >/dev/null 2>&1; then
    echo "FAIL: invar web build should reject prebuilt wasm with unknown host imports" >&2
    exit 1
  fi
  LG_AFTER="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/lg_web.wasm")"
  if [ "$LG_BEFORE" != "$LG_AFTER" ]; then
    echo "FAIL: prebuilt wasm with unknown host imports modified existing lg_web.wasm" >&2
    exit 1
  fi
  # --out-wasm pointing at a managed output file (index.html or nested LICENSES/*) must fail without clobbering it.
  if "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$MY_WEB_WASM" --out-wasm "$SHARED_OUT/index.html" --json >/dev/null 2>&1; then
    echo "FAIL: invar web build should reject --out-wasm conflicting with index.html" >&2
    exit 1
  fi
  IDX_AFTER2="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/index.html")"
  if [ "$IDX_BEFORE" != "$IDX_AFTER2" ]; then
    echo "FAIL: conflicting --out-wasm modified index.html" >&2
    exit 1
  fi
  LIC_BEFORE="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/LICENSES/Apache-2.0.txt")"
  if "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$MY_WEB_WASM" --out-wasm "$SHARED_OUT/LICENSES/Apache-2.0.txt" --json >/dev/null 2>&1; then
    echo "FAIL: invar web build should reject --out-wasm conflicting with LICENSES/Apache-2.0.txt" >&2
    exit 1
  fi
  LIC_AFTER="$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$SHARED_OUT/LICENSES/Apache-2.0.txt")"
  if [ "$LIC_BEFORE" != "$LIC_AFTER" ]; then
    echo "FAIL: conflicting --out-wasm modified LICENSES/Apache-2.0.txt" >&2
    exit 1
  fi
  echo "OK: web build rejects missing/truncated/bad-import --prebuilt-wasm, conflicting --out-wasm (incl. LICENSES/*), and web dev --prebuilt-wasm"

  # A file of the caller's own in the output dir: never invar's to remove.
  printf '\0asm\1\0\0\0' > "$SHARED_OUT/decoder.wasm"
  "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$MY_WEB_WASM" --json >/dev/null
  assert_py "
import os, sys
out = sys.argv[1]
assert not os.path.exists(os.path.join(out, '_headers')), 'stale _headers must be removed for custom pkg'
assert not os.path.exists(os.path.join(out, '_redirects')), 'stale _redirects must be removed for custom pkg'
assert not os.path.exists(os.path.join(out, 'lg_web.wasm')), 'stale lg_web.wasm must be removed for custom pkg'
assert os.path.isfile(os.path.join(out, 'my_web_app.wasm')), 'custom wasm must exist'
assert os.path.isfile(os.path.join(out, 'decoder.wasm')), 'caller-owned decoder.wasm must be kept'
assert open(os.path.join(out, 'decoder.wasm'), 'rb').read() == b'\\0asm\\1\\0\\0\\0', 'decoder.wasm untouched'
assert not any(n.startswith('.invar') for n in os.listdir(out)), 'no build record inside the output dir'
print('OK: shared web out dir cleaned of stale _headers, _redirects, and lg_web.wasm; decoder.wasm kept')
" "$SHARED_OUT"
  "$INVAR" web build --pkg "$ROOT/packages/lg_web" --out "$SHARED_OUT" --prebuilt-wasm "$LG_WASM_OUT" --json >/dev/null
  assert_py "
import os, sys
out = sys.argv[1]
assert not os.path.exists(os.path.join(out, 'my_web_app.wasm')), 'stale my_web_app.wasm must be removed when lg_web is built again'
assert os.path.isfile(os.path.join(out, 'lg_web.wasm')), 'lg_web.wasm rebuilt'
assert os.path.isfile(os.path.join(out, 'decoder.wasm')), 'decoder.wasm still kept'
print('OK: rebuilding lg_web removes my_web_app.wasm and still keeps decoder.wasm')
" "$SHARED_OUT"
  # The caller makes the output dir again and puts their own _headers there: a file under a name an
  # earlier build wrote, but not its bytes, is the caller's and stays.
  rm -rf "$SHARED_OUT"
  mkdir -p "$SHARED_OUT"
  printf '/*\n  X-Mine: 1\n' > "$SHARED_OUT/_headers"
  "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$MY_WEB_WASM" --json >/dev/null
  assert_py "
import os, sys
out = sys.argv[1]
assert open(os.path.join(out, '_headers'), 'rb').read() == b'/*\\n  X-Mine: 1\\n', 'caller _headers in a remade dir kept byte for byte'
assert os.path.isfile(os.path.join(out, 'my_web_app.wasm')), 'custom wasm built'
print('OK: a caller _headers in a remade output dir is kept')
" "$SHARED_OUT"
  # The same when the caller only rewrites a file invar wrote there.
  "$INVAR" web build --pkg "$ROOT/packages/lg_web" --out "$SHARED_OUT" --prebuilt-wasm "$LG_WASM_OUT" --json >/dev/null
  printf '/*\n  X-Mine: 2\n' > "$SHARED_OUT/_headers"
  "$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --prebuilt-wasm "$MY_WEB_WASM" --json >/dev/null
  assert_py "
import os, sys
out = sys.argv[1]
assert open(os.path.join(out, '_headers'), 'rb').read() == b'/*\\n  X-Mine: 2\\n', 'rewritten _headers kept'
assert not os.path.exists(os.path.join(out, '_redirects')), 'untouched stale _redirects still removed'
assert not os.path.exists(os.path.join(out, 'lg_web.wasm')), 'untouched stale lg_web.wasm still removed'
print('OK: a rewritten _headers is kept; untouched stale files are removed')
" "$SHARED_OUT"
)
) > "$TMP/log_step7_8b.txt" 2>&1 &
PID_STEP7_8B=$!

(
set -euo pipefail
SECONDS=0; trap 'rc=$?; [ -z "${RUNNER_TEMP:-}" ] || echo "cli-step9-12 ${SECONDS}s rc=$rc" >> "$RUNNER_TEMP/timings.txt"' EXIT
echo "=== 9. invar diff --json --out --sbs (synthetic PNGs) ==="
python3 - "$TMP" << 'PYEOF'
import os, struct, sys, zlib

def write_png(path, w, h, rgba):
    def chunk(ct, body):
        crc = zlib.crc32(body, zlib.crc32(ct)) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + ct + body + struct.pack(">I", crc)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y*w*4:(y+1)*w*4])
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)

tmp = sys.argv[1]
w, h = 16, 16
b1 = bytearray([40, 60, 80, 255] * (w * h))
b2 = bytearray(b1)
# Change 4 pixels at x in [4,5], y in [6,7] with R +100
for y in (6, 7):
    for x in (4, 5):
        o = (y * w + x) * 4
        b2[o] = 140
write_png(os.path.join(tmp, "before.png"), w, h, b1)
write_png(os.path.join(tmp, "after.png"), w, h, b2)
PYEOF

DIFF_JSON="$("$INVAR" diff "$TMP/before.png" "$TMP/after.png" --out "$TMP/diff.png" --sbs "$TMP/sbs.png" --json)"
assert_py "
import json, os, struct, sys
d = json.loads(sys.argv[1])
tmp = sys.argv[2]
assert d['ok'] is True and d['same_size'] is True
assert d['width'] == 16 and d['height'] == 16
assert d['changed_pixels'] == 4, f'changed_pixels={d}'
assert d['max_delta'] == 100, f'max_delta={d}'
assert d['changed_bbox'] == [4, 6, 2, 2], f'bbox={d}'
assert d['identical'] is False
with open(os.path.join(tmp, 'sbs.png'), 'rb') as f:
    hdr = f.read(24)
sw, sh = struct.unpack('>II', hdr[16:24])
assert sw == 16 * 3 + 8 and sh == 16, f'sbs size={sw}x{sh}'
assert os.path.isfile(os.path.join(tmp, 'diff.png'))
print('OK: diff changed=4, max_delta=100, bbox=[4,6,2,2], sbs=%dx%d' % (sw, sh))
" "$DIFF_JSON" "$TMP"

IDENT_JSON="$("$INVAR" diff "$TMP/before.png" "$TMP/before.png" --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['identical'] is True and d['changed_pixels'] == 0 and d['max_delta'] == 0
print('OK: diff identical=True')
" "$IDENT_JSON"

echo "=== 10. scripts/install.sh symlink verification ==="
INST_BIN="$TMP/bin"
INVAR_BIN_DIR="$INST_BIN" bash "$ROOT/scripts/install.sh" >/dev/null
SYM_JSON="$("$INST_BIN/invar" doctor --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True
print('OK: installed symlink invar doctor passed')
" "$SYM_JSON"

echo "=== 11. invar shot (--list, fresh output tracking, unknown preset rejection, scale override) ==="
SHOT_BIN="$TMP/lg_shots_bin"
SHOT_LIST="$("$INVAR" shot --list --out-bin "$SHOT_BIN" --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and 'ios27-parity' in d['output'], f'shot --list failed: {d}'
print('OK: invar shot --list (build %d ms)' % d['build_ms'])
" "$SHOT_LIST"

SHOT_DIR="$TMP/shots"
mkdir -p "$SHOT_DIR"
# Place a stale PNG in SHOT_DIR and verify unknown --only fails and never reports stale PNG as fresh
cp "$TMP/before.png" "$SHOT_DIR/stale_old.png"
BOGUS_RC=0
BOGUS_JSON="$("$INVAR" shot --only __no_such_preset__ --out "$SHOT_DIR" --prebuilt-bin "$SHOT_BIN" --json)" || BOGUS_RC=$?
if [ "$BOGUS_RC" -eq 0 ]; then
  echo "FAIL: invar shot --only __no_such_preset__ must exit non-zero" >&2
  exit 1
fi
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is False and d['pngs'] == [] and d['mp4s'] == [], f'stale PNG leaked on failure: {d}'
print('OK: unknown preset rejected cleanly without leaking stale PNG')
" "$BOGUS_JSON"

# A quarter of the preset's 390x844 page: the run steps 1 s of frames through llvmpipe on Linux.
SHOT_OK="$("$INVAR" shot --only ios27-parity-light --scale 1 --w 195 --h 422 --out "$SHOT_DIR" --prebuilt-bin "$SHOT_BIN" --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'ios27-parity-light shot failed: {d}'
assert len(d['pngs']) == 1 and d['mp4s'] == [], f'unexpected shot outputs: {d}'
p = d['pngs'][0]
assert os.path.basename(p['file']) == 'ios27-parity-light.png', f'wrong file: {p}'
assert p['width'] == 195 and p['height'] == 422, f'scale=1 / size override failed: {p}'
print('OK: invar shot fresh output + scale=1 and size override (%dx%d, build %d ms, run %d ms)' % (p['width'], p['height'], d['build_ms'], d['run_ms']))
" "$SHOT_OK"

# Relative --prebuilt-bin from a different cwd must resolve against caller cwd, and combining --prebuilt-bin + --out-bin must fail.
SHOT_REL="$(cd "$TMP" && "$INVAR" shot --list --prebuilt-bin ./lg_shots_bin --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['build_ms'] == 0 and 'ios27-parity' in d['output'], f'relative prebuilt-bin failed: {d}'
print('OK: invar shot relative --prebuilt-bin works across cwd')
" "$SHOT_REL"
if "$INVAR" shot --list --prebuilt-bin "$SHOT_BIN" --out-bin "$TMP/conflict_bin" --json >/dev/null 2>&1; then
  echo "FAIL: invar shot should reject combining --prebuilt-bin and --out-bin" >&2
  exit 1
fi
echo "OK: invar shot rejects --prebuilt-bin + --out-bin combination"

echo "=== 12. invar compare (full, multi-ROI @2x + zoom + with-diff, exact luma spread, error gates) ==="
CMP_DIR="$TMP/cmp"
mkdir -p "$CMP_DIR"
python3 - "$CMP_DIR" << 'PYEOF'
import json, os, struct, sys, zlib

def write_png(path, w, h, rgba):
    def chunk(ct, body):
        crc = zlib.crc32(body, zlib.crc32(ct)) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + ct + body + struct.pack(">I", crc)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y*w*4:(y+1)*w*4])
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)

d = sys.argv[1]
w, h = 40, 40
# ref: 95% black (0,0,0) and 5% white (255,255,255) inside ROI [4..24, 4..24] (20x20 = 400 px: 380 black + 20 white)
ref = bytearray([0, 0, 0, 255] * (w * h))
cand = bytearray(ref)
for y in range(4, 24):
    for x in range(4, 24):
        idx = (y - 4) * 20 + (x - 4)
        o = (y * w + x) * 4
        if idx >= 380:
            ref[o:o+4] = bytes([255, 255, 255, 255])
            cand[o:o+4] = bytes([128, 128, 128, 255])
write_png(os.path.join(d, "ref.png"), w, h, ref)
write_png(os.path.join(d, "cand.png"), w, h, cand)
# Mismatched size PNG
small = bytearray([10, 20, 30, 255] * (20 * 20))
write_png(os.path.join(d, "small.png"), 20, 20, small)
with open(os.path.join(d, "rois.json"), "w") as f:
    json.dump([{"name": "top_left", "x": 0, "y": 0, "w": 6, "h": 6}], f)
PYEOF

# 12a. Full image compare (no --roi)
CMP_FULL_OUT="$CMP_DIR/out_full"
CMP_FULL_JSON="$("$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/cand.png" --out "$CMP_FULL_OUT" --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['same_size'] is True
assert len(d['rois']) == 1 and d['rois'][0]['name'] == 'full'
assert d['rois'][0]['changed_pixels'] == 20
assert os.path.isfile(d['rois'][0]['crop_file'])
assert os.path.isfile(d['sheet_file'])
assert os.path.isfile(d['json_file'])
print('OK: invar compare full-image')
" "$CMP_FULL_JSON"

# 12b. Multi-ROI @2x logical coords + --roi-file + --zoom 2 + --with-diff + exact p99.5-p5 check
CMP_ROI_OUT="$CMP_DIR/out_roi"
CMP_ROI_JSON="$("$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/cand.png" --out "$CMP_ROI_OUT" --scale 2 --zoom 2 --with-diff --roi-file "$CMP_DIR/rois.json" --roi "panel=2,2,10,10" --json)"
assert_py "
import json, os, struct, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and len(d['rois']) == 2
r_tl, r_p = d['rois'][0], d['rois'][1]
assert r_tl['name'] == 'top_left' and r_tl['rect_px'] == [0, 0, 12, 12]
assert r_p['name'] == 'panel' and r_p['rect_px'] == [4, 4, 20, 20]
assert r_p['changed_pixels'] == 20 and r_p['total_pixels'] == 400
# ref ROI has 380 black (0) + 20 white (255) -> p5=0.0, p99_5=1.0, spread=1.0
assert abs(r_p['ref_luma']['p5'] - 0.0) < 1e-5 and abs(r_p['ref_luma']['p99_5'] - 1.0) < 1e-5
assert abs(r_p['ref_luma']['spread'] - 1.0) < 1e-5, f'ref spread={r_p[\"ref_luma\"]}'
# cand ROI has 380 black (0) + 20 gray (128) -> p5=0.0, p99_5=128/255, spread=128/255
expected_cand_sp = 128.0 / 255.0
assert abs(r_p['cand_luma']['spread'] - expected_cand_sp) < 1e-4, f'cand spread={r_p[\"cand_luma\"]}'
assert abs(r_p['luma_spread_delta'] - (expected_cand_sp - 1.0)) < 1e-4
# Verify crop_panel.png dimensions: 3 panels of 20*2=40 px + 2*2=4 px red separators = 124 wide, 20 header + 40 = 60 tall
with open(r_p['crop_file'], 'rb') as f:
    hdr = f.read(24)
cw, ch = struct.unpack('>II', hdr[16:24])
assert cw == 124 and ch == 60, f'unexpected crop_panel size: {cw}x{ch}'
assert os.path.isfile(d['sheet_file'])
with open(d['json_file']) as f:
    saved = json.load(f)
assert saved['rois'][1]['changed_pixels'] == 20
print('OK: invar compare multi-ROI @2x + zoom 2 + with-diff + exact luma spread (%dx%d)' % (cw, ch))
" "$CMP_ROI_JSON"

# 12c. Size mismatch exits non-zero by default
if "$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/small.png" --out "$CMP_DIR/out_mismatch" --json >/dev/null 2>&1; then
  echo "FAIL: invar compare should fail on size mismatch without --allow-size-diff" >&2
  exit 1
fi
echo "OK: invar compare rejects size mismatch"

# 12d. Out-of-bounds ROI exits non-zero
if "$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/cand.png" --out "$CMP_DIR/out_oob" --roi "bad=30,30,20,20" --json >/dev/null 2>&1; then
  echo "FAIL: invar compare should fail on out-of-bounds ROI" >&2
  exit 1
fi
echo "OK: invar compare rejects out-of-bounds ROI"

# 12e. Malformed --roi syntax exits non-zero
if "$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/cand.png" --out "$CMP_DIR/out_syntax" --roi "bad_no_equals" --json >/dev/null 2>&1; then
  echo "FAIL: invar compare should fail on malformed --roi syntax" >&2
  exit 1
fi
echo "OK: invar compare rejects malformed --roi syntax"

# 12f. Uncovered hotspot detection & --auto-hotspots 1 auto-cropping
python3 - "$CMP_DIR" << 'PYEOF'
import os, struct, sys, zlib
def write_png(path, w, h, rgba):
    def chunk(ct, body):
        crc = zlib.crc32(body, zlib.crc32(ct)) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + ct + body + struct.pack(">I", crc)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y*w*4:(y+1)*w*4])
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)
d = sys.argv[1]
w, h = 192, 128
b1 = bytearray([20, 20, 20, 255] * (w * h))
b2 = bytearray(b1)
# Put a strong diff block outside user ROI [0,0,64,64] at [128..160, 64..96] (cell [128,64,64,64])
for y in range(64, 96):
    for x in range(128, 160):
        o = (y * w + x) * 4
        b2[o:o+4] = bytes([240, 40, 40, 255])
write_png(os.path.join(d, "hs_ref.png"), w, h, b1)
write_png(os.path.join(d, "hs_cand.png"), w, h, b2)
PYEOF

HS_JSON="$("$INVAR" compare "$CMP_DIR/hs_ref.png" "$CMP_DIR/hs_cand.png" --out "$CMP_DIR/out_hs" --roi "left=0,0,64,64" --auto-hotspots 1 --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True
hs = d['hotspots']
assert len(hs) >= 1, f'expected hotspot: {d}'
assert hs[0]['rect_px'] == [128, 64, 64, 64], f'wrong hotspot rect: {hs[0]}'
assert hs[0]['covered_by_roi'] is False, f'hotspot should be uncovered: {hs[0]}'
names = [r['name'] for r in d['rois']]
assert names == ['left', 'hotspot_1'], f'expected auto-cropped hotspot_1: {names}'
assert os.path.isfile(d['rois'][1]['crop_file']), 'missing crop_hotspot_1.png'
print('OK: invar compare hotspots + --auto-hotspots 1 (%s)' % os.path.basename(d['rois'][1]['crop_file']))
" "$HS_JSON"

# 12g. Adversarial hotspot test 1: 6 decreasing diff cells, ROIs cover top 5 -> 6th uncovered cell MUST be reported & cropped
python3 - "$CMP_DIR" << 'PYEOF'
import os, struct, sys, zlib
def write_png(path, w, h, rgba):
    def chunk(ct, body):
        crc = zlib.crc32(body, zlib.crc32(ct)) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + ct + body + struct.pack(">I", crc)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y*w*4:(y+1)*w*4])
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)
d = sys.argv[1]
w, h = 384, 128
b1 = bytearray([20, 20, 20, 255] * (w * h))
b2 = bytearray(b1)
# 6 cells across top row: (0,0)..(320,0) with decreasing deltas 220, 190, 160, 130, 100, 70
for ci, delta in enumerate([220, 190, 160, 130, 100, 70]):
    x0 = ci * 64
    for y in range(10, 54):
        for x in range(x0 + 10, x0 + 54):
            o = (y * w + x) * 4
            b2[o] = 20 + delta
write_png(os.path.join(d, "hs6_ref.png"), w, h, b1)
write_png(os.path.join(d, "hs6_cand.png"), w, h, b2)

# Half-cell adversarial image: diff in x=0..32, ROI covers x=32..64 (50% area, 0% diff pixels)
b3 = bytearray([20, 20, 20, 255] * (128 * 64))
b4 = bytearray(b3)
for y in range(64):
    for x in range(32):
        o = (y * 128 + x) * 4
        b4[o] = 240
write_png(os.path.join(d, "half_ref.png"), 128, 64, b3)
write_png(os.path.join(d, "half_cand.png"), 128, 64, b4)

# Empty ROI file
with open(os.path.join(d, "empty_rois.json"), "w") as f:
    f.write("[]\n")
PYEOF

HS6_JSON="$("$INVAR" compare "$CMP_DIR/hs6_ref.png" "$CMP_DIR/hs6_cand.png" --out "$CMP_DIR/out_hs6" \
  --roi "c0=0,0,64,64" --roi "c1=64,0,64,64" --roi "c2=128,0,64,64" --roi "c3=192,0,64,64" --roi "c4=256,0,64,64" \
  --auto-hotspots 1 --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True
uncov = [h for h in d['hotspots'] if not h['covered_by_roi']]
assert len(uncov) == 1 and uncov[0]['rect_px'] == [320, 0, 64, 64], f'6th cell missed: {d[\"hotspots\"]}'
hs_roi = [r for r in d['rois'] if r['name'] == 'hotspot_1']
assert len(hs_roi) == 1 and hs_roi[0]['rect_px'] == [320, 0, 64, 64] and os.path.isfile(hs_roi[0]['crop_file'])
print('OK: invar compare 6th uncovered hotspot detected & auto-cropped despite top-5 covered')
" "$HS6_JSON"

# 12h. Adversarial hotspot test 2: diff in x=0..32, ROI in x=32..64 (50% area but 0% diff pixels) -> uncovered!
HALF_JSON="$("$INVAR" compare "$CMP_DIR/half_ref.png" "$CMP_DIR/half_cand.png" --out "$CMP_DIR/out_half" \
  --roi "right_half=32,0,32,64" --auto-hotspots 1 --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True
uncov = [h for h in d['hotspots'] if not h['covered_by_roi']]
assert len(uncov) == 1 and uncov[0]['rect_px'] == [0, 0, 64, 64], f'half-cell misclassified as covered: {d[\"hotspots\"]}'
hs_roi = [r for r in d['rois'] if r['name'] == 'hotspot_1']
assert len(hs_roi) == 1 and os.path.isfile(hs_roi[0]['crop_file'])
print('OK: invar compare diff-pixel hotspot coverage avoids half-cell blindspot')
" "$HALF_JSON"

# 12i. Case-insensitive global slug dedup + empty --roi-file rejection
DEDUP_JSON="$("$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/cand.png" --out "$CMP_DIR/out_dedup" \
  --roi "a=0,0,10,10" --roi "a=10,0,10,10" --roi "a_2=20,0,10,10" --roi "A=30,0,10,10" --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and len(d['rois']) == 4
paths = [r['crop_file'] for r in d['rois']]
assert all(os.path.isfile(p) for p in paths), f'missing files: {paths}'
assert len(set(p.lower() for p in paths)) == 4, f'case-insensitive collision: {paths}'
print('OK: invar compare case-insensitive slug dedup (%s)' % [os.path.basename(p) for p in paths])
" "$DEDUP_JSON"

if "$INVAR" compare "$CMP_DIR/ref.png" "$CMP_DIR/cand.png" --out "$CMP_DIR/out_empty_rf" --roi-file "$CMP_DIR/empty_rois.json" --json >/dev/null 2>&1; then
  echo "FAIL: invar compare should fail on empty --roi-file []" >&2
  exit 1
fi
echo "OK: invar compare rejects empty --roi-file []"
) > "$TMP/log_step9_12.txt" 2>&1 &
PID_STEP9_12=$!

CLI_PAR_FAIL=0
wait "$PID_STEP6" || CLI_PAR_FAIL=1
wait "$PID_STEP7_8B" || CLI_PAR_FAIL=1
wait "$PID_STEP9_12" || CLI_PAR_FAIL=1

cat "$TMP/log_step6.txt" "$TMP/log_step7_8b.txt" "$TMP/log_step9_12.txt"
if [ "$CLI_PAR_FAIL" -ne 0 ]; then
  echo "FAIL: one or more invar CLI E2E test groups failed" >&2
  exit 1
fi

echo "=== ALL 12 INVAR CLI E2E TESTS PASSED! ==="
