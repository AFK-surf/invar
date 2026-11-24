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
AUD_JSON="$("$INVAR" audit --json)"
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
WS_CHK_JSON="$("$INVAR" check --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and len(d['packages']) >= 40, f'workspace check failed: {d}'
print('OK: workspace check (%d packages, %d ms)' % (len(d['packages']), d['elapsed_ms']))
" "$WS_CHK_JSON"

WS_TST_JSON="$("$INVAR" test --json)"
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
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['total_failed'] == 0, f'multi-pkg test failed: {d}'
assert [p['pkg'] for p in d['packages']] == ['damage', 'halt'], f'order mismatch: {d}'
# damage has ~12 own tests and halt has ~2 own tests; --only-pkg must not re-run transitive deps
assert d['packages'][0]['passed'] < 40 and d['packages'][1]['passed'] >= 1, f'unexpected counts: {d}'
print('OK: parallel multi-pkg --only-pkg (%d passed, %d ms)' % (d['total_passed'], d['elapsed_ms']))
" "$MULTI_TST_JSON"

ZERO_FLT_JSON="$("$INVAR" test --pkg damage --filter __no_such_test_xyz__ --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True and d['total_passed'] == 0 and d['total_failed'] == 0, f'zero-match filter failed: {d}'
print('OK: zero-match filter')
" "$ZERO_FLT_JSON"

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

echo "=== 7. invar new web-app + invar web build ==="
WEB_DIR="$TMP/my_web_app"
WEB_OUT="$TMP/web_dist"
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

echo "=== 8. invar web check --json (packages/lg_web) ==="
WEB_CHK="$("$INVAR" web check --json)"
assert_py "
import json, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'web check lg_web failed: {d}'
assert d['imports_total'] > 100 and d['imports_missing'] == []
print('OK: web check lg_web (%d B raw, %d imports, 0 missing)' % (d['raw_bytes'], d['imports_total']))
" "$WEB_CHK"

echo "=== 8b. invar web build cleans stale _headers/_redirects/wasm across packages ==="
SHARED_OUT="$TMP/shared_web_out"
"$INVAR" web build --pkg "$ROOT/packages/lg_web" --out "$SHARED_OUT" --json >/dev/null
assert_py "
import os, sys
out = sys.argv[1]
assert os.path.isfile(os.path.join(out, '_headers')), 'lg_web must emit _headers'
assert os.path.isfile(os.path.join(out, '_redirects')), 'lg_web must emit _redirects'
assert os.path.isfile(os.path.join(out, 'lg_web.wasm')), 'lg_web must emit lg_web.wasm'
" "$SHARED_OUT"
# A file of the caller's own in the output dir: never invar's to remove.
printf '\0asm\1\0\0\0' > "$SHARED_OUT/decoder.wasm"
"$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --json >/dev/null
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
"$INVAR" web build --pkg "$ROOT/packages/lg_web" --out "$SHARED_OUT" --json >/dev/null
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
"$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --json >/dev/null
assert_py "
import os, sys
out = sys.argv[1]
assert open(os.path.join(out, '_headers'), 'rb').read() == b'/*\\n  X-Mine: 1\\n', 'caller _headers in a remade dir kept byte for byte'
assert os.path.isfile(os.path.join(out, 'my_web_app.wasm')), 'custom wasm built'
print('OK: a caller _headers in a remade output dir is kept')
" "$SHARED_OUT"
# The same when the caller only rewrites a file invar wrote there.
"$INVAR" web build --pkg "$ROOT/packages/lg_web" --out "$SHARED_OUT" --json >/dev/null
printf '/*\n  X-Mine: 2\n' > "$SHARED_OUT/_headers"
"$INVAR" web build --pkg "$WEB_DIR" --out "$SHARED_OUT" --json >/dev/null
assert_py "
import os, sys
out = sys.argv[1]
assert open(os.path.join(out, '_headers'), 'rb').read() == b'/*\\n  X-Mine: 2\\n', 'rewritten _headers kept'
assert not os.path.exists(os.path.join(out, '_redirects')), 'untouched stale _redirects still removed'
assert not os.path.exists(os.path.join(out, 'lg_web.wasm')), 'untouched stale lg_web.wasm still removed'
print('OK: a rewritten _headers is kept; untouched stale files are removed')
" "$SHARED_OUT"

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
SHOT_LIST="$("$INVAR" shot --list --json)"
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
BOGUS_JSON="$("$INVAR" shot --only __no_such_preset__ --out "$SHOT_DIR" --json)" || BOGUS_RC=$?
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

SHOT_OK="$("$INVAR" shot --only ios27-parity-light --scale 1 --out "$SHOT_DIR" --json)"
assert_py "
import json, os, sys
d = json.loads(sys.argv[1])
assert d['ok'] is True, f'ios27-parity-light shot failed: {d}'
assert len(d['pngs']) == 1 and d['mp4s'] == [], f'unexpected shot outputs: {d}'
p = d['pngs'][0]
assert os.path.basename(p['file']) == 'ios27-parity-light.png', f'wrong file: {p}'
assert p['width'] == 390 and p['height'] == 844, f'scale=1 override failed: {p}'
print('OK: invar shot fresh output + scale=1 override (%dx%d, build %d ms, run %d ms)' % (p['width'], p['height'], d['build_ms'], d['run_ms']))
" "$SHOT_OK"

echo "=== ALL 11 INVAR CLI E2E TESTS PASSED! ==="
