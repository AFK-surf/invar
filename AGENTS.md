# Working on `AFK-surf/invar` (for agents)

Invar is a pure-Pion, fine-grained-Signals + retained-mode pure-node-tree cross-platform GPU UI framework (**zero MoonBit runtime, zero DOM widgets**), targeting five native/web hosts:

- **macOS** — Apple Metal + CoreText (`packages/host/native/apple_*.m`, `mac_app.m`)
- **iOS** — Apple Metal + CoreText (`packages/host/native/apple_*.m`, `ios_app.m`, `ios_share.m`)
- **Linux** — EGL / GLES3 + GTK4 + Pango/Cairo (`packages/host/native/gles_render.h`, `linux_*.c`)
- **Windows** — Direct3D 11 (`packages/host/native/win_render.cpp`, `win_*.c*`)
- **Web** — `wasm32` + WebGL2 (`packages/host_web`, `web/host.js`, `web/pion_host_bridge.mjs`)

---

## 1. Non-Negotiable Rules

### 1.1 Zero blocking IO on the render thread (`ui::Worker` + `ui::wake_now()` + `#render_thread`)
- The render thread owns `view::Tree`, `signals::Signal`, layout, and frame presentation. **Never run blocking IO (TCP/HTTP, filesystem, subprocesses, sleep, heavy CPU compute) or pass `World` on the render thread.**
- Always isolate IO/compute in a background OS thread using `ui::Worker::pair()` + `ui::WorkerCtx::run_loop()` + `worker.bind()` + `ui::wake_now()` (and mark render-thread view code with `#render_thread`).
- For `wasm32` and deterministic `ui::Tester` unit tests, use `ui::Worker::inline(handle_cmd)` so view and command logic are 100% shared without OS threads.

Standard 30-line dual-thread template (see [`examples/worker_app/`](examples/worker_app/) for a complete runnable app):

```pion
import "host"
import "signals"
import "ui"
import "view"

pub enum Cmd { Fetch(String) }
pub enum Ev { Loaded(String) }

pub fn handle_cmd(world : World, cmd : Cmd, emit : (Ev) -> Unit) -> Unit {
  match cmd {
    Cmd::Fetch(url) => emit(Ev::Loaded("ok: " + url))
  }
}

pub fn build_view(root : view::View, worker : ui::Worker[Cmd, Ev]) -> Unit {
  let status = signals::signal("Idle")
  worker.bind(fn(ev : Ev) {
    match ev {
      Ev::Loaded(msg) => status.set(msg)
    }
  })
  let _ = root.box(fn(b : view::View) {
    let _ = b.words(view::Live::By(fn() { status.get() }), look=view::words_plain)
      .on_press(fn() { let _ = worker.send(Cmd::Fetch("/api/status")) })
  })
}

pub fn main(world : World) -> Int {
  host::install()
  let (worker, ctx) : (ui::Worker[Cmd, Ev], ui::WorkerCtx[Cmd, Ev]) = ui::Worker::pair()
  scope(world, fn(s : Scope) -> Unit {
    spawn(s, fn() -> Unit {
      ctx.run_loop(fn(cmd : Cmd, emit : (Ev) -> Unit) { handle_cmd(world, cmd, emit) })
    })
    ui::run("Invar Worker App", fn(root : view::View) { build_view(root, worker) })
    worker.close()
  })
  0
}
```

### 1.2 Never hand-roll `Glass::liquid` in downstream app code
- Downstream application and business packages **MUST NEVER** call `paint::Glass::liquid(...)` directly.
- 100% of Liquid Glass UI must be built from `packages/lg` components. Before building any glass surface, control, toolbar, panel, menu, or overlay, look up the right component in [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md) and read [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md).
- Downstream repos must enforce two automated gates:
  1. Static grep gate: `grep -rn "Glass::liquid" src/` must return **0 hits** outside `invar/packages/lg`.
  2. Runtime/layout test gate: every UI test suite must run `lg::assert_no_nested_glass(t, label="...")` (exported from `packages/lg/src/audit.pion`).

### 1.3 Upstream missing primitives to `AFK-surf/invar` first
- If `packages/lg` lacks a desktop size, variant, or reusable glass primitive, **add it to `AFK-surf/invar` (`packages/lg`) first** with full unit tests and offscreen verification. Never degrade or hand-roll a one-off glass surface in downstream business code.

### 1.4 Keep all 4 GPU renderers in lockstep
- Any shader, optical transfer model, or instance-layout change in the GPU renderer **MUST** be updated simultaneously across all 4 backends:
  - `packages/host/native/apple_render.m` (macOS / iOS Metal)
  - `packages/host/native/gles_render.h` (Linux EGL / GLES3)
  - `packages/host/native/win_render.cpp` (Windows D3D11)
  - `web/host.js` (Web WebGL2)

### 1.5 Offscreen GPU evidence for every UI change
- Any visible UI, optical, or motion change must be verified with same-size, same-data **before/after** offscreen renders via Invar's native Metal (`film::picture` / `film::film` on macOS) or Linux GLES3 offscreen pipeline.
- Never use browser or simulator screenshots/screen-recordings as UI verification evidence.

### 1.6 Clean branches & merge discipline
- Never push directly to `main`; commit on feature branches and verify all gates before merge.

### 1.7 Desktop apps: what a window needs (learned shipping one on Windows, 2026-11-22)
- **Hooks go inside the maker.** `ui::run` forgets every `ui::on_before_frame` hook (and `worker.bind`) made before it: register them inside the `fn(root)` passed to `ui::run`, as `examples/worker_app` does.
- **The tree's clock is the host's.** `ui::run`'s tree starts at `hostapi::now()` (on Windows, ms since boot). Schedule with `View::at_moment(v.cause_at() + ms, ...)`; never with a moment of your own counted from 0. A device or service to watch is a `ui::Worker` (or an `EventQueue` fed by a thread that blocks on it) that emits on change, not a timer polling it.
- **Windows draws only when asked.** `ui::run` asks for the frames; any other loop must turn `tree.on_due` into `hostapi::request_frame`.
- **Shaders.** The startup shaders are baked into the program (`win_shaders_baked.h`, `packages/host/tools/bake_win_shaders.c`: rebake when `win_render.cpp`'s shaders change). Others are compiled at first use and kept on disk only where the program is an app (`MBUI_APP_ID`) or says where (`MBUI_SHADERS`): set them in an `MBUI_ENV` RCDATA resource (`NAME=VALUE` lines) in the program's `.rc`, or each run compiles them again.
- **No jump, no system title bar.** `hostapi::window_setup(min_w, min_h, w, h, true)` gives the page the whole window (give its top row `.moves_window(...)`, and minimize/close via `hostapi::window_act`); `hostapi::window_hold()` before `ui::run` keeps it out of sight until `hostapi::window_show()`, so it is placed before it is seen.
- **Measure starts with marks, not guesses.** Time stamps at each stage (process start → main → tree built → window → renderer → first frame → shown) find where a start goes; a profiler that suspends the window's thread from inside the process can deadlock it, sample from another process.

---

## 2. Quick Checks, Host Verification & Benchmarks

Set `PIONC=/path/to/pionc2` (with sibling `qbe` and `libpionrt.a`, or `PION_QBE` / `PION_RT_LIB` exported).

```sh
# Type-check key packages
$PIONC check packages/hostapi
$PIONC check packages/host
$PIONC check packages/host_web
$PIONC check packages/ui
$PIONC check packages/lg
$PIONC check packages/lg_pages
$PIONC check packages/lg_shots
$PIONC check packages/lg_web
$PIONC check examples/worker_app

# Unit tests across core layers & Liquid Glass
$PIONC test packages/layer0_check
$PIONC test packages/layer1a_check
$PIONC test packages/layer1b_check
$PIONC test packages/view
$PIONC test packages/damage
$PIONC test packages/headless
$PIONC test packages/hostapi
$PIONC test packages/ui
$PIONC test packages/lg
$PIONC test packages/bench
$PIONC test packages/site
$PIONC test examples/worker_app

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
- [`examples/worker_app/`](examples/worker_app/) — Reference dual-thread Invar app using `ui::Worker` and `packages/lg`.
