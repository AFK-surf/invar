# Third-Party Notices

Invar itself is licensed under the MIT License (see `LICENSE`,
`Copyright (c) 2026 AFK-surf contributors`).

Invar includes, or is derived from, the following third-party works, which
retain their respective copyright notices and licenses:

- Where an upstream license below is **MIT**, the full MIT License text is in
  [`LICENSE`](LICENSE) (with the respective upstream copyright notice listed
  below).
- Where an upstream license below is **Apache-2.0**, the full Apache License
  2.0 text is in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt), and
  required attribution notices are also preserved in [`NOTICE`](NOTICE).
- Where an upstream license below is **SIL OFL-1.1**, the full license text is
  in [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt).
- Where an upstream license below is **Unicode-3.0**, the full license text is
  in [`LICENSES/Unicode-3.0.txt`](LICENSES/Unicode-3.0.txt).
- Full license texts for **BSD-3-Clause**, **HarfBuzz Old MIT**, and
  **SunPro fdlibm** are reproduced verbatim in their respective sections below.

---

## 1. mbui (`AFK-surf/monoid`)

- **Files**: `packages/host/native/*` (78 mapped host files),
  `packages/view/src/*`, `packages/ui/src/*`, `packages/flex/src/*`,
  `packages/zip/src/*`, and other modules marked `Ported from libs/mbui/...`.
- **Source**: <https://github.com/AFK-surf/monoid/tree/1eec0cd3ce63e0c127930aa9b4136bd17e5c67ef/libs/mbui>
- **License**: MIT (full text in [`LICENSE`](LICENSE))
- **Copyright**: `Copyright (c) 2026 still.fail contributors`
- **Modifications**: Ported from MoonBit to Pion and evolved for Invar.

---

## 2. `monoid/libs/liquid-glass` & `Kyant0/AndroidLiquidGlass`

- **Files**: `packages/lg/src/{damped_drag,interactive_highlight,liquid_button,liquid_control_tile,liquid_dialog,liquid_slider,liquid_surface,liquid_toggle,tokens}.pion`,
  `packages/lg_pages/src/*`, `packages/lg_shots/src/*`, and the refraction,
  dispersion, and highlight shaders in the four GPU renderers.
- **Sources**:
  - <https://github.com/AFK-surf/monoid/tree/1eec0cd3ce63e0c127930aa9b4136bd17e5c67ef/libs/liquid-glass>
  - <https://github.com/Kyant0/AndroidLiquidGlass/tree/eb4a0ea2e17542ea28c2e71088f7dd9acf77aad5>
- **License**: Apache-2.0 (full text in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt);
  attribution lineage preserved in [`NOTICE`](NOTICE))
- **Copyright**:
  - `Copyright 2026 still.fail contributors`
  - `Copyright 2025 Kyant`
- **Modifications**: Ported and rewritten in Pion over Invar's retained-mode
  view tree, headless components, motion, and GPU glass pipeline.

---

## 3. `whynotmake-it/flutter_liquid_glass` (`liquid_glass_renderer`)

- **Files**: `packages/host/native/apple_render.m`,
  `packages/host/native/gles_render.h`, `packages/host/native/win_render.cpp`,
  `packages/host/native/win_shaders_baked.h`, `web/host.js`.
- **Source**: <https://github.com/whynotmake-it/flutter_liquid_glass/tree/8e1e7d649bcc220312ba3d3493cfaf76a91382f2/packages/liquid_glass_renderer>
- **License**: Apache-2.0 (full text in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt))
- **Copyright**: `Copyright 2025, Tim Lehmann for whynotmake.it`
- **Modifications**: Adapted from GLSL (`liquid_glass_final_render_core.glsl`)
  into Metal, OpenGL ES 3, Direct3D 11 (including baked DXBC shaders), and
  WebGL 2.

---

## 4. Inter Variable Font (`4.001;git-9221beed3`)

- **Files**: `web/inter.woff2` (subset variable font, loaded by `web/index.html`
  and `packages/site/src/main.pion`).
- **Source**: <https://github.com/rsms/inter/tree/9221beed3>
- **License**: SIL Open Font License 1.1 (full text in
  [`LICENSES/OFL-1.1.txt`](LICENSES/OFL-1.1.txt))
- **Copyright**: `Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)`

---

## 5. JZlib 1.1.3

- **Files**: `android/jzlib/` (see `android/jzlib/LICENSE` and
  `android/jzlib/README.md`).
- **Source**: <https://repo.maven.apache.org/maven2/com/jcraft/jzlib/1.1.3/jzlib-1.1.3-sources.jar>
- **License**: BSD-3-Clause (JZlib 0.0.* were released under GNU LGPL; 1.1.3 is
  BSD-3-Clause; preserves upstream zlib attribution to Jean-loup Gailly and
  Mark Adler).

```text
Copyright (c) 2000-2011 ymnk, JCraft,Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the distribution.

  3. The names of the authors may not be used to endorse or promote products
     derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL JCRAFT,
INC. OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 6. Unicode Character Database (UCD)

- **Files**:
  - UCD 18.0.0: `packages/text/src/tables.pion`,
    `packages/text/src/bidi_tables.pion`, `packages/shape/src/ucd_table.pion`,
    `packages/shape/src/arabic_table.pion`,
    `packages/shape/src/indic_table.pion`.
  - UCD 11.0.0 (`Vertical_Orientation`): `packages/svg/src/vo_table.pion`.
- **Sources**:
  - <https://www.unicode.org/Public/18.0.0/ucd/ReadMe.txt>
  - <https://www.unicode.org/Public/11.0.0/ucd/VerticalOrientation.txt>
- **License**: Unicode License v3 (full text in
  [`LICENSES/Unicode-3.0.txt`](LICENSES/Unicode-3.0.txt))
- **Copyright**:
  - UCD 18.0.0 data files (`ReadMe.txt`): `© 2026 Unicode®, Inc.`
  - Unicode License v3: `Copyright © 1991-2026 Unicode, Inc.`
  - Preserved intermediate upstream (`mbui`) notice: `Copyright © 1991-2025 Unicode, Inc.`
  - UCD 11.0.0 `Vertical_Orientation`: `© 2018 Unicode®, Inc.`

---

## 7. MoonBit core `math` & FreeBSD `msun` / SunPro `fdlibm` / `musl`

- **Files**: `packages/dmath/src/{exp,log,pow,scalbn,sincos,trig,utils}.pion`.
- **Source**: <https://github.com/moonbitlang/core> (`math/`), derived from
  FreeBSD `msun`, SunPro `fdlibm`, and `musl`.
- **License**: Apache-2.0 (full text in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt)),
  plus preserved upstream SunPro `fdlibm`, FreeBSD 2-Clause, and `musl` MIT
  notices below.
- **Copyright**: `Copyright 2025 International Digital Economy Academy`
- **Modifications**: Ported from MoonBit core `math` to Pion.

```text
Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.

Developed at SunPro, a Sun Microsystems, Inc. business.
Permission to use, copy, modify, and distribute this
software is freely granted, provided that this notice
is preserved.
```

```text
Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.

Developed at SunSoft, a Sun Microsystems, Inc. business.
Permission to use, copy, modify, and distribute this
software is freely granted, provided that this notice
is preserved.
```

```text
Copyright (c) 1992-2026 The FreeBSD Project.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

`musl` portions (`scalbn` and math helpers): `Copyright (c) 2005-2020 Rich Felker, et al.` (MIT, full text in [`LICENSE`](LICENSE)).

---

## 8. `tiny-skia` / Skia

- **Files**: `packages/skia/src/*.pion`, `packages/host/native/corner.sl`.
- **Source**: <https://github.com/linebender/tiny-skia>
- **License**: BSD-3-Clause
- **Modifications**: Ported to Pion (`packages/skia`) and GPU shader helpers.

```text
Copyright (c) 2011 Google Inc. All rights reserved.
Copyright (c) 2020 Yevhenii Reizner All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of the copyright holder nor the names of its
    contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 9. `resvg` / `usvg` / `svgtypes` / `roxmltree` / `simplecss` / `kurbo`

- **Files**: `packages/svg/src/*`.
- **Sources**:
  - <https://github.com/linebender/resvg> (`resvg`, `usvg`)
  - <https://github.com/linebender/svgtypes>
  - <https://github.com/RazrFalcon/roxmltree>
  - <https://github.com/linebender/simplecss>
  - <https://github.com/linebender/kurbo>
- **License**: MIT (selected from dual MIT/Apache-2.0; full text in [`LICENSE`](LICENSE))
- **Copyright**:
  - `Copyright 2017 the Resvg Authors` (`resvg`, `usvg`)
  - `Copyright (c) 2018 Yevhenii Reizner` (`svgtypes`, `roxmltree`)
  - `Copyright (c) 2018 Reizner Evgeniy` (`simplecss`)
  - `Copyright (c) 2018 Raph Levien` (`kurbo`)
- **Modifications**: Ported to Pion.

---

## 10. `Fontations` (`skrifa` / `read-fonts` / `font-types`), `fontdb`, `font-kit`, `unicode-vo`

- **Files**: `packages/font/src/*`, `packages/svg/src/vo_table.pion`.
- **Sources**:
  - <https://github.com/googlefonts/fontations>
  - <https://github.com/RazrFalcon/fontdb>
  - <https://github.com/servo/font-kit>
  - <https://github.com/RazrFalcon/unicode-vo>
- **License**: MIT (selected where dual-licensed MIT/Apache-2.0; full text in [`LICENSE`](LICENSE))
- **Copyright**:
  - `Copyright (c) 2019 Fontations Developers` (`skrifa`, `read-fonts`, `font-types`)
  - `Copyright (c) 2020 Yevhenii Reizner` (`fontdb`)
  - `Copyright © 2018 The Pathfinder Project Developers.` (`font-kit` matching logic)
  - `Copyright (c) 2018 Reizner Evgeniy` (`unicode-vo`)
- **Modifications**: Ported to Pion.

---

## 11. `HarfRust` & `HarfBuzz`

- **Files**: `packages/shape/src/*`.
- **Sources**:
  - <https://github.com/harfbuzz/harfrust>
  - <https://github.com/harfbuzz/harfbuzz>
- **HarfRust License**: MIT (full text in [`LICENSE`](LICENSE))
  - `Copyright (c) HarfBuzz developers`
  - `Copyright (c) 2020 Yevhenii Reizner`
- **HarfBuzz License**: Old MIT (verbatim `COPYING` below):

```text
HarfBuzz is licensed under the so-called "Old MIT" license.  Details follow.
For parts of HarfBuzz that are licensed under different licenses see individual
files names COPYING in subdirectories where applicable.

Copyright © 2010-2022  Google, Inc.
Copyright © 2015-2020  Ebrahim Byagowi
Copyright © 2019,2020  Facebook, Inc.
Copyright © 2012,2015  Mozilla Foundation
Copyright © 2011  Codethink Limited
Copyright © 2008,2010  Nokia Corporation and/or its subsidiary(-ies)
Copyright © 2009  Keith Stribley
Copyright © 2011  Martin Hosken and SIL International
Copyright © 2007  Chris Wilson
Copyright © 2005,2006,2020,2021,2022,2023  Behdad Esfahbod
Copyright © 2004,2007,2008,2009,2010,2013,2021,2022,2023  Red Hat, Inc.
Copyright © 1998-2005  David Turner and Werner Lemberg
Copyright © 2016  Igalia S.L.
Copyright © 2022  Matthias Clasen
Copyright © 2018,2021  Khaled Hosny
Copyright © 2018,2019,2020  Adobe, Inc
Copyright © 2013-2015  Alexei Podtelezhnikov

For full copyright notices consult the individual files in the package.


Permission is hereby granted, without written agreement and without
license or royalty fees, to use, copy, modify, and distribute this
software and its documentation for any purpose, provided that the
above copyright notice and the following two paragraphs appear in
all copies of this software.

IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
```

---

## 12. Yoga

- **Files**: `packages/flex/src/*`.
- **Source**: <https://github.com/facebook/yoga/tree/52f6d0bd3824296b8e852cb4fcdf3b9821ab2aa5>
- **License**: MIT (full text in [`LICENSE`](LICENSE))
- **Copyright**: `Copyright (c) Facebook, Inc. and its affiliates.`
- **Modifications**: Layout tests and defaults ported to Pion.

---

## 13. Motion

- **Files**: `packages/motion/src/{easing,generators,transition,utils}.pion`.
- **Source**: <https://github.com/motiondivision/motion/tree/ce1d082>
- **License**: MIT (full text in [`LICENSE`](LICENSE))
- **Copyright**: `Copyright (c) 2024 [Motion](https://motion.dev) B.V.`
- **Modifications**: Ported from TypeScript to Pion.

---

## 14. Android Open Source Project (AOSP) / Jetpack Compose

- **Files**: `packages/view/src/view_input.pion`,
  `packages/view/src/list_motion.pion`, `packages/motion/src/spec.pion`.
- **Sources**:
  - <https://android.googlesource.com/platform/frameworks/support> (`VelocityTracker.kt`, `SpringSimulation.kt`)
  - <https://android.googlesource.com/platform/frameworks/base> (`OverScroller.java`)
- **License**: Apache-2.0 (full text in [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt);
  notices also in [`NOTICE`](NOTICE))
- **Copyright**:
  - `Copyright 2019 The Android Open Source Project`
  - `Copyright (C) 2010 The Android Open Source Project`
- **Modifications**: Velocity tracking, fling splines, and spring physics
  ported to Pion.

---

## 15. `alien-signals` 3.2.1

- **Files**: `packages/signals/src/{system,signals}.pion`.
- **Source**: <https://github.com/stackblitz/alien-signals>
- **License**: MIT (full text in [`LICENSE`](LICENSE))
- **Copyright**: `Copyright (c) 2024-present Johnson Chu`
- **Modifications**: Ported from TypeScript to Pion.

---

## 16. `zune-jpeg`, `stb_image`, `image-webp`, `ThumbHash`

- **Files**: `packages/pixels/src/{jpeg_decode,webp_decode,thumbhash}.pion`.
- **Sources & Licenses**:
  - **zune-jpeg** (<https://github.com/etemesi254/zune-image>): MIT (selected
    from MIT/Apache-2.0/Zlib; full text in [`LICENSE`](LICENSE)),
    `Copyright (c) zune-image developers`.
  - **stb_image** (IDCT derivation, <https://github.com/nothings/stb>): MIT
    (full text in [`LICENSE`](LICENSE)), `Copyright (c) 2017 Sean Barrett`.
  - **image-webp** (<https://github.com/image-rs/image-webp>): MIT (selected
    from MIT/Apache-2.0; full text in [`LICENSE`](LICENSE); upstream
    `LICENSE-MIT` carries no separate copyright-holder year line).
  - **ThumbHash** (<https://github.com/evanw/thumbhash>): MIT (full text in
    [`LICENSE`](LICENSE)), `Copyright (c) 2023 Evan Wallace`.
- **Modifications**: Ported to Pion.

---

## 17. MyGo

- **Files**: `packages/host/native/bands.h`.
- **Source**: <https://github.com/egoist/mygo>
- **License**: MIT (full text in [`LICENSE`](LICENSE))
- **Copyright**: `Copyright (c) 2026 MyGo contributors`
- **Modifications**: Banding geometry helpers ported to C.

---

## 18. `AFK-surf/pion` WebAssembly Runtime

- **Files**: `web/pion_wasm_rt_web.mjs`, `web/pion_wasm_rt_web.min.mjs`.
- **Source**: <https://github.com/AFK-surf/pion>
- **License**: MIT (full text in [`LICENSE`](LICENSE))
- **Copyright**: `Copyright (c) 2026 AFK-surf contributors`

---

## 19. Fast Rounded Rectangle Shadows & Build-Time Dependencies

- **Fast Rounded Rectangle Shadows** (`packages/host/native/corner.sl`,
  `packages/host/native/corner_sl.h`, `web/host.js`):
  - Source: <https://www.madebyevan.com/shaders/fast-rounded-rectangle-shadows/>
  - Author: Evan Wallace
  - License: CC0-1.0 (Public Domain Dedication)
- **Microsoft WebView2 SDK (`1.0.4258.31`, build-time download only)**:
  - **Not included in this repository** (excluded via `packages/host/.gitignore`).
  - Downloaded at Windows build time from NuGet by
    `packages/host/tools/fetch-webview2.sh`.
  - Subject to the Microsoft Software License Terms for Microsoft.Web.WebView2
    1.0.4258.31 (<https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4258.31/License>).
