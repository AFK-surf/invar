#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/invar_reactive_audit_test.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

# Verify pionc is available and supports `query analyze`
PIONC_BIN="${PIONC:-}"
if [ -z "$PIONC_BIN" ]; then
  PIONC_BIN="$(command -v pionc 2>/dev/null || command -v pionc2 2>/dev/null || true)"
fi
if [ -z "$PIONC_BIN" ] || [ ! -x "$PIONC_BIN" ]; then
  echo "FAIL: pionc not found (set PIONC=/path/to/pionc2)" >&2
  exit 1
fi
export PIONC="$PIONC_BIN"

make_pkg() {
  local name="$1"
  local body="$2"
  local pdir="$TMP_DIR/$name"
  mkdir -p "$pdir/src"
  cat > "$pdir/pion.toml" <<EOF
[package]
name = "$name"
version = "0.1.0"

[dependencies]
signals = { path = "$ROOT_DIR/packages/signals" }
view = { path = "$ROOT_DIR/packages/view" }
ui = { path = "$ROOT_DIR/packages/ui" }
EOF
  printf '%s\n' "$body" > "$pdir/src/lib.pion"
  echo "$pdir"
}

run_case() {
  local label="$1"
  local pkg_dir="$2"
  local expected_json_check="$3"
  shift 3
  local out_file="$TMP_DIR/${label}.json"
  set +e
  python3 "$ROOT_DIR/scripts/invar" audit --json "$@" "$pkg_dir" > "$out_file"
  local rc=$?
  set -e
  python3 - "$label" "$out_file" "$rc" "$expected_json_check" <<'PY'
import json, sys

label, out_file, rc_s, mode = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
rc = int(rc_s)
with open(out_file, "r", encoding="utf-8") as f:
    raw = f.read()
try:
    data = json.loads(raw)
except Exception as e:
    print(f"[FAIL] {label}: invalid JSON ({e})\n{raw}", file=sys.stderr)
    sys.exit(1)

diags = data.get("diagnostics", [])
codes = [d["code"] for d in diags if d.get("severity") == "error"]
ra = data.get("reactive_audit", {})

if mode == "clean":
    assert rc == 0, f"{label}: expected rc=0, got {rc}: {diags}"
    assert data.get("ok") is True, f"{label}: expected ok=True"
    assert data.get("error_count") == 0, f"{label}: expected 0 errors, got {codes}: {diags}"
    assert ra.get("ran") is True, f"{label}: expected reactive_audit.ran=True, got {ra}"
    assert ra.get("closures_checked", 0) > 0, f"{label}: expected closures_checked > 0, got {ra}"
elif mode == "degraded":
    assert rc == 0, f"{label}: expected rc=0 in degraded mode, got {rc}: {diags}"
    assert data.get("ok") is True, f"{label}: expected ok=True in degraded mode"
    assert data.get("error_count") == 0, f"{label}: expected 0 errors in degraded mode, got {codes}"
    assert ra.get("ran") is False, f"{label}: expected reactive_audit.ran=False, got {ra}"
    assert isinstance(ra.get("reason"), str) and len(ra["reason"]) > 0, f"{label}: missing reason in {ra}"
else:
    # mode is expected error code, optionally with ":substring"
    if ":" in mode:
        exp_code, exp_substr = mode.split(":", 1)
    else:
        exp_code, exp_substr = mode, ""
    assert rc != 0, f"{label}: expected non-zero exit code for {exp_code}"
    assert data.get("ok") is False, f"{label}: expected ok=False"
    if exp_code == "INVAR_E007_REACTIVE_AUDIT_SKIPPED":
        assert ra.get("ran") is False, f"{label}: expected reactive_audit.ran=False, got {ra}"
    else:
        assert ra.get("ran") is True, f"{label}: expected reactive_audit.ran=True, got {ra}"
    matching = [d for d in diags if d.get("code") == exp_code and d.get("severity") == "error"]
    assert len(matching) >= 1, f"{label}: expected {exp_code}, got codes={codes}, diags={diags}"
    if exp_substr:
        assert any(exp_substr in d.get("message", "") for d in matching), (
            f"{label}: expected message containing {exp_substr!r}, got {[d.get('message') for d in matching]}"
        )
print(f"[PASS] {label}: ok={data.get('ok')} errors={data.get('error_count')} codes={codes} reactive={ra}")
PY
}

echo "=== Track I2 Typed Reactive Static Audit Test Suite ==="

# 1a. Positive E007: direct signal write inside Live::By
PKG_E007_DIRECT="$(make_pkg "pos_e007_direct" '
import "signals"
import "view"

pub fn bad_direct(root : view::View) -> Unit {
  let s = signals::signal(0)
  let _ = root.words(view::Live::By(fn() -> String {
    s.set(1)
    "bad"
  }), look=view::words_plain)
}
')"
run_case "e007_direct" "$PKG_E007_DIRECT" "INVAR_E007_REACTIVE_WRITE:signals::Signal::set"

# 1b. Positive E007: transitive signal write through helper function
PKG_E007_TRANS="$(make_pkg "pos_e007_trans" '
import "signals"
import "view"

fn helper_mutate(s : signals::Signal[Int]) -> Int {
  s.update(fn(x : Int) -> Int { x + 1 })
  0
}

pub fn bad_transitive(root : view::View) -> Unit {
  let s = signals::signal(0)
  let _ = root.words(view::Live::By(fn() -> String {
    let _ = helper_mutate(s)
    "bad"
  }), look=view::words_plain)
}
')"
run_case "e007_transitive" "$PKG_E007_TRANS" "INVAR_E007_REACTIVE_WRITE:helper_mutate -> signals::Signal::update"

# 2a. Positive E008: capturing let mut variable
PKG_E008_MUT="$(make_pkg "pos_e008_let_mut" '
import "signals"

pub fn bad_let_mut() -> signals::Computed[Int] {
  let mut count = 0
  count = count + 1
  signals::computed(fn(_prev : Option[Int]) -> Int {
    count
  })
}
')"
run_case "e008_let_mut" "$PKG_E008_MUT" "INVAR_E008_REACTIVE_MUT_CAPTURE:let mut count"

# 2b. Positive E008: capturing immutable binding to mutable struct
PKG_E008_STRUCT="$(make_pkg "pos_e008_mut_struct" '
import "signals"

pub struct MutBox {
  mut n : Int
}

pub fn bad_mut_struct() -> signals::Computed[Int] {
  let st = MutBox::{ n: 0 }
  signals::computed(fn(_prev : Option[Int]) -> Int {
    st.n
  })
}
')"
run_case "e008_mut_struct" "$PKG_E008_STRUCT" "INVAR_E008_REACTIVE_MUT_CAPTURE:MutBox"

# 2c. Positive E008: capturing mutable Array
PKG_E008_ARRAY="$(make_pkg "pos_e008_array" '
import "signals"

pub fn bad_array() -> signals::Computed[Int] {
  let xs : Array[Int] = [1, 2]
  signals::computed(fn(_prev : Option[Int]) -> Int {
    xs.length()
  })
}
')"
run_case "e008_array" "$PKG_E008_ARRAY" "INVAR_E008_REACTIVE_MUT_CAPTURE:Array[Int]"

# 3. Positive E009: signals::effect both reading and writing captured signal
PKG_E009="$(make_pkg "pos_e009_effect" '
import "signals"

pub fn bad_effect() -> () -> Unit {
  let s = signals::signal(0)
  signals::effect(fn() -> Unit {
    let v = s.get()
    if v < 10 {
      s.set(v + 1)
    }
  })
}
')"
run_case "e009_effect_self_write" "$PKG_E009" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 4. Positive E010: transitive blocking IO / scope inside reactive binding
PKG_E010="$(make_pkg "pos_e010_io" '
import "signals"

fn helper_io(world : World) -> Int {
  let _ = fs_mmap_ro(world, "/etc/hosts")
  scope(world, fn(_s : Scope) -> Unit { () })
  1
}

pub fn bad_io(world : World) -> signals::Computed[Int] {
  signals::computed(fn(_prev : Option[Int]) -> Int {
    helper_io(world)
  })
}
')"
run_case "e010_reactive_io" "$PKG_E010" "INVAR_E010_REACTIVE_IO:helper_io -> builtin::"

# 5. Negative (clean): valid ReadSignal, Signal, Computed, val struct, scalar, String, untracked, on_press write
PKG_CLEAN="$(make_pkg "neg_clean" '
import "signals"
import "view"

pub val struct Snap {
  label : String
  n : Int
}

pub fn good_view(root : view::View) -> Unit {
  let count = signals::signal(0)
  let ro = count.read()
  let doubled = signals::computed(fn(_prev : Option[Int]) -> Int {
    ro.get() * 2
  })
  let snap = Snap::{ label: "ok", n: 42 }
  let prefix = "count="
  let _ = root.box(fn(b : view::View) -> Unit {
    let _ = b
      .words(view::Live::By(fn() -> String {
        let cur = signals::untracked(fn() -> Int { count.get() })
        "\{prefix}\{doubled.get()}:\{snap.label}:\{snap.n}:\{cur}"
      }), look=view::words_plain)
      .on_press(fn() -> Unit {
        count.set(count.untracked() + 1)
      })
  })
}
')"
run_case "neg_clean" "$PKG_CLEAN" "clean"

# 6. Inline allow comment test
PKG_ALLOW="$(make_pkg "neg_inline_allow" '
import "signals"

pub fn allowed_mut() -> signals::Computed[Int] {
  let xs : Array[Int] = [1, 2]
  // invar-audit-allow: E008
  signals::computed(fn(_prev : Option[Int]) -> Int {
    xs.length()
  })
}
')"
run_case "neg_inline_allow" "$PKG_ALLOW" "clean"

# 7a. Graceful degradation: --no-reactive flag
run_case "degrade_no_reactive_flag" "$PKG_E007_DIRECT" "degraded" --no-reactive

# 7b. Graceful degradation: legacy pionc shim without `query analyze`
FAKE_PIONC="$TMP_DIR/fake_pionc"
cat > "$FAKE_PIONC" <<'SH'
#!/bin/sh
if [ "${1:-}" = "--help" ]; then
  echo "pionc 0.0.1"
  echo "  pionc check [input]"
  echo "  pionc query defs|sig|callers|deps [--json] [input]"
  exit 0
fi
echo "error: unknown query kind \`analyze\`" >&2
exit 1
SH
chmod +x "$FAKE_PIONC"
PIONC="$FAKE_PIONC" run_case "degrade_legacy_pionc" "$PKG_E007_DIRECT" "degraded"

# 7c. --require-reactive fails when reactive audit is skipped (--no-reactive or legacy pionc)
run_case "require_reactive_no_reactive" "$PKG_E007_DIRECT" "INVAR_E007_REACTIVE_AUDIT_SKIPPED" --no-reactive --require-reactive
PIONC="$FAKE_PIONC" run_case "require_reactive_legacy_pionc" "$PKG_E007_DIRECT" "INVAR_E007_REACTIVE_AUDIT_SKIPPED" --require-reactive

# 8. Negative E009: effect reading ReadSignal A and writing distinct Signal B must pass clean
PKG_E009_CROSS="$(make_pkg "neg_e009_cross_signal" '
import "signals"

pub fn cross_signal_effect(src : signals::ReadSignal[Int]) -> () -> Unit {
  let dst = signals::signal(0)
  signals::effect(fn() -> Unit {
    dst.set(src.get() + 1)
  })
}
')"
run_case "neg_e009_cross_signal" "$PKG_E009_CROSS" "clean"

# 9. Negative E009: effect reading writable Signal A and writing distinct writable Signal B must pass clean
PKG_E009_TWO_WRITABLE="$(make_pkg "neg_e009_two_writable" '
import "signals"

pub fn two_writable_signals_effect() -> () -> Unit {
  let a = signals::signal(1)
  let b = signals::signal(0)
  signals::effect(fn() -> Unit {
    b.set(a.get() + 1)
  })
}
')"
run_case "neg_e009_two_writable" "$PKG_E009_TWO_WRITABLE" "clean"

# 10. Positive E009: effect reading readonly alias `let ro = s.read()` and writing `s` must fail E009
PKG_E009_RO_ALIAS="$(make_pkg "pos_e009_ro_alias" '
import "signals"

pub fn ro_alias_self_write() -> () -> Unit {
  let s = signals::signal(0)
  let ro = s.read()
  signals::effect(fn() -> Unit {
    let v = ro.get()
    if v < 5 {
      s.set(v + 1)
    }
  })
}
')"
run_case "pos_e009_ro_alias" "$PKG_E009_RO_ALIAS" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 11. Positive E009: effect reading and writing the same writable Signal via Keyed::count/set
PKG_E009_SAME_SIG="$(make_pkg "pos_e009_same_signal" '
import "signals"

pub fn same_signal_self_write() -> () -> Unit {
  let counter = signals::signal(0)
  signals::effect(fn() -> Unit {
    let cur = counter.get()
    if cur < 3 {
      counter.set(cur + 1)
    }
  })
}
')"
run_case "pos_e009_same_signal" "$PKG_E009_SAME_SIG" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 12. Positive E009: plain signal alias `let a2 = a`, read `a.get()`, write `a2.set(...)`
PKG_E009_PLAIN_ALIAS="$(make_pkg "pos_e009_plain_alias" '
import "signals"

pub fn plain_alias_self_write() -> () -> Unit {
  let a = signals::signal(0)
  let a2 = a
  signals::effect(fn() -> Unit {
    let v = a.get()
    if v < 5 {
      a2.set(v + 1)
    }
  })
}
')"
run_case "pos_e009_plain_alias" "$PKG_E009_PLAIN_ALIAS" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 13. Positive E009: distant alias `let ro = s.read()` more than 130 lines before effect
DISTANT_PAD=""
for _i in $(seq 1 135); do
  DISTANT_PAD="${DISTANT_PAD}  let _pad_${_i} = ${_i}
"
done
PKG_E009_DISTANT_ALIAS="$(make_pkg "pos_e009_distant_alias" "
import \"signals\"

pub fn distant_alias_self_write() -> () -> Unit {
  let s = signals::signal(0)
  let ro = s.read()
${DISTANT_PAD}  signals::effect(fn() -> Unit {
    let v = ro.get()
    if v < 5 {
      s.set(v + 1)
    }
  })
}
")"
run_case "pos_e009_distant_alias" "$PKG_E009_DISTANT_ALIAS" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 14. Negative E009: commented-out fake alias `// let r = b.read()` must NOT cause false positive
PKG_E009_COMMENT_ALIAS="$(make_pkg "neg_e009_comment_alias" '
import "signals"

pub fn comment_alias_clean() -> () -> Unit {
  let a = signals::signal(1)
  let b = signals::signal(0)
  let r = a.read()
  // let r = b.read()
  /* let a = b */
  signals::effect(fn() -> Unit {
    b.set(r.get() + 1)
  })
}
')"
run_case "neg_e009_comment_alias" "$PKG_E009_COMMENT_ALIAS" "clean"

# 15. Positive E009: inner local alias inside effect closure body `let r = s.read()`
PKG_E009_INNER_ALIAS="$(make_pkg "pos_e009_inner_alias" '
import "signals"

pub fn inner_alias_self_write() -> () -> Unit {
  let s = signals::signal(0)
  signals::effect(fn() -> Unit {
    let r = s.read()
    let v = r.get()
    if v < 5 {
      s.set(v + 1)
    }
  })
}
')"
run_case "pos_e009_inner_alias" "$PKG_E009_INNER_ALIAS" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 16. Positive E009: an inner block's `let ro = b.read()` ends with its block; the outer
#     `let ro = a.read()` is what the effect reads, and it writes `a`
PKG_E009_BLOCK_SHADOW="$(make_pkg "pos_e009_block_shadow_alias" '
import "signals"

pub fn block_shadow_alias_self_write() -> () -> Unit {
  let a = signals::signal(0)
  let b = signals::signal(0)
  let ro = a.read()
  if true {
    let ro = b.read()
    let _ = ro
  }
  signals::effect(fn() -> Unit {
    let v = ro.get()
    if v < 5 {
      a.set(v + 1)
    }
  })
}
')"
run_case "pos_e009_block_shadow_alias" "$PKG_E009_BLOCK_SHADOW" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

# 17. Positive E009: a char literal `'"'` before the alias must not read as a string opening
PKG_E009_CHAR_QUOTE="$(make_pkg "pos_e009_char_quote_literal" '
import "signals"

pub fn char_quote_alias_self_write() -> () -> Unit {
  let q = '\''"'\''
  let _ = q
  let s = signals::signal(0)
  let ro = s.read()
  signals::effect(fn() -> Unit {
    let v = ro.get()
    if v < 5 {
      s.set(v + 1)
    }
  })
}
')"
run_case "pos_e009_char_quote_literal" "$PKG_E009_CHAR_QUOTE" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set"

echo "=== ALL REACTIVE AUDIT TESTS PASSED ==="
