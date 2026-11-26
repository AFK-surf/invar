# `Invar · Liquid Glass` Design Specification

This specification defines the visual and architectural laws for `packages/lg` (40+ components on Invar's iOS 26/27 & macOS Tahoe `paint::Glass::liquid` optical model). Every component in `packages/lg/src` and every exhibition page in `packages/lg_pages/src` must uphold all five laws.

---

## Law 1 — Two-Layer Apple Architecture: Solid Content Layer vs. Floating Liquid Glass Chrome

Following Apple's iOS 26/27 & macOS Tahoe Human Interface Guidelines, every screen is strictly separated into **two distinct layers**: a calm, high-contrast **Solid Content Layer** underneath, and a lightweight **Floating Liquid Glass Chrome Layer** above it. Liquid Glass is a functional floating material — never a wallpaper or a content box, and never glass stacked on glass.

### 1. Solid Content Layer (Zero `paint::Glass::liquid` — Mandatory for Page Content)
All primary reading surfaces and structured page content **MUST NOT** use `paint::Glass::liquid`. Use the unified semantic palette `lg::surface()` (`packages/lg/src/surface_tokens.pion`) and zero-glass content components:

- **Forbidden for `Glass::liquid`**:
  - Page backgrounds / canvases (`surface().canvas`, `surface().grouped_bg`, `surface().sidebar`)
  - Message feeds, timelines, document bodies, and reading panes
  - Grouped lists, settings rows, and disclosure sections (`liquid_list_node` / `liquid_disclosure_node` default to `glass=false`)
  - Content cards, dashboards, and form containers (`surface_card_node` / `<lg::SurfaceCard>`, `surface_section_node` / `<lg::SurfaceSection>`, `liquid_panel_node` / `<lg::Panel>` default `floating=false`, `liquid_card_node` / `<lg::Card>` default `floating=false`)
  - Code blocks and monospace viewports (`surface_code_block_node` / `<lg::CodeBlock>` with `ui::scroll_row` horizontal scroll so long lines never clip on 320pt narrow viewports)
- **Inner elements inside floating glass**:
  - `liquid_button_node(..., flat=true)` / `flat=true, ghost=true`, `liquid_toolbar_divider`, `liquid_control_item_node`, `liquid_checkbox_node`, `liquid_radio_group_node`, `liquid_badge_node`, `liquid_count_badge_node`, `liquid_progress_bar_node`, `liquid_progress_ring_node`, and `liquid_list_node(..., flat=true)` inside sheets/drawers.

### 2. Floating Liquid Glass Chrome Layer (Single-Layer `paint::Glass::liquid`)
Reserve single-layer `paint::Glass::liquid` exclusively for floating navigation, floating controls, transient overlays, and active optical lenses:

- **Standard Apple App Shells & Chrome (Start Here)**:
  - `page_shell_node` / `<lg::PageShell>`: full-height (100%) `ui::scroll_box` over a solid canvas with automatic top/bottom content insets, and floating top/bottom slots — guaranteeing scrolled content glides underneath floating glass chrome. **No bare text over content, no scroll-edge blur**: bars made only of self-contained glass islands (`<lg::BackButton>`, channel pills, `<lg::BarPillGroup>`, `<lg::ComposerBar>`, `<lg::BottomTabs>`) get no blur veil, so crisp content refracts straight through the islands. Only when bare `title` / `subtitle` text sits over the content does the shell mount a compact full-height `smoothstep` fade (`liquid_scroll_edge_node`, `UIScrollEdgeEffect.soft`) of `header_height + 22` (66pt by default); `top_edge_blur` overrides the choice, and `bottom_edge_blur=true` opts into a `bottom_height + 8` (64pt) bottom fade for a bottom bar that carries bare text. A bottom bar whose height changes passes `bottom_inset_by` (`() -> Double`), which wins over `bottom_inset` and updates the scroll content's bottom inset live. Without a bottom fade, the 36pt seam under the bottom bar is fully opaque and never takes the pointer (`points=false`), so rows cut short under it are covered rather than counted as undersized targets, while the wheel and text selection beside the bar still reach the content.
  - `split_shell_node` / `<lg::SplitShell>`: responsive two-column `NavigationSplitView` (`>= 760pt` fixed solid sidebar + `0.5pt` hairline + `.fill()` detail; `< 760pt` full-screen sidebar/detail switch).
  - `composer_bar_node` / `<lg::ComposerBar>`: Apple Messages-grade single-layer adaptive floating glass composer (`min_height=44`, `r=22`, flat circular leading icons, borderless transparent `ui::field`, `34pt` circular `ArrowUp` send button with `36pt` hit area).
  - `liquid_back_button_node` / `<lg::BackButton>` + `icon_node` / `<lg::Icon>` (`31+` pure-vector `paint::shape_of` icons, zero emoji/Unicode font dependency).
- **Floating navigation chrome**:
  - `liquid_nav_bar_node` (transparent layout chrome `glass=false` hosting floating glass islands in `leading`/`trailing`), `liquid_bar_pill_group_node`, `liquid_bottom_tabs_node`, `liquid_collapsible_tabs_node`, `liquid_search_bar_node`, `liquid_search_morph_node`.
- **Anti-Pattern Red Lines (Strictly Forbidden)**:
  1. **No solid top-bar backdrop or full-width horizontal divider** cutting off the scroll viewport (use `<lg::PageShell>`; blur the scroll edge only behind bare text, never behind glass-island-only bars).
  2. **No plain-text top-bar capsules** like `"Back"` or `"Search"` (use `<lg::BackButton>` and `<lg::BarPillGroup>` with `<lg::Icon>`).
  3. **No box-in-box composer bars** or rectangular `"Send"` text buttons (use `<lg::ComposerBar>`).
  4. **No three-box opaque stack (`[header] / [scroll_box] / [footer]`)** that prevents content from scrolling beneath floating Liquid Glass chrome.
- **Floating toolbars & controls & automatic `SurfaceContext` material adaptation**:
  - `liquid_toolbar_node`, `liquid_button_node`, `liquid_icon_button_node`, `liquid_button_group_node`, `liquid_segmented_node`, `liquid_stepper_node`, `liquid_chip_node`, `liquid_select_node`, `liquid_text_field_node`.
  - Every Content Layer container (`surface_card_node`, `liquid_list_node`, `page_shell_node` scrolling body, `liquid_panel_node`, `liquid_disclosure_node`) and every Glass Overlay container (`liquid_sheet_node`, `liquid_action_sheet_node`, `liquid_drawer_node`, `liquid_dialog_shell_node`, `liquid_dialog_node`, `liquid_popover_node`, `liquid_panel_shell_node`, `liquid_toolbar_node`, `liquid_bar_pill_group_node`, `liquid_pill_group_node`, `lg_edit_bar`) automatically lends `SurfaceContext::Content` down the view tree via `view::Lent` (zero extra nodes, zero signal overhead).
  - Controls inside `SurfaceContext::Content` (`Button`, `Chip` including removable `on_remove`, `Stepper`, `Segmented`, `Select`, `TextField`) automatically resolve to unified Apple Content Layer solid surfaces (`ctrl_flat_fill` / `ctrl_flat_edge` / `ctrl_recessed_fill` / `ctrl_recessed_edge` / `ctrl_flat_selected_fill` = `surface().accent`) with zero Liquid Glass — no need to manually pass `flat=true` everywhere. Floating top/bottom chrome slots in `PageShell` (`leading`, `trailing`, `bottom`) lend `SurfaceContext::Floating` so floating islands automatically retain Liquid Glass. Passing explicit `flat=true` or `flat=false` always overrides context.
- **Floating overlays & inspectors**:
  - `liquid_menu_node`, `liquid_menu_panel_node`, `liquid_menu_button_node`, `liquid_select_node`, `liquid_popover_node`, `liquid_tooltip_node`, `liquid_sheet_node`, `liquid_action_sheet_node`, `liquid_drawer_node`, `liquid_dialog_node`, `liquid_toast_node`, plus opt-in floating inspectors (`liquid_panel_node(..., floating=true)`) and Control Center tiles (`liquid_control_tile_node`, `liquid_card_node(..., floating=true)`).
- **Refractive interaction lenses & optical physics**:
  - `liquid_toggle_node` (lens on press), `liquid_slider_node` (lens on press), `lg_glass_lift_surface` (continuous flat-to-glass elevation), `liquid_adaptive_surface_node`, `liquid_magnifier_node`, `liquid_progressive_blur_node`, `liquid_merge_node`.

---

## Law 2 — Hard Zero Glass-on-Glass & Content-Coverage Invariant

At rest (non-dragged steady state):
1. **Zero glass overlap/nesting**: No two drawn `paint::Glass::liquid` (`kind == 6`) rectangles may overlap or nest (`overlap_area > 1.0 pt²` beyond `0.5 pt` edge tolerance).
2. **Zero content-layer glass misuse**: In non-modal scenes (`allow_large_overlay=false`), total `kind == 6` Liquid Glass coverage must not exceed `45%` of the viewport (`max_glass_coverage=0.45`), and no single glass rectangle may exceed both `65%` viewport width and `55%` viewport height.

- **Allowed exceptions**:
  1. **Transient modal/anchored overlays** (`Sheet`, `Dialog`, `ActionSheet`, `Menu`, `Popover`, `Tooltip`, `Drawer`) floating temporarily above the scene — pass `allow_large_overlay=true` when auditing full-screen modal viewports, and ensure **zero child `paint::Glass::liquid` rectangles exist inside the overlay panel itself**.
  2. **Intentional capillary fusion pairs** (`LiquidMerge` bonded droplet + capsule, which share a smooth-min merge pass).
  3. **Active press/drag loupe transitions** (`p > 0.001` while a finger holds `liquid_segmented_node` or `liquid_bottom_tabs_node`).
- **Enforcement**: Automated `lg::audit_glass`, `lg::assert_no_nested_glass`, and one-stop `lg::assert_apple_ui` gates exported from `packages/lg/src/audit.pion`.

---

## Law 3 — Concentric Corner Ladder

Every nested rounded surface obeys exact concentric geometry via `lg::concentric(outer, inset)`:

```
R_inner = lg::concentric(R_outer, pad) = max(0.0, R_outer - pad)
```

- **Standard ladder (`packages/lg/src/surface_tokens.pion`)**: `r_sheet = 34.0`, `r_panel = 26.0`, `r_card = 20.0`, `r_row = 12.0`, `r_chip = 8.0`, `hairline_w = 0.5`.
- **Component examples**: `Menu` (`r=18, pad=6 -> item r=12`), `List` (`r=24, pad=6 -> row r=18`), `Disclosure` (`r=22, pad=6 -> header r=16`), `ActionSheet` (`r=22, pad=6 -> item r=16`), `Sheet` (`r=28, inset=12 -> close / 32pt footer button r=16`), `Drawer` (`r=28, pad=12 -> close / 32pt button r=16`), `Dialog` (`r=28, inset=12 -> 32pt button r=16`), `Toolbar` (`h=36 r=18, pad=3 -> 30pt button r=15`), `BarPillGroup` (`h=34 r=17, pad=2 -> 30pt button r=15`), `SegmentedControl` (`h=30 r=15, pad=2.5 -> thumb h=25 r=12.5`), `BottomTabs` (`h=50 r=25, pad=4 -> lens h=42 r=21`).
- **Enforcement**: `lg::assert_apple_ui` runs `ui::Tester::assert_ui_craft()` and `unasked_faults()` with **0 faults** across Light and Dark themes.

---

## Law 4 — Optical Restraint & Contrast

- **Semantic Content Layer palette (`lg::surface()`)**: Light (`canvas #F5F5F7`, `grouped_bg #F2F2F7`, `card #FFFFFF`, `ink #1C1C1E`, `separator 8% black`) and Dark (`canvas #0B0C0E`, `grouped_bg #000000`, `card #1C1C1E`, `ink #F5F5F7`, `separator 10% white`) automatically follow `lg::is_dark()`.
- **Symmetric light/dark glass density ladder**: Regular floating controls and capsules use `0x20202067L` (dark) / `0xFDFCFD67L` (light) with `blur = 1.2pt` (keeping 2x/3x screens on full-resolution blur level `k = 0`); unified floating overlays (`lg_panel_glass`, `lg_sheet_glass`, `lg_drawer_glass`, `liquid_dialog_node`) use `0x20202080L` (dark) / `0xFDFCFD80L` (light) with `blur = 14.0pt` and `chroma = 0.0`, while `lg_toast_glass` uses `0x80` at `blur = 12.0pt` and expanded sheets morph smoothly up to `0xDC`.
- **Contrast**: All foreground text and icons maintain **WCAG >= 4.5:1** contrast against both Content Layer surfaces and glass `solid` fallbacks in Light and Dark modes.
- **Zero hidden glass instances at rest**: Controls that reveal a clear refractive loupe only while pressed/dragged (`liquid_segmented_node`, `liquid_bottom_tabs_node`, `liquid_toggle_node`, `liquid_slider_node`) must return `None` from `.glass(...)` whenever `p <= 0.001`. Never submit `alpha == 0` glass passes to the GPU.
- **No `outside=false` shadows under glass & 3-tier outside shadow ladder**: Shadows beneath `paint::Glass::liquid` render outside the SDF contour (`shadow_outside(view::Live::Is(true))`) so they never darken the refracted interior, following a unified 3-tier theme-adaptive ladder:
  1. Compact top/bottom bar pills (`lg_bar_pill_shadow`): Light `(4.0, 1.5, 0x00000012L)` / Dark `(6.0, 2.0, 0x00000040L)`.
  2. Floating glass controls (`glass_control_shadow`, also `shadow_default` in Light): Light `(5.0, 2.0, 0x00000010L)` / Dark `(6.0, 2.0, 0x00000038L)`.
  3. Floating glass overlays (`glass_overlay_shadow` — `Popover`, `Menu`, `Panel`, `Sheet`, `Drawer`, `Dialog`, `Toast`, `Tooltip`, `Toolbar`): Light `(8.0, 4.0, 0x00000014L)` / Dark `(8.0, 4.0, 0x00000042L)`.

---

## Law 5 — Closed-Form Motion & `headless::` Semantics

- **Closed-form springs**: All motion (`lg::DampedDrag`, `lg::InteractiveHighlight`, `motion::NodeWay`, `motion::overlay_presence`, `motion::hover_fade`) is computed purely from cause moment and elapsed time — never frame counters or tick loops — guaranteeing byte-identical trajectories at **60, 120, 24, and 7 fps**.
- **Headless semantics**: Every interactive component is built on `packages/headless` (`headless::button`, `headless::toggle`, `headless::slider`, `headless::radio_group`, `headless::dialog`, `headless::Popover`, `headless::menu`, `headless::disclosure`, `headless::progress`, `ui::Field`). Touch/drag interactions use `view::View::gesture`, never `on_drag` on headless nodes.
