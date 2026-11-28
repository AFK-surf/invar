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

init_multi_pkg() {
  local name="$1"
  local pdir="$TMP_DIR/$name"
  mkdir -p "$pdir/src"
  cat > "$pdir/pion.toml" <<EOF
[package]
name = "$name"
version = "0.1.0"

[dependencies]
signals = { path = "$ROOT_DIR/packages/signals" }
view = { path = "$ROOT_DIR/packages/view" }
EOF
  echo "$pdir"
}

NEG_PKG="$(init_multi_pkg "neg_all_clean")"
POS_PKG="$(init_multi_pkg "pos_all_errors")"
NEG_EXPECT_FILE="$TMP_DIR/neg_expect.jsonl"
POS_EXPECT_FILE="$TMP_DIR/pos_expect.jsonl"
: > "$NEG_EXPECT_FILE"
: > "$POS_EXPECT_FILE"

add_clean_case() {
  local label="$1"
  local body="$2"
  printf '%s\n' "$body" > "$NEG_PKG/src/${label}.pion"
  python3 - "$NEG_EXPECT_FILE" "$label" <<'PY'
import json, sys
p, label = sys.argv[1:3]
with open(p, "a", encoding="utf-8") as f:
    f.write(json.dumps({"label": label}) + "\n")
PY
}

add_error_case() {
  local label="$1"
  local expected="$2"
  local body="$3"
  printf '%s\n' "$body" > "$POS_PKG/src/${label}.pion"
  python3 - "$POS_EXPECT_FILE" "$label" "$expected" <<'PY'
import json, sys
p, label, expected = sys.argv[1:4]
with open(p, "a", encoding="utf-8") as f:
    f.write(json.dumps({"label": label, "expected": expected}) + "\n")
PY
}

echo "=== Track I2 Typed Reactive Static Audit Test Suite ==="

# 1a. Positive E007: direct signal write inside Live::By
add_error_case "e007_direct" "INVAR_E007_REACTIVE_WRITE:signals::Signal::set" '
import "signals"
import "view"

pub fn bad_direct(root : view::View) -> Unit {
  let s = signals::signal(0)
  let _ = root.words(view::Live::By(fn() -> String {
    s.set(1)
    "bad"
  }), look=view::words_plain)
}
'

# 1b. Positive E007: transitive signal write through helper function
add_error_case "e007_transitive" "INVAR_E007_REACTIVE_WRITE:helper_mutate -> signals::Signal::update" '
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
'

# 2a. Positive E008: capturing let mut variable
add_error_case "e008_let_mut" "INVAR_E008_REACTIVE_MUT_CAPTURE:let mut count" '
import "signals"

pub fn bad_let_mut() -> signals::Computed[Int] {
  let mut count = 0
  count = count + 1
  signals::computed(fn(_prev : Option[Int]) -> Int {
    count
  })
}
'

# 2b. Positive E008: capturing immutable binding to mutable struct
add_error_case "e008_mut_struct" "INVAR_E008_REACTIVE_MUT_CAPTURE:MutBox" '
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
'

# 2c. Positive E008: capturing mutable Array
add_error_case "e008_array" "INVAR_E008_REACTIVE_MUT_CAPTURE:Array[Int]" '
import "signals"

pub fn bad_array() -> signals::Computed[Int] {
  let xs : Array[Int] = [1, 2]
  signals::computed(fn(_prev : Option[Int]) -> Int {
    xs.length()
  })
}
'

# 3. Positive E009: signals::effect both reading and writing captured signal
add_error_case "e009_effect_self_write" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 4. Positive E010: transitive blocking IO / scope inside reactive binding
add_error_case "e010_reactive_io" "INVAR_E010_REACTIVE_IO:helper_io -> builtin::" '
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
'

# 5. Negative (clean): valid ReadSignal, Signal, Computed, val struct, scalar, String, untracked, on_press write
add_clean_case "neg_clean" '
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
'

# 6. Inline allow comment test
add_clean_case "neg_inline_allow" '
import "signals"

pub fn allowed_mut() -> signals::Computed[Int] {
  let xs : Array[Int] = [1, 2]
  // invar-audit-allow: E008
  signals::computed(fn(_prev : Option[Int]) -> Int {
    xs.length()
  })
}
'

# 7. Graceful degradation fixture: legacy pionc shim without `query analyze`
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

# 8. Negative E009: effect reading ReadSignal A and writing distinct Signal B must pass clean
add_clean_case "neg_e009_cross_signal" '
import "signals"

pub fn cross_signal_effect(src : signals::ReadSignal[Int]) -> () -> Unit {
  let dst = signals::signal(0)
  signals::effect(fn() -> Unit {
    dst.set(src.get() + 1)
  })
}
'

# 9. Negative E009: effect reading writable Signal A and writing distinct writable Signal B must pass clean
add_clean_case "neg_e009_two_writable" '
import "signals"

pub fn two_writable_signals_effect() -> () -> Unit {
  let a = signals::signal(1)
  let b = signals::signal(0)
  signals::effect(fn() -> Unit {
    b.set(a.get() + 1)
  })
}
'

# 10. Positive E009: effect reading readonly alias `let ro = s.read()` and writing `s` must fail E009
add_error_case "pos_e009_ro_alias" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 11. Positive E009: effect reading and writing the same writable Signal via Keyed::count/set
add_error_case "pos_e009_same_signal" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 12. Positive E009: plain signal alias `let a2 = a`, read `a.get()`, write `a2.set(...)`
add_error_case "pos_e009_plain_alias" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 13. Positive E009: distant alias `let ro = s.read()` more than 130 lines before effect
DISTANT_PAD=""
for _i in $(seq 1 135); do
  DISTANT_PAD="${DISTANT_PAD}  let _pad_${_i} = ${_i}
"
done
add_error_case "pos_e009_distant_alias" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" "
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
"

# 14. Negative E009: commented-out fake alias `// let r = b.read()` must NOT cause false positive
add_clean_case "neg_e009_comment_alias" '
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
'

# 15. Positive E009: inner local alias inside effect closure body `let r = s.read()`
add_error_case "pos_e009_inner_alias" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 16. Positive E009: an inner block's `let ro = b.read()` ends with its block; the outer
#     `let ro = a.read()` is what the effect reads, and it writes `a`
add_error_case "pos_e009_block_shadow_alias" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 17. Positive E009: a char literal `'"'` before the alias must not read as a string opening
add_error_case "pos_e009_char_quote_literal" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
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
'

# 18. Positive E009: inside the effect too, an inner block's `let ro = b.read()` ends with its
#     block; after it `ro` is the outer `a.read()` again, and the effect writes `a`
add_error_case "pos_e009_inner_block_shadow_alias" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
import "signals"

pub fn inner_block_shadow_alias_self_write() -> () -> Unit {
  let a = signals::signal(0)
  let b = signals::signal(0)
  let ro = a.read()
  signals::effect(fn() -> Unit {
    if true {
      let ro = b.read()
      let _ = ro
    }
    let v = ro.get()
    if v < 5 {
      a.set(v + 1)
    }
  })
}
'

# 19. Positive E009: `ro` is bound to the outer `a` as it is declared; an inner block's `let a = b`
#     read through `ro` does not make `ro` the inner `b`, so writing the outer `a` after the block
#     is still a self-write
add_error_case "pos_e009_decl_identity" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
import "signals"

pub fn decl_identity_self_write() -> () -> Unit {
  let a = signals::signal(0)
  let b = signals::signal(0)
  let ro = a.read()
  signals::effect(fn() -> Unit {
    {
      let a = b
      let _ = a
      let _ = ro.get()
    }
    a.set(1)
  })
}
'

# 20. Negative E009: the same effect writing `b` instead reads `a` (through `ro`) and writes `b`:
#     two signals, no self-write, though the inner block named `b` `a` while `ro` was read
add_clean_case "neg_e009_decl_identity" '
import "signals"

pub fn decl_identity_cross_write() -> () -> Unit {
  let a = signals::signal(0)
  let b = signals::signal(0)
  let ro = a.read()
  signals::effect(fn() -> Unit {
    {
      let a = b
      let _ = a
      let _ = ro.get()
    }
    b.set(1)
  })
}
'

# 21. Positive E009: `let a = a.get()` reads the outer `a` in its initializer before the shadowed
#     `a` enters scope, so writing `a` afterwards is still a self-write
add_error_case "pos_e009_self_shadow_rhs" "INVAR_E009_EFFECT_SELF_WRITE:signals::Signal::set" '
import "signals"

pub fn self_shadow_rhs_write() -> () -> Unit {
  let a = signals::signal(0)
  signals::effect(fn() -> Unit {
    {
      let a = a.get()
      let _ = a
    }
    a.set(1)
  })
}
'

# ── Batched `packages/glass` partial-handle E008 fixtures (3 multi-file packages) ──
init_glass_multi_pkg() {
  local name="$1"
  local pdir="$TMP_DIR/$name"
  mkdir -p "$pdir/src"
  cat > "$pdir/pion.toml" <<EOF
[package]
name = "$name"
version = "0.1.0"

[dependencies]
signals = { path = "$ROOT_DIR/packages/signals" }
view = { path = "$ROOT_DIR/packages/view" }
glass = { path = "$ROOT_DIR/packages/glass" }
EOF
  echo "$pdir"
}

GLASS_POS_PKG="$(init_glass_multi_pkg "glass_pos_all")"
GLASS_NEG_PKG="$(init_glass_multi_pkg "glass_neg_all")"
GLASS_POS_EXPECT_FILE="$TMP_DIR/glass_pos_expect.jsonl"
GLASS_NEG_EXPECT_FILE="$TMP_DIR/glass_neg_expect.jsonl"
: > "$GLASS_POS_EXPECT_FILE"
: > "$GLASS_NEG_EXPECT_FILE"

add_glass_error_case() {
  local label="$1"
  local expected="$2"
  local body="$3"
  printf '%s\n' "$body" > "$GLASS_POS_PKG/src/${label}.pion"
  python3 - "$GLASS_POS_EXPECT_FILE" "$label" "$expected" <<'PY'
import json, sys
p, label, expected = sys.argv[1:4]
with open(p, "a", encoding="utf-8") as f:
    f.write(json.dumps({"label": label, "expected": expected}) + "\n")
PY
}

add_glass_clean_case() {
  local label="$1"
  local body="$2"
  printf '%s\n' "$body" > "$GLASS_NEG_PKG/src/${label}.pion"
  python3 - "$GLASS_NEG_EXPECT_FILE" "$label" <<'PY'
import json, sys
p, label = sys.argv[1:3]
with open(p, "a", encoding="utf-8") as f:
    f.write(json.dumps({"label": label}) + "\n")
PY
}

# 22. Positive E008: partial handle `glass::DampedDrag` calling non-reactive `host_node()` must fail E008
add_glass_error_case "pos_e008_dd_host_node" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"

pub fn bad_dd_host_node(dd : glass::DampedDrag) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    dd.host_node() is Some(_)
  })
}
'

# 23. Positive E008: partial handle `glass::LiquidMerge` calling non-reactive `is_bonded()` must fail E008
add_glass_error_case "pos_e008_lm_is_bonded" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::LiquidMerge" '
import "glass"
import "signals"

pub fn bad_lm_is_bonded(lm : glass::LiquidMerge) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    lm.is_bonded()
  })
}
'

# 24. Negative E008: partial handle `glass::DampedDrag` calling audited reactive `value(v)` must pass
add_glass_clean_case "neg_e008_dd_value" '
import "glass"
import "signals"
import "view"

pub fn ok_dd_value(dd : glass::DampedDrag, v : view::View) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    dd.value(v)
  })
}
'

# 25. Negative E008: partial handle `glass::LiquidMerge` calling audited reactive `is_separated()` must pass
add_glass_clean_case "neg_e008_lm_is_separated" '
import "glass"
import "signals"

pub fn ok_lm_is_separated(lm : glass::LiquidMerge) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    lm.is_separated()
  })
}
'

# 26. Positive E008: forwarding partial handle `dd` to helper function must fail E008 (fail-closed)
add_glass_error_case "pos_e008_dd_helper_forward" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"

fn check_host(d : glass::DampedDrag) -> Bool {
  d.host_node() is Some(_)
}

pub fn bad_dd_helper_forward(dd : glass::DampedDrag) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    check_host(dd)
  })
}
'

# 27. Positive E008: forwarding partial handle `dd` via function value `f(dd)` must fail E008 (fail-closed)
add_glass_error_case "pos_e008_dd_fnval_forward" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"

pub fn bad_dd_fnval_forward(dd : glass::DampedDrag) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    let f = fn(x : glass::DampedDrag) -> Bool { x.host_node() is Some(_) }
    f(dd)
  })
}
'

# 28. Positive E008: Option-wrapped partial handle `Option[glass::DampedDrag]` must fail E008 (no Option stripping)
add_glass_error_case "pos_e008_opt_dd_unwrap" "INVAR_E008_REACTIVE_MUT_CAPTURE:Option[glass::DampedDrag]" '
import "glass"
import "signals"

fn check_host_opt(d : glass::DampedDrag) -> Bool {
  d.host_node() is Some(_)
}

pub fn bad_opt_dd_unwrap(opt_dd : Option[glass::DampedDrag]) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    match opt_dd {
      Some(x) => check_host_opt(x)
      None => false
    }
  })
}
'

# 29. Negative E008: reactive `dd.progress(v)` (tracked via `bounds_seq` + `value.shown`) must pass
add_glass_clean_case "neg_e008_dd_progress" '
import "glass"
import "signals"
import "view"

pub fn ok_dd_progress(dd : glass::DampedDrag, v : view::View) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    dd.progress(v)
  })
}
'

# 30. Positive E008: non-reactive `lm.pos_at(1000.0)` must fail E008
add_glass_error_case "pos_e008_lm_pos_at" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::LiquidMerge" '
import "glass"
import "signals"

pub fn bad_lm_pos_at(lm : glass::LiquidMerge) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    lm.pos_at(1000.0).0
  })
}
'

# 31. Negative E008: inner block shadowing `let dd = [1, 2]; dd.length()` after safe `dd.value(v)` must pass
add_glass_clean_case "neg_e008_dd_inner_shadow" '
import "glass"
import "signals"
import "view"

pub fn ok_dd_inner_shadow(dd : glass::DampedDrag, v : view::View) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    let x = dd.value(v)
    {
      let dd = [1, 2]
      let _ = dd.length()
    }
    x
  })
}
'

# 32. Positive E008: struct field escape `Holder::{ drag: dd }` must fail E008
add_glass_error_case "pos_e008_dd_struct_escape" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"

struct Holder {
  drag : glass::DampedDrag
}

pub fn bad_dd_struct_escape(dd : glass::DampedDrag) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    let h = Holder::{ drag: dd }
    h.drag.host_node() is Some(_)
  })
}
'

# 33. Positive E008: string interpolation calling unsafe method `dd.host_node()` must fail E008
add_glass_error_case "pos_e008_dd_interp_unsafe" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"

pub fn bad_dd_interp_unsafe(dd : glass::DampedDrag) -> signals::Computed[String] {
  signals::computed(fn(_) -> String {
    "node=\{dd.host_node() is Some(_)}"
  })
}
'

# 34. Positive E008: multiline self-shadowing initializer `let dd =\n  check_host_ml(dd)` must fail E008
add_glass_error_case "pos_e008_dd_multiline_shadow" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"

fn check_host_ml(d : glass::DampedDrag) -> Bool {
  d.host_node() is Some(_)
}

pub fn bad_dd_multiline_shadow(dd : glass::DampedDrag) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    let dd =
      check_host_ml(dd)
    dd
  })
}
'

# 35. Negative E008: match arm binding shadows `dd` while outer `dd` only calls `dd.value(v)` -> clean
add_glass_clean_case "neg_e008_dd_match_shadow" '
import "glass"
import "signals"
import "view"

pub fn ok_dd_match_shadow(dd : glass::DampedDrag, v : view::View) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    let x = dd.value(v)
    let opt : Option[glass::DampedDrag] = None
    let _ = match opt {
      Some(dd) => dd.host_node() is Some(_)
      None => false
    }
    x
  })
}
'

# 36. Negative E008: inner local `DampedDrag` calling `local_dd.host_node()` while outer `dd` only calls `dd.value(v)` -> clean
add_glass_clean_case "neg_e008_dd_local_new" '
import "glass"
import "signals"
import "view"

pub fn ok_dd_local_new(dd : glass::DampedDrag, v : view::View) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    let local_dd = glass::DampedDrag::new(0.0, lo=0.0, hi=1.0, held_scale=1.1)
    let _ = local_dd.host_node() is Some(_)
    dd.value(v)
  })
}
'

# 37. Positive E008: nested closure capturing `dd` marks outer `escaped: true` -> fail E008
add_glass_error_case "pos_e008_dd_nested_closure" "INVAR_E008_REACTIVE_MUT_CAPTURE:glass::DampedDrag" '
import "glass"
import "signals"
import "view"

pub fn bad_dd_nested_closure(dd : glass::DampedDrag, v : view::View) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    let inner = fn() -> Double { dd.value(v) }
    inner()
  })
}
'

# 38-39. Single whitebox package inside `packages/glass` testing `.dragging_sig.get()` (clean) and `dd.lo` (E008)
GLASS_WB_PKG="$TMP_DIR/glass_whitebox_all/packages/glass"
mkdir -p "$GLASS_WB_PKG/src"
sed "s|\.\./|$ROOT_DIR/packages/|g" "$ROOT_DIR/packages/glass/pion.toml" > "$GLASS_WB_PKG/pion.toml"
for f in "$ROOT_DIR/packages/glass/src/"*.pion; do
  case "$(basename "$f")" in
    *_test.pion) continue ;;
  esac
  ln -s "$f" "$GLASS_WB_PKG/src/$(basename "$f")"
done
cat > "$GLASS_WB_PKG/src/zz_probe_neg_dragging_sig.pion" <<'EOF'
pub fn ok_lm_dragging_sig(lm : LiquidMerge) -> signals::Computed[Bool] {
  signals::computed(fn(_) -> Bool {
    lm.dragging_sig.get()
  })
}
EOF
cat > "$GLASS_WB_PKG/src/zz_probe_pos_field_lo.pion" <<'EOF'
pub fn bad_dd_field_lo(dd : DampedDrag) -> signals::Computed[Double] {
  signals::computed(fn(_) -> Double {
    dd.lo
  })
}
EOF

python3 - "$ROOT_DIR" "$TMP_DIR" "$NEG_PKG" "$POS_PKG" "$NEG_EXPECT_FILE" "$POS_EXPECT_FILE" "$PIONC" "$FAKE_PIONC" \
  "$GLASS_POS_PKG" "$GLASS_NEG_PKG" "$GLASS_POS_EXPECT_FILE" "$GLASS_NEG_EXPECT_FILE" "$GLASS_WB_PKG" <<'PY'
import concurrent.futures
import json
import os
import stat
import subprocess
import sys

(
    root_dir,
    tmp_dir,
    neg_pkg,
    pos_pkg,
    neg_expect_file,
    pos_expect_file,
    pionc_bin,
    fake_pionc,
    glass_pos_pkg,
    glass_neg_pkg,
    glass_pos_expect_file,
    glass_neg_expect_file,
    glass_wb_pkg,
) = sys.argv[1:14]
invar_cli = os.path.join(root_dir, "scripts", "invar")
REACTIVE_PREFIXES = ("INVAR_E007", "INVAR_E008", "INVAR_E009", "INVAR_E010")

with open(neg_expect_file, "r", encoding="utf-8") as f:
    neg_cases = [json.loads(line) for line in f if line.strip()]
with open(pos_expect_file, "r", encoding="utf-8") as f:
    pos_cases = [json.loads(line) for line in f if line.strip()]
with open(glass_pos_expect_file, "r", encoding="utf-8") as f:
    glass_pos_cases = [json.loads(line) for line in f if line.strip()]
with open(glass_neg_expect_file, "r", encoding="utf-8") as f:
    glass_neg_cases = [json.loads(line) for line in f if line.strip()]

def run_pionc_check(label, pkg_dir):
    proc = subprocess.run([pionc_bin, "check", "--json", pkg_dir], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert proc.returncode == 0, f"{label}: pionc check failed (rc={proc.returncode}):\n{proc.stdout}\n{proc.stderr}"
    data = json.loads(proc.stdout)
    assert data.get("ok") is True and not data.get("diagnostics"), f"{label}: pionc check reported diagnostics: {data}"

def make_query_capture_wrapper(tag):
    capture_file = os.path.join(tmp_dir, f"{tag}_query_analyze.json")
    wrap_bin = os.path.join(tmp_dir, f"{tag}_pionc_wrap.py")
    with open(wrap_bin, "w", encoding="utf-8") as wf:
        wf.write(
            "#!/usr/bin/env python3\n"
            "import os, subprocess, sys\n"
            f"pionc = {pionc_bin!r}\n"
            "if sys.argv[1:3] == ['query', 'analyze']:\n"
            "    p = subprocess.run([pionc, *sys.argv[1:]], stdout=subprocess.PIPE)\n"
            f"    with open({capture_file!r}, 'wb') as f:\n"
            "        f.write(p.stdout)\n"
            "    sys.stdout.buffer.write(p.stdout)\n"
            "    sys.stdout.buffer.flush()\n"
            "    sys.exit(p.returncode)\n"
            "os.execv(pionc, [pionc, *sys.argv[1:]])\n"
        )
    os.chmod(wrap_bin, os.stat(wrap_bin).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return capture_file, wrap_bin

def run_audit(pkg_dir, extra_args, pionc_override=None):
    env = os.environ.copy()
    env["PIONC"] = pionc_override or pionc_bin
    cmd = [sys.executable, invar_cli, "audit", "--json", *extra_args, pkg_dir]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        data = json.loads(proc.stdout)
    except Exception as e:
        raise AssertionError(f"invalid JSON ({e}):\n{proc.stdout}\n{proc.stderr}")
    return proc.returncode, data

def check_neg_all():
    capture_file, wrap_bin = make_query_capture_wrapper("neg")
    rc, data = run_audit(neg_pkg, [], pionc_override=wrap_bin)
    diags = data.get("diagnostics", [])
    codes = [d["code"] for d in diags if d.get("severity") == "error"]
    ra = data.get("reactive_audit", {})
    assert rc == 0, f"neg_all_clean: expected rc=0, got {rc}: {diags}"
    assert data.get("ok") is True, f"neg_all_clean: expected ok=True"
    assert data.get("error_count") == 0, f"neg_all_clean: expected 0 errors, got {codes}: {diags}"
    assert ra.get("ran") is True, f"neg_all_clean: expected reactive_audit.ran=True, got {ra}"
    assert ra.get("closures_checked", 0) >= 10, f"neg_all_clean: expected >=10 closures_checked, got {ra}"

    assert os.path.isfile(capture_file), "neg_all_clean: invar audit did not invoke pionc query analyze"
    with open(capture_file, "r", encoding="utf-8") as cf:
        qraw = cf.read()
    assert qraw.strip(), "neg_all_clean: captured pionc query analyze stdout was empty"
    qdata = json.loads(qraw)
    closures = qdata.get("closures", [])
    assert isinstance(closures, list) and len(closures) > 0, (
        f"neg_all_clean: captured query analyze returned no closures: {qdata}"
    )
    per_file_counts = {}
    for case in neg_cases:
        label = case["label"]
        suffix = f"src/{label}.pion"
        cnt = sum(1 for c in closures if str(c.get("file", "")).replace("\\", "/").endswith(suffix))
        assert cnt >= 1, f"neg_all_clean: {suffix} had 0 analyzed closures (silently skipped?)"
        per_file_counts[label] = cnt
    return [f"[PASS] neg_all_clean ({len(neg_cases)} files): ok=True errors=0 closures_checked={ra.get('closures_checked')} per_file={per_file_counts}"]

def check_pos_all():
    rc, data = run_audit(pos_pkg, [])
    diags = data.get("diagnostics", [])
    err_diags = [d for d in diags if d.get("severity") == "error"]
    ra = data.get("reactive_audit", {})
    assert rc != 0, f"pos_all_errors: expected non-zero rc, got 0"
    assert data.get("ok") is False, f"pos_all_errors: expected ok=False"
    assert ra.get("ran") is True, f"pos_all_errors: expected reactive_audit.ran=True, got {ra}"
    lines = []
    matched_indices = set()
    for case in pos_cases:
        label = case["label"]
        expected = case["expected"]
        if ":" in expected:
            exp_code, exp_substr = expected.split(":", 1)
        else:
            exp_code, exp_substr = expected, ""
        suffix = f"src/{label}.pion"
        file_diags = [
            (idx, d) for idx, d in enumerate(err_diags)
            if str(d.get("file", "")).replace("\\", "/").endswith(suffix)
        ]
        matching = [
            (idx, d) for idx, d in file_diags
            if d.get("code") == exp_code and (not exp_substr or exp_substr in d.get("message", ""))
        ]
        assert len(matching) >= 1, (
            f"{label}: expected {exp_code} ({exp_substr!r}) in {suffix}, got {[d for _, d in file_diags]}"
        )
        for idx, _ in file_diags:
            matched_indices.add(idx)
        lines.append(f"[PASS] {label}: code={exp_code}")
    assert len(err_diags) == len(pos_cases), (
        f"pos_all_errors: expected exactly {len(pos_cases)} error diagnostics, got {len(err_diags)}: {err_diags}"
    )
    assert len(matched_indices) == len(err_diags), (
        f"pos_all_errors: unmatched error diagnostics: {[d for i, d in enumerate(err_diags) if i not in matched_indices]}"
    )
    return lines

def check_glass_pos_all():
    run_pionc_check("glass_pos_all", glass_pos_pkg)
    rc, data = run_audit(glass_pos_pkg, ["--require-reactive"])
    err_diags = [
        d for d in data.get("diagnostics", [])
        if d.get("severity") == "error" and d.get("code", "").startswith(REACTIVE_PREFIXES)
    ]
    ra = data.get("reactive_audit", {})
    assert rc != 0, "glass_pos_all: expected non-zero rc, got 0"
    assert ra.get("ran") is True, f"glass_pos_all: expected reactive_audit.ran=True, got {ra}"
    lines = []
    matched_indices = set()
    for case in glass_pos_cases:
        label = case["label"]
        exp_code, _, exp_substr = case["expected"].partition(":")
        suffix = f"src/{label}.pion"
        file_diags = [
            (idx, d) for idx, d in enumerate(err_diags)
            if str(d.get("file", "")).replace("\\", "/").endswith(suffix)
        ]
        matching = [
            (idx, d) for idx, d in file_diags
            if d.get("code") == exp_code and (not exp_substr or exp_substr in d.get("message", ""))
        ]
        assert len(matching) == 1, (
            f"{label}: expected 1 {exp_code} ({exp_substr!r}) in {suffix}, got {[d for _, d in file_diags]}"
        )
        for idx, _ in file_diags:
            matched_indices.add(idx)
        lines.append(f"[PASS] {label}: code={exp_code}")
    assert len(err_diags) == len(glass_pos_cases), (
        f"glass_pos_all: expected exactly {len(glass_pos_cases)} reactive errors, got {len(err_diags)}: {err_diags}"
    )
    assert len(matched_indices) == len(err_diags), (
        f"glass_pos_all: unmatched reactive errors: {[d for i, d in enumerate(err_diags) if i not in matched_indices]}"
    )
    return lines

def check_glass_neg_all():
    run_pionc_check("glass_neg_all", glass_neg_pkg)
    capture_file, wrap_bin = make_query_capture_wrapper("glass_neg")
    rc, data = run_audit(glass_neg_pkg, ["--require-reactive"], pionc_override=wrap_bin)
    err_diags = [
        d for d in data.get("diagnostics", [])
        if d.get("severity") == "error" and d.get("code", "").startswith(REACTIVE_PREFIXES)
    ]
    ra = data.get("reactive_audit", {})
    assert rc == 0 and data.get("ok") is True and len(err_diags) == 0, (
        f"glass_neg_all: expected clean, got rc={rc} err_diags={err_diags}"
    )
    assert ra.get("ran") is True, f"glass_neg_all: expected reactive_audit.ran=True, got {ra}"
    assert os.path.isfile(capture_file), "glass_neg_all: invar audit did not invoke pionc query analyze"
    with open(capture_file, "r", encoding="utf-8") as cf:
        qdata = json.loads(cf.read())
    closures = qdata.get("closures", [])
    per_file_counts = {}
    for case in glass_neg_cases:
        label = case["label"]
        suffix = f"src/{label}.pion"
        cnt = sum(1 for c in closures if str(c.get("file", "")).replace("\\", "/").endswith(suffix))
        assert cnt >= 1, f"glass_neg_all: {suffix} had 0 analyzed closures (silently skipped?)"
        per_file_counts[label] = cnt
    return [f"[PASS] glass_neg_all ({len(glass_neg_cases)} files): ok=True reactive_errors=0 closures_checked={ra.get('closures_checked')} per_file={per_file_counts}"]

def check_glass_whitebox():
    run_pionc_check("glass_whitebox_all", glass_wb_pkg)
    capture_file, wrap_bin = make_query_capture_wrapper("glass_wb")
    rc, data = run_audit(glass_wb_pkg, ["--require-reactive"], pionc_override=wrap_bin)
    err_diags = [
        d for d in data.get("diagnostics", [])
        if d.get("severity") == "error" and d.get("code", "").startswith(REACTIVE_PREFIXES)
    ]
    ra = data.get("reactive_audit", {})
    assert rc != 0, "glass_whitebox_all: expected non-zero rc, got 0"
    assert ra.get("ran") is True, f"glass_whitebox_all: expected reactive_audit.ran=True, got {ra}"
    assert os.path.isfile(capture_file), "glass_whitebox_all: invar audit did not invoke pionc query analyze"
    with open(capture_file, "r", encoding="utf-8") as cf:
        qdata = json.loads(cf.read())
    closures = qdata.get("closures", [])
    neg_suffix = "src/zz_probe_neg_dragging_sig.pion"
    pos_suffix = "src/zz_probe_pos_field_lo.pion"
    neg_cnt = sum(1 for c in closures if str(c.get("file", "")).replace("\\", "/").endswith(neg_suffix))
    assert neg_cnt >= 1, f"glass_whitebox_all: {neg_suffix} had 0 analyzed closures"
    neg_diags = [d for d in err_diags if str(d.get("file", "")).replace("\\", "/").endswith(neg_suffix)]
    assert len(neg_diags) == 0, f"glass_whitebox_all: {neg_suffix} had unexpected reactive errors: {neg_diags}"
    pos_diags = [
        d for d in err_diags
        if str(d.get("file", "")).replace("\\", "/").endswith(pos_suffix)
        and d.get("code") == "INVAR_E008_REACTIVE_MUT_CAPTURE"
        and "glass::DampedDrag" in d.get("message", "")
    ]
    assert len(pos_diags) == 1, f"glass_whitebox_all: expected 1 E008 on {pos_suffix}, got {err_diags}"
    assert len(err_diags) == 1, f"glass_whitebox_all: expected exactly 1 total reactive error, got {err_diags}"
    return [
        f"[PASS] neg_e008_lm_dragging_sig: closures={neg_cnt} reactive_errors=0",
        f"[PASS] pos_e008_dd_field_lo: code=INVAR_E008_REACTIVE_MUT_CAPTURE total_reactive_errors=1",
    ]

def check_degraded(label, extra_args, mode, pionc_override=None):
    rc, data = run_audit(pos_pkg, extra_args, pionc_override=pionc_override)
    diags = data.get("diagnostics", [])
    codes = [d["code"] for d in diags if d.get("severity") == "error"]
    ra = data.get("reactive_audit", {})
    if mode == "degraded":
        assert rc == 0, f"{label}: expected rc=0, got {rc}: {diags}"
        assert data.get("ok") is True, f"{label}: expected ok=True"
        assert data.get("error_count") == 0, f"{label}: expected 0 errors, got {codes}"
        assert ra.get("ran") is False, f"{label}: expected reactive_audit.ran=False, got {ra}"
        assert isinstance(ra.get("reason"), str) and len(ra["reason"]) > 0, f"{label}: missing reason in {ra}"
    else:
        assert rc != 0, f"{label}: expected non-zero rc"
        assert data.get("ok") is False, f"{label}: expected ok=False"
        assert ra.get("ran") is False, f"{label}: expected reactive_audit.ran=False, got {ra}"
        assert mode in codes, f"{label}: expected {mode}, got {codes}"
    return [f"[PASS] {label}: ok={data.get('ok')} errors={data.get('error_count')} codes={codes} reactive={ra}"]

tasks = [
    ("pos_all", check_pos_all),
    ("neg_all", check_neg_all),
    ("glass_pos_all", check_glass_pos_all),
    ("glass_neg_all", check_glass_neg_all),
    ("glass_whitebox_all", check_glass_whitebox),
    ("degrade_no_reactive_flag", lambda: check_degraded("degrade_no_reactive_flag", ["--no-reactive"], "degraded")),
    ("degrade_legacy_pionc", lambda: check_degraded("degrade_legacy_pionc", [], "degraded", pionc_override=fake_pionc)),
    ("require_reactive_no_reactive", lambda: check_degraded("require_reactive_no_reactive", ["--no-reactive", "--require-reactive"], "INVAR_E007_REACTIVE_AUDIT_SKIPPED")),
    ("require_reactive_legacy_pionc", lambda: check_degraded("require_reactive_legacy_pionc", ["--require-reactive"], "INVAR_E007_REACTIVE_AUDIT_SKIPPED", pionc_override=fake_pionc)),
]

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
    futs = [(name, ex.submit(fn)) for name, fn in tasks]
    failed = 0
    for name, fut in futs:
        try:
            for line in fut.result():
                print(line)
        except Exception as e:
            print(f"[FAIL] {name}: {e}", file=sys.stderr)
            failed += 1
    if failed:
        sys.exit(1)
PY

echo "=== ALL REACTIVE AUDIT TESTS PASSED ==="
