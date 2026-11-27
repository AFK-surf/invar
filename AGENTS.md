# Working on `AFK-surf/invar` (for agents)

Invar is a pure-Pion, fine-grained-Signals + retained-mode pure-node-tree cross-platform GPU UI framework (**zero MoonBit runtime, zero DOM widgets**), targeting six native/web hosts (47 packages across core layers, `editor`, `nav`, `code_field`, `terminal`, `player`, `media`, `figure`, `webpage`, `updater`, `textdiff`, `runfs`, `lg`, and 6 automated UI craft audit gates + `InputTape` replay in `ui`):

- **macOS** — Apple Metal + CoreText (`packages/host/native/apple_*.m`, `mac_app.m`)
- **iOS** — Apple Metal + CoreText (`packages/host/native/apple_*.m`, `ios_app.m`, `ios_share.m`)
- **Android** — EGL / GLES3 + NDK / JNI + FramePacer (`packages/host/native/gles_render.h`, `android_host.c`, `packages/host/native/android/`)
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

### 1.4 Scrolling, Apple Scroll-Under Page Shell & Flex Container Rules
- **Default to `<lg::PageShell>` / `lg::page_shell_node` (and `<lg::SplitShell>` for two-column apps)** instead of a rigid `[header] / [scroll_box] / [footer]` three-box stack. `<lg::PageShell>` mounts a single **100%-height `ui::scroll_box`** with automatic top/bottom content insets so content glides directly underneath floating Liquid Glass top/bottom chrome — delivering genuine optical refraction with zero solid header backdrop and zero full-width hairline divider.
- **No bare text over content, no scroll-edge blur.** Bars made only of self-contained glass islands (`<lg::BackButton>`, channel pills, `<lg::BarPillGroup>`, `<lg::ComposerBar>`, `<lg::BottomTabs>`) get no blur veil, so crisp content refracts straight through the islands. Only bare `title` / `subtitle` text over the content turns on a compact full-height `smoothstep` fade (`liquid_scroll_edge_node`, `UIScrollEdgeEffect.soft`, `header_height + 22` = 66pt by default). Force it with `top_edge_blur=true/false`; a bottom bar carrying bare text opts in with `bottom_edge_blur=true` (`bottom_height + 8` = 64pt).
- Golden Apple scroll-under page skeleton (copy-paste ready):

```pion
pub fn build_page(root : view::View, field : ui::Field, on_send : () -> Unit) -> Unit {
  <lg::PageShell
    in={root}
    title="Messages"
    trailing={fn(tr : view::View) -> Unit {
      <lg::BarPillGroup in={tr} label="Actions">
        <lg::Button flat ghost height={30.0} pad_x={7.0} label="Search" on_press=>{ () }>
          <lg::Icon kind={.Search} size={15.0} />
        </lg::Button>
        <lg::Button flat ghost height={30.0} pad_x={7.0} label="Info" on_press=>{ () }>
          <lg::Icon kind={.Info} size={15.0} />
        </lg::Button>
      </lg::BarPillGroup>
    }}
    bottom={fn(bot : view::View) -> Unit {
      <lg::ComposerBar in={bot} field={field} on_send={on_send} placeholder="Message" />
    }}
  >
    <ui::Col pad={16.0} gap={12.0}>
      <lg::SurfaceCard pad={16.0} gap={8.0}>
        <ui::Text text="Clean solid Apple Content Layer — glides under floating glass chrome." size={14.0} wrap />
      </lg::SurfaceCard>
    </ui::Col>
  </lg::PageShell>
}
```

- **Low-level flex scrolling rule**: when building custom scroll containers with `ui::scroll_box`, `ui::scroll_row`, or `kit::scroll_area`, **every flex ancestor from `root` down to the scroller must bound its main-axis size** via `.fill()` (i.e. `.grow(Is(1.0)).shrink(Is(1.0)).basis(Is(Some(0.0))).min_width(Is(Some(0.0))).min_height(Is(Some(0.0)))`) or an explicit `.height(Is(Some(h)))` / `.width(Is(Some(w)))`.
- **Four anti-patterns to avoid**:
  1. **Three-box opaque stack (`[solid header] / [scroll_box] / [solid footer]`)**: cuts off scrolled content before it can pass underneath floating glass chrome, destroying Liquid Glass refraction and leaving an outdated solid hairline divider. Use `<lg::PageShell>`.
  2. **Unbounded flex ancestor**: wrapping a scroller in `parent.box(...)` without `.fill()` or `.min_height(Is(Some(0.0)))` lets the ancestor expand to full content height, leaving `reach == 0` (silent non-scrolling).
  3. **Calling `.scrolls(...)` on a container that already has sibling children**: mutating `parent` into a scroller after adding headers/footers breaks layout and clipping; always wrap the scrollable region in its own child box (`ui::scroll_box` / `View::scroll_box`).
  4. **Using `clips(true)` alone or raw `View::scrolls` without `bar` / `slop`**: clipping does not scroll, and raw `View::scrolls` without `bar=ui::tree_scroll_bar(), slop=ui::touch_slop()` drops scrollbars and touch-slop ergonomics; prefer `<lg::PageShell>` or `ui::scroll_box`.
- **Mandatory test gate**: every UI test suite must run `lg::assert_apple_ui(t, label="...")` (which checks zero nested glass, content-layer glass limits, `t.scroll_faults().length() == 0`, and `t.assert_ui_craft()`).

### 1.5 Keep all 4 GPU renderers in lockstep
- Any shader, optical transfer model (including 15-tap trimmed-mean temporal adaptive liquid glass and 9-tap small-sigma progressive blur), or instance-layout change in the GPU renderer **MUST** be updated simultaneously across all 4 backends:
  - `packages/host/native/apple_render.m` (macOS / iOS Metal)
  - `packages/host/native/gles_render.h` (Linux / Android EGL / GLES3)
  - `packages/host/native/win_render.cpp` (Windows D3D11)
  - `web/host.js` (Web WebGL2)

### 1.6 Offscreen GPU evidence for every UI change
- Any visible UI, optical, or motion change must be verified with same-size, same-data **before/after** offscreen renders via Invar's native Metal (`film::picture` / `film::film` on macOS) or Linux GLES3 offscreen pipeline.
- Never use browser or simulator screenshots/screen-recordings as UI verification evidence.

### 1.7 Clean branches & merge discipline
- Never push directly to `main`; commit on feature branches and verify all gates before merge.

### 1.8 Desktop apps: what a window needs (learned shipping one on Windows, 2026-11-22)
- **Hooks survive startup.** `ui::on_before_frame` hooks (and `worker.bind`) registered before the first `ui::run` / `Tester::tree` call are preserved; hooks are cleared when replacing an already-running app (`set_view_app` with a live tree) or on `Tester::dispose()` / `view_forget()`.
- **The tree's clock is the host's.** `ui::run`'s tree starts at `hostapi::now()` (on Windows, ms since boot). Schedule with `View::at_moment(v.cause_at() + ms, ...)`; never with a moment of your own counted from 0. A device or service to watch is a `ui::Worker` (or an `EventQueue` fed by a thread that blocks on it) that emits on change, not a timer polling it.
- **Windows draws only when asked.** `ui::run` asks for the frames; any other loop must turn `tree.on_due` into `hostapi::request_frame`.
- **Shaders.** The startup shaders are baked into the program (`win_shaders_baked.h`, `packages/host/tools/bake_win_shaders.c`: rebake when `win_render.cpp`'s shaders change). Others are compiled at first use and kept on disk only where the program is an app (`MBUI_APP_ID`) or says where (`MBUI_SHADERS`): set them in an `MBUI_ENV` RCDATA resource (`NAME=VALUE` lines) in the program's `.rc`, or each run compiles them again.
- **No jump, no system title bar.** `hostapi::window_setup(min_w, min_h, w, h, true)` gives the page the whole window (give its top row `.moves_window(...)`, and minimize/close via `hostapi::window_act`); `hostapi::window_hold()` before `ui::run` keeps it out of sight until `hostapi::window_show()`, so it is placed before it is seen.
- **Measure starts with marks, not guesses.** Time stamps at each stage (process start → main → tree built → window → renderer → first frame → shown) find where a start goes; a profiler that suspends the window's thread from inside the process can deadlock it, sample from another process.

### 1.9 Generated Packages & Overlay Contract (`scripts/check-generated.sh`)
- Files starting with `// GENERATED BY mbt2pion` are machine-generated and **must never be hand-edited**; all newly generated files must declare valid 40-character lowercase hex `mbt2pion_commit` and `source_commit` SHAs (no `dev` or `working-tree`).
- Inside any package containing generated files ("generated package"), new hand-written Pion files are restricted to:
  - `*_test.pion` (unit / conformance / regression tests),
  - `zz_invar_*.pion` or `invar_ext.pion` (Invar-specific extensions/overlays), and
  - `00_init_order.pion` (topological module init order).
- Keep pure-Pion Layer-0/1 primitives (`packages/redacted`, `packages/causal`, `packages/machine`, `packages/signals`, `packages/headless_core`) in small standalone micro-packages so they remain isolated from generated-package refreshes and maximize `pionc` CAS cache hits.
- Audit generated provenance and overlay boundaries with `bash scripts/check-generated.sh [--strict] [--report-only] [--require-single-commit] [--json]`. Default ratchet mode tracks existing legacy items (`packages/headless` pending next publish and `packages/hostapi/src/json.pion`) so counts may only shrink; use `--strict` for zero-exemption enforcement.

---

## 2. Quick Checks, Unified `invar` CLI, Host Verification & Benchmarks

Install or run the unified CLI (`scripts/install.sh` puts `invar` on `PATH`, or invoke `python3 scripts/invar` directly). Set `PIONC=/path/to/pionc2` (with sibling `qbe` and `libpionrt.a`, or `PION_QBE` / `PION_RT_LIB` exported).

```sh
# Unified invar CLI (JSON output for agent automation)
bash scripts/install.sh                            # install `invar` CLI shim
invar doctor --json                                # verify pionc2, qbe, libpionrt.a, host deps
invar audit --json                                 # static gates (zero Glass::liquid outside packages/lg, etc.)
invar check --json                                 # type-check workspace in one shot (pass --fmt, --pkg, -j/--jobs)
invar test --json                                  # run deduplicated workspace unit tests in parallel (--pkg defaults to --only-pkg; pass --with-deps or -j/--jobs)
invar new my_app --template jsx-app                # scaffold a new Pion-JSX + ui::Worker app
invar web build|dev|check                          # build/serve/verify wasm32 showcase & JS bridge
invar shot / invar diff / invar compare            # deterministic offscreen PNG render, pixel diff & ROI side-by-side compare
invar host-check                                   # native iOS Metal / Linux EGL+GLES3 host gates
invar bench                                        # unified CPU + GPU + wasm32 benchmark suite

# Direct pionc2 commands (single-package fast loop)
$PIONC check examples/jsx_app
$PIONC test examples/jsx_app
$PIONC build --release --target wasm32 packages/lg_web

# Native host compilation & symbol gates
bash scripts/check-ios-host.sh                     # macOS: compiles all 13 iOS Metal host sources -> libinvar_host_ios.a
bash scripts/check-android-host.sh                 # macOS: NDK 27.2 JNI/EGL/GLES3 host + FramePacerTest + hygiene gate
PIONC=$PIONC bash scripts/check-linux-host.sh      # Linux: compiles all 11 EGL/GLES3+GTK4 host sources + llvmpipe host test

# Unified Layer-1 CPU + Layer-2 GPU + wasm32 benchmark suite
PIONC=$PIONC bash scripts/bench.sh [--out bench.json] [--compare baseline.json] [--cpu-only] [--filter <substr>]
```

> **Offscreen rendering (`packages/film`)**: `film::picture` and `film::film` support both **macOS Apple Metal** and **Linux EGL/GLES3** (including headless Mesa `llvmpipe`).

---

## 3. Key References

- [`README.md`](README.md) — 47-package architecture overview, unified `invar` CLI, and 6-platform host matrix.
- [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md) — The 5 optical and architectural laws of Invar Liquid Glass.
- [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md) — One-page quick-reference catalog of all `lg::*` components and audit gates.
- [`examples/jsx_app/`](examples/jsx_app/) — Recommended Pion-JSX + Leading-Dot `.Enum` + `ui::Worker` + `packages/lg` showcase app.
- [`examples/worker_app/`](examples/worker_app/) — Classic builder-API dual-thread Invar app using `ui::Worker` and `packages/lg`.
