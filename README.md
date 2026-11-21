# Invar

Invar 是一个纯 Pion 编写的「细粒度 Signals + 保留模式纯节点树」跨端 GPU UI 框架：**零 MoonBit 运行时依赖、零 DOM 控件**，直接驱动五端原生/Web GPU 宿主渲染与排版。

Invar is a pure-Pion, fine-grained-Signals + retained-mode pure-node-tree cross-platform GPU UI framework (**zero MoonBit runtime, zero DOM widgets**), targeting five native and web GPU hosts.

> 开发与 Agent 协作红线见 [`AGENTS.md`](AGENTS.md)；液态玻璃设计规范见 [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md)；组件速查目录见 [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md)。

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
| **Layer 2（节点树与交互框架）** | `view`、`headless`、`ui` | 3 | 保留模式纯节点树（`View`）、无头交互与无障碍原语（`headless`）、主题/输入法/离屏 `Tester` 门禁（`ui`） |
| **Layer 3（宿主桥接与出片）** | `hostapi`、`host`、`host_web`、`invar_host`、`film`、`kit` | 6 | 统一宿主 ABI、多端原生宿主、Web 宿主、兼容宿主包、确定性离屏出图/录片引擎（`film`）、基础 UI 套件（`kit`） |
| **Liquid Glass 组件与展台** | `lg`、`lg_pages`、`lg_shots`、`lg_web` | 4 | iOS 27 液态玻璃组件库（37+ 公开组件 + `audit_glass` 门禁）、展厅页面、Metal 离屏出图器、WebAssembly 展台 |
| **站点与性能基准** | `site`、`bench`、`bench_gpu` | 3 | 官方展示站点、Layer-1 CPU 基准套件、Layer-2 同步 GPU 帧时基准套件 |
| **合计** | `packages/*` | **36** | 全量纯 Pion 构建，零 MoonBit 依赖 |

---

## 快检与验证 / Quick Checks

需要已自举的 `pionc2` 工具链（可将 `PIONC=/path/to/pionc2`，同目录含 `qbe` 与 `libpionrt.a`）：

```sh
# 1. 核心纯 Pion 包单测
pionc2 test packages/layer0_check
pionc2 test packages/layer1a_check
pionc2 test packages/layer1b_check
pionc2 test packages/view
pionc2 test packages/damage
pionc2 test packages/headless
pionc2 test packages/ui
pionc2 test packages/lg
pionc2 test packages/bench
pionc2 test packages/site

# 2. 宿主与 Liquid Glass 包类型检查
pionc2 check packages/host
pionc2 check packages/lg
pionc2 check packages/lg_pages
pionc2 check packages/lg_shots
pionc2 check packages/lg_web

# 3. Web wasm32 产物构建
pionc2 build --release --target wasm32 packages/lg_web

# 4. 原生宿主编译与符号门禁
bash scripts/check-ios-host.sh                  # macOS：编译全部 12 个 iOS Metal 源文件
PIONC=pionc2 bash scripts/check-linux-host.sh   # Linux：编译全部 10 个 EGL/GLES3+GTK4 源文件并跑 llvmpipe 单测
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

- [`AGENTS.md`](AGENTS.md) — 仓库架构红线、防手搓规范与快检命令
- [`packages/lg/DESIGN.md`](packages/lg/DESIGN.md) — Liquid Glass 五大光学与架构铁律
- [`packages/lg/CATALOG.md`](packages/lg/CATALOG.md) — `packages/lg` 一页纸组件速查目录与复制即用样例
