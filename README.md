# Invar

Invar 是一个纯 Pion 编写的「细粒度 Signals + 保留模式纯节点树」跨端 GPU UI 框架：**零 MoonBit 运行时依赖、零 DOM 控件**，直接驱动五端原生/Web GPU 宿主渲染与排版。

Invar is a pure-Pion, fine-grained-Signals + retained-mode pure-node-tree cross-platform GPU UI framework (**zero MoonBit runtime, zero DOM widgets**), targeting five native and web GPU hosts.

## For AI Agents

If you are an AI coding agent (Claude Code, Codex, Cursor, etc.) working with Invar:

1. **Read [`AGENTS.md`](AGENTS.md) first** before writing or modifying any code.
2. **Never run blocking IO on the render thread**: isolate all network, filesystem, process, and heavy compute work on a background thread using `ui::Worker::pair()` + `ui::WorkerCtx::run_loop()` + `worker.bind()` + `ui::wake_now()` + `#render_thread` (use `ui::Worker::inline()` for `wasm32` and `ui::Tester` tests; see [`examples/worker_app/`](examples/worker_app/)).
3. **Build all Liquid Glass UI with `packages/lg` (never hand-roll `paint::Glass::liquid`)**: pick components from [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md), follow [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md), and verify with `lg::assert_no_nested_glass` and native offscreen rendering (`film::picture` / `film::film`).
4. **Scrolling & Flex Layout**: default to `ui::scroll_box(parent, fn(body) { ... })` / `ui::scroll_row(...)` (or `kit::scroll_area`) to create a dedicated scrollable child container with `.fill()`, scrollbar (`ui::tree_scroll_bar()`), and drag-claim takeover wired automatically. Every flex ancestor from `root` down to a scroller **must bound its height** (`.fill()`, `.grow(Is(1.0)).basis(Is(Some(0.0))).min_height(Is(Some(0.0)))`, or fixed `.height(Is(Some(h)))`); never call `.scrolls(...)` directly on a container that already has sibling children; always assert `t.scroll_faults().length() == 0` (or `t.assert_scrollable()`) in UI unit tests.

---

## 双线程渲染与 Worker 骨架 / Dual-Thread Worker Architecture

Invar 渲染线程专注于细粒度 `Signals`、纯节点树排版与 GPU 帧提交；所有阻塞 IO 与重计算通过 `ui::Worker::pair()` 放到独立 OS 线程，结果通过线程安全的 `ui::wake_now()` 唤醒主循环并在帧前（`ui::on_before_frame`）自动 drain 到 `Signals`。`wasm32` 与确定性 `ui::Tester` 单测可直接切换为 `ui::Worker::inline()`，100% 复用同一套视图与命令处理逻辑（完整范例见 [`examples/worker_app/`](examples/worker_app/)）：

```pion
let (worker, ctx) : (ui::Worker[Cmd, Ev], ui::WorkerCtx[Cmd, Ev]) = ui::Worker::pair()
scope(world, fn(s : Scope) -> Unit {
  spawn(s, fn() -> Unit {
    ctx.run_loop(fn(cmd, emit) { handle_cmd(world, cmd, emit) })
  })
  ui::run("Invar App", fn(root) { build_view(root, worker) })
  worker.close()
})
```

---

## 五端宿主现状 / 5-Platform Host Matrix

| 平台 | 渲染后端 | 文本与系统集成 | 核心源码 | 验证门禁 |
|---|---|---|---|---|
| **macOS** | Apple Metal | CoreText / AppKit / AVFoundation | `packages/host/native/apple_*.m`, `mac_app.m` | `pionc2 check packages/host` / `packages/lg_shots` |
| **iOS** | Apple Metal | CoreText / UIKit | `packages/host/native/apple_*.m`, `ios_app.m`, `ios_share.m` | `bash scripts/check-ios-host.sh` |
| **Linux** | EGL / GLES3 | GTK4 / Pango / Cairo / libsoup-3.0 | `packages/host/native/gles_render.h`, `linux_*.c` | `bash scripts/check-linux-host.sh` |
| **Windows** | Direct3D 11 | DirectWrite / Win32 | `packages/host/native/win_render.cpp`, `win_*.c*` | 四端着色器同步门禁 |
| **Web** | WebGL2 | `wasm32` + JS Host Bridge | `packages/host_web`, `web/host.js`, `web/pion_host_bridge.mjs` | `pionc2 build --release --target wasm32 packages/lg_web` |

---

## 36 个包架构总览 / 36-Package Layout

| 层级 | 包列表 | 数量 | 职责说明 |
|---|---|---:|---|
| **Layer 0（基础库）** | `halt`、`dmath`、`codec`、`zip`、`text`、`signals`、`layer0_check` | 7 | 中止原语、确定性数学、二进制编解码、inflate/SHA-256、Unicode（Bidi/断行/字素）、细粒度响应式 Signals 及联合检查 |
| **Layer 1A（布局与绘制基础）** | `flex`、`pixels`、`paint`、`damage`、`layer1a_check` | 5 | Flexbox 布局、图像解码（PNG/JPEG/WebP）、矢量/圆角/液态玻璃绘制指令、脏区与遮挡裁剪及联合检查 |
| **Layer 1B（图形与动效引擎）** | `shape`、`svg`、`font`、`skia`、`effect`、`motion`、`render`、`layer1b_check` | 8 | 文本塑形、SVG 解析、字体管理、2D 路径光栅化、后处理效果、闭式弹簧动效、实例合批渲染及联合检查 |
| **Layer 2（节点树与交互框架）** | `view`、`headless`、`ui` | 3 | 保留模式纯节点树（`View`）、无头交互与无障碍原语（`headless`）、主题/输入法/`ui::Worker`/离屏 `Tester` 门禁（`ui`） |
| **Layer 3（宿主桥接与出片）** | `hostapi`、`host`、`host_web`、`invar_host`、`film`、`kit` | 6 | 统一宿主 ABI、多端原生宿主、Web 宿主、兼容宿主包、确定性离屏出图/录片引擎（`film`）、基础 UI 套件（`kit`） |
| **Liquid Glass 组件与展台** | `lg`、`lg_pages`、`lg_shots`、`lg_web` | 4 | iOS 27 液态玻璃组件库（37+ 公开组件 + `audit_glass` 门禁）、展厅页面、Metal 离屏出图器、WebAssembly 展台 |
| **站点与性能基准** | `site`、`bench`、`bench_gpu` | 3 | 官方展示站点、Layer-1 CPU 基准套件、Layer-2 同步 GPU 帧时基准套件 |
| **合计** | `packages/*` | **36** | 全量纯 Pion 构建，零 MoonBit 依赖 |

---

## 快检与统一 CLI / Quick Checks & `invar` CLI

推荐使用统一的 `invar` CLI（运行 `bash scripts/install.sh` 安装到 `PATH`，或直接调用 `python3 scripts/invar`），并配置 `PIONC=/path/to/pionc2`（同目录含 `qbe` 与 `libpionrt.a`）：

```sh
# 1. 环境体检、静态审计、格式/类型检查与全量单测（支持 --json）
invar doctor --json
invar audit --json
invar check --json
invar test --json

# 2. 新建 Pion-JSX 应用与 WebAssembly 验证
invar new my_app --template jsx-app
invar web check --json          # 编译 lg_web.wasm + node --check + 导入表校验
invar web dev                   # 启动本地 Web 预览

# 3. 离屏出图/像素对比（packages/film 支持 macOS Metal 与 Linux EGL/GLES3 llvmpipe）与原生宿主门禁
invar shot
invar diff
invar host-check
```

单包开发快检也可以直接调用 `pionc2`：

```sh
pionc2 check examples/jsx_app
pionc2 test examples/jsx_app
pionc2 build --release --target wasm32 packages/lg_web
```

---

## Benchmark 用法 / Benchmarks

使用统一的 `scripts/bench.sh` 一键运行 Layer-1 CPU、Layer-2 GPU（Apple Metal / Linux GLES3）与 `lg_web.wasm` 体积基准，并支持 JSON 报告与 A/B 回归对比：

```sh
# 全量基准（CPU + GPU + wasm32）并导出基线 JSON
PIONC=/path/to/pionc2 bash scripts/bench.sh --out baseline.json

# 与基线做 A/B 回归对比（P50 变化超过 +10% 标为 REGRESSION）
PIONC=/path/to/pionc2 bash scripts/bench.sh --compare baseline.json

# 仅跑 CPU 基准或按名称过滤
PIONC=/path/to/pionc2 bash scripts/bench.sh --cpu-only --filter flex
```

---

## 文档导航 / Documentation

- [`AGENTS.md`](AGENTS.md) — 仓库架构红线、双线程 Worker 规范、统一 `invar` CLI 与快检命令
- [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md) — Liquid Glass 五大光学与架构铁律
- [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md) — `packages/lg` 一页纸组件速查目录与复制即用样例
- [`examples/jsx_app/`](examples/jsx_app/) — 推荐标杆：纯 Pion-JSX + Leading-Dot `.Enum` + `ui::Worker` + `packages/lg` 应用
- [`examples/worker_app/`](examples/worker_app/) — 标准双线程 Builder API Invar App 完整范例
