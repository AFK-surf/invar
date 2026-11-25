# `packages/aura` Component Catalog（一页纸速查目录）

> **铁律**：
> 1. **只用 `aura::*`，零手搓**：按钮、字段、浮层、表格、取色、日期等一律从本目录选组件；每个组件同时提供 Builder `aura::*_node(parent, ...)` 与 Pion-JSX `<aura::*>` 标签（如 `<aura::Button in={parent} ...>`）两种写法，参数一一对应（JSX 额外提供 `*_by=>{...}` 响应式闭包与 `children`）。
> 2. **零液态玻璃**：Aura 全库是实色纸面体系，业务代码不得调用 `paint::Glass::liquid(...)`，也不要把 `packages/lg` 的玻璃组件混进 Aura 页面。
> 3. **根上挂配置**：每棵树先 `aura::aura_provide_config(root, cfg)` 并 `ui::set_tree_theme(fn() { aura::aura_ui_theme(cfg.get()) })`；明暗、Vibrant、圆角形状、密度、RTL、减弱动效都只改这一个 `signals::Signal[aura::AuraConfig]`。
> 4. **单测必跑门禁**：每个 Aura 界面的 `ui::Tester` 用例调用 `aura::assert_aura_ui(t, label="...")`。

共 **86 个组件**（158 个 Builder 函数 / 158 个 JSX 标签，含复合子部件与 1 个辅助 `Icon`）。设计法则见 [`DESIGN.md`](DESIGN.md)。

---

## 0. 起步：配置、主题与门禁

```pion
import "aura"
import "signals"
import "ui"
import "view"

pub fn build(root : view::View, cfg : signals::Signal[aura::AuraConfig]) -> Unit {
  ui::set_tree_theme(fn() -> ui::Theme { aura::aura_ui_theme(cfg.get()) })
  let _ = aura::aura_provide_config(root, cfg)
  // Builder
  let _ = aura::card_node(root, content=fn(c : view::View) -> Unit {
    let _ = aura::card_title_node(c, view::Live::Is("Builder card"))
    let _ = aura::text_field_node(c, label=view::Live::Is("Email"))
    let _ = aura::button_node(c, view::Live::Is("Save"), on_press=fn() { () })
  })
  // Pion-JSX
  <aura::Card in={root}>
    <aura::CardHeader>
      <aura::CardTitle text="JSX card" />
    </aura::CardHeader>
    <aura::CardContent>
      <aura::Select label="Region" items={[aura::AuraSelectOption::new("eu", "Europe")]} />
    </aura::CardContent>
    <aura::CardFooter>
      <aura::Button label="Continue" variant={.Secondary} on_press=>{ () } />
    </aura::CardFooter>
  </aura::Card>
}

test "screen" {
  let cfg = signals::signal(aura::AuraConfig::default())
  let t = ui::Tester::tree(fn(root : view::View) -> Unit { build(root, cfg) }, width=900.0, height=700.0)
  t.frame()
  aura::assert_aura_ui(t, label="screen")
}
```

| 用途 | API |
|---|---|
| 配置结构与预设 | `aura::AuraConfig`（`dark` / `vibrant` / `radius` / `spacing` / `accent` / `auto_surface` / `density` / `reduced_motion` / `rtl` / `corner_circular`）、`AuraConfig::default()` / `AuraConfig::light(vibrant~, corner_circular~)` / `AuraConfig::dark(...)`、`aura::aura_default_accent()` |
| 借出与读取配置 | `aura::aura_provide_config`、`aura::aura_config_of`、`aura::aura_config_signal_of`、`aura::aura_rtl`、`aura::aura_corner_circular`、`aura::aura_reduced_motion` |
| 响应式断点 | `aura::aura_bp_sm`（640pt）、`aura::aura_bp_md`（768pt）、`aura::aura_is_sm`、`aura::aura_is_md`、`aura::aura_is_coarse_pointer`、`aura::AuraDensity` |
| 主题与色板 | `aura::aura_theme`、`aura::aura_theme_from`、`aura::AuraTheme`、`aura::aura_ui_theme`、`aura::aura_palette`、`aura::AuraPalette` |
| 表面上下文 | `aura::AuraSurfaceLevel`、`aura::aura_provide_surface` / `aura::aura_provide_surface_by`、`aura::aura_surface_level_of`、`aura::aura_has_surface_context`、`aura::aura_next_surface_level`、`aura::aura_surface_bg` / `aura::aura_surface_fg`、`aura::AuraFieldVariant`、`aura::aura_field_effective_variant`、`aura::aura_field_bg` |
| 阴影与动效常量 | `aura::aura_surface_shadows`、`aura::aura_overlay_shadows`、`aura::aura_switch_thumb_shadows`、`aura::aura_button_press_scale`（`_sm` / `_lg`）、`aura::aura_close_button_press_scale`、`aura::aura_tooltip_delay_ms`、`aura::aura_tooltip_close_delay_ms`、`aura::aura_spinner_period_ms` |
| 门禁 | `aura::assert_aura_ui`、`aura::audit_aura_ui`、`aura::count_liquid_glass` |

---

## 分类索引

| # | 组件 | Builder | Pion-JSX | 源文件 |
|---|---|---|---|---|
| 1 | Surface | `aura::surface_node` | `<aura::Surface>` | `src/surface.pion` |
| 2 | Separator | `aura::separator_node` | `<aura::Separator>` | `src/surface.pion` |
| 3 | Button | `aura::button_node` | `<aura::Button>` | `src/button.pion` |
| 4 | CloseButton | `aura::close_button_node` | `<aura::CloseButton>` | `src/button.pion` |
| 5 | Spinner | `aura::spinner_node` | `<aura::Spinner>` | `src/feedback_atoms.pion` |
| 6 | ScrollShadow | `aura::scroll_shadow_node` | `<aura::ScrollShadow>` | `src/feedback_atoms.pion` |
| 7 | Label | `aura::label_node` | `<aura::Label>` | `src/text_atoms.pion` |
| 8 | Description | `aura::description_node` | `<aura::Description>` | `src/text_atoms.pion` |
| 9 | ErrorMessage | `aura::error_message_node` | `<aura::ErrorMessage>` | `src/text_atoms.pion` |
| 10 | FieldError | `aura::field_error_node` | `<aura::FieldError>` | `src/text_atoms.pion` |
| 11 | Card | `aura::card_node` | `<aura::Card>` | `src/a_card.pion` |
| 12 | Header | `aura::header_node` | `<aura::Header>` | `src/a_header_empty.pion` |
| 13 | Typography | `aura::typography_node` | `<aura::Typography>` | `src/a_typography.pion` |
| 14 | Kbd | `aura::kbd_node` | `<aura::Kbd>` | `src/a_kbd.pion` |
| 15 | EmptyState | `aura::empty_state_node` | `<aura::EmptyState>` | `src/a_header_empty.pion` |
| 16 | ButtonGroup | `aura::button_group_node` | `<aura::ButtonGroup>` | `src/a_button_group.pion` |
| 17 | ToggleButton | `aura::toggle_button_node` | `<aura::ToggleButton>` | `src/a_toggle_button.pion` |
| 18 | ToggleButtonGroup | `aura::toggle_button_group_node` | `<aura::ToggleButtonGroup>` | `src/a_toggle_button.pion` |
| 19 | Toolbar | `aura::toolbar_node` | `<aura::Toolbar>` | `src/a_toolbar.pion` |
| 20 | Link | `aura::link_node` | `<aura::Link>` | `src/a_link.pion` |
| 21 | Input | `aura::input_node` | `<aura::Input>` | `src/b_inputs.pion` |
| 22 | TextArea | `aura::textarea_node` | `<aura::TextArea>` | `src/b_inputs.pion` |
| 23 | TextField | `aura::text_field_node` | `<aura::TextField>` | `src/b_inputs.pion` |
| 24 | InputGroup | `aura::input_group_node` | `<aura::InputGroup>` | `src/b_inputs.pion` |
| 25 | SearchField | `aura::search_field_node` | `<aura::SearchField>` | `src/b_specialized_fields.pion` |
| 26 | NumberField | `aura::number_field_node` | `<aura::NumberField>` | `src/b_specialized_fields.pion` |
| 27 | InputOTP | `aura::input_otp_node` | `<aura::InputOTP>` | `src/b_specialized_fields.pion` |
| 28 | Fieldset | `aura::fieldset_node` | `<aura::Fieldset>` | `src/b_specialized_fields.pion` |
| 29 | Form | `aura::form_node` | `<aura::Form>` | `src/b_specialized_fields.pion` |
| 30 | Checkbox | `aura::checkbox_node` | `<aura::Checkbox>` | `src/b_selection.pion` |
| 31 | CheckboxGroup | `aura::checkbox_group_node` | `<aura::CheckboxGroup>` | `src/b_selection.pion` |
| 32 | Radio | `aura::radio_node` | `<aura::Radio>` | `src/b_selection.pion` |
| 33 | RadioGroup | `aura::radio_group_node` | `<aura::RadioGroup>` | `src/b_selection.pion` |
| 34 | Switch | `aura::switch_node` | `<aura::Switch>` | `src/b_selection.pion` |
| 35 | SwitchGroup | `aura::switch_group_node` | `<aura::SwitchGroup>` | `src/b_selection.pion` |
| 36 | Slider | `aura::slider_node` | `<aura::Slider>` | `src/b_range_meter.pion` |
| 37 | Meter | `aura::meter_node` | `<aura::Meter>` | `src/b_range_meter.pion` |
| 38 | Tabs | `aura::tabs_node` | `<aura::Tabs>` | `src/c_navigation.pion` |
| 39 | Accordion | `aura::accordion_node` | `<aura::Accordion>` | `src/c_navigation.pion` |
| 40 | Disclosure | `aura::disclosure_node` | `<aura::Disclosure>` | `src/c_navigation.pion` |
| 41 | DisclosureGroup | `aura::disclosure_group_node` | `<aura::DisclosureGroup>` | `src/c_navigation.pion` |
| 42 | Breadcrumbs | `aura::breadcrumbs_node` | `<aura::Breadcrumbs>` | `src/c_navigation.pion` |
| 43 | Pagination | `aura::pagination_node` | `<aura::Pagination>` | `src/c_navigation.pion` |
| 44 | ListBox | `aura::listbox_node` | `<aura::ListBox>` | `src/c_collections.pion` |
| 45 | ListBoxSection | `aura::listbox_section_node` | `<aura::ListBoxSection>` | `src/c_collections.pion` |
| 46 | ListBoxItem | `aura::listbox_item_node` | `<aura::ListBoxItem>` | `src/c_collections.pion` |
| 47 | Menu | `aura::menu_node` | `<aura::Menu>` | `src/c_collections.pion` |
| 48 | MenuSection | `aura::menu_section_node` | `<aura::MenuSection>` | `src/c_collections.pion` |
| 49 | MenuItem | `aura::menu_item_node` | `<aura::MenuItem>` | `src/c_collections.pion` |
| 50 | Dropdown | `aura::dropdown_node` | `<aura::Dropdown>` | `src/c_collections.pion` |
| 51 | Tag | `aura::tag_node` | `<aura::Tag>` | `src/c_collections.pion` |
| 52 | TagGroup | `aura::tag_group_node` | `<aura::TagGroup>` | `src/c_collections.pion` |
| 53 | Modal | `aura::modal_node` | `<aura::Modal>` | `src/c_overlays.pion` |
| 54 | AlertDialog | `aura::alert_dialog_node` | `<aura::AlertDialog>` | `src/c_overlays.pion` |
| 55 | Drawer | `aura::drawer_node` | `<aura::Drawer>` | `src/c_overlays.pion` |
| 56 | Sheet | `aura::sheet_node` | `<aura::Sheet>` | `src/c_overlays.pion` |
| 57 | Popover | `aura::popover_node` | `<aura::Popover>` | `src/c_overlays.pion` |
| 58 | Tooltip | `aura::tooltip_node` | `<aura::Tooltip>` | `src/c_overlays.pion` |
| 59 | Alert | `aura::alert_node` | `<aura::Alert>` | `src/d_feedback.pion` |
| 60 | Badge | `aura::badge_node` | `<aura::Badge>` | `src/d_feedback.pion` |
| 61 | Chip | `aura::chip_node` | `<aura::Chip>` | `src/d_feedback.pion` |
| 62 | Avatar | `aura::avatar_node` | `<aura::Avatar>` | `src/d_feedback.pion` |
| 63 | AvatarGroup | `aura::avatar_group_node` | `<aura::AvatarGroup>` | `src/d_feedback.pion` |
| 64 | ProgressBar | `aura::progress_bar_node` | `<aura::ProgressBar>` | `src/d_feedback.pion` |
| 65 | ProgressCircle | `aura::progress_circle_node` | `<aura::ProgressCircle>` | `src/d_feedback.pion` |
| 66 | Skeleton | `aura::skeleton_node` | `<aura::Skeleton>` | `src/d_feedback.pion` |
| 67 | ColorSwatch | `aura::color_swatch_node` | `<aura::ColorSwatch>` | `src/d_color.pion` |
| 68 | ColorSwatchPicker | `aura::color_swatch_picker_node` | `<aura::ColorSwatchPicker>` | `src/d_color.pion` |
| 69 | ColorArea | `aura::color_area_node` | `<aura::ColorArea>` | `src/d_color.pion` |
| 70 | ColorSlider | `aura::color_slider_node` | `<aura::ColorSlider>` | `src/d_color.pion` |
| 71 | ColorInputGroup | `aura::color_input_group_node` | `<aura::ColorInputGroup>` | `src/d_color.pion` |
| 72 | ColorField | `aura::color_field_node` | `<aura::ColorField>` | `src/d_color.pion` |
| 73 | Calendar | `aura::calendar_node` | `<aura::Calendar>` | `src/d_datetime.pion` |
| 74 | RangeCalendar | `aura::range_calendar_node` | `<aura::RangeCalendar>` | `src/d_datetime.pion` |
| 75 | CalendarYearPicker | `aura::calendar_year_picker_node` | `<aura::CalendarYearPicker>` | `src/d_datetime.pion` |
| 76 | DateField | `aura::date_field_node` | `<aura::DateField>` | `src/d_datetime.pion` |
| 77 | TimeField | `aura::time_field_node` | `<aura::TimeField>` | `src/d_datetime.pion` |
| 78 | DateInputGroup | `aura::date_input_group_node` | `<aura::DateInputGroup>` | `src/d_datetime.pion` |
| 79 | Select | `aura::select_node` | `<aura::Select>` | `src/x_select.pion` |
| 80 | ComboBox | `aura::combo_box_node` | `<aura::ComboBox>` | `src/x_combobox.pion` |
| 81 | Autocomplete | `aura::autocomplete_node` | `<aura::Autocomplete>` | `src/x_autocomplete.pion` |
| 82 | ColorPicker | `aura::color_picker_node` | `<aura::ColorPicker>` | `src/x_color_picker.pion` |
| 83 | DatePicker | `aura::date_picker_node` | `<aura::DatePicker>` | `src/x_date_picker.pion` |
| 84 | DateRangePicker | `aura::date_range_picker_node` | `<aura::DateRangePicker>` | `src/x_date_picker.pion` |
| 85 | Table | `aura::table_node` | `<aura::Table>` | `src/x_table.pion` |
| 86 | Toast | `aura::toast_node` | `<aura::Toast>` | `src/x_toast.pion` |

---

## 常见需求 → 对应 `aura::*` 组件（严禁手搓）

| 常见业务需求 | 必须使用的 `aura::*` 组件 | 禁止的做法 |
|---|---|---|
| 页面 / 区块底色、三层纸面 | `aura::Surface`（`Default`/`Secondary`/`Tertiary`/`Transparent`）+ `AuraConfig.auto_surface` | 硬编码底色；在纸面里再手写一层同色背景 |
| 内容卡片（头 / 体 / 尾） | `aura::Card` + `CardHeader` / `CardTitle` / `CardDescription` / `CardContent` / `CardFooter` | 手拼阴影与圆角 |
| 主 / 次 / 危险操作按钮、加载中按钮 | `aura::Button`（`variant`、`size`、`pending`） | 自己画按压缩放、自己换 Spinner |
| 拼接按钮条 / 切换按钮组 | `aura::ButtonGroup` + `ButtonGroupItem` / `aura::ToggleButtonGroup` + `ToggleButton` | 手算外圆角与分割线 |
| 工具栏 | `aura::Toolbar` | 多个按钮硬贴、焦点不漫游 |
| 文本输入（带标签、说明、错误） | `aura::TextField`（简单场景 `aura::Input` / `aura::TextArea`） | `ui::field` 外手套边框与焦点环 |
| 带前后缀的输入、搜索、数字、验证码 | `aura::InputGroup` / `aura::SearchField` / `aura::NumberField` / `aura::InputOTP` | 手写步进按钮与粘贴校验 |
| 表单分组、整表提交与校验 | `aura::Form` + `aura::Fieldset` | 逐字段手动传 `disabled` 与错误 |
| 复选 / 单选 / 开关 | `aura::Checkbox`(`Group`) / `aura::Radio` + `RadioGroup` / `aura::Switch`(`Group`) | 自绘勾选框或开关动画 |
| 数值滑块 / 区间 / 计量 | `aura::Slider` / `aura::RangeSlider` / `aura::Meter` | 手写拖拽命中与 RTL 方向 |
| 下拉选择、可输入筛选、自动补全 | `aura::Select` / `aura::ComboBox` / `aura::Autocomplete`（`AuraSelectOption`） | 按钮 + 自绘弹出列表 |
| 选项列表、命令菜单、下拉菜单 | `aura::ListBox` / `aura::Menu` / `aura::Dropdown` | 手写键盘漫游与 Typeahead |
| 标签页、手风琴、可折叠区块 | `aura::Tabs` / `aura::Accordion` / `aura::Disclosure`(`Group`) | 手写选中滑块动画 |
| 面包屑、分页 | `aura::Breadcrumbs` / `aura::Pagination` | 手算省略号窗口 |
| 模态框、确认框、抽屉、Sheet | `aura::Modal` / `aura::AlertDialog` / `aura::Drawer` / `aura::Sheet` | 手写遮罩、Escape 关闭与焦点归还 |
| 气泡浮层、悬浮提示 | `aura::Popover` / `aura::Tooltip` | 绝对定位手绘气泡 |
| 行内提示、徽标、状态芯片、标签 | `aura::Alert` / `aura::Badge` + `BadgeAnchor` / `aura::Chip` / `aura::Tag` + `TagGroup` | 硬编码状态色 |
| 头像、头像堆叠 | `aura::Avatar` / `aura::AvatarGroup` | 手算重叠与 `+N` |
| 进度、加载、骨架 | `aura::ProgressBar` / `aura::ProgressCircle` / `aura::Spinner` / `aura::Skeleton`(`Group`) | 逐帧计数器驱动的动画 |
| 应用级通知 | `aura::ToastQueue` + `aura::ToastProvider`（单条 `aura::Toast`） | 自己维护计时器与堆叠 |
| 数据表格（排序、多选、列宽） | `aura::Table` + `TableContent` / `TableColumn` / `TableRow` / `TableCell` | `Col` + `Row` 拼表格 |
| 取色 | `aura::ColorPicker`（零件 `ColorArea` / `ColorSlider` / `ColorField` / `ColorSwatchPicker`） | CPU 逐像素生成渐变图 |
| 日期、时间、区间 | `aura::DatePicker` / `aura::DateRangePicker`（零件 `Calendar` / `RangeCalendar` / `DateField` / `TimeField`） | 文本框里手写日期解析 |
| 空状态、分区标题、键帽、链接、排版 | `aura::EmptyState` / `aura::Header` / `aura::Kbd` / `aura::Link` / `aura::Typography` | 裸 `ui::Text` 硬编码字号颜色 |
| 滚区边缘渐隐 | `aura::ScrollShadow` | 叠渐变图片遮罩 |
| 单测 UI 门禁 | `aura::assert_aura_ui(t, label="...")`（`aura::audit_aura_ui` 取故障列表） | 肉眼看图代替门禁 |

---

## 一、基础原子与纸面（Foundations）

### 1. `aura::surface_node` / `<aura::Surface>`
- **适用场景**：三层实色纸面容器（`Default`/`Secondary`/`Tertiary`/`Transparent`），向子树借出 `SurfaceContext`；`Transparent` 动态继承外层级别，`AuraConfig.auto_surface = true` 且未传 `variant` 时自动下沉一级。默认无阴影，`shadow=true` 得到浅色 3 层微阴影（暗色无外阴影）。
- **核心参数**：可选 `variant?` `radius?` `pad~` `gap~` `shadow~` `bordered~` `content~`

### 2. `aura::separator_node` / `<aura::Separator>`
- **适用场景**：横/竖分割线（`AuraOrientation`），三档 `AuraSeparatorVariant` 对应 `separator` / `separator_secondary` / `separator_tertiary` 语义色。
- **核心参数**：可选 `orientation~` `variant~`

### 3. `aura::button_node` / `<aura::Button>`
- **适用场景**：主按钮：7 种 `AuraButtonVariant`（`Primary`/`Secondary`/`Tertiary`/`Ghost`/`Outline`/`Danger`/`DangerSoft`）× 3 尺寸，`pending` 自动换入 `Spinner` 并锁交互，按压闭合形式缩放（0.97 / Sm 0.98 / Lg 0.96），焦点仅 focus-visible 显示 2pt 间隙 + 2pt 外环。`corners` 供拼接组自定义四角。
- **核心参数**：必填 `label : view::Live[String]`；可选 `on_press~` `variant~` `size~` `icon_only~` `full_width~` `disabled~` `pending~` `corners?` `content?`

### 4. `aura::close_button_node` / `<aura::CloseButton>`
- **适用场景**：关闭叉号按钮（按压 0.93），用于浮层、Alert、Toast 等关闭位。
- **核心参数**：可选 `on_press~` `label~` `size~` `disabled~`

### 5. `aura::spinner_node` / `<aura::Spinner>`
- **适用场景**：圆形加载指示（`Sm` 16 / `Md` 24 / `Lg` 32 / `Xl` 40pt），矢量路径 + 形状局部 GPU 线性渐变，旋转零接缝；`reduced_motion` 下静止。
- **核心参数**：可选 `size~` `color~` `ink?` `label~`

### 6. `aura::scroll_shadow_node` / `<aura::ScrollShadow>`
- **适用场景**：给 `view::Scroll` 滚区加边缘渐隐阴影（`vertical` / `shadow_size` / `threshold`），只订阅实际滚动位置。
- **核心参数**：必填 `scroll : view::Scroll`；可选 `vertical~` `shadow_size~` `threshold~` `content~`

### 7. `aura::label_node` / `<aura::Label>`
- **适用场景**：表单标签（14pt Medium，`required=true` 追加 danger 星号）。
- **核心参数**：必填 `text : view::Live[String]`；可选 `required~` `disabled~`

### 8. `aura::description_node` / `<aura::Description>`
- **适用场景**：表单辅助说明文字（12pt muted）。
- **核心参数**：必填 `text : view::Live[String]`；可选 `wrap~`

### 9. `aura::error_message_node` / `<aura::ErrorMessage>`
- **适用场景**：常显的行内错误文字（12pt danger）。
- **核心参数**：必填 `text : view::Live[String]`；可选 `wrap~`

### 10. `aura::field_error_node` / `<aura::FieldError>`
- **适用场景**：条件错误文字：仅 `visible` 为真且文案非空时占位渲染，适合校验结果。
- **核心参数**：必填 `text : view::Live[String]`；可选 `visible~` `wrap~`

---

## 二、布局、排版与操作（Layout, Typography & Actions）

### 11. `aura::card_node` / `<aura::Card>`
- **适用场景**：内容卡片（默认 `Default` 纸面 + 3 层微阴影，`pad=16`、`gap=12`），配合具名子部件组成头/体/尾。
- **核心参数**：可选 `variant~` `radius?` `pad~` `gap~` `shadow~` `bordered~` `content~`
- **子部件**：`aura::card_header_node` / `<aura::CardHeader>`、`aura::card_title_node` / `<aura::CardTitle>`、`aura::card_description_node` / `<aura::CardDescription>`、`aura::card_content_node` / `<aura::CardContent>`、`aura::card_footer_node` / `<aura::CardFooter>`

### 12. `aura::header_node` / `<aura::Header>`
- **适用场景**：列表/菜单分区小标题（12pt Medium muted）。
- **核心参数**：必填 `text : view::Live[String]`；可选 `content?`

### 13. `aura::typography_node` / `<aura::Typography>`
- **适用场景**：排版层级 `AuraTypographyType`（`H1`..`H6`/`Body`/`BodySm`/`BodyXs`/`Muted`/`Small`/`Code`），可调颜色、对齐、字重、单行截断、等宽。
- **核心参数**：必填 `text_val : view::Live[String]`；可选 `kind~` `color?` `align~` `weight~` `truncate~` `wrap~` `mono~`

### 14. `aura::kbd_node` / `<aura::Kbd>`
- **适用场景**：键帽（`Default`/`Light`），`KbdAbbr` 绘制修饰键矢量符号（`AuraKbdKey`，跨端确定性，不依赖字体），`KbdContent` 放文字键。
- **核心参数**：可选 `variant~` `content~`
- **子部件**：`aura::kbd_abbr_node` / `<aura::KbdAbbr>`、`aura::kbd_content_node` / `<aura::KbdContent>`

### 15. `aura::empty_state_node` / `<aura::EmptyState>`
- **适用场景**：空状态占位（标题、描述、图标、操作或自定义内容，可居中）。
- **核心参数**：可选 `title?` `description?` `icon?` `action?` `pad~` `gap~` `centered~` `content?`

### 16. `aura::button_group_node` / `<aura::ButtonGroup>`
- **适用场景**：拼接按钮组（横/竖），由 `ButtonGroupItem` 按 `index`/`total` 自动算外圆角与分割线。
- **核心参数**：可选 `label~` `orientation~` `variant~` `size~` `full_width~` `hide_separator~` `disabled~` `content~`
- **子部件**：`aura::button_group_item_node` / `<aura::ButtonGroupItem>`、`aura::button_group_separator_node` / `<aura::ButtonGroupSeparator>`

### 17. `aura::toggle_button_node` / `<aura::ToggleButton>`
- **适用场景**：可选中按钮（`selected` + `on_change`，`Default`/`Ghost`）。
- **核心参数**：必填 `label : view::Live[String]`、`selected : () -> Bool`；可选 `on_change~` `variant~` `size~` `icon_only~` `full_width~` `disabled~` `corners?` `content?`

### 18. `aura::toggle_button_group_node` / `<aura::ToggleButtonGroup>`
- **适用场景**：单选/多选切换按钮组（`AuraSelectionMode`），`content` 回调拿到 `AuraToggleGroup[K]` 挂子按钮，支持 `detached`、换行与 RTL。
- **核心参数**：必填 `label : view::Live[String]`、`is_on : (K) -> Bool`、`on_change : (K, Bool) -> Unit`、`content : (view::View, AuraToggleGroup[K]) -> Unit`；可选 `mode~` `orientation~` `variant~` `size~` `detached~` `full_width~` `hide_separator~` `wraps~` `rtl~` `disabled~`
- **子部件**：`aura::toggle_button_group_separator_node` / `<aura::ToggleButtonGroupSeparator>`

### 19. `aura::toolbar_node` / `<aura::Toolbar>`
- **适用场景**：工具栏容器（横/竖、`attached` 拼接、换行、RTL），方向键在子控件间漫游焦点。
- **核心参数**：可选 `label~` `orientation~` `attached~` `gap~` `wraps~` `rtl~` `disabled~` `content~`

### 20. `aura::link_node` / `<aura::Link>`
- **适用场景**：文字链接（`href`、`external` 时附 `LinkIcon` 外链图标、下划线、禁用）。
- **核心参数**：必填 `label : view::Live[String]`；可选 `on_press~` `href~` `external~` `underline~` `font_size~` `line_height~` `disabled~` `content?`
- **子部件**：`aura::link_icon_node` / `<aura::LinkIcon>`

---

## 三、表单输入与选择控件（Forms & Selection）

### 21. `aura::input_node` / `<aura::Input>`
- **适用场景**：单行输入框（`ui::Field` 驱动，`Primary`/`Secondary`/`Auto` 底色随 `SurfaceContext`，`invalid`/`read_only`/`secret`）。
- **核心参数**：可选 `field?` `value~` `placeholder~` `label~` `variant~` `full_width~` `invalid~` `disabled~` `read_only~` `secret~` `height?` `pad_x~` `on_change~` `on_submit~`

### 22. `aura::textarea_node` / `<aura::TextArea>`
- **适用场景**：多行输入（`rows`/`max_rows` 自动增高）。
- **核心参数**：可选 `field?` `value~` `placeholder~` `label~` `variant~` `full_width~` `rows~` `max_rows~` `min_height?` `invalid~` `disabled~` `read_only~` `on_change~` `on_submit~`

### 23. `aura::text_field_node` / `<aura::TextField>`
- **适用场景**：完整字段：`Label` + 输入 + `Description` + `FieldError`，`validate` 回调、`required`、`multiline`；设置 `name` 后自动注册到外层 `Form` 接收错误路由，并继承外层 `Form`/`Fieldset` 的禁用。
- **核心参数**：可选 `label~` `name~` `field?` `value~` `placeholder~` `description~` `error~` `variant~` `multiline~` `rows~` `full_width~` `required~` `invalid~` `disabled~` `read_only~` `secret~` `validate?` `on_change~` `on_submit~` `content?`

### 24. `aura::input_group_node` / `<aura::InputGroup>`
- **适用场景**：带前后缀的输入组（整组共享一个焦点环）。
- **核心参数**：可选 `variant~` `full_width~` `invalid~` `disabled~` `height?` `content~`
- **子部件**：`aura::input_group_prefix_node` / `<aura::InputGroupPrefix>`、`aura::input_group_input_node` / `<aura::InputGroupInput>`、`aura::input_group_suffix_node` / `<aura::InputGroupSuffix>`

### 25. `aura::search_field_node` / `<aura::SearchField>`
- **适用场景**：搜索框（放大镜图标 + 一键清空 `on_clear`）。
- **核心参数**：可选 `field?` `value~` `label~` `placeholder~` `description~` `error~` `variant~` `full_width~` `invalid~` `disabled~` `read_only~` `on_change~` `on_submit~` `on_clear~`

### 26. `aura::number_field_node` / `<aura::NumberField>`
- **适用场景**：数字输入（`min`/`max`/`step`/`large_step`/`precision`，`AuraNumberFormat` 格式化，+/- 步进按钮、方向键与 PageUp/PageDown 大步进）。
- **核心参数**：必填 `value : view::Live[Double]`、`on_change : (Double) -> Unit`；可选 `label~` `description~` `error~` `min~` `max~` `step~` `large_step~` `precision~` `format_preset~` `format?` `parse?` `variant~` `full_width~` `invalid~` `disabled~` `read_only~`

### 27. `aura::input_otp_node` / `<aura::InputOTP>`
- **适用场景**：一次性验证码（单一 `ui::Field` 驱动多格，`allow` 字符集、粘贴校验、`on_complete`）。
- **核心参数**：可选 `max_length~` `value~` `field?` `allow~` `label~` `variant~` `invalid~` `disabled~` `on_change~` `on_complete~` `content?`
- **子部件**：`aura::input_otp_group_node` / `<aura::InputOTPGroup>`、`aura::input_otp_slot_node` / `<aura::InputOTPSlot>`、`aura::input_otp_separator_node` / `<aura::InputOTPSeparator>`

### 28. `aura::fieldset_node` / `<aura::Fieldset>`
- **适用场景**：字段分组（`disabled` 向内继承），带图例、说明、组与操作行子部件。
- **核心参数**：可选 `label~` `disabled~` `gap~` `content~`
- **子部件**：`aura::fieldset_legend_node` / `<aura::FieldsetLegend>`、`aura::fieldset_description_node` / `<aura::FieldsetDescription>`、`aura::fieldset_group_node` / `<aura::FieldsetGroup>`、`aura::fieldset_actions_node` / `<aura::FieldsetActions>`

### 29. `aura::form_node` / `<aura::Form>`
- **适用场景**：表单（`on_submit`、`validate_form` 整表校验并把错误路由到各字段，`disabled` 向内继承；`aura_form_of` 读取上下文）。
- **核心参数**：可选 `label~` `on_submit~` `validate_form?` `disabled~` `gap~` `content~`

### 30. `aura::checkbox_node` / `<aura::Checkbox>`
- **适用场景**：复选框（`checked`/`indeterminate`、说明与错误、`stacked` 布局）。
- **核心参数**：可选 `label~` `value~` `checked~` `indeterminate~` `on_change~` `description~` `error~` `variant?` `invalid~` `disabled~` `stacked~`

### 31. `aura::checkbox_group_node` / `<aura::CheckboxGroup>`
- **适用场景**：复选框组（`selected` 键集 + `on_change`）。
- **核心参数**：可选 `label~` `selected~` `on_change~` `description~` `error~` `variant~` `invalid~` `disabled~` `gap~` `content~`

### 32. `aura::radio_node` / `<aura::Radio>`
- **适用场景**：单选项（放在 `RadioGroup` 内，`value` 为键）。
- **核心参数**：必填 `value : String`；可选 `label~` `selected~` `on_select~` `description~` `variant?` `invalid~` `disabled~` `stacked~`

### 33. `aura::radio_group_node` / `<aura::RadioGroup>`
- **适用场景**：单选组（横/竖，方向键循环选中，RTL 镜像）。
- **核心参数**：必填 `value : view::Live[String]`、`on_change : (String) -> Unit`；可选 `label~` `description~` `error~` `orientation~` `variant~` `invalid~` `disabled~` `gap~` `content~`

### 34. `aura::switch_node` / `<aura::Switch>`
- **适用场景**：开关（`Sm`/`Md`/`Lg`），滑块弹簧 + 轨道颜色闭合形式插值，`reduced_motion` 下瞬切。
- **核心参数**：必填 `checked : view::Live[Bool]`、`on_change : (Bool) -> Unit`；可选 `label~` `description~` `size~` `disabled~` `stacked~`

### 35. `aura::switch_group_node` / `<aura::SwitchGroup>`
- **适用场景**：开关分组（标题、说明、错误、横/竖排列）。
- **核心参数**：可选 `label~` `description~` `error~` `orientation~` `disabled~` `gap~` `item_gap~` `content~`

### 36. `aura::slider_node` / `<aura::Slider>`
- **适用场景**：滑块（`min`/`max`/`step`、刻度 `AuraSliderMark`、输出文字）；双端区间用 `range_slider_node` / `<aura::RangeSlider>`（`min_gap`），RTL 指针与键盘方向镜像。
- **核心参数**：必填 `value : view::Live[Double]`、`on_change : (Double) -> Unit`；可选 `label~` `min~` `max~` `step~` `show_output~` `output_text?` `marks~` `disabled~`
- **子部件**：`aura::range_slider_node` / `<aura::RangeSlider>`

### 37. `aura::meter_node` / `<aura::Meter>`
- **适用场景**：计量条（`AuraMeterColor` × `AuraMeterSize`，只读数值，区别于进度）。
- **核心参数**：必填 `value : view::Live[Double]`；可选 `label~` `min~` `max~` `color~` `size~` `show_output~` `value_text?` `disabled~`

---

## 四、导航、集合与浮层（Navigation, Collections & Overlays）

### 38. `aura::tabs_node` / `<aura::Tabs>`
- **适用场景**：标签页（`AuraTabItem` 数组，`Primary` 胶囊滑块 / `Secondary` 下划线，横/竖，RTL 方向键镜像，可只渲染标签条 `show_panels=false`）。
- **核心参数**：必填 `items : Array[AuraTabItem]`、`selected : () -> String`、`on_select : (String) -> Unit`；可选 `variant~` `orientation~` `label~` `disabled~` `panel_gap~` `show_panels~`

### 39. `aura::accordion_node` / `<aura::Accordion>`
- **适用场景**：手风琴（`AuraAccordionItem` 数组，`multiple` 多开，`Default`/`Surface`）。
- **核心参数**：必填 `items : Array[AuraAccordionItem]`、`expanded_keys : () -> Array[String]`、`on_toggle : (String) -> Unit`；可选 `variant~` `multiple~` `hide_separator~` `disabled~`

### 40. `aura::disclosure_node` / `<aura::Disclosure>`
- **适用场景**：单个可展开区块（受控 `open` + `on_toggle`）。
- **核心参数**：必填 `heading : view::Live[String]`、`open : () -> Bool`、`on_toggle : () -> Unit`、`content : (view::View) -> Unit`；可选 `disabled~` `show_indicator~`

### 41. `aura::disclosure_group_node` / `<aura::DisclosureGroup>`
- **适用场景**：多个 Disclosure 的组（共享展开键集）。
- **核心参数**：必填 `items : Array[AuraAccordionItem]`、`expanded_keys : () -> Array[String]`、`on_toggle : (String) -> Unit`；可选 `multiple~` `disabled~`

### 42. `aura::breadcrumbs_node` / `<aura::Breadcrumbs>`
- **适用场景**：面包屑（`AuraBreadcrumbItem` 数组，末项为当前页，RTL 分隔符镜像）。
- **核心参数**：必填 `items : Array[AuraBreadcrumbItem]`；可选 `label~`

### 43. `aura::pagination_node` / `<aura::Pagination>`
- **适用场景**：分页（`siblings`/`boundaries` 动态窗口与省略号，`summary` 自定义摘要）。
- **核心参数**：必填 `total : () -> Int`、`current : () -> Int`、`on_change : (Int) -> Unit`；可选 `size~` `siblings~` `boundaries~` `disabled~` `label~` `summary?`

### 44. `aura::listbox_node` / `<aura::ListBox>`
- **适用场景**：选项列表（单/多选，`content` 回调拿到 `headless::Listbox[String]` 挂分区与项，可放进浮层 `in_overlay`）。
- **核心参数**：必填 `is_selected : (String) -> Bool`、`on_select : (String) -> Unit`、`content : (view::View, headless::Listbox[String]) -> Unit`；可选 `label~` `multiple~` `in_overlay~` `disabled~`

### 45. `aura::listbox_section_node` / `<aura::ListBoxSection>`
- **适用场景**：ListBox 分区（标题 + 子项）。
- **核心参数**：必填 `lb : headless::Listbox[String]`、`title : String`、`content : (view::View) -> Unit`

### 46. `aura::listbox_item_node` / `<aura::ListBoxItem>`
- **适用场景**：ListBox 选项（描述、图标、`Danger` 变体、选中指示）。
- **核心参数**：必填 `lb : headless::Listbox[String]`、`key : String`、`label : String`；可选 `description?` `icon?` `variant~` `disabled~` `show_indicator~` `plain_text~`

### 47. `aura::menu_node` / `<aura::Menu>`
- **适用场景**：命令菜单（`content` 回调拿到 `headless::Menu`，键盘漫游、Typeahead，可嵌入 Popover）。
- **核心参数**：必填 `content : (view::View, headless::Menu) -> Unit`；可选 `label~` `on_close?` `in_popover~` `min_width~` `disabled~`

### 48. `aura::menu_section_node` / `<aura::MenuSection>`
- **适用场景**：Menu 分区。
- **核心参数**：必填 `m : headless::Menu`、`title : String`、`content : (view::View) -> Unit`

### 49. `aura::menu_item_node` / `<aura::MenuItem>`
- **适用场景**：Menu 项（`on_action`、快捷键提示、描述、子菜单箭头、`Danger`）。
- **核心参数**：必填 `m : headless::Menu`、`label : String`；可选 `on_action~` `close_on_select~` `icon?` `shortcut?` `description?` `variant~` `disabled~` `has_submenu~`

### 50. `aura::dropdown_node` / `<aura::Dropdown>`
- **适用场景**：触发按钮 + 下拉 Menu 一体（受控 `open`/`set_open`，`placement`）。
- **核心参数**：必填 `trigger_label : String`、`open : () -> Bool`、`set_open : (Bool) -> Unit`、`menu_content : (view::View, headless::Menu) -> Unit`；可选 `trigger_variant~` `placement~` `gap~` `min_width~` `disabled~`

### 51. `aura::tag_node` / `<aura::Tag>`
- **适用场景**：标签（`Default`/`Surface` × 3 尺寸，可移除）。
- **核心参数**：必填 `tg : headless::TagGroup[String]`、`key : String`、`label : String`；可选 `variant~` `size~` `removable~` `disabled~`

### 52. `aura::tag_group_node` / `<aura::TagGroup>`
- **适用场景**：标签组（可选中、`on_remove`、Backspace/Delete 移除并转移焦点）。
- **核心参数**：必填 `content : (view::View, headless::TagGroup[String]) -> Unit`；可选 `label?` `description?` `error_message?` `is_selected?` `on_select?` `on_remove?` `multiple~` `disabled~`

### 53. `aura::modal_node` / `<aura::Modal>`
- **适用场景**：模态对话框（`Opaque`/`Blur`/`Transparent` 遮罩 × `AuraModalSize`，挂到窗口根层并继承 `AuraConfig`，Escape/遮罩关闭，退场首帧即释放交互）。
- **核心参数**：必填 `open : () -> Bool`、`on_close : () -> Unit`；可选 `heading~` `backdrop~` `size~` `width?` `show_close_button~` `exiting~` `body~` `footer?`

### 54. `aura::alert_dialog_node` / `<aura::AlertDialog>`
- **适用场景**：警示确认框（`Role::AlertDialog`，`AuraAlertIconVariant` 语义图标）。
- **核心参数**：必填 `open : () -> Bool`、`on_close : () -> Unit`、`heading : String`、`description : String`；可选 `icon_variant~` `icon?` `backdrop~` `size~` `width?` `exiting~` `footer?`

### 55. `aura::drawer_node` / `<aura::Drawer>`
- **适用场景**：抽屉（`AuraDrawerPlacement` 四边，可选拖拽手柄与关闭钮）。
- **核心参数**：必填 `open : () -> Bool`、`on_close : () -> Unit`；可选 `heading~` `placement~` `backdrop~` `show_handle~` `show_close_button~` `panel_width~` `exiting~` `body~` `footer?`

### 56. `aura::sheet_node` / `<aura::Sheet>`
- **适用场景**：底/侧 Sheet（与 Drawer 共用无头行为，独立视觉）。
- **核心参数**：必填 `open : () -> Bool`、`on_close : () -> Unit`；可选 `heading~` `placement~` `backdrop~` `show_handle~` `panel_width~` `exiting~` `body~` `footer?`

### 57. `aura::popover_node` / `<aura::Popover>`
- **适用场景**：气泡浮层（锚点定位、视口内实时翻边、可选箭头）。
- **核心参数**：必填 `trigger_label : String`、`open : () -> Bool`、`set_open : (Bool) -> Unit`、`content : (view::View) -> Unit`；可选 `heading?` `trigger_variant~` `placement~` `gap~` `width~` `show_arrow~` `disabled~`

### 58. `aura::tooltip_node` / `<aura::Tooltip>`
- **适用场景**：悬浮提示（`open_delay_ms` 默认 1500 / `close_delay_ms` 默认 500，`trigger` 回调返回锚点视图）。
- **核心参数**：必填 `text : view::Live[String]`、`trigger : (view::View) -> view::View`；可选 `placement~` `gap~` `show_arrow~` `open_delay_ms~` `close_delay_ms~` `disabled~`

---

## 五、反馈、数据展示、色彩与日期时间（Feedback, Data, Color & Date/Time）

### 59. `aura::alert_node` / `<aura::Alert>`
- **适用场景**：行内提示（`AuraAlertStatus` 5 态，指示图标、标题、描述、操作位），亦可用子部件自由拼装。
- **核心参数**：可选 `status~` `title?` `description?` `show_indicator~` `icon?` `action?` `content?`
- **子部件**：`aura::alert_indicator_node` / `<aura::AlertIndicator>`、`aura::alert_content_node` / `<aura::AlertContent>`、`aura::alert_title_node` / `<aura::AlertTitle>`、`aura::alert_description_node` / `<aura::AlertDescription>`

### 60. `aura::badge_node` / `<aura::Badge>`
- **适用场景**：徽标（`Primary`/`Secondary`/`Soft`、圆点、描边），用 `BadgeAnchor` 包住目标后按 `AuraBadgePlacement` 角标定位。
- **核心参数**：可选 `label~` `variant~` `color~` `size~` `dot~` `bordered~` `placement?`
- **子部件**：`aura::badge_anchor_node` / `<aura::BadgeAnchor>`

### 61. `aura::chip_node` / `<aura::Chip>`
- **适用场景**：状态芯片（`AuraChipVariant` × `AuraStatusColor` × 3 尺寸，前后插槽）。
- **核心参数**：必填 `label : view::Live[String]`；可选 `variant~` `color~` `size~` `leading?` `trailing?`

### 62. `aura::avatar_node` / `<aura::Avatar>`
- **适用场景**：头像（图片 `src` 或首字母 `fallback`，`Default`/`Soft`）。
- **核心参数**：可选 `fallback~` `src?` `alt~` `size~` `color~` `variant~`

### 63. `aura::avatar_group_node` / `<aura::AvatarGroup>`
- **适用场景**：头像堆叠（`max` 截断 + `+N`，`overlap` 与接缝色）。
- **核心参数**：必填 `items : Array[AuraAvatarItem]`；可选 `max~` `size~` `overlap~` `seam_color?`

### 64. `aura::progress_bar_node` / `<aura::ProgressBar>`
- **适用场景**：进度条（`value` 返回 `None` 为不定态动画）。
- **核心参数**：必填 `value : () -> Option[Double]`；可选 `min~` `max~` `label~` `show_output~` `value_text?` `size~` `color~` `disabled~`

### 65. `aura::progress_circle_node` / `<aura::ProgressCircle>`
- **适用场景**：环形进度（同上，不定态旋转）。
- **核心参数**：必填 `value : () -> Option[Double]`；可选 `min~` `max~` `label~` `size~` `color~` `disabled~`

### 66. `aura::skeleton_node` / `<aura::Skeleton>`
- **适用场景**：骨架占位（`Shimmer`/`Pulse`/`None`，单矩形 GPU 渐变扫光），`SkeletonGroup` 让组内扫光同相位。
- **核心参数**：可选 `width?` `height~` `radius~` `animation~` `period_ms~`
- **子部件**：`aura::skeleton_group_node` / `<aura::SkeletonGroup>`

### 67. `aura::color_swatch_node` / `<aura::ColorSwatch>`
- **适用场景**：色块（`Xs`..`Xl` × `Circle`/`Square`，透明色带棋盘格）。
- **核心参数**：必填 `color_val : view::Live[Int64]`；可选 `size~` `shape~` `label~`

### 68. `aura::color_swatch_picker_node` / `<aura::ColorSwatchPicker>`
- **适用场景**：预设色板单选（`colors` 为 hex 字符串，`columns` 网格，方向键漫游）。
- **核心参数**：必填 `colors : Array[String]`、`selected : () -> Option[String]`、`on_select : (String) -> Unit`；可选 `size~` `shape~` `columns~` `gap~` `label~` `disabled~`

### 69. `aura::color_area_node` / `<aura::ColorArea>`
- **适用场景**：二维取色面（`x_channel`/`y_channel` 任选 `headless_core::ColorChannel`；HSV/HSL 走代数分层纯 GPU 渐变，零 CPU 像素栅格化）。
- **核心参数**：必填 `model : () -> headless_core::ColorModel`、`on_change : (headless_core::ColorModel) -> Unit`；可选 `x_channel~` `y_channel~` `size~` `label~` `disabled~`

### 70. `aura::color_slider_node` / `<aura::ColorSlider>`
- **适用场景**：单通道色条（色相/饱和/亮度/RGB/Alpha，Alpha 带棋盘格）。
- **核心参数**：必填 `channel : headless_core::ColorChannel`、`model : () -> headless_core::ColorModel`、`on_change : (headless_core::ColorModel) -> Unit`；可选 `label~` `show_output~` `output_text?` `disabled~`

### 71. `aura::color_input_group_node` / `<aura::ColorInputGroup>`
- **适用场景**：颜色文本输入组（前后缀插槽，常放色块预览）。
- **核心参数**：必填 `field : ui::Field`；可选 `variant~` `placeholder~` `invalid~` `disabled~` `prefix?` `suffix?` `on_edited?` `on_submitted?` `label~`

### 72. `aura::color_field_node` / `<aura::ColorField>`
- **适用场景**：Hex 颜色字段（解析、失焦归一、`include_alpha`、`show_swatch`）。
- **核心参数**：必填 `model : () -> headless_core::ColorModel`、`on_change : (headless_core::ColorModel) -> Unit`；可选 `label~` `description~` `error_message~` `placeholder~` `show_swatch~` `include_alpha~` `variant~` `invalid~` `disabled~` `field_ref?`

### 73. `aura::calendar_node` / `<aura::Calendar>`
- **适用场景**：单日日历（`min`/`max`/`is_unavailable`、`first_day_of_week`、年份选择器）。
- **核心参数**：必填 `initial_focus : headless_core::IsoDate`、`selected : () -> Option[headless_core::IsoDate]`、`on_select : (headless_core::IsoDate) -> Unit`；可选 `today~` `min?` `max?` `is_unavailable~` `first_day_of_week~` `show_year_picker~` `width~` `label~` `disabled~`

### 74. `aura::range_calendar_node` / `<aura::RangeCalendar>`
- **适用场景**：区间日历（两次点击确定起止）。
- **核心参数**：必填 `initial_focus : headless_core::IsoDate`、`range : () -> Option[(headless_core::IsoDate, headless_core::IsoDate)]`、`on_change : (headless_core::IsoDate, headless_core::IsoDate) -> Unit`；可选 `today~` `min?` `max?` `is_unavailable~` `first_day_of_week~` `width~` `label~` `disabled~`

### 75. `aura::calendar_year_picker_node` / `<aura::CalendarYearPicker>`
- **适用场景**：年份网格选择（`page_size` 分页，`min_year`/`max_year`）。
- **核心参数**：必填 `selected_year : () -> Int`、`on_select : (Int) -> Unit`；可选 `page_size~` `min_year?` `max_year?` `width~` `label~` `disabled~`

### 76. `aura::date_field_node` / `<aura::DateField>`
- **适用场景**：分段日期（时间）输入（`granularity`、12/24 小时制、`AuraDateOrder`，方向键/数字逐段编辑）。
- **核心参数**：必填 `value : () -> headless_core::SegmentedDateTime`、`on_change : (headless_core::SegmentedDateTime) -> Unit`；可选 `label~` `description~` `error_message~` `granularity~` `hour_cycle_12~` `order~` `variant~` `invalid~` `disabled~` `read_only~` `prefix?` `suffix?`

### 77. `aura::time_field_node` / `<aura::TimeField>`
- **适用场景**：分段时间输入。
- **核心参数**：必填 `value : () -> headless_core::SegmentedDateTime`、`on_change : (headless_core::SegmentedDateTime) -> Unit`；可选 `label~` `description~` `error_message~` `granularity~` `hour_cycle_12~` `variant~` `invalid~` `disabled~` `read_only~` `prefix?` `suffix?`

### 78. `aura::date_input_group_node` / `<aura::DateInputGroup>`
- **适用场景**：日期分段输入的外壳组（前后缀、整组焦点环），DateField/TimeField/DatePicker 内部复用。
- **核心参数**：可选 `variant~` `invalid~` `disabled~` `prefix?` `suffix?` `content~`

---

## 六、复合组件（Composites）

### 79. `aura::select_node` / `<aura::Select>`
- **适用场景**：下拉选择（`AuraSelectOption` 列表，单/多选、可清空、关闭态 Typeahead、虚拟焦点 `active_descendant`、视口内滚动定位选中项、RTL）；可用子部件自定义触发器与浮层。
- **核心参数**：必填 `items : Array[AuraSelectOption]`；可选 `label~` `placeholder~` `description~` `error~` `initial_selected~` `selected_keys?` `multiple~` `clearable~` `variant~` `full_width~` `required~` `invalid~` `disabled~` `rtl~` `dir~` `default_open~` `open?` `placement~` `gap~` `popover_width?` `on_open_change?` `on_selection_change?` `on_clear?` `selector_ref?` `content?`
- **子部件**：`aura::select_trigger_node` / `<aura::SelectTrigger>`、`aura::select_value_node` / `<aura::SelectValue>`、`aura::select_indicator_node` / `<aura::SelectIndicator>`、`aura::select_clear_button_node` / `<aura::SelectClearButton>`、`aura::select_popover_node` / `<aura::SelectPopover>`

### 80. `aura::combo_box_node` / `<aura::ComboBox>`
- **适用场景**：可输入筛选的组合框（筛选不丢已选、失焦恢复标签、IME 组合态保护）。
- **核心参数**：必填 `items : Array[AuraSelectOption]`；可选 `label~` `placeholder~` `description~` `error~` `field?` `initial_selected~` `selected_keys?` `initial_query~` `multiple~` `clearable~` `variant~` `full_width~` `required~` `invalid~` `disabled~` `rtl~` `dir~` `default_open~` `open?` `placement~` `gap~` `popover_width?` `on_open_change?` `on_selection_change?` `on_query_change?` `on_clear?` `selector_ref?` `field_ref?` `content?`
- **子部件**：`aura::combo_box_input_group_node` / `<aura::ComboBoxInputGroup>`、`aura::combo_box_input_node` / `<aura::ComboBoxInput>`、`aura::combo_box_trigger_node` / `<aura::ComboBoxTrigger>`、`aura::combo_box_clear_button_node` / `<aura::ComboBoxClearButton>`、`aura::combo_box_popover_node` / `<aura::ComboBoxPopover>`

### 81. `aura::autocomplete_node` / `<aura::Autocomplete>`
- **适用场景**：触发器 + 浮层内搜索框的自动补全（多选、可清空）。
- **核心参数**：必填 `items : Array[AuraSelectOption]`；可选 `label~` `placeholder~` `filter_placeholder~` `description~` `error~` `field?` `initial_selected~` `selected_keys?` `multiple~` `clearable~` `variant~` `full_width~` `required~` `invalid~` `disabled~` `rtl~` `dir~` `default_open~` `open?` `placement~` `gap~` `popover_width?` `on_open_change?` `on_selection_change?` `on_query_change?` `on_clear?` `selector_ref?` `field_ref?` `content?`
- **子部件**：`aura::autocomplete_trigger_node` / `<aura::AutocompleteTrigger>`、`aura::autocomplete_value_node` / `<aura::AutocompleteValue>`、`aura::autocomplete_indicator_node` / `<aura::AutocompleteIndicator>`、`aura::autocomplete_clear_button_node` / `<aura::AutocompleteClearButton>`、`aura::autocomplete_filter_node` / `<aura::AutocompleteFilter>`、`aura::autocomplete_popover_node` / `<aura::AutocompletePopover>`

### 82. `aura::color_picker_node` / `<aura::ColorPicker>`
- **适用场景**：取色器：色块触发器 + 浮层内 `ColorArea` + 色相/Alpha `ColorSlider` + Hex `ColorField` + 预设色板，受控/非受控 `model` 均可。
- **核心参数**：可选 `model?` `default_model~` `on_change~` `label~` `open?` `set_open?` `default_open~` `placement~` `popover_width~` `show_alpha~` `show_field~` `show_presets~` `presets?` `disabled~` `content?`
- **子部件**：`aura::color_picker_trigger_node` / `<aura::ColorPickerTrigger>`、`aura::color_picker_popover_node` / `<aura::ColorPickerPopover>`

### 83. `aura::date_picker_node` / `<aura::DatePicker>`
- **适用场景**：日期选择：分段输入 + 日历浮层（`read_only`、`min`/`max`、`is_unavailable`、选中即关 `close_on_select`）。
- **核心参数**：可选 `value?` `default_value~` `on_change~` `label~` `description~` `error_message~` `order~` `variant~` `open?` `set_open?` `default_open~` `close_on_select~` `placement~` `today~` `min?` `max?` `is_unavailable~` `first_day_of_week~` `show_year_picker~` `invalid~` `disabled~` `read_only~` `content?`
- **子部件**：`aura::date_picker_trigger_node` / `<aura::DatePickerTrigger>`、`aura::date_picker_trigger_indicator_node` / `<aura::DatePickerTriggerIndicator>`、`aura::date_picker_popover_node` / `<aura::DatePickerPopover>`

### 84. `aura::date_range_picker_node` / `<aura::DateRangePicker>`
- **适用场景**：区间日期选择（起止两段输入 + 区间日历，强制 `start <= end`）。
- **核心参数**：可选 `start_value?` `end_value?` `default_start_value~` `default_end_value~` `on_change~` `label~` `description~` `error_message~` `separator~` `order~` `variant~` `open?` `set_open?` `default_open~` `close_on_select~` `placement~` `today~` `min?` `max?` `is_unavailable~` `first_day_of_week~` `invalid~` `disabled~` `read_only~` `content?`
- **子部件**：`aura::date_range_picker_trigger_node` / `<aura::DateRangePickerTrigger>`、`aura::date_range_picker_trigger_indicator_node` / `<aura::DateRangePickerTriggerIndicator>`、`aura::date_range_picker_range_separator_node` / `<aura::DateRangePickerRangeSeparator>`、`aura::date_range_picker_popover_node` / `<aura::DateRangePickerPopover>`

### 85. `aura::table_node` / `<aura::Table>`
- **适用场景**：数据表 / DataGrid（`Primary`/`Secondary`）：`TableContent` 持有行列键与排序、单/多选（`aura_table_sel_*`）、Shift 区间选择、列宽拖拽、加载更多；单元格 O(1) 卸载。
- **核心参数**：可选 `variant~` `content~`
- **子部件**：`aura::table_scroll_container_node` / `<aura::TableScrollContainer>`、`aura::table_resizable_container_node` / `<aura::TableResizableContainer>`、`aura::table_content_node` / `<aura::TableContent>`、`aura::table_header_node` / `<aura::TableHeader>`、`aura::table_column_node` / `<aura::TableColumn>`、`aura::table_column_resizer_node` / `<aura::TableColumnResizer>`、`aura::table_body_node` / `<aura::TableBody>`、`aura::table_row_node` / `<aura::TableRow>`、`aura::table_cell_node` / `<aura::TableCell>`、`aura::table_load_more_node` / `<aura::TableLoadMore>`、`aura::table_footer_node` / `<aura::TableFooter>`

### 86. `aura::toast_node` / `<aura::Toast>`
- **适用场景**：轻提示：单条 `Toast`（5 种 `AuraToastVariant`、操作钮、关闭钮、倒计时进度）；应用级用 `ToastQueue` + `ToastProvider`（6 个 `AuraToastPlacement` 独立队列、同 id 原位更新、键盘聚焦暂停计时、单一 `Role::Status` 播报）。
- **核心参数**：可选 `variant~` `variant_by?` `title?` `description?` `show_indicator~` `show_indicator_by?` `icon?` `action_label?` `on_action?` `show_close~` `show_close_by?` `on_close?` `on_button_mount?` `announce~` `progress?` `width~` `content?`
- **子部件**：`aura::toast_provider_node` / `<aura::ToastProvider>`、`aura::toast_indicator_node` / `<aura::ToastIndicator>`、`aura::toast_content_node` / `<aura::ToastContent>`、`aura::toast_title_node` / `<aura::ToastTitle>`、`aura::toast_description_node` / `<aura::ToastDescription>`、`aura::toast_action_button_node` / `<aura::ToastActionButton>`、`aura::toast_close_button_node` / `<aura::ToastCloseButton>`

---

## 七、辅助（Utility）

### `aura::aura_icon_node` / `<aura::Icon>`
- **适用场景**：15 个内置单色矢量图标（`AuraIconKind`：`Check`、`ChevronDown`、`Xmark`、`Magnifier`、`CircleInfo`、`Calendar`、`Plus` 等），组件内部与业务图标位共用。
- **核心参数**：必填 `kind : AuraIconKind`；可选 `size~` `ink?`

---

## 八、审计门禁（Audit Gate）

- **`aura::assert_aura_ui(t, label~="scene")`**：一站式断言，`aura::audit_aura_ui` 返回非空即失败。
- **`aura::audit_aura_ui(t, label~="scene")`** 收集三类故障：`t.ui_craft_audit()`（排除 `corner:` 同心圆角条目，因为 Aura 的圆角由 `AuraConfig.radius` 倍率体系决定）、`t.unasked_faults()`、以及任何液态玻璃实例（`aura::count_liquid_glass(t) > 0`）。
- 首帧静默白名单：`Spinner`、`Skeleton`、不定态 `ProgressBar` / `ProgressCircle` 与 `Toast` 入场允许首帧动画；`AuraConfig.reduced_motion = true` 时它们全部静止。

```pion
aura::assert_aura_ui(t, label="settings_dark_rtl")
```
