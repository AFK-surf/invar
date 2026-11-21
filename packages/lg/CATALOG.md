# `packages/lg` Component Catalog (一页纸速查目录)

> **铁律**：下游业务代码严禁直接调用 `paint::Glass::liquid(...)`，所有液态玻璃界面必须 100% 从本目录选用 `lg::*` 组件，并在单测中调用 `lg::assert_no_nested_glass(t, label="...")`。若缺少桌面尺寸或通用原语，必须先向 `AFK-surf/invar` 提补齐。

---

## 常见需求 → 对应 `lg::*` 组件（严禁手搓）

| 常见业务需求 | 必须使用的 `lg::*` 组件 | 禁止手搓的做法 |
|---|---|---|
| 浮动主按钮 / 次按钮 / 玻璃容器内扁平按钮 | `lg::liquid_button_node`（容器内传 `flat=true` / `ghost=true`） | 严禁 `v.glass(Glass::liquid(...))` 拼按钮 |
| 图标按钮（圆/胶囊） | `lg::liquid_icon_button_node` | 严禁手动圆框 + `Glass::liquid` |
| 多图标合并胶囊操作组 | `lg::liquid_button_group_node` + `lg::LiquidGroupAction` | 严禁横排多个玻璃图标按钮硬贴 |
| 浮动工具栏 + 分割线 | `lg::liquid_toolbar_node` + `lg::liquid_toolbar_divider` + `flat=true, ghost=true` 按钮 | 严禁在工具栏内再放 `flat=false` 玻璃按钮 |
| 顶部导航栏 | `lg::liquid_nav_bar_node`（默认 `glass=false`，左右槽放浮动玻璃岛） | 严禁玻璃栏里再嵌玻璃按钮 |
| 底部 Tab 栏 / iOS 26 折叠 Tab 栏 | `lg::liquid_bottom_tabs_node` / `lg::liquid_collapsible_tabs_node` | 严禁手写底栏放大镜 |
| 搜索栏 / 球形展开搜索框 | `lg::liquid_search_bar_node` / `lg::liquid_search_morph_node` | 严禁手写搜索变形动效 |
| 浮动检查器 / 卡片 / 控制中心磁贴 | `lg::liquid_panel_node` / `lg::liquid_card_node` / `lg::liquid_control_tile_node` | 严禁直接套 `Glass::liquid` 面板 |
| 分组列表 / 折叠面板 | `lg::liquid_list_node` + `lg::LiquidListRow` / `lg::liquid_disclosure_node` | 严禁手工拼列表圆角（破坏同心圆角） |
| 触发式下拉菜单 / 图标菜单按钮 / 独立命令面板 | `lg::liquid_menu_node` / `lg::liquid_menu_button_node` / `lg::liquid_menu_panel_node` | 严禁手写浮层菜单与键盘导航 |
| 下拉选择器（Select） | `lg::liquid_select_node` + `lg::LiquidSelectOption` | 严禁手写按钮 + 弹出列表 |
| 气泡弹层 / 悬浮提示 | `lg::liquid_popover_node` / `lg::liquid_tooltip_node` | 严禁绝对定位手绘气泡 |
| 弹窗 / 底部 Sheet / 操作表 | `lg::liquid_dialog_node` / `lg::liquid_sheet_node` / `lg::liquid_action_sheet_node` | 严禁手写模态遮罩与 Escape 关闭 |
| 开关 / 滑块 / 分段控制 / 步进器 / 标签 | `lg::liquid_toggle_node` / `lg::liquid_slider_node` / `lg::liquid_segmented_node` / `lg::liquid_stepper_node` / `lg::liquid_chip_node` | 严禁手写按压折射透镜 |
| 文本输入 / 复选框 / 单选组 | `lg::liquid_text_field_node` / `lg::liquid_checkbox_node` / `lg::liquid_radio_group_node` | 严禁在玻璃内再嵌玻璃输入项 |
| 轻提示 / 徽标 / 进度条 / 进度环 | `lg::liquid_toast_node` / `lg::liquid_badge_node` / `lg::liquid_count_badge_node` / `lg::liquid_progress_bar_node` / `lg::liquid_progress_ring_node` | 严禁手写状态色与进度动画 |
| 毛细融合 / 放大镜 / 自适应亮度 / 渐进模糊 | `lg::liquid_merge_node` / `lg::liquid_magnifier_node` / `lg::liquid_adaptive_surface_node` / `lg::liquid_progressive_blur_node` | 严禁手调 shader 参数 |
| 单测零玻璃嵌套门禁 | `lg::audit_glass` / `lg::assert_no_nested_glass` / `lg::count_liquid_glass` | 严禁肉眼检查代替自动门禁 |

---

## 一、按钮与操作（Buttons & Actions）

### 1. `lg::liquid_button_node`
- **适用场景**：独立液态玻璃胶囊按钮，或作为玻璃容器内部的扁平/幽灵按钮（传 `flat=true`、`ghost=true`）。
- **签名**：`pub fn liquid_button_node(parent : view::View, label : () -> String, press : () -> Unit, content? : (view::View) -> Unit, tint~ : Int64 = clear, surface~ : Int64 = clear, flat~ : Bool = false, interactive~ : Bool = true, disabled~ : () -> Bool = fn() { false }, ghost~ : Bool = false, height~ : Double = button_h, pad_x~ : Double = button_px, active~ : () -> Bool = fn() { false }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_button_node(row, fn() { "Publish" }, fn() { on_publish() }, tint=0x007AFFFFL)
let _ = lg::liquid_button_node(bar, fn() { "Pen" }, fn() { tool.set(1) }, flat=true, ghost=true, height=32.0, pad_x=10.0, active=fn() { tool.get() == 1 })
```

### 2. `lg::liquid_icon_button_node` & `lg::LiquidButtonSize`
- **适用场景**：浮动单层玻璃圆形图标按钮（支持 `Small` / `Regular` / `Large` 与激活态）。
- **签名**：`pub enum LiquidButtonSize`；`pub fn liquid_icon_button_node(parent : view::View, label : () -> String, icon : (view::View, () -> Int64) -> Unit, press : () -> Unit, size~ : LiquidButtonSize = LiquidButtonSize::Regular, tint~ : Int64 = clear, surface~ : Int64 = clear, active~ : () -> Bool = fn() { false }, disabled~ : () -> Bool = fn() { false }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_icon_button_node(
  row, fn() { "Favorite" },
  fn(c, ink) { lg::flight_icon(c, 18.0, ink) },
  fn() { fav.set(!fav.get()) },
  active=fn() { fav.get() },
)
```

### 3. `lg::liquid_button_group_node` & `lg::LiquidGroupAction`
- **适用场景**：多个相关图标操作共用同一片单层液态玻璃胶囊（如编辑工具组）。
- **签名**：`pub struct LiquidGroupAction`；`pub fn LiquidGroupAction::new(label~ : () -> String, icon~ : (view::View, () -> Int64) -> Unit, press~ : () -> Unit, active~ : () -> Bool = fn() { false }, disabled~ : () -> Bool = fn() { false }) -> LiquidGroupAction`；`pub fn liquid_button_group_node(parent : view::View, actions : () -> Array[LiquidGroupAction], size~ : LiquidButtonSize = LiquidButtonSize::Regular, surface~ : Int64 = clear, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_button_group_node(row, fn() {
  [
    lg::LiquidGroupAction::new(label=fn() { "Cut" }, icon=fn(c, ink) { lg::flight_icon(c, 16.0, ink) }, press=fn() { () }),
    lg::LiquidGroupAction::new(label=fn() { "Copy" }, icon=fn(c, ink) { lg::flight_icon(c, 16.0, ink) }, press=fn() { () }),
  ]
})
```

### 4. `lg::liquid_control_tile_node` & `lg::liquid_control_item_node` & `lg::flight_icon`
- **适用场景**：控制中心风格方圆角玻璃磁贴（单层玻璃）以及卡片内扁平切换项（零玻璃）。
- **签名**：
  - `pub fn liquid_control_tile_node(parent : view::View, label : () -> String, press : () -> Unit, angle~ : Double = 45.0, icon? : (view::View, () -> Int64) -> Unit, loc~ : String = "") -> view::View`
  - `pub fn liquid_control_item_node(parent : view::View, on : () -> Bool, turn : (Bool) -> Unit, label : () -> String, icon? : (view::View, () -> Int64) -> Unit, loc~ : String = "") -> view::View`
  - `pub fn flight_icon(parent : view::View, size : Double, ink : () -> Int64, loc~ : String = "") -> Unit`
```pion
let _ = lg::liquid_control_tile_node(row, fn() { "Airplane" }, fn() { toggle_flight() })
let _ = lg::liquid_control_item_node(card, fn() { wifi.get() }, fn(v) { wifi.set(v) }, fn() { "Wi-Fi" })
```

---

## 二、工具栏与导航（Toolbars & Navigation）

### 5. `lg::liquid_toolbar_node` & `lg::liquid_toolbar_divider`
- **适用场景**：单层液态玻璃浮动工具栏容器（内部只放 `flat=true` 按钮与 `liquid_toolbar_divider`）。
- **签名**：
  - `pub fn liquid_toolbar_node(parent : view::View, body : (view::View) -> Unit, height~ : Double = 48.0, pad_x~ : Double = ovl_toolbar_px, gap~ : Double = ovl_toolbar_gap, loc~ : String = "") -> view::View`
  - `pub fn liquid_toolbar_divider(parent : view::View, height~ : Double = 20.0, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_toolbar_node(page, fn(bar) {
  let _ = lg::liquid_button_node(bar, fn() { "Select" }, fn() { () }, flat=true, ghost=true, height=32.0, pad_x=10.0)
  let _ = lg::liquid_toolbar_divider(bar, height=18.0)
  let _ = lg::liquid_button_node(bar, fn() { "Note" }, fn() { () }, flat=true, ghost=true, height=32.0, pad_x=10.0)
}, height=40.0, pad_x=8.0, gap=4.0)
```

### 6. `lg::liquid_nav_bar_node`
- **适用场景**：页面顶部导航栏（默认透明布局层 `glass=false`，左右槽托管独立单层玻璃岛）。
- **签名**：`pub fn liquid_nav_bar_node(parent : view::View, title : () -> String, subtitle~ : () -> String = fn() { "" }, leading? : (view::View) -> Unit, trailing? : (view::View) -> Unit, glass~ : Bool = false, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_nav_bar_node(page, fn() { "Settings" }, subtitle=fn() { "Invar" }, trailing=fn(t) {
  let _ = lg::liquid_icon_button_node(t, fn() { "More" }, fn(c, ink) { lg::flight_icon(c, 16.0, ink) }, fn() { () })
})
```

### 7. `lg::liquid_bottom_tabs_node`、`lg::liquid_bottom_tab_node` & `lg::tab_face`
- **适用场景**：iOS 27 浮动液态玻璃底部 Tab 栏，按压拖拽时触发折射放大镜。
- **签名**：
  - `pub fn liquid_bottom_tabs_node(parent : view::View, labels : Array[String], selected : () -> Int, select : (Int) -> Unit, label~ : String = "", faces? : Array[(view::View, () -> Int64) -> Unit], drag? : DampedDrag, light? : InteractiveHighlight, loc~ : String = "") -> view::View`
  - `pub fn tab_face(name : String) -> (view::View, () -> Int64) -> Unit`
```pion
let _ = lg::liquid_bottom_tabs_node(wrap, ["Home", "Explore", "Library"], fn() { tab.get() }, fn(i) { tab.set(i) }, label="Main Tabs")
```

### 8. `lg::liquid_collapsible_tabs_node`
- **适用场景**：iOS 26/27 滚动时自动折叠为胶囊圆球 + 独立操作球的液态玻璃 Tab 栏。
- **签名**：`pub fn liquid_collapsible_tabs_node(parent : view::View, tabs : Array[String], chosen : () -> Int, choose : (Int) -> Unit, minimized : () -> Bool, on_expand : () -> Unit, on_action : () -> Unit, action_label~ : () -> String = fn() { "Search" }, expanded_width~ : Double = 320.0, label~ : String = "", loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_collapsible_tabs_node(
  page, ["Home", "Browse", "Radio"],
  fn() { tab.get() }, fn(i) { tab.set(i) },
  fn() { minimized.get() }, fn() { minimized.set(false) }, fn() { open_search() },
)
```

### 9. `lg::liquid_search_morph_node`
- **适用场景**：圆形搜索球与展开态搜索栏 + 独立取消球之间的双岛液态变形。
- **签名**：`pub fn liquid_search_morph_node(parent : view::View, field : ui::Field, expanded : () -> Bool, set_expanded : (Bool) -> Unit, width~ : Double = 360.0, placeholder~ : String = "Search", label~ : () -> String = fn() { "Search" }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_search_morph_node(page, search_field, fn() { exp.get() }, fn(v) { exp.set(v) }, width=360.0, placeholder="Search docs")
```

---

## 三、面板与浮层（Panels & Overlays）

### 10. `lg::liquid_panel_node`
- **适用场景**：自适应内容高度的高密度液态玻璃检查器/浮层面板（暗色自动采用 `0x202020BF` 75% 密度）。
- **签名**：`pub fn liquid_panel_node(parent : view::View, body : (view::View) -> Unit, radius~ : Double = ovl_popover_radius, pad~ : Double = ovl_popover_pad, gap~ : Double = 0.0, label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_panel_node(page, fn(p) {
  let _ = p.box(fn(w) { let _ = w.words(view::Live::Is("Inspector"), look=lg::words_at(14.0)) })
}, gap=8.0, label=fn() { "Inspector" })
```

### 11. `lg::liquid_card_node`
- **适用场景**：固定宽高（`w × h`）的单层液态玻璃展示卡片容器。
- **签名**：`pub fn liquid_card_node(parent : view::View, w : Double, h : Double, label : () -> String, content~ : (view::View) -> Unit = fn(_v) { () }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_card_node(page, 280.0, 160.0, fn() { "Metrics" }, content=fn(c) {
  let _ = c.box(fn(w) { let _ = w.words(view::Live::Is("120 fps"), look=lg::words_at(20.0, weight=600)) })
})
```

### 12. `lg::liquid_list_node` & `lg::LiquidListRow`
- **适用场景**：设置页与分组列表单层玻璃容器，内置同心圆角（`r=24, pad=6 -> row r=18`）与无悬空分割线。
- **签名**：`pub struct LiquidListRow`；`pub fn LiquidListRow::new(title~ : String, subtitle~ : String = "", value~ : String = "", icon? : (view::View, () -> Int64) -> Unit, chevron~ : Bool = false, on_press? : () -> Unit, active~ : Bool = false, destructive~ : Bool = false, disabled~ : Bool = false) -> LiquidListRow`；`pub fn liquid_list_node(parent : view::View, rows : () -> Array[LiquidListRow], header~ : () -> String = fn() { "" }, footer~ : () -> String = fn() { "" }, dividers~ : Bool = true, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_list_node(page, fn() {
  [lg::LiquidListRow::new(title="Wi-Fi", value="Office", chevron=true, on_press=fn() { () })]
}, header=fn() { "Network" })
```

### 13. `lg::liquid_disclosure_node`
- **适用场景**：可展开/收起的单层液态玻璃折叠面板（同心圆角 `r=22 -> header r=16`）。
- **签名**：`pub fn liquid_disclosure_node(parent : view::View, title : () -> String, open : () -> Bool, on_toggle : (Bool) -> Unit, content : (view::View) -> Unit, subtitle~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_disclosure_node(page, fn() { "Advanced" }, fn() { open.get() }, fn(v) { open.set(v) }, fn(b) {
  let _ = b.box(fn(w) { let _ = w.words(view::Live::Is("Details"), look=lg::words_at(14.0)) })
})
```

### 14. `lg::liquid_popover_node` & `lg::liquid_tooltip_node`
- **适用场景**：锚定在触发器旁的液态玻璃气泡弹层与悬浮提示（含完整 `headless::Placement` 定位）。
- **签名**：
  - `pub fn liquid_popover_node(parent : view::View, host : view::View, open : () -> Bool, set_open : (Bool) -> Unit, trigger_label : () -> String, trigger_look : (view::View, headless::Disclosure) -> Unit, content_label : () -> String, content_look : (view::View) -> Unit, placement~ : headless::Placement = headless::Placement::BelowCenter, width~ : Double = 280.0, anchor_size~ : (Double, Double) = (40.0, 40.0), trigger_face? : (view::View) -> Unit, loc~ : String = "") -> view::View`
  - `pub fn liquid_tooltip_node(parent : view::View, host : view::View, tip : () -> String, trigger_look : (view::View) -> Unit, placement~ : headless::Placement = headless::Placement::AboveCenter, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_tooltip_node(page, host_root, fn() { "Reset optical params" }, fn(tr) {
  let _ = lg::liquid_button_node(tr, fn() { "Reset" }, fn() { () })
})
```

### 15. `lg::liquid_dialog_node`
- **适用场景**：模态居中液态玻璃确认弹窗（高密度暗色 `0x202020BF`，内置 Escape 关闭与扁平操作按钮）。
- **签名**：`pub fn liquid_dialog_node(parent : view::View, open : () -> Bool, on_close : () -> Unit, title : () -> String, text : () -> String, on_action~ : () -> Unit = fn() { () }, action_label~ : () -> String = fn() { "OK" }, cancel_label~ : () -> String = fn() { "Cancel" }, destructive~ : Bool = false, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_dialog_node(page, fn() { dlg.get() }, fn() { dlg.set(false) }, fn() { "Delete Item?" }, fn() { "This cannot be undone." }, on_action=fn() { do_delete() }, destructive=true)
```

### 16. `lg::liquid_sheet_node` & `lg::liquid_action_sheet_node` & `lg::LiquidActionSheetItem`
- **适用场景**：底部独立浮动 Sheet（暗色 80% 密度 `0x202020CC`）与底部操作表。
- **签名**：
  - `pub fn liquid_sheet_node(parent : view::View, open : () -> Bool, on_close : () -> Unit, title : () -> String, body : (view::View) -> Unit, subtitle~ : () -> String = fn() { "" }, footer? : (view::View) -> Unit, loc~ : String = "") -> view::View`
  - `pub struct LiquidActionSheetItem`；`pub fn LiquidActionSheetItem::new(label~ : String, on_select~ : () -> Unit, destructive~ : Bool = false, disabled~ : Bool = false) -> LiquidActionSheetItem`
  - `pub fn liquid_action_sheet_node(parent : view::View, open : () -> Bool, on_close : () -> Unit, items : () -> Array[LiquidActionSheetItem], title~ : () -> String = fn() { "" }, message~ : () -> String = fn() { "" }, cancel_label~ : () -> String = fn() { "Cancel" }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_sheet_node(page, fn() { open.get() }, fn() { open.set(false) }, fn() { "Share" }, fn(b) {
  let _ = b.box(fn(w) { let _ = w.words(view::Live::Is("Sheet body"), look=lg::words_at(15.0)) })
})
```

---

## 四、菜单与选择（Menus & Selection）

### 17. `lg::liquid_menu_node`、`lg::liquid_menu_button_node`、`lg::liquid_menu_panel_node` & `lg::LiquidMenuEntry`
- **适用场景**：带液态融合过渡的下拉菜单（自定义触发器 / 开箱即用圆或胶囊菜单按钮 / 独立命令面板如 Slash Menu）。
- **签名**：
  - `pub struct LiquidMenuEntry` (`LiquidMenuEntry::item`, `LiquidMenuEntry::check`, `LiquidMenuEntry::separator`)
  - `pub fn liquid_menu_node(parent : view::View, host : view::View, open : () -> Bool, set_open : (Bool) -> Unit, trigger_label : () -> String, trigger_look : (view::View, headless::Disclosure) -> Unit, entries : () -> Array[LiquidMenuEntry], menu_label~ : () -> String = fn() { "" }, placement~ : headless::Placement = headless::Placement::BelowStart, width~ : Double = 220.0, loc~ : String = "") -> view::View`
  - `pub fn liquid_menu_button_node(parent : view::View, host : view::View, open : () -> Bool, set_open : (Bool) -> Unit, label : () -> String, entries : () -> Array[LiquidMenuEntry], icon? : (view::View, () -> Int64) -> Unit, show_label~ : Bool = true, height~ : Double = 44.0, pad_x~ : Double = 14.0, menu_label~ : () -> String = fn() { "" }, placement~ : headless::Placement = headless::Placement::BelowStart, width~ : Double = 220.0, loc~ : String = "") -> view::View`
  - `pub fn liquid_menu_panel_node(parent : view::View, entries : () -> Array[LiquidMenuEntry], on_close : () -> Unit, header~ : () -> String = fn() { "" }, menu_label~ : () -> String = fn() { "Menu" }, width~ : Double = 240.0, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_menu_button_node(row, page, fn() { open.get() }, fn(v) { open.set(v) }, fn() { "Insert" }, fn() {
  [
    lg::LiquidMenuEntry::item(label="Sticky Note", on_select=fn() { add_note() }),
    lg::LiquidMenuEntry::separator(),
    lg::LiquidMenuEntry::item(label="Delete", destructive=true, on_select=fn() { del() }),
  ]
}, height=36.0, pad_x=12.0)
```

### 18. `lg::liquid_select_node` & `lg::LiquidSelectOption`
- **适用场景**：单选下拉选择器（胶囊触发器 + 高密度玻璃菜单）。
- **签名**：`pub struct LiquidSelectOption`；`pub fn LiquidSelectOption::new(label~ : String, detail~ : String = "", disabled~ : Bool = false) -> LiquidSelectOption`；`pub fn liquid_select_node(parent : view::View, host : view::View, options : () -> Array[LiquidSelectOption], selected : () -> Int, on_select : (Int) -> Unit, placeholder~ : String = "Select...", label~ : () -> String = fn() { "" }, placement~ : headless::Placement = headless::Placement::BelowStart, width~ : Double = 240.0, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_select_node(page, page, fn() {
  [lg::LiquidSelectOption::new(label="Metal"), lg::LiquidSelectOption::new(label="GLES3")]
}, fn() { idx.get() }, fn(i) { idx.set(i) })
```

### 19. `lg::liquid_segmented_node`
- **适用场景**：分段切换控件（支持拖拽滑动与按压清透透镜）。
- **签名**：`pub fn liquid_segmented_node(parent : view::View, labels : () -> Array[String], selected : () -> Int, on_select : (Int) -> Unit, width? : Double, height~ : Double = 40.0, drag? : DampedDrag, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_segmented_node(page, fn() { ["Day", "Week", "Month"] }, fn() { seg.get() }, fn(i) { seg.set(i) }, width=300.0)
```

### 20. `lg::liquid_checkbox_node`、`lg::liquid_radio_group_node`、`lg::liquid_stepper_node`、`lg::liquid_chip_node`
- **适用场景**：复选框（含空标签仅渲染 `22×22` 勾选框 + `aria_label`）、单选组、数值步进器、可筛选玻璃标签。
- **签名**：
  - `pub fn liquid_checkbox_node(parent : view::View, checked : () -> Bool, on_change : (Bool) -> Unit, label : () -> String, mixed~ : () -> Bool = fn() { false }, disabled~ : () -> Bool = fn() { false }, aria_label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
  - `pub fn liquid_radio_group_node(parent : view::View, labels : () -> Array[String], selected : () -> Int, on_select : (Int) -> Unit, horizontal~ : Bool = false, disabled~ : () -> Bool = fn() { false }, loc~ : String = "") -> view::View`
  - `pub fn liquid_stepper_node(parent : view::View, value : () -> Int, on_change : (Int) -> Unit, min~ : Int = 0, max~ : Int = 100, step~ : Int = 1, label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
  - `pub fn liquid_chip_node(parent : view::View, label : () -> String, on_press : () -> Unit, selected~ : () -> Bool = fn() { false }, tint~ : Int64 = clear, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_checkbox_node(page, fn() { ok.get() }, fn(v) { ok.set(v) }, fn() { "Agree" })
let _ = lg::liquid_stepper_node(page, fn() { qty.get() }, fn(v) { qty.set(v) }, min=0, max=10, label=fn() { "Qty" })
let _ = lg::liquid_chip_node(page, fn() { "Metal" }, fn() { sel.set(!sel.get()) }, selected=fn() { sel.get() })
```

---

## 五、表单输入与反馈（Inputs & Feedback）

### 21. `lg::liquid_text_field_node` & `lg::liquid_search_bar_node`
- **适用场景**：单层液态玻璃文本输入框（支持清除按钮与标签）与常驻搜索栏。
- **签名**：
  - `pub fn liquid_text_field_node(parent : view::View, field : ui::Field, placeholder~ : String = "", label~ : () -> String = fn() { "" }, clearable~ : Bool = false, disabled~ : () -> Bool = fn() { false }, loc~ : String = "") -> view::View`
  - `pub fn liquid_search_bar_node(parent : view::View, field : ui::Field, placeholder~ : String = "Search", label~ : () -> String = fn() { "Search" }, loc~ : String = "") -> view::View`
```pion
let field = ui::Field::new(text="")
let _ = lg::liquid_text_field_node(page, field, placeholder="Project name", label=fn() { "Name" }, clearable=true)
```

### 22. `lg::liquid_toggle_node` & `lg::liquid_slider_node`
- **适用场景**：iOS 27 阻尼拖拽开关与连续/步进滑块（静止态零隐藏玻璃，按压时浮现折射透镜）。
- **签名**：
  - `pub fn liquid_toggle_node(parent : view::View, on : () -> Bool, turn : (Bool) -> Unit, label : () -> String, disabled~ : () -> Bool = fn() { false }, drag? : DampedDrag, loc~ : String = "") -> view::View`
  - `pub fn liquid_slider_node(parent : view::View, value : () -> Double, change : (Double) -> Unit, label : () -> String, least~ : Double = 0.0, most~ : Double = 100.0, step~ : Double = 0.0, committed~ : (Double) -> Unit = fn(_v) { () }, drag? : DampedDrag, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_toggle_node(page, fn() { wifi.get() }, fn(v) { wifi.set(v) }, fn() { "Wi-Fi" })
let _ = lg::liquid_slider_node(page, fn() { vol.get() }, fn(v) { vol.set(v) }, fn() { "Volume" }, least=0.0, most=100.0)
```

### 23. `lg::liquid_toast_node` & `lg::LiquidStatus`
- **适用场景**：胶囊形态轻提示（`Info` / `Success` / `Warning` / `Error`，支持操作按钮与关闭）。
- **签名**：`pub enum LiquidStatus`；`pub fn liquid_toast_node(parent : view::View, visible : () -> Bool, title : () -> String, subtitle~ : () -> String = fn() { "" }, status~ : LiquidStatus = LiquidStatus::Info, action_label~ : () -> String = fn() { "" }, on_action? : () -> Unit, on_dismiss? : () -> Unit, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_toast_node(page, fn() { saved.get() }, fn() { "Saved" }, status=lg::LiquidStatus::Success)
```

### 24. `lg::liquid_badge_node`、`lg::liquid_count_badge_node` & `lg::LiquidTone`
- **适用场景**：状态徽标与角标计数（扁平非玻璃，可安全放进任何玻璃容器内）。
- **签名**：`pub enum LiquidTone`；`pub fn liquid_badge_node(parent : view::View, label : () -> String, tone~ : LiquidTone = LiquidTone::Neutral, dot~ : Bool = false, loc~ : String = "") -> view::View`；`pub fn liquid_count_badge_node(parent : view::View, count : () -> Int, max_count~ : Int = 99, tone~ : LiquidTone = LiquidTone::Danger, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_badge_node(row, fn() { "Online" }, tone=lg::LiquidTone::Success, dot=true)
let _ = lg::liquid_count_badge_node(row, fn() { unread.get() }, tone=lg::LiquidTone::Danger)
```

### 25. `lg::liquid_progress_bar_node` & `lg::liquid_progress_ring_node`
- **适用场景**：确定值（`Some(0.0..1.0)`）与不确定态（`None`）线形/环形进度指示器。
- **签名**：
  - `pub fn liquid_progress_bar_node(parent : view::View, value : () -> Option[Double], label~ : () -> String = fn() { "" }, show_value~ : Bool = false, tint~ : Int64 = clear, loc~ : String = "") -> view::View`
  - `pub fn liquid_progress_ring_node(parent : view::View, value : () -> Option[Double], size~ : Double = 36.0, stroke~ : Double = 3.5, tint~ : Int64 = clear, label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_progress_bar_node(page, fn() { Some(0.65) }, label=fn() { "Syncing" }, show_value=true)
let _ = lg::liquid_progress_ring_node(page, fn() { Some(0.75) }, size=32.0)
```

---

## 六、光学物理原语与动效控制器（Optical Physics & Motion Primitives）

### 26. `lg::liquid_merge_node` & `lg::LiquidMerge`
- **适用场景**：毛细管液滴融合/分离物理舞台（带滞后断开与回弹控制器）。
- **签名**：`pub struct LiquidMerge` (`new`, `attach`, `is_separated`, `is_bonded`, `set_separated`, `merge_at`, `gap_at`)；`pub fn liquid_merge_node(parent : view::View, controller? : LiquidMerge, main_w~ : Double = merge_main_w, main_h~ : Double = merge_main_h, sat_size~ : Double = merge_sat_size, arena_w~ : Double = 0.0, arena_h~ : Double = 0.0, arena? : () -> (Double, Double), main_label~ : () -> String = fn() { "" }, label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
```pion
let ctrl = lg::LiquidMerge::new(separated=false)
let _ = lg::liquid_merge_node(page, controller=ctrl, label=fn() { "Merge Stage" })
```

### 27. `lg::liquid_adaptive_surface_node` & `lg::adaptive_glass`
- **适用场景**：根据背景亮度自动在浅色/暗色光学传输模型间切换的自适应液态玻璃表面。
- **签名**：`pub fn adaptive_glass(lum : Double, min_side : Double) -> paint::Glass`；`pub fn liquid_adaptive_surface_node(parent : view::View, luminance : () -> Double, size~ : Double = adaptive_size, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_adaptive_surface_node(stage, fn() { bg_lum.get() }, size=160.0)
```

### 28. `lg::liquid_magnifier_node` & `lg::liquid_progressive_blur_node`
- **适用场景**：文本光标折射放大镜与边缘渐进模糊阅读带。
- **签名**：
  - `pub fn liquid_magnifier_node(parent : view::View, words : () -> String, label~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
  - `pub fn liquid_progressive_blur_node(parent : view::View, words~ : () -> String = fn() { "" }, loc~ : String = "") -> view::View`
```pion
let _ = lg::liquid_magnifier_node(stage, fn() { "Drag loupe over copy" })
let _ = lg::liquid_progressive_blur_node(stage, words=fn() { "Progressive blur band" })
```

### 29. `lg::DampedDrag`、`lg::InteractiveHighlight` 与主题/令牌工具
- **适用场景**：闭式弹簧阻尼拖拽、按压镜面高光与弹性拉伸、主题深浅切换与标准字号/颜色工具。
- **签名**：
  - `pub struct DampedDrag` (`new`, `attach`, `value`, `progress`, `pressed`, `jelly`, `animate_to`, `slide_to`)
  - `pub struct InteractiveHighlight` (`new`, `attach`, `pressed`, `offset`, `spot`, `stretch`)
  - `pub fn set_dark(dark : Option[Bool]) -> Unit` / `pub fn is_dark() -> Bool` / `pub fn colors() -> Colors` / `pub fn ui_theme() -> ui::Theme` / `pub fn words_at(size : Double, weight~ : Int = 400, wrap~ : Bool = false, lines~ : Int = 0) -> view::WordsLook` / `pub fn alpha(c : Int64, share : Double) -> Int64` / `pub fn over(base : Int64, top : Int64) -> Int64`
```pion
lg::set_dark(Some(true))
ui::set_tree_theme(lg::ui_theme)
```

---

## 七、审计门禁（Audit Gates）

### 30. `lg::count_liquid_glass`、`lg::audit_glass` & `lg::assert_no_nested_glass`
- **适用场景**：在所有业务 UI 与组件单测中自动校验「零液态玻璃重叠/嵌套（容差 `0.5pt`，面积 `> 1.0 pt²` 精确报索引与坐标）」与 `ui::Tester::unasked_faults()`。
- **签名**：
  - `pub fn count_liquid_glass(t : ui::Tester) -> Int`
  - `pub fn audit_glass(t : ui::Tester, label~ : String = "scene") -> Array[String]`
  - `pub fn assert_no_nested_glass(t : ui::Tester, label~ : String = "scene") -> Unit`
```pion
let n = lg::count_liquid_glass(t)
let faults = lg::audit_glass(t, label="toolbar_scene")
debug_assert(faults.length() == 0, "expected 0 glass faults")
lg::assert_no_nested_glass(t, label="toolbar_scene")
```
