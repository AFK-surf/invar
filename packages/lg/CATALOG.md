# `packages/lg` Component Catalog (一页纸速查目录)

> **铁律**：
> 1. **全高滚动穿透页面壳优先**：建任何页面直接用 `<lg::PageShell>` / `<lg::SplitShell>` + `<lg::BackButton>` + `<lg::BarPillGroup>` + `<lg::Icon>` + `<lg::ComposerBar>`；**严禁手写三段式实色死盒（`[实色顶栏] / [scroll_box] / [实色底栏]`）、严禁实色顶栏横贯分割线、严禁纯文字顶栏胶囊（如 `"Back"`）、严禁框套框输入栏**！
> 2. **内容层零玻璃**：页面底色、消息流、列表、内容卡片、表单与代码块**一律使用 Content Surfaces（实色纸面 `lg::surface()` / `SurfaceCard` / `SurfaceSection` / `CodeBlock`）**，严禁对页面内容套 `Glass::liquid`。
> 3. **悬浮层单层玻璃**：下游业务代码严禁直接调用 `paint::Glass::liquid(...)`，所有悬浮导航、工具栏、弹层与交互控件必须 100% 从本目录选用 `lg::*` 组件，并在单测中调用 `lg::assert_apple_ui(t, label="...")`。

---

## Standard Apple App Shells, Vector Icons & Composer (Start Here)

### 00a. `lg::page_shell_node` / `<lg::PageShell>`
- **适用场景**：全高（100%）滚动穿透式苹果页面壳：干净实色底面（`surface().canvas` / `grouped_bg`）+ 唯一全高 `ui::scroll_box`（自动加 `top_inset` / `bottom_inset`）+ 自动挂载 `liquid_scroll_edge_node` 渐进模糊卷边（`UIScrollEdgeEffect.soft`）+ 悬浮顶/底槽（`leading` / `title` / `subtitle` / `trailing` / `bottom`）。零实色顶栏、零横贯硬分割线，内容滚动直接穿过玻璃岛下方产生真实折射。

### 00b. `lg::split_shell_node` / `<lg::SplitShell>`
- **适用场景**：响应式双栏 `NavigationSplitView`（`breakpoint=760.0`、`sidebar_width=264.0`）：宽屏渲染固定宽实色 `surface().sidebar` + `0.5pt` `surface().separator` 竖细线 + `.fill()` 详情区；窄屏按 `show_detail()` 自动全屏切换侧栏或详情，绝不挤压截断。

### 00c. `lg::composer_bar_node` / `<lg::ComposerBar>`
- **适用场景**：苹果 Messages 级极简悬浮单层自适应液态玻璃输入胶囊（`min_height=44, r=22`，左侧扁平无框圆形 `+`/`Paperclip` 图标钮、中间透明无框 `ui::field`、右侧 `34pt` 正圆 `ArrowUp` 发送钮 + `36pt` 热区），零框套框、零矩形 `"Send"` 文字按钮。

### 00d. `lg::icon_node` / `<lg::Icon>` & `lg::liquid_back_button_node` / `<lg::BackButton>`
- **适用场景**：31 个 100% `paint::shape_of` 几何矢量图标（`ChevronLeft`、`Search`、`Plus`、`Paperclip`、`ArrowUp`、`Pin`、`Users`、`Info`、`Settings`、`Sparkles`、`Refresh` 等，1.6–1.8pt 线宽，零 emoji/字体依赖），以及单层悬浮自适应液态玻璃返回岛 `<lg::BackButton>`（`36×36` 正圆 `<` 岛或 `< Label` 胶囊岛，36pt 热区）。

---

## Content Surfaces (Non-Glass — Use for Page Content)

所有页面正文、卡片、分组设置、代码块与背景底面均从这里选用（自动跟随 `lg::is_dark()` 切换浅/深苹果色板，**零 `Glass::liquid`**）：

### 0a. `lg::surface()` & `lg::SurfacePalette` & 同心圆角阶梯
- **适用场景**：页面底色与内容层语义配色唯一权威来源（`canvas` / `grouped_bg` / `sidebar` / `card` / `card_secondary` / `ink` / `ink_secondary` / `ink_tertiary` / `separator` / `separator_strong` / `row_hover` / `row_selected` / `accent` / `accent_tint` / `positive` / `warning` / `danger` / `code_bg` / `code_header`）。
- **圆角与分割线常量**：`r_sheet = 34.0`、`r_panel = 26.0`、`r_card = 20.0`、`r_row = 12.0`、`r_chip = 8.0`、`hairline_w = 0.5`、`lg::concentric(outer, inset)`。
```pion
let _ = root.ground(view::Live::By(fn() { lg::surface().canvas }))
let inner_r = lg::concentric(lg::r_card, 8.0) // 12.0
```

### 0b. `lg::surface_card_node` / `<lg::SurfaceCard>`
- **适用场景**：干净实色苹果内容卡片（`surface().card` 或 `secondary=true` 时 `surface().card_secondary` + `0.5pt` `surface().separator` 细边，零玻璃、零厚重外阴影）。
- **签名**：`pub fn surface_card_node(parent : view::View, body : (view::View) -> Unit, radius~ : Double = r_card, pad~ : Double = 16.0, gap~ : Double = 0.0, secondary~ : Bool = false, label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
```pion
<lg::SurfaceCard pad={16.0} gap={12.0} label="Summary">
  <ui::Text size={15.0} weight={600} color_by=>{lg::surface().ink} text="Overview" />
</lg::SurfaceCard>
```

### 0c. `lg::surface_section_node` / `<lg::SurfaceSection>`
- **适用场景**：iOS 分组内容区块（可选顶部小标题 + 干净实色卡片 + 可选底部脚注）。
- **签名**：`pub fn surface_section_node(parent : view::View, body : (view::View) -> Unit, header~ : () -> String = fn() { "" }, footer~ : () -> String = fn() { "" }, radius~ : Double = r_card, pad~ : Double = 0.0, loc~ : String = "") -> view::View`
```pion
<lg::SurfaceSection header="GENERAL" footer="Changes apply immediately." pad={14.0}>
  <ui::Text size={14.0} color_by=>{lg::surface().ink} text="Automatic Updates" />
</lg::SurfaceSection>
```

### 0d. `lg::surface_code_block_node` / `<lg::CodeBlock>`
- **适用场景**：自带标题条（语言标签 + 扁平 Copy 按钮，36pt 热区）与 `ui::scroll_row` 横向滚区的实色代码块，宽屏与 320pt 窄屏均绝不右侧截断。
- **签名**：`pub fn surface_code_block_node(parent : view::View, code : () -> String, lang~ : () -> String = fn() { "" }, on_copy? : () -> Unit, max_height? : Double, radius~ : Double = r_row, loc~ : String = "") -> view::View`
```pion
<lg::CodeBlock lang="pion" code="lg::assert_apple_ui(t, label=\"screen\")" max_height={180.0} />
```

---

## 常见需求 → 对应 `lg::*` 组件（严禁手搓）

| 常见业务需求 | 必须使用的 `lg::*` 组件 | 禁止手搓的做法 |
|---|---|---|
| 页面底色 / 卡片 / 分组区块 / 代码块 | `lg::surface()` / `lg::SurfaceCard` / `lg::SurfaceSection` / `lg::CodeBlock` | 严禁给页面内容套 `Glass::liquid` 或硬编码杂色 |
| 分组列表 / 折叠面板（默认实色卡片） | `lg::liquid_list_node`（默认 `glass=false`）/ `lg::liquid_disclosure_node`（默认 `glass=false`） | 严禁内容列表默认挂玻璃或手拼非同心圆角 |
| 内容面板 / 悬浮检查器 / 卡片 / 控制中心磁贴 | `lg::liquid_panel_node` / `lg::liquid_card_node`（默认实色；悬浮检查器/磁贴显式 `floating=true`）/ `lg::liquid_control_tile_node` | 严禁直接套 `Glass::liquid` 面板 |
| 浮动主按钮 / 次按钮 / 玻璃容器内扁平按钮 | `lg::liquid_button_node`（容器内传 `flat=true` / `ghost=true`，主按钮 `prominent=true`） | 严禁 `v.glass(Glass::liquid(...))` 拼按钮 |
| 图标按钮（圆/胶囊） | `lg::liquid_icon_button_node` | 严禁手动圆框 + `Glass::liquid` |
| 多图标合并胶囊操作组 | `lg::liquid_button_group_node` + `lg::LiquidGroupAction` | 严禁横排多个玻璃图标按钮硬贴 |
| 浮动工具栏 + 分割线 | `lg::liquid_toolbar_node` + `lg::liquid_toolbar_divider` + `flat=true, ghost=true` 按钮 | 严禁在工具栏内再放 `flat=false` 玻璃按钮 |
| 顶栏悬浮药丸操作组 | `lg::liquid_bar_pill_group_node`（紧凑 `lg_bar_pill_shadow`，内置 `flat=true, ghost=true` 按钮） | 严禁手写顶栏胶囊或套用大阴影弹层 |
| 顶部导航栏 | `lg::liquid_nav_bar_node`（默认 `glass=false`，左右槽放浮动玻璃岛） | 严禁玻璃栏里再嵌玻璃按钮 |
| 底部 Tab 栏 / iOS 26 折叠 Tab 栏 | `lg::liquid_bottom_tabs_node` / `lg::liquid_collapsible_tabs_node` | 严禁手写底栏放大镜 |
| 搜索栏 / 球形展开搜索框 | `lg::liquid_search_bar_node` / `lg::liquid_search_morph_node` | 严禁手写搜索变形动效 |
| 触发式下拉菜单 / 图标菜单按钮 / 独立命令面板 | `lg::liquid_menu_node` / `lg::liquid_menu_button_node` / `lg::liquid_menu_panel_node` | 严禁手写浮层菜单与键盘导航 |
| 下拉选择器（Select） | `lg::liquid_select_node` + `lg::LiquidSelectOption` | 严禁手写按钮 + 弹出列表 |
| 气泡弹层 / 悬浮提示 | `lg::liquid_popover_node` / `lg::liquid_tooltip_node` | 严禁绝对定位手绘气泡 |
| 弹窗 / 底部 Sheet / 操作表 / 侧边浮岛抽屉 | `lg::liquid_dialog_node` / `lg::liquid_sheet_node` / `lg::liquid_action_sheet_node` / `lg::liquid_drawer_node` + `lg::DrawerSide` | 严禁手写模态遮罩、拖拽关闭与 Escape 关闭 |
| 开关 / 滑块 / 分段控制 / 步进器 / 标签 | `lg::liquid_toggle_node` / `lg::liquid_slider_node` / `lg::liquid_segmented_node` / `lg::liquid_stepper_node` / `lg::liquid_chip_node` | 严禁手写按压折射透镜 |
| 文本输入 / 复选框 / 单选组 | `lg::liquid_text_field_node` / `lg::liquid_checkbox_node` / `lg::liquid_radio_group_node` | 严禁在玻璃内再嵌玻璃输入项 |
| 轻提示 / 徽标 / 进度条 / 进度环 | `lg::liquid_toast_node` / `lg::liquid_badge_node` / `lg::liquid_count_badge_node` / `lg::liquid_progress_bar_node` / `lg::liquid_progress_ring_node` | 严禁手写状态色与进度动画 |
| 连续平面→玻璃浮升容器 | `lg::lg_glass_lift_surface`（静止 `strength <= 0.001` 零玻璃，浮起渐现） | 严禁手动插值 `Glass::liquid` 或静止留 `alpha=0` 玻璃 |
| 毛细融合 / 放大镜 / 自适应亮度 / 渐进模糊 | `lg::liquid_merge_node` / `lg::liquid_magnifier_node` / `lg::liquid_adaptive_surface_node` / `lg::liquid_progressive_blur_node` | 严禁手调 shader 参数 |
| 单测苹果双层 UI 门禁 | `lg::assert_apple_ui` / `lg::audit_glass` / `lg::assert_no_nested_glass` / `lg::count_liquid_glass` | 严禁肉眼检查代替自动门禁 |
| 纵向/横向可滚动区域 | `ui::scroll_box` / `ui::scroll_row` / `kit::scroll_area` | 严禁在已有兄弟节点的容器上直接 `.scrolls(...)` 或遗漏 Flex 祖先 `.fill()` / `min_height(0)` |

---

## 一、按钮与操作（Buttons & Actions）

### 1. `lg::liquid_button_node` / `<lg::Button>`
- **适用场景**：独立液态玻璃胶囊按钮（默认 `height=30.0`、`pad_x=12.0`，自动扩展 `36pt` 热区），主操作传 `prominent=true`，玻璃容器内部传 `flat=true`、`ghost=true`。多激活工具栏按钮传 `neutral_active=true` 可使用中性半透明选中底，避免同时泛蓝。
- **签名**：`pub fn liquid_button_node(parent : view::View, label : () -> String, press : () -> Unit, content? : (view::View) -> Unit, tint~ : Int64 = clear, surface~ : Int64 = clear, flat~ : Bool = false, prominent~ : Bool = false, interactive~ : Bool = true, disabled~ : () -> Bool = fn() { false }, ghost~ : Bool = false, height~ : Double = button_h, pad_x~ : Double = button_px, active~ : () -> Bool = fn() { false }, neutral_active~ : Bool = false, loc~ : String = "") -> view::View`

### 2. `lg::liquid_icon_button_node` / `<lg::IconButton>` & `lg::LiquidButtonSize`
- **适用场景**：浮动单层玻璃圆形图标按钮（`Small=28pt` / `Regular=32pt` / `Large=40pt`）。

### 3. `lg::liquid_button_group_node` & `lg::LiquidGroupAction`
- **适用场景**：多个相关图标操作共用同一片单层液态玻璃胶囊。

### 4. `lg::liquid_control_tile_node` & `lg::liquid_control_item_node` & `lg::flight_icon`
- **适用场景**：控制中心风格悬浮液态玻璃磁贴（单层玻璃，内部 `floating=true`）以及卡片内扁平切换项（零玻璃）。

---

## 二、工具栏与导航（Toolbars & Navigation）

### 5. `lg::liquid_toolbar_node` / `<lg::Toolbar>` & `lg::liquid_toolbar_divider` / `<lg::ToolbarDivider>`
- **适用场景**：单层液态玻璃浮动工具栏容器（`height=36.0, r=18.0`，内放 `flat=true, ghost=true` 按钮与分割线）。

### 5b. `lg::liquid_bar_pill_group_node` / `<lg::BarPillGroup>`
- **适用场景**：顶栏悬浮液态玻璃药丸操作组（紧凑外阴影 `lg_bar_pill_shadow()`，防止相邻 6pt 间距药丸互相渗出光晕；内部按钮一律 `flat=true, ghost=true`）。

### 6. `lg::liquid_nav_bar_node` / `<lg::NavBar>`
- **适用场景**：页面顶部导航栏（默认 `height=46.0`、透明布局层 `glass=false`，左右槽托管独立单层玻璃岛）。

### 7. `lg::liquid_bottom_tabs_node`、`lg::liquid_collapsible_tabs_node` & `lg::liquid_search_morph_node`
- **适用场景**：iOS 26/27 浮动液态玻璃底部 Tab 栏（按压拖拽触发折射放大镜）、滚动自动折叠 Tab 栏与双岛变形搜索框。

---

## 三、面板、列表与浮层（Panels, Lists & Overlays）

### 10. `lg::liquid_panel_node` / `<lg::Panel>`
- **适用场景**：默认渲染干净实色苹果内容卡片（`floating=false`）；显式传 `floating=true` 时挂载 75% 对称高密度单层液态玻璃（悬浮检查器如 `OPTICAL PARAMETERS`）。
- **签名**：`pub fn liquid_panel_node(parent : view::View, body : (view::View) -> Unit, radius~ : Double = ovl_panel_radius, pad~ : Double = ovl_panel_pad, gap~ : Double = 0.0, floating~ : Bool = false, label~ : () -> String = fn() { "Panel" }, loc~ : String = "") -> view::View`

### 11. `lg::liquid_card_node` / `<lg::Card>`
- **适用场景**：固定宽高（`w × h`）卡片容器，默认实色苹果卡片（`floating=false`）；控制中心悬浮磁贴显式传 `floating=true` 启用液态玻璃折射。
- **签名**：`pub fn liquid_card_node(parent : view::View, w : Double, h : Double, label : () -> String, content~ : (view::View) -> Unit = fn(_v) { () }, radius~ : Double = tile_size / 2.0, angle~ : Double = 45.0, floating~ : Bool = false, press? : () -> Unit, light? : InteractiveHighlight, loc~ : String = "") -> view::View`

### 12. `lg::liquid_list_node` & `lg::LiquidListRow`
- **适用场景**：设置页与分组列表容器（默认 `glass=false` 为干净实色苹果分组卡片，内置同心圆角 `r=24, pad=6 -> row r=18` 与无悬空分割线；玻璃弹层/抽屉内部传 `flat=true`；仅悬浮弹层独立列表显式传 `glass=true`）。
- **签名**：`pub fn liquid_list_node(parent : view::View, rows : () -> Array[LiquidListRow], header~ : () -> String = fn() { "" }, footer~ : () -> String = fn() { "" }, dividers~ : Bool = true, flat~ : Bool = false, glass~ : Bool = false, loc~ : String = "") -> view::View`

### 13. `lg::liquid_disclosure_node` / `<lg::Disclosure>`
- **适用场景**：可展开/收起折叠面板（默认 `glass=false` 为干净实色苹果卡片，同心圆角 `r=22 -> header r=16`；悬浮弹层内显式传 `glass=true`）。
- **签名**：`pub fn liquid_disclosure_node(parent : view::View, title : () -> String, open : () -> Bool, on_toggle : (Bool) -> Unit, content : (view::View) -> Unit, subtitle~ : () -> String = fn() { "" }, disabled~ : () -> Bool = fn() { false }, glass~ : Bool = false, loc~ : String = "") -> view::View`

### 14–16. 悬浮弹层与模态容器（单层液态玻璃）
- `lg::liquid_popover_node`、`lg::liquid_popover_button_node`、`lg::liquid_tooltip_node`
- `lg::liquid_dialog_node` / `<lg::Dialog>`（70% 对称密度，`r=28 -> 32pt 按钮 r=16`）
- `lg::liquid_sheet_node` / `<lg::Sheet>`（80%→86% 双档 detent 响应）与 `lg::liquid_action_sheet_node`
- `lg::liquid_drawer_node` / `<lg::Drawer>`（80% 浮岛侧栏抽屉，`DrawerSide::Leading | Trailing`）

---

## 四、菜单、选择、表单与反馈（Menus, Controls, Inputs & Feedback）

- **菜单与选择**：`liquid_menu_node`、`liquid_menu_button_node`、`liquid_menu_panel_node`、`liquid_select_node`、`liquid_segmented_node` / `<lg::Segmented>`、`liquid_swatch_grid_node`、`liquid_color_picker_node`
- **表单与控件**：`liquid_text_field_node` / `<lg::TextField>`、`liquid_search_bar_node` / `<lg::SearchBar>`、`liquid_toggle_node` / `<lg::Toggle>`、`liquid_slider_node` / `<lg::Slider>`、`liquid_checkbox_node` / `<lg::Checkbox>`、`liquid_radio_group_node`、`liquid_stepper_node` / `<lg::Stepper>`、`liquid_chip_node` / `<lg::Chip>`
- **反馈**：`liquid_toast_node` / `<lg::Toast>`、`liquid_badge_node` / `<lg::Badge>`、`liquid_count_badge_node` / `<lg::CountBadge>`、`liquid_progress_bar_node` / `<lg::ProgressBar>`、`liquid_progress_ring_node` / `<lg::ProgressRing>`

---

## 五、光学物理原语与动效控制器（Optical Physics & Motion）

- `liquid_merge_node` / `LiquidMerge`、`liquid_adaptive_surface_node` / `adaptive_glass`、`liquid_magnifier_node`、`liquid_progressive_blur_node`、`lg_glass_lift_surface`（静止 `strength <= 0.001` 零玻璃，浮升渐现单层玻璃）、`DampedDrag`、`InteractiveHighlight`、`set_dark` / `is_dark` / `colors()` / `ui_theme()` / `words_at()` / `alpha()` / `over()`。

---

## 六、审计门禁（Audit Gates）

### `lg::assert_apple_ui`、`lg::audit_glass`、`lg::assert_no_nested_glass` & `lg::count_liquid_glass`
- **`lg::audit_glass(t, label~="scene", max_glass_coverage~=0.45, allow_large_overlay~=false)`**：同时检测 Law 2 玻璃重叠/嵌套、Law 1 内容层滥用液态玻璃（总面积 > 45% 或单块 > 65%w × 55%h）与 `t.unasked_faults()`。
- **`lg::assert_apple_ui(t, label~="scene", max_glass_coverage~=0.45, allow_large_overlay~=false)`**：一站式校验 `assert_no_nested_glass` + `t.assert_scrollable()` + `t.assert_ui_craft()`。
```pion
lg::assert_apple_ui(t, label="main_screen")
```
