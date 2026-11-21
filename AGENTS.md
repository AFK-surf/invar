# Working on `AFK-surf/invar` (for agents)

Invar is a pure-Pion, fine-grained-Signals + retained-mode pure-node-tree cross-platform GPU UI framework (**zero MoonBit runtime, zero DOM widgets**), targeting five native/web hosts:

- **macOS** — Apple Metal + CoreText (`packages/host/native/apple_*.m`, `mac_app.m`)
- **iOS** — Apple Metal + CoreText (`packages/host/native/apple_*.m`, `ios_app.m`, `ios_share.m`)
- **Linux** — EGL / GLES3 + GTK4 + Pango/Cairo (`packages/host/native/gles_render.h`, `linux_*.c`)
- **Windows** — Direct3D 11 (`packages/host/native/win_render.cpp`, `win_*.c*`)
- **Web** — `wasm32` + WebGL2 (`packages/host_web`, `web/host.js`, `web/pion_host_bridge.mjs`)

---

## 1. Non-Negotiable Rules (铁律红线)

### 1.1 Never hand-roll `Glass::liquid` in downstream app code
- Downstream application and business packages **MUST NEVER** call `paint::Glass::liquid(...)` directly.
- 100% of Liquid Glass UI must be built from `packages/lg` components. Before building any glass surface, control, toolbar, panel, menu, or overlay, look up the right component in [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md) and read [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md).
- Downstream repos must enforce two automated gates:
  1. Static grep gate: `grep -rn "Glass::liquid" src/` must return **0 hits** outside `invar/packages/lg`.
  2. Runtime/layout test gate: every UI test suite must run `lg::assert_no_nested_glass(t, label="...")` (exported from `packages/lg/src/audit.pion`).

### 1.2 Upstream missing primitives to `AFK-surf/invar` first
- If `packages/lg` lacks a desktop size, variant, or reusable glass primitive, **add it to `AFK-surf/invar` (`packages/lg`) first** with full unit tests and offscreen verification. Never degrade or hand-roll a one-off glass surface in downstream business code.

### 1.3 Keep all 4 GPU renderers in lockstep
- Any shader, optical transfer model, or instance-layout change in the GPU renderer **MUST** be updated simultaneously across all 4 backends:
  - `packages/host/native/apple_render.m` (macOS / iOS Metal)
  - `packages/host/native/gles_render.h` (Linux EGL / GLES3)
  - `packages/host/native/win_render.cpp` (Windows D3D11)
  - `web/host.js` (Web WebGL2)

### 1.4 Offscreen GPU evidence for every UI change
- Any visible UI, optical, or motion change must be verified with same-size, same-data **before/after** offscreen renders via Invar's native Metal (`film::picture` / `film::film` on macOS) or Linux GLES3 offscreen pipeline.
- Never use browser or simulator screenshots/screen-recordings as UI verification evidence.

### 1.5 Clean branches & merge discipline
- Never push directly to `main`; commit on feature branches and verify all gates before merge.

---

## 2. Quick Checks, Host Verification & Benchmarks

Set `PIONC=/path/to/pionc2` (with sibling `qbe` and `libpionrt.a`, or `PION_QBE` / `PION_RT_LIB` exported).

```sh
# Type-check key packages
$PIONC check packages/host
$PIONC check packages/lg
$PIONC check packages/lg_pages
$PIONC check packages/lg_shots
$PIONC check packages/lg_web

# Unit tests across core layers & Liquid Glass
$PIONC test packages/layer0_check
$PIONC test packages/layer1a_check
$PIONC test packages/layer1b_check
$PIONC test packages/view
$PIONC test packages/damage
$PIONC test packages/headless
$PIONC test packages/ui
$PIONC test packages/lg
$PIONC test packages/bench
$PIONC test packages/site

# Web wasm32 release build
$PIONC build --release --target wasm32 packages/lg_web

# Native host compilation & symbol gates
bash scripts/check-ios-host.sh                     # macOS: compiles all 12 iOS Metal host sources -> libinvar_host_ios.a
PIONC=$PIONC bash scripts/check-linux-host.sh      # Linux: compiles all 10 EGL/GLES3+GTK4 host sources + llvmpipe host test

# Unified Layer-1 CPU + Layer-2 GPU + wasm32 benchmark suite
PIONC=$PIONC bash scripts/bench.sh [--out bench.json] [--compare baseline.json] [--cpu-only] [--filter <substr>]
```

---

## 3. Key References

- [`README.md`](README.md) — 36-package architecture overview and 5-platform host matrix.
- [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md) — The 5 optical and architectural laws of Invar Liquid Glass.
- [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md) — One-page quick-reference catalog of all `lg::*` components and audit gates.
