# Invar

Invar 是一个纯 Pion 原生的跨端 GPU UI 框架：零 MoonBit 运行时依赖，直接复用原生 C/Objective-C 宿主（Apple 上为 Metal 渲染与 CoreText 排版）。

Invar is a pure-Pion, native, cross-platform GPU UI framework. It has no MoonBit runtime dependency and links directly against native C/Objective-C hosts (Metal and CoreText on Apple platforms).

## 包结构 / Layout

| 层 | 路径 | 说明 |
|---|---|---|
| 宿主 Host | `packages/invar_host` | Pion FFI 绑定 + 原生 Apple 宿主（`upstream/apple_*.m` 原样复用，`compat/` 为 C 适配层，其中 `compat/moonbit.h` 仅供上游 `.m` 编译链接） |
| 示例 Example | `examples/metal_glass_shot` | Metal 离屏首帧：玻璃面板、阴影与中英文本 |
| Layer 0 | `packages/halt`、`dmath`、`codec`、`zip`、`text`、`signals` | 基础库：中止、确定性数学、编解码、inflate/SHA-256、Unicode 文本（bidi/断行/字素）、响应式信号 |
| Layer 0 检查 | `packages/layer0_check` | Layer 0 联合编译与测试 |
| Layer 1A | `packages/flex`、`pixels`、`paint`、`damage` | Flex 布局、像素与图片解码（PNG/JPEG/WebP）、矢量绘制（圆角/线/玻璃）、脏区与遮挡 |
| Layer 1A 检查 | `packages/layer1a_check` | Layer 1A 联合编译与测试 |

## 快检 / Quick checks

```sh
pionc test packages/layer1a_check
pionc check examples/metal_glass_shot
pionc build examples/metal_glass_shot
```

`pionc build examples/metal_glass_shot` 需在 macOS 上运行（链接 Metal、CoreText 等系统框架）。
