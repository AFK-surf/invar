# `Invar · Aura` Design Specification

This specification defines the visual and engineering laws for `packages/aura`, Invar's second official component library: 86 solid-surface components (158 Builder functions / 158 Pion-JSX tags including compound slots), each built on `packages/headless` / `packages/headless_core` behaviour and `packages/color` OKLCH / OKLab token math. Every component in `packages/aura/src` and every page that renders them (`packages/aura_pages`, `packages/aura_shots`, `packages/aura_web`) must uphold all seven laws. The component index is [`CATALOG.md`](CATALOG.md).

Aura and `packages/lg` are siblings, not layers: `lg` is the floating-glass library, Aura is a flat paper library. Never mix them on one screen.

---

## Law 1 — Architecture: Headless Behaviour, Solid Paper, Zero Liquid Glass

### 1. Package layering
```
packages/color            Layer 0: floating-point OKLab / OKLCH, CSS Color 4 mixing, per-channel sRGB clip
packages/headless_core    pure state machines (calendar, date segments, color model, number input, OTP, ...)
packages/headless         behaviour bound to view::View (focus, keys, roles, overlays, selection, grids)
packages/view + ui        retained node tree, layout, text, Tester
packages/aura             look: tokens, surfaces, geometry, motion — no behaviour of its own
```
- **All interaction semantics live below Aura.** Selection, roving focus, typeahead, virtual focus (`controls` / `active_descendant`), dismissal, date and color arithmetic are `headless` / `headless_core` contracts; Aura only decides how they look. A behaviour Aura needs and `headless` lacks is added to `headless` first, with its own tests, so every look library benefits.
- **Zero `paint::Glass::liquid`.** No Aura component, page or downstream Aura app may draw liquid glass (`kind == 6`). Depth comes from solid fills, hairline edges, and the multi-layer shadow ladder of Law 2. `aura::assert_aura_ui` fails on a single glass instance (Law 6).

### 2. Overlays are root-level, config-inheriting, and release input immediately
- `Modal`, `AlertDialog`, `Drawer`, `Sheet`, `Popover`, `Tooltip`, `Dropdown`, and every composite popover (`Select`, `ComboBox`, `Autocomplete`, `ColorPicker`, `DatePicker`, `DateRangePicker`) mount on `parent.tree.root()` via `View::show_scoped`, anchored with `View::beside`, and re-borrow the nearest `AuraConfig` signal so a nested dark / RTL / circular-corner subtree stays consistent inside its overlay.
- Overlays re-borrow `AuraSurfaceLevel::Overlay`, mount the dark-mode inner rim, and stop taking pointer input on the first frame of their exit transition.

---

## Law 2 — Semantic Tokens, Three-Tier Surfaces, and Two Theme Axes

### 1. One configuration signal per tree
```pion
let cfg = signals::signal(aura::AuraConfig::default())        // light, standard, smooth corners
ui::set_tree_theme(fn() -> ui::Theme { aura::aura_ui_theme(cfg.get()) })
let _ = aura::aura_provide_config(root, cfg)                  // borrowed via view::Lent, zero extra nodes
cfg.set(aura::AuraConfig::dark(vibrant=true))                 // every component restyles live
```
- `AuraConfig` fields: `dark`, `vibrant`, `radius` (anchor, default `8.0`), `spacing` (default `4.0`), `accent` (`color::Oklch`, default `aura::aura_default_accent()`), `auto_surface`, `density` (`AuraDensity::AutoResponsive | Compact | Regular | Touch`), `reduced_motion`, `rtl`, `corner_circular`.
- Components read configuration only through `aura::aura_config_of(v)` / `aura::aura_theme(v)` inside `view::Live::By` closures, so a `cfg.set(...)` reaches every mounted component without remounting.
- `aura::aura_ui_theme(cfg)` bridges the palette into `ui::Theme` (proportional digits `tabular: false`, default glyph forms `font_alternates: false`) so plain `ui::*` text under an Aura root matches Aura's own text.

### 2. Derived palette: Light / Dark × Standard / Vibrant
- `aura::aura_palette(cfg)` derives ~70 semantic tokens (`AuraPalette`: `background*`, `foreground`, `muted`, `surface*`, `overlay`, `default*`, `accent*`, `success*`, `warning*`, `danger*`, `field_*`, `border*`, `separator*`, `segment*`, `scrollbar*`, `focus`) from a handful of OKLCH anchors.
- Every derived token (`*_hover`, `*_soft`, `*_soft_hover`, `*_soft_foreground`) is an OKLab mix computed in floating point by `packages/color` (`mix_oklab`, transparent-aware premultiplied interpolation) and packed to RGBA8 only at the end (`color::pack_rgba8(color::oklab_to_srgb_clip(...))`). Never hard-code a hover or soft color; change the anchor.
- **Light / Dark** swap the neutral anchors (background, foreground, muted, surfaces). **Standard / Vibrant** is an orthogonal axis: Vibrant pulls every `*_soft_foreground` toward its saturated status color (92% status / 8% foreground) instead of toward the neutral ink, for brand-forward screens. All four modes are covered by token tests.
- Changing `accent` re-derives the entire accent family (`accent_hover`, `accent_soft`, `accent_soft_hover`, `accent_soft_foreground`, `focus`) from one `color::Oklch`.
- `aura::aura_theme_from(cfg)` keeps a two-slot cache keyed by `AuraConfig` equality, so light and dark subtrees side by side never thrash and `Live::By` reads never recompute the palette.

### 3. Three-tier surface hierarchy (`SurfaceContext`)
- `AuraSurfaceLevel`: `Primary` → `Secondary` → `Tertiary` (three solid paper tiers), plus `Transparent` (inherits the enclosing tier dynamically) and `Overlay` (floating panels).
- `aura::surface_node` / `<aura::Surface>` and `Card` lend their level to descendants through `view::Lent` (`aura_provide_surface_by`); with `AuraConfig.auto_surface = true` an unspecified `Surface` steps one tier deeper than its parent (`aura_next_surface_level`). An explicit `variant` always wins.
- **Fields adapt to the paper they sit on.** `AuraFieldVariant::Auto` resolves to `Primary` outside any surface and to `Secondary` on a concrete surface; `aura::aura_field_bg` picks `field_background`, `surface_secondary`, or `surface_tertiary` accordingly. Inputs, selects, date and color fields never hard-code their background.

### 4. Elevation ladder
- Light surfaces and fields: 3 stacked `view::Shadow::drop` layers (`aura::aura_surface_shadows`); light overlays: 3 larger layers including an upward `dy = -6` layer (`aura::aura_overlay_shadows`). Layers are declared bottom-to-top with `sigma = blur / 2`.
- **Dark mode casts no outer shadows.** Overlays instead get a two-edge inner rim (`0.5pt` at `0xFFFFFF11` over `1pt` at `0xFFFFFF03`) drawn over their ground and under their content, taking no room and no pointer.

---

## Law 3 — Geometry and Corners

### 1. One radius anchor
`AuraTheme` derives every corner from `AuraConfig.radius` (`r`, default `8pt`): control `r`, `field_radius = 1.5r` (12), `surface_radius = 1.5r` (12), `card_radius = 3r` (24), `overlay_radius = 3r` (24); `border_width = 1`, `ring_offset_width = 2`, `disabled_opacity = 0.5`. Raising or lowering `radius` rescales the whole library coherently; components never introduce a free-standing radius constant for a themable corner.

### 2. Continuous smooth corners by default, circular arcs on request
- **Default: `AuraConfig.corner_circular = false`** — every Aura corner renders as Invar's continuous smooth (squircle) corner, the same curvature-continuous shape the rest of Invar uses.
- **Reactive switch:** `corner_circular = true` turns every corner into a standard circular arc. All 100+ corner sites read `aura::aura_corner_circular(v)` inside `View::corner_circular(view::Live::By(...))`, so toggling the flag restyles a live tree, overlays included, without remounting.
- Shapes that are geometrically round (avatars, radio dots, swatch circles, switch thumbs, spinner paths) stay exact circles in both modes.
- **Fixed calibration mode:** pixel-calibration scenes in `packages/aura_shots` pin `corner_circular = true` explicitly so their per-pixel gates stay comparable to circular-arc reference renders; showcase scenes use the default smooth corners. A new calibration scene must state its corner mode rather than inherit the default.

### 3. Spacing and breakpoints
- Spacing is multiples of `AuraConfig.spacing` (4pt).
- Responsive sizing follows logical width only: `aura::aura_is_sm` (`>= 640pt`) and `aura::aura_is_md` (`>= 768pt`) track `Tree::room_signal()`; `AuraDensity::Compact | Regular | Touch` pin them. Pointer coarseness (`aura::aura_is_coarse_pointer`) only suppresses hover washes and similar pointer-specific effects, never layout sizes.

---

## Law 4 — Dual API, Focus, and Accessibility

### 1. Builder and Pion-JSX are one API
- Every component ships as a Builder function `aura::<name>_node(parent, ...) -> view::View` and a PascalCase Pion-JSX tag (`<aura::Button in={parent} ... />` for `aura::button_node`) with the same parameters. The JSX tag adds `*_by=>{...}` reactive closures next to static props (`label` / `label_by`, `disabled` / `disabled_by`, ...), `children` blocks, and Leading-Dot enums (`variant={.Secondary}`).
- Compound components expose named slot parts (`CardHeader`, `SelectTrigger`, `TableColumn`, `ToastProvider`, ...) for custom layouts, while the root component renders the common layout in one call.
- Reactive inputs are `view::Live[T]` (`Is(value)` or `By(getter)`) or `() -> T` getters; controlled components take a getter plus a change callback (`selected` + `on_select`, `open` + `set_open`), uncontrolled ones take `default_*` / `initial_*`.

### 2. Focus-ring grammar
- Interactive controls (buttons, switches, triggers, tabs): ring only on keyboard focus-visible — 2pt `background` gap plus 2pt `focus` outer ring.
- Field slots: ring on focus or focus-within, including pointer focus — 2pt `focus` flush ring; invalid fields show a 1pt `danger` ring at rest and a 2pt `danger` ring while focused.
- `force_focus_visible` / `force_focused` / `force_pressed` exist only so deterministic calibration scenes can freeze a state; application code never sets them.

### 3. RTL and BiDi
- `AuraConfig.rtl = true` mirrors the whole subtree: row order (`row_reverse`), paddings and insets, chevrons and separators, popover placements (`BelowStart` / `BelowEnd` swap), slider and range-slider pointer and arrow-key direction, tab and radio arrow keys, table column-resizer edges. Components that can be mirrored individually also accept `rtl` / `dir`.
- Text runs through Invar's UAX #9 bidirectional layout: Hebrew and Arabic render in visual order with joining and marks intact, and `ui::Field` carets, selections, hit testing and IME preedit use the same visual-cluster ↔ logical-byte mapping. Never reverse strings or hand-flip text alignment in Aura code.

### 4. Semantics
Every interactive component sets its `view::Role` and states (`Said`: checked, selected, expanded, `controls`, `active_descendant`, `multiselectable`, `current`, `sort`, `busy`) through `headless`, so all host accessibility bridges see the same tree. Toasts announce through a single `Role::Status` region.

---

## Law 5 — Performance and Lifecycle

### 1. Pure-GPU color planes
`ColorArea` and `ColorSlider` are built only from `View::fades` gradient instances: HSV and HSL planes use exact algebraic decompositions (on the order of 8–17 instances), other channel pairs use 64 integer-aligned gradient strips. Multi-layer planes composite once in an offscreen group (`View::group_alpha`) and are clipped by the rounded corner afterwards, so smooth corners never leak an under-layer color at the edge. **Never rasterize a color plane per pixel on the CPU or encode images for it**: zero image-cache entries, cold mount and color updates well under a millisecond.

### 2. O(1) teardown with `View::letting_go()`
When a whole subtree is being disposed, per-child cleanup must not do registry work that the parent's own disposal makes redundant. Child `owns` callbacks check `parent.letting_go()` and return immediately; for example each `ColorSwatchPicker` swatch skips removing itself from the picker's swatch array while the picker itself is going away. Initial mounts batch into one buffer committed once. Mounting or unmounting a picker with N swatches therefore stays linear O(N) instead of O(N²) from N array removals. Apply the same pattern to any registry-style component (table cells, listbox options, tags).

### 3. Linear-time collections
Selection, filtering and range operations are linear or better: indexed option lookup plus memoized filters in `Select` / `ComboBox` / `Autocomplete`, O(n) Shift-range selection over thousands of table rows, O(1) per-cell unmount, and O(1) reads of grid state snapshots. A new collection feature ships with a scale test, not only a correctness test.

### 4. Closed-form motion
All Aura motion is a function of cause moment and elapsed time: `motion::NodeWay`, `motion::backdrop_press_spring`, `motion::knob_spring`, `motion::motion_spring`, and the press scales `aura::aura_button_press_scale` (0.97, `_sm` 0.98, `_lg` 0.96) and `aura::aura_close_button_press_scale` (0.93). Never count frames or tick a loop; trajectories must be identical at any frame rate. `aura::aura_reduced_motion(v)` (system setting or `AuraConfig.reduced_motion`) turns every spinner, skeleton shimmer, indeterminate progress, switch spring and toast entrance static.

### 5. Zero blocking IO on the render thread
Aura components never take `World`, never touch the network, disk or subprocesses, and never sleep. Data a component shows (table pages, autocomplete results, avatars) arrives through `ui::Worker` events bound by the application; `on_load_more`-style callbacks only send commands.

---

## Law 6 — The `aura::assert_aura_ui` Gate

Every `ui::Tester` suite that renders Aura must call `aura::assert_aura_ui(t, label="...")` after each meaningful state (initial, opened overlay, dark, RTL). `aura::audit_aura_ui` collects:
1. `t.ui_craft_audit()` faults, excluding `corner:` entries (Aura corners follow the radius ladder of Law 3, not concentric insets);
2. `t.unasked_faults()` (unrequested motion and layout faults); a first-frame allowlist covers `Spinner`, `Skeleton`, indeterminate `ProgressBar` / `ProgressCircle`, and the `Toast` entrance, all static under `reduced_motion`;
3. any liquid glass instance (`aura::count_liquid_glass(t) > 0`).

WCAG 4.5:1 contrast is deliberately not part of the gate because the muted and disabled tokens are lighter by design; contrast-critical text uses `foreground` tokens.

Visible changes additionally require same-size, same-data before/after offscreen renders from `packages/aura_shots` on both native backends (see the repository `AGENTS.md`).

---

## Law 7 — Public Repository Hygiene

- Aura is an independent name. Public API names (packages, types, functions, tags, enum variants), documentation prose, and showcase copy describe Aura's own laws and must not use third-party product or brand names, neither as identifiers nor as claims of equivalence or endorsement.
- Upstream attribution for ported design specifications, icon paths and fonts lives only in [`NOTICE`](../../NOTICE), [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md), and the provenance comment at the top of each ported source file. Keep those in sync when a package starts or stops deriving from an upstream work.
- Sample data in tests and showcases is neutral (no real people, companies or brands).
- The repository is public: no internal machine names, internal product names, or private paths anywhere in Aura sources or docs.
