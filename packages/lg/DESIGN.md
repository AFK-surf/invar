# `Invar · Liquid Glass` Design Specification

This specification defines the visual and architectural laws for `packages/lg` (37+ components on Invar's iOS 27 `paint::Glass::liquid` optical model). Every component in `packages/lg/src` and every exhibition page in `packages/lg_pages/src` must uphold all five laws.

---

## Law 1 — Strict Single-Layer Liquid Glass Architecture

Liquid Glass is a **single-layer translucent functional material** — never an opaque solid block, and never glass stacked on top of glass. The interface is strictly divided into two roles:

### 1. Single-Layer Translucent `paint::Glass::liquid` Surfaces
Standalone floating controls, navigation islands, overlays, and surface containers render **one** layer of `paint::Glass::liquid` with high-contrast opaque `solid` fallback so backdrops and colour fields refract cleanly underneath:

- **Standalone surface containers** (single-layer `paint::Glass::liquid` shell; **all children inside must be flat non-glass**):
  - `liquid_list_node`, `liquid_disclosure_node`, `liquid_toolbar_node`, `liquid_panel_node`, `liquid_card_node`, `liquid_control_tile_node`, `liquid_menu_node`, `liquid_menu_panel_node`, `liquid_menu_button_node`, `liquid_select_node`, `liquid_popover_node`, `liquid_tooltip_node`, `liquid_sheet_node`, `liquid_action_sheet_node`, `liquid_dialog_node`, `liquid_toast_node`, and parameter/telemetry inspector panels (`OPTICAL PARAMETERS`, `CAPILLARY HYSTERESIS`).
- **Floating-island navigation chrome**:
  - `liquid_nav_bar_node` is transparent layout chrome by default (`glass=false`) so `leading` and `trailing` slots host floating single-layer Liquid Glass islands (`liquid_icon_button_node`, `liquid_segmented_node`, `liquid_button_group_node`) without glass-on-glass nesting. Pass `glass=true` only when the bar contains zero inner glass controls.
- **Standalone floating controls & optical physics**:
  - `liquid_button_node` (`flat=false`), `liquid_icon_button_node`, `liquid_button_group_node`, `liquid_segmented_node`, `liquid_stepper_node`, `liquid_chip_node`, `liquid_text_field_node`, `liquid_search_bar_node`, `liquid_search_morph_node`, `liquid_bottom_tabs_node`, `liquid_collapsible_tabs_node`, `liquid_toggle_node` (lens on press), `liquid_slider_node` (lens on press), `liquid_adaptive_surface_node`, `liquid_magnifier_node`, `liquid_progressive_blur_node`, `liquid_merge_node`.

### 2. Flat Translucent Inner / Content Elements (Zero `paint::Glass::liquid`)
Every element placed *inside* a glass container (or alongside glass chrome) uses crisp, airy translucent ink fills (`.ground(...)` + subtle `0.5pt` hairline `.edge(...)`) — never a second layer of `paint::Glass::liquid`:

- **Inner glass-surface actions**: `liquid_button_node(..., flat=true)`, `liquid_button_node(..., flat=true, ghost=true)`, `liquid_toolbar_divider`, `liquid_control_item_node`, dialog/sheet/menu/action-sheet/list rows and close buttons.
- **Form & metadata primitives**: `liquid_checkbox_node`, `liquid_radio_group_node`, `liquid_badge_node`, `liquid_count_badge_node`, `liquid_progress_bar_node`, `liquid_progress_ring_node`, hairlines and slider tracks.
- **Multi-specimen staging containers**: containers that directly host child `paint::Glass::liquid` specimens (such as `ADAPTIVE LUMINANCE` hosting two `liquid_adaptive_surface_node` lenses) use a light translucent surface rather than `paint::Glass::liquid`.

---

## Law 2 — Hard Zero Glass-on-Glass Invariant

At rest (non-dragged steady state), no two drawn `paint::Glass::liquid` (`kind == 6`) rectangles may overlap or nest (`overlap_area > 1.0 pt²` beyond `0.5 pt` edge tolerance):

- **Allowed exceptions**:
  1. **Transient modal/anchored overlays** (`Sheet`, `Dialog`, `ActionSheet`, `Menu`, `Popover`, `Tooltip`) floating temporarily above the underlying scene — **provided that zero child `paint::Glass::liquid` rectangles exist inside the overlay panel itself**.
  2. **Intentional capillary fusion pairs** (`LiquidMerge` bonded droplet + capsule, which share a smooth-min merge pass).
  3. **Active press/drag loupe transitions** (`p > 0.001` while a finger holds `liquid_segmented_node` or `liquid_bottom_tabs_node`).
- **Enforcement**: Automated `lg::audit_glass(t, label~)` and `lg::assert_no_nested_glass(t, label~)` gates exported from `packages/lg/src/audit.pion` over `ui::Tester::drawn_glass()` and `ui::Tester::unasked_faults()`, verified across `packages/lg/src/components_test.pion` and all downstream app test suites in both Light and Dark themes.

---

## Law 3 — Concentric Corner Ladder

Every nested rounded surface obeys exact concentric geometry:

```
R_inner = R_outer - pad
```

- Examples: `Menu` (`r=22, pad=6 -> item r=16`), `List` (`r=24, pad=6 -> row r=18`), `Disclosure` (`r=22, pad=6 -> header r=16`), `ActionSheet` (`r=28, pad=8 -> item r=20`), `Sheet` (`r=36, inset=20 -> close r=16`), `SegmentedControl` (`r=h/2, pad=3 -> thumb r=h/2-3`).
- **Enforcement**: Invar `ui::Tester::audit()` / `unasked_faults()` must report **0 faults** across light and dark themes.

---

## Law 4 — Optical Restraint & Contrast

- **Neutral dark density ladder**: Regular floating controls use `0x20202067L` (matching the neutral dark regular baseline); dense panels and dialogs (`ovl_panel_glass`, `liquid_dialog_node`) use `0x202020BFL` (75% veil); detached sheets (`ovl_sheet_glass`) use `0x202020CCL` (80% veil) so white foreground ink stays legible even over saturated or bright backdrops.
- **Contrast**: All foreground text and icons maintain **WCAG >= 4.5:1** contrast against both the glass `solid` fallback and Content Layer surfaces in Light and Dark modes (verified by `ui::Tester` audits).
- **Zero hidden glass instances at rest**: Controls that reveal a clear refraction loupe only while pressed/dragged (`liquid_segmented_node`, `liquid_bottom_tabs_node`, `liquid_toggle_node`, `liquid_slider_node`) must return `None` from `.glass(...)` whenever `p <= 0.001`, submitting `Some(...)` only during active interaction. Never submit `alpha == 0` glass passes to the GPU.
- **No `outside=false` shadows under glass**: Shadows beneath `paint::Glass::liquid` render outside the SDF contour (`shadow_outside(view::Live::Is(true))`) so they never darken the refracted interior.

---

## Law 5 — Closed-Form Motion & `headless::` Semantics

- **Closed-form springs**: All motion (`lg::DampedDrag`, `lg::InteractiveHighlight`, `motion::NodeWay`, `motion::overlay_presence`, `motion::hover_fade`) is computed purely from cause moment and elapsed time — never frame counters or tick loops — guaranteeing byte-identical trajectories at **60, 120, 24, and 7 fps**.
- **Headless semantics**: Every interactive component is built on `packages/headless` (`headless::button`, `headless::toggle`, `headless::slider`, `headless::radio_group`, `headless::dialog`, `headless::Popover`, `headless::menu`, `headless::disclosure`, `headless::progress`, `ui::Field`). Touch/drag interactions use `view::View::gesture`, never `on_drag` on headless nodes.
