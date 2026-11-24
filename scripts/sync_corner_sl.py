#!/usr/bin/env python3
"""Synchronize packages/host/native/corner.sl -> corner_sl.h and web/host.js.

Usage:
  python3 scripts/sync_corner_sl.py          # regenerate corner_sl.h and web/host.js
  python3 scripts/sync_corner_sl.py --check  # verify both outputs are up-to-date (exit 1 on drift)
"""

from __future__ import annotations

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORNER_SL = os.path.join(ROOT, "packages", "host", "native", "corner.sl")
CORNER_SL_H = os.path.join(ROOT, "packages", "host", "native", "corner_sl.h")
WEB_HOST_JS = os.path.join(ROOT, "web", "host.js")


def generate_corner_sl_h(sl_text: str) -> str:
    lines = sl_text.splitlines()
    out = [
        "// packages/host/native/corner.sl as a C string (scripts/sync_corner_sl.py makes it: edit corner.sl, then run it). The shaders",
        "// write float2; GLSL's take it with `#define float2 vec2` and `#define float4 vec4` before it.",
        "#define MB_CORNER_SL \\",
    ]
    for line in lines:
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'  "{escaped}\\n" \\')
    out.append('  ""')
    return "\n".join(out) + "\n"


def build_web_blocks(sl_text: str) -> tuple[str, str]:
    """Split corner.sl into (CORNERS_body, CONTAINED_GLSL_body) matching web/host.js."""
    lines = sl_text.splitlines()

    def find_prefix(prefix: str, start: int = 0) -> int:
        for i in range(start, len(lines)):
            if lines[i].startswith(prefix):
                return i
        raise ValueError(f"Expected marker {prefix!r} in corner.sl")

    i_contained = find_prefix("// A contained blur extends the picture")
    i_shadow = find_prefix("// A shadow as Core Animation draws one", i_contained)
    i_shape = find_prefix("// A shape (", i_shadow)
    i_dither = find_prefix("// How far to move a smooth ground", i_shape)
    i_endif = len(lines) - 1
    while i_endif >= 0 and lines[i_endif].strip() != "#endif":
        i_endif -= 1
    if i_endif < i_dither:
        raise ValueError("Expected trailing #endif in corner.sl")

    part_head = lines[:i_contained]
    part_contained = lines[i_contained:i_shadow]
    part_shadow = lines[i_shadow:i_shape]
    part_shape = lines[i_shape:i_dither]
    part_dither = lines[i_dither:i_endif]

    corners_lines: list[str] = [
        "  // corner.sl begin (scripts/sync_corner_sl.py: not edited here)",
        "  #define float2 vec2",
        "  #define float4 vec4",
    ]
    for ln in part_head:
        corners_lines.append("  " + ln if ln else "")
    corners_lines.append("  #if defined(ONE_KIND) && (ONE_KIND < 0 || ONE_KIND == 2)")
    for ln in part_shadow:
        corners_lines.append("  " + ln if ln else "")
    corners_lines.append("  #endif")
    corners_lines.append("  #if defined(ONE_KIND) && (ONE_KIND < 0 || ONE_KIND == 10 || ONE_KIND == 11)")
    for ln in part_shape:
        corners_lines.append("  " + ln if ln else "")
    corners_lines.append("  #endif")
    corners_lines.append("  #if defined(ONE_KIND) && (ONE_KIND < 0 || ONE_KIND == 5 || ONE_KIND == 6 || ONE_KIND == 13)")
    for ln in part_dither:
        corners_lines.append("  " + ln if ln else "")
    corners_lines.append("  #endif")
    corners_lines.append("  #endif")
    corners_lines.append("  // corner.sl end")

    contained_lines: list[str] = []
    for ln in part_contained:
        contained_lines.append("  " + ln if ln else "")

    return "\n".join(corners_lines), "\n".join(contained_lines)


def update_web_host_js(js_text: str, sl_text: str) -> str:
    corners_body, contained_body = build_web_blocks(sl_text)

    c_start_marker = "  const CORNERS = `\n"
    c_end_marker = "\n  `;\n  const CONTAINED_GLSL = `\n"
    g_end_marker = "\n  `;\n  const FS = `#version 300 es"

    i0 = js_text.find(c_start_marker)
    if i0 < 0:
        raise ValueError("Could not locate `const CORNERS =` in web/host.js")
    i1 = js_text.find(c_end_marker, i0)
    if i1 < 0:
        raise ValueError("Could not locate `const CONTAINED_GLSL =` in web/host.js")
    i2 = js_text.find(g_end_marker, i1)
    if i2 < 0:
        raise ValueError("Could not locate `const FS =` after CONTAINED_GLSL in web/host.js")

    return (
        js_text[: i0 + len(c_start_marker)]
        + corners_body
        + c_end_marker
        + contained_body
        + js_text[i2:]
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Sync corner.sl to corner_sl.h and web/host.js")
    parser.add_argument("--check", action="store_true", help="Check only; exit 1 if out of sync")
    args = parser.parse_args()

    with open(CORNER_SL, "r", encoding="utf-8") as f:
        sl_text = f.read()
    with open(CORNER_SL_H, "r", encoding="utf-8") as f:
        cur_h = f.read()
    with open(WEB_HOST_JS, "r", encoding="utf-8") as f:
        cur_js = f.read()

    want_h = generate_corner_sl_h(sl_text)
    want_js = update_web_host_js(cur_js, sl_text)

    if args.check:
        drifted: list[str] = []
        if cur_h != want_h:
            drifted.append("packages/host/native/corner_sl.h")
        if cur_js != want_js:
            drifted.append("web/host.js")
        if drifted:
            sys.stderr.write(
                "corner.sl out of sync with: "
                + ", ".join(drifted)
                + "\nRun: python3 scripts/sync_corner_sl.py\n"
            )
            return 1
        print("corner.sl sync check passed (corner_sl.h and web/host.js match).")
        return 0

    if cur_h != want_h:
        with open(CORNER_SL_H, "w", encoding="utf-8") as f:
            f.write(want_h)
    if cur_js != want_js:
        with open(WEB_HOST_JS, "w", encoding="utf-8") as f:
            f.write(want_js)
    print("Synced packages/host/native/corner.sl -> corner_sl.h and web/host.js.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
