#!/usr/bin/env python3
"""Strict pixel-level verification gate for packages/aura against HeroUI v3.2.8 ground truth.

Usage:
  python3 scripts/aura_verify.py --self-test
  python3 scripts/aura_verify.py --manifest ref_w0_manifest.json --ref-dir ../ref_suite --actual-dir out/shots [--required-manifest base.json] [--sbs-dir out/sbs] [--only <id>]
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import sys
import tempfile
import zlib
from typing import Any

MAX_AA_P95_OVERRIDE = 18.0
ALLOWED_DPRS = (1, 2)
MAX_TEXT_CORNER_DIFF = 16
MAX_TEXT_BG_FILL_FRAC = 0.22
MIN_SOLID_BLOCK_SIDE = 8
MAX_SOLID_BLOCK_AREA_FRAC = 0.015
MAX_SOLID_MASK_COVERAGE = 0.75
# Text glyph-content & glyph-order verification (calibrated across 567 Wave 0+1 ROIs):
# - Real cross-engine (Chromium Skia vs Linux FreeType / macOS CoreText): min 2D NCC = 0.6485 (Kbd),
#   min Latin text 2D NCC = 0.7673, median = 0.9511; min col NCC = 0.6661.
# - Per-glyph segmented matching: across all 567 real ROIs, every 2x Latin ROI has min_seg_ncc >= 0.7879,
#   every 1x Wave 1 Latin ROI has min_seg_ncc >= 0.7584 (Metal) / 0.7791 (Linux); only m01-kbd-text-0
#   (vector U+2318 vs font) and two 1x Wave 0 ROIs sit below 0.70 and carry explicit, hard-floored
#   `min_seg_ncc_override` entries (>= 0.45 with mandatory reason).
# - Faults: real Inter "end->edn" min_seg_ncc = 0.5701..0.6275 (< 0.70, REJECTED); real Inter
#   "Seocndary" (c/o swap) cell_diff = 0.6082; real Inter "Sec ndary" (erased o) min_seg_ncc = 0.0;
#   mirrored "Secondary" = 0.2697; RTL BiDi reversed = 0.1342..0.2725.
MIN_TEXT_GLYPH_NCC_2D = 0.62
MIN_TEXT_COL_NCC = 0.64
MAX_TEXT_RUN_EXTENT_DIFF_PT = 2.5
MIN_TEXT_SEG_NCC = 0.70
MIN_TEXT_SEG_NCC_OVERRIDE_FLOOR = 0.45
MIN_TEXT_SEG_MASS_RATIO = 0.45
MAX_TEXT_SEG_CELL_DIFF = 0.42
# AA override interior solid square cap: rejects whole controls (Switch/Radio/Checkbox/Button)
# stamped kind="aa" with max_p95_override (non-bg interior uniform solid square >= 8x8 logical pt,
# i.e. >= 8px at 1x or >= 16px at 2x, OR oversized box min(w,h)>16pt & max(w,h)>28pt).
MAX_AA_OVERRIDE_SOLID_SQ_PT = 8
# Midpoint transition-curve tolerance between Chromium CSS ease (48ms) and Aura spring (48ms):
# actual measured thumb centroid diff is 1.51pt (well within 6.0pt); Linux-vs-Metal is <= 1px.
# Plus independent track color interpolation, thumb appearance/area, and outer background gates.
MIDPOINT_MIN_ENDPOINT_DELTA = 40
MIDPOINT_MIN_TRAVEL_MARGIN_PT = 1.5
MIDPOINT_MAX_REF_CX_DIFF_PT = 6.0
MIDPOINT_TRACK_MAX_PERP_DIST = 18.0
MIDPOINT_TRACK_MIN_PROGRESS = 0.12
MIDPOINT_TRACK_MAX_PROGRESS = 0.88
MIDPOINT_TRACK_MAX_REF_CH_DIFF = 35
MIDPOINT_THUMB_MAX_RGB_DIFF = 15
MIDPOINT_THUMB_MIN_AREA_RATIO = 0.75
MIDPOINT_THUMB_MAX_AREA_RATIO = 1.35
MIDPOINT_OUTER_BG_MAX_DIFF = 3


# ---------------------------------------------------------------------------
# Pure-Python minimal PNG reader/writer (zero external dependencies)
# ---------------------------------------------------------------------------

def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png_rgba(path: str) -> tuple[int, int, bytearray]:
    """Decode an 8-bit PNG (RGB, RGBA, Gray, Gray+Alpha, Indexed) to (w, h, rgba_bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"Not a PNG file: {path}")
    pos = 8
    w = h = bit_depth = color_type = interlace = 0
    idat = bytearray()
    plte = b""
    trns = b""
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            w, h, bit_depth, color_type, _comp, _filt, interlace = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"PLTE":
            plte = chunk
        elif ctype == b"tRNS":
            trns = chunk
        elif ctype == b"IDAT":
            idat.extend(chunk)
        elif ctype == b"IEND":
            break
    if w <= 0 or h <= 0 or bit_depth != 8 or interlace != 0:
        raise ValueError(f"Unsupported PNG format in {path}: {w}x{h} depth={bit_depth} interlace={interlace}")
    bpp_map = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in bpp_map:
        raise ValueError(f"Unsupported PNG color_type={color_type} in {path}")
    bpp = bpp_map[color_type]
    raw = zlib.decompress(bytes(idat))
    stride = w * bpp
    if len(raw) < h * (stride + 1):
        raise ValueError(f"Truncated PNG IDAT in {path}")
    recon = bytearray(h * stride)
    prev = bytearray(stride)
    idx = 0
    for y in range(h):
        ftype = raw[idx]
        idx += 1
        row = bytearray(raw[idx : idx + stride])
        idx += stride
        if ftype == 0:
            pass
        elif ftype == 1:
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up = prev[i]
                up_left = prev[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + _paeth(left, up, up_left)) & 0xFF
        else:
            raise ValueError(f"Unsupported PNG filter {ftype} in {path}")
        recon[y * stride : (y + 1) * stride] = row
        prev = row

    if color_type == 6:
        return w, h, recon

    rgba = bytearray(w * h * 4)
    if color_type == 2:
        si = di = 0
        for _ in range(w * h):
            rgba[di] = recon[si]
            rgba[di + 1] = recon[si + 1]
            rgba[di + 2] = recon[si + 2]
            rgba[di + 3] = 255
            si += 3
            di += 4
    elif color_type == 0:
        for i in range(w * h):
            v = recon[i]
            di = i * 4
            rgba[di] = rgba[di + 1] = rgba[di + 2] = v
            rgba[di + 3] = 255
    elif color_type == 4:
        for i in range(w * h):
            v = recon[i * 2]
            a = recon[i * 2 + 1]
            di = i * 4
            rgba[di] = rgba[di + 1] = rgba[di + 2] = v
            rgba[di + 3] = a
    elif color_type == 3:
        for i in range(w * h):
            pi = recon[i]
            di = i * 4
            rgba[di] = plte[pi * 3]
            rgba[di + 1] = plte[pi * 3 + 1]
            rgba[di + 2] = plte[pi * 3 + 2]
            rgba[di + 3] = trns[pi] if pi < len(trns) else 255
    return w, h, rgba


def write_png_rgba(path: str, w: int, h: int, rgba: bytes | bytearray) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride : (y + 1) * stride])
    comp = zlib.compress(bytes(raw), 6)

    def chunk(ctype: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(ctype + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + ctype + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", comp) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def write_sbs_png(
    path: str,
    w: int,
    h: int,
    ref_rgba: bytes | bytearray,
    act_rgba: bytes | bytearray,
) -> None:
    """Write 3-panel side-by-side PNG: [HeroUI ref | Aura actual | 6x amplified diff]."""
    gap = 4
    out_w = w * 3 + gap * 2
    out = bytearray(out_w * h * 4)
    for y in range(h):
        for gx in (w, w * 2 + gap):
            for dx in range(gap):
                di = (y * out_w + gx + dx) * 4
                out[di] = 128
                out[di + 1] = 128
                out[di + 2] = 128
                out[di + 3] = 255
        ref_row_off = y * w * 4
        out_row_off = y * out_w * 4
        out[out_row_off : out_row_off + w * 4] = ref_rgba[ref_row_off : ref_row_off + w * 4]
        mid_off = out_row_off + (w + gap) * 4
        out[mid_off : mid_off + w * 4] = act_rgba[ref_row_off : ref_row_off + w * 4]
        diff_off = out_row_off + (w * 2 + gap * 2) * 4
        for x in range(w):
            si = ref_row_off + x * 4
            dr = abs(ref_rgba[si] - act_rgba[si])
            dg = abs(ref_rgba[si + 1] - act_rgba[si + 1])
            db = abs(ref_rgba[si + 2] - act_rgba[si + 2])
            da = abs(ref_rgba[si + 3] - act_rgba[si + 3])
            d = max(dr, dg, db, da)
            di = diff_off + x * 4
            if d == 0:
                out[di] = 16
                out[di + 1] = 18
                out[di + 2] = 24
                out[di + 3] = 255
            else:
                v = min(255, d * 6)
                out[di] = v
                out[di + 1] = min(255, max(0, (d - 2) * 4))
                out[di + 2] = min(255, max(0, 64 - d * 2))
                out[di + 3] = 255
    write_png_rgba(path, out_w, h, out)


# ---------------------------------------------------------------------------
# Per-pixel delta & ROI metrics
# ---------------------------------------------------------------------------

def _roi_deltas(
    w: int,
    ref_rgba: bytes | bytearray,
    act_rgba: bytes | bytearray,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
) -> list[int]:
    deltas: list[int] = []
    for y in range(y0, y1):
        row = y * w * 4
        for x in range(x0, x1):
            i = row + x * 4
            d = max(
                abs(ref_rgba[i] - act_rgba[i]),
                abs(ref_rgba[i + 1] - act_rgba[i + 1]),
                abs(ref_rgba[i + 2] - act_rgba[i + 2]),
                abs(ref_rgba[i + 3] - act_rgba[i + 3]),
            )
            deltas.append(d)
    return deltas


def _percentile(sorted_vals: list[int], pct: float) -> float:
    if not sorted_vals:
        return 0.0
    n = len(sorted_vals)
    idx = int(math.ceil((pct / 100.0) * n)) - 1
    idx = max(0, min(n - 1, idx))
    return float(sorted_vals[idx])


def _text_ink_stats(
    w: int,
    rgba: bytes | bytearray,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
) -> dict[str, Any] | None:
    """Detect foreground glyph bbox, core ink RGB, and ink pixel count/mass relative to ROI background."""
    corners = [
        (x0, y0),
        (x1 - 1, y0),
        (x0, y1 - 1),
        (x1 - 1, y1 - 1),
    ]
    corner_rgbs = [
        (rgba[(cy * w + cx) * 4], rgba[(cy * w + cx) * 4 + 1], rgba[(cy * w + cx) * 4 + 2])
        for cx, cy in corners
    ]
    corner_diff = 0
    for ci in range(4):
        cr0, cg0, cb0 = corner_rgbs[ci]
        for cj in range(ci + 1, 4):
            cr1, cg1, cb1 = corner_rgbs[cj]
            cd = max(abs(cr0 - cr1), abs(cg0 - cg1), abs(cb0 - cb1))
            if cd > corner_diff:
                corner_diff = cd
    bg_r = sorted(c[0] for c in corner_rgbs)[1]
    bg_g = sorted(c[1] for c in corner_rgbs)[1]
    bg_b = sorted(c[2] for c in corner_rgbs)[1]

    roi_w = x1 - x0
    roi_h = y1 - y0
    total_px = roi_w * roi_h
    min_x = x1
    min_y = y1
    max_x = x0 - 1
    max_y = y0 - 1
    thresh = 24
    ink_pixels: list[tuple[int, int, int, int]] = []  # (dist, r, g, b)
    bucket_counts: dict[tuple[int, int, int], int] = {}
    prev_dp = [0] * roi_w
    max_solid_sq = 0
    max_dist = 0
    sum_yd = 0.0
    sum_d = 0.0
    row_w: list[float] = [0.0] * roi_h
    for y in range(y0, y1):
        row = y * w * 4
        prev_row = (y - 1) * w * 4
        ry_rel = y - y0
        rw_acc = 0.0
        curr_dp = [0] * roi_w
        for x in range(x0, x1):
            rx_rel = x - x0
            i = row + x * 4
            r = rgba[i]
            g = rgba[i + 1]
            b = rgba[i + 2]
            d = max(abs(r - bg_r), abs(g - bg_g), abs(b - bg_b))
            if d >= thresh:
                if x < min_x:
                    min_x = x
                if y < min_y:
                    min_y = y
                if x > max_x:
                    max_x = x
                if y > max_y:
                    max_y = y
                if d > max_dist:
                    max_dist = d
                sum_yd += ry_rel * d
                sum_d += d
                rw_acc += float(d)
                ink_pixels.append((d, r, g, b))
                bk = (r >> 3, g >> 3, b >> 3)
                bucket_counts[bk] = bucket_counts.get(bk, 0) + 1
                if ry_rel == 0 or rx_rel == 0:
                    sq = 1
                else:
                    left_dp = curr_dp[rx_rel - 1]
                    up_dp = prev_dp[rx_rel]
                    ul_dp = prev_dp[rx_rel - 1]
                    if left_dp > 0 and up_dp > 0 and ul_dp > 0:
                        li = i - 4
                        ui = prev_row + x * 4
                        uli = ui - 4
                        if (
                            abs(r - rgba[li]) <= 2
                            and abs(g - rgba[li + 1]) <= 2
                            and abs(b - rgba[li + 2]) <= 2
                            and abs(r - rgba[ui]) <= 2
                            and abs(g - rgba[ui + 1]) <= 2
                            and abs(b - rgba[ui + 2]) <= 2
                            and abs(r - rgba[uli]) <= 2
                            and abs(g - rgba[uli + 1]) <= 2
                            and abs(b - rgba[uli + 2]) <= 2
                        ):
                            m = left_dp if left_dp < up_dp else up_dp
                            if ul_dp < m:
                                m = ul_dp
                            sq = m + 1
                        else:
                            sq = 1
                    else:
                        sq = 1
                curr_dp[rx_rel] = sq
                if sq > max_solid_sq:
                    max_solid_sq = sq
        row_w[ry_rel] = rw_acc
        prev_dp = curr_dp
    if max_x < min_x or max_y < min_y or not ink_pixels or sum_d <= 0.0:
        return None
    max_fill_bucket_frac = (
        max(bucket_counts.values()) / float(total_px) if bucket_counts and total_px > 0 else 0.0
    )
    solid_sq_area_frac = (
        float(max_solid_sq * max_solid_sq) / float(total_px) if total_px > 0 else 0.0
    )

    # Segment contiguous horizontal ink bands (text lines) so multi-line ROIs check every
    # line's vertical centroid / baseline independently (<= 1.0 device px at both 1x and 2x).
    line_bands: list[dict[str, float]] = []
    in_band = False
    band_start = 0
    for idx, val in enumerate(row_w):
        if val > 0.0 and not in_band:
            in_band = True
            band_start = idx
        elif val <= 0.0 and in_band:
            in_band = False
            tot = sum(row_w[band_start:idx])
            yc = sum(float(k) * row_w[k] for k in range(band_start, idx)) / tot
            line_bands.append({"y0": float(band_start), "y1": float(idx), "y_center": yc})
    if in_band:
        tot = sum(row_w[band_start:])
        yc = sum(float(k) * row_w[k] for k in range(band_start, len(row_w))) / tot
        line_bands.append({"y0": float(band_start), "y1": float(len(row_w)), "y_center": yc})

    # Core ink color: mean RGB among peak-contrast ink pixels (>= 80% of peak contrast).
    # Mean avoids discontinuous median jumps on multi-color text ROIs (e.g. white text + accent CTA in one box),
    # and the 80% peak band strips 1x anti-aliasing fringe so true glyph ink color is measured consistently at 1x/2x.
    core_thresh = max(thresh, int(max_dist * 0.8))
    core_r = 0
    core_g = 0
    core_b = 0
    core_n = 0
    for d, r, g, b in ink_pixels:
        if d >= core_thresh:
            core_r += r
            core_g += g
            core_b += b
            core_n += 1
    if core_n == 0:
        core_r = sum(p[1] for p in ink_pixels)
        core_g = sum(p[2] for p in ink_pixels)
        core_b = sum(p[3] for p in ink_pixels)
        core_n = len(ink_pixels)
    ink_rgb = [
        int(round(core_r / float(core_n))),
        int(round(core_g / float(core_n))),
        int(round(core_b / float(core_n))),
    ]

    return {
        "bbox": (min_x - x0, min_y - y0, max_x - x0, max_y - y0),
        "y_center": sum_yd / sum_d,
        "line_bands": line_bands,
        "ink_count": len(ink_pixels),
        "ink_mass": sum_d,
        "ink_rgb": ink_rgb,
        "bg_rgb": [bg_r, bg_g, bg_b],
        "corner_diff": corner_diff,
        "max_fill_bucket_frac": max_fill_bucket_frac,
        "max_solid_sq": max_solid_sq,
        "solid_sq_area_frac": solid_sq_area_frac,
    }


def _text_glyph_ncc(
    w: int,
    ref_rgba: bytes | bytearray,
    act_rgba: bytes | bytearray,
    rx: int,
    ry: int,
    rw: int,
    rh: int,
    dpr: int,
    ref_st: dict[str, Any],
    act_st: dict[str, Any],
) -> dict[str, Any]:
    """Compute 2D ink-intensity NCC, column-projection NCC, and per-glyph-run vertical extent check."""
    rbb = ref_st["bbox"]
    abb = act_st["bbox"]
    bx0 = max(0, min(rbb[0], abb[0]) - 1)
    by0 = max(0, min(rbb[1], abb[1]) - 1)
    bx1 = min(rw, max(rbb[2], abb[2]) + 2)
    by1 = min(rh, max(rbb[3], abb[3]) + 2)
    cw = bx1 - bx0
    ch = by1 - by0
    if cw <= 1 or ch <= 1:
        return {
            "ncc_2d": 1.0,
            "ncc_col": 1.0,
            "best_shift": [0, 0],
            "max_run_extent_diff_px": 0,
            "min_seg_ncc": 1.0,
            "min_seg_mass_ratio": 1.0,
            "max_seg_cell_diff": 0.0,
            "seg_count": 0,
        }

    rbr, rbg, rbb_c = ref_st["bg_rgb"]
    abr, abg, abb_c = act_st["bg_rgb"]
    rm: list[float] = [0.0] * (cw * ch)
    am: list[float] = [0.0] * (cw * ch)
    rcol: list[float] = [0.0] * cw
    acol: list[float] = [0.0] * cw
    inv255 = 1.0 / 255.0
    for y in range(ch):
        row = (ry + by0 + y) * w * 4
        ro = y * cw
        for x in range(cw):
            i = row + (rx + bx0 + x) * 4
            rv = max(abs(ref_rgba[i] - rbr), abs(ref_rgba[i + 1] - rbg), abs(ref_rgba[i + 2] - rbb_c)) * inv255
            av_ = max(abs(act_rgba[i] - abr), abs(act_rgba[i + 1] - abg), abs(act_rgba[i + 2] - abb_c)) * inv255
            rm[ro + x] = rv
            am[ro + x] = av_
            rcol[x] += rv
            acol[x] += av_

    best_2d = -1.0
    best_shift = (0, 0)
    for dy in (-1, 0, 1):
        sy0 = max(0, -dy)
        sy1 = min(ch, ch - dy)
        if sy1 <= sy0:
            continue
        for dx in (-2, -1, 0, 1, 2):
            sx0 = max(0, -dx)
            sx1 = min(cw, cw - dx)
            if sx1 <= sx0:
                continue
            r_all: list[float] = []
            a_all: list[float] = []
            for y in range(sy0, sy1):
                ro = y * cw
                ao = (y + dy) * cw
                r_all.extend(rm[ro + sx0 : ro + sx1])
                a_all.extend(am[ao + sx0 + dx : ao + sx1 + dx])
            n = len(r_all)
            if n <= 1:
                continue
            mr = sum(r_all) / n
            ma = sum(a_all) / n
            num = sum((u - mr) * (v - ma) for u, v in zip(r_all, a_all))
            dr2 = sum((u - mr) * (u - mr) for u in r_all)
            da2 = sum((v - ma) * (v - ma) for v in a_all)
            if dr2 > 1e-9 and da2 > 1e-9:
                c = num / math.sqrt(dr2 * da2)
                if c > best_2d:
                    best_2d = c
                    best_shift = (dx, dy)

    best_col = -1.0
    for dx in (-2, -1, 0, 1, 2):
        sx0 = max(0, -dx)
        sx1 = min(cw, cw - dx)
        n = sx1 - sx0
        if n <= 1:
            continue
        rc = rcol[sx0:sx1]
        ac = acol[sx0 + dx : sx1 + dx]
        mr = sum(rc) / n
        ma = sum(ac) / n
        num = sum((u - mr) * (v - ma) for u, v in zip(rc, ac))
        dr2 = sum((u - mr) * (u - mr) for u in rc)
        da2 = sum((v - ma) * (v - ma) for v in ac)
        if dr2 > 1e-9 and da2 > 1e-9:
            c = num / math.sqrt(dr2 * da2)
            if c > best_col:
                best_col = c

    # Per-glyph-run vertical extent check (catches same-width character swaps like "Copy" -> "Cpoy"
    # where 'o' and 'p' share a similar circular bowl but differ in ascender/descender extent).
    max_run_extent_diff_px = 0
    if len(ref_st.get("line_bands", [])) == 1 and len(act_st.get("line_bands", [])) == 1:
        ink_thr = 24.0 * inv255

        def _extract_runs(imap: list[float]) -> list[tuple[int, int, int, int]]:
            runs: list[tuple[int, int, int, int]] = []
            in_r = False
            s0 = 0
            r_miny = ch
            r_maxy = -1
            for x in range(cw):
                c_miny = ch
                c_maxy = -1
                for y in range(ch):
                    if imap[y * cw + x] >= ink_thr:
                        if y < c_miny:
                            c_miny = y
                        if y > c_maxy:
                            c_maxy = y
                if c_maxy >= 0:
                    if not in_r:
                        in_r = True
                        s0 = x
                        r_miny = c_miny
                        r_maxy = c_maxy
                    else:
                        if c_miny < r_miny:
                            r_miny = c_miny
                        if c_maxy > r_maxy:
                            r_maxy = c_maxy
                elif in_r:
                    in_r = False
                    runs.append((s0, x, r_miny, r_maxy))
            if in_r:
                runs.append((s0, cw, r_miny, r_maxy))
            return runs

        r_runs = _extract_runs(rm)
        a_runs = _extract_runs(am)
        if len(r_runs) >= 2 and len(r_runs) == len(a_runs):
            x_tol = 2.5 * dpr
            aligned = all(
                abs((rr[0] + rr[1]) * 0.5 - (ar[0] + ar[1]) * 0.5) <= x_tol
                for rr, ar in zip(r_runs, a_runs)
            )
            if aligned:
                for rr, ar in zip(r_runs, a_runs):
                    if (rr[1] - rr[0]) >= 3 * dpr and (ar[1] - ar[0]) >= 3 * dpr:
                        d_top = abs(rr[2] - ar[2])
                        d_bot = abs(rr[3] - ar[3])
                        md = d_top if d_top > d_bot else d_bot
                        if md > max_run_extent_diff_px:
                            max_run_extent_diff_px = md

    # Per-line-band, per-glyph horizontal segmented matching with monotonic order,
    # local ink-mass existence check, and 3x3 sub-cell topology check.
    # Catches single-glyph erasures ("Sec ndary") and same-height adjacent swaps ("Seocndary").
    ink_thr = 24.0 * inv255
    bdx, bdy = best_shift
    ref_bands = ref_st.get("line_bands", [])
    if not ref_bands:
        ref_bands = [{"y0": float(rbb[1]), "y1": float(rbb[3] + 1)}]

    min_seg_ncc = 1.0
    min_seg_mass_ratio = 1.0
    max_seg_cell_diff = 0.0
    seg_count = 0

    for rb in ref_bands:
        ly0 = max(0, int(rb["y0"]) - by0)
        ly1 = min(ch, int(rb["y1"]) - by0)
        if ly1 <= ly0:
            continue

        col_ink = [0.0] * cw
        col_any = [False] * cw
        for y in range(ly0, ly1):
            ro = y * cw
            for x in range(cw):
                v = rm[ro + x]
                if v >= ink_thr:
                    col_any[x] = True
                    col_ink[x] += v

        raw_segs: list[tuple[int, int]] = []
        in_s = False
        s0 = 0
        for x in range(cw):
            if col_any[x] and not in_s:
                in_s = True
                s0 = x
            elif not col_any[x] and in_s:
                in_s = False
                raw_segs.append((s0, x))
        if in_s:
            raw_segs.append((s0, cw))

        segs: list[tuple[int, int, int, int, float]] = []
        for sx0, sx1 in raw_segs:
            smass = sum(col_ink[sx0:sx1])
            if smass < 1.5 * dpr:
                continue
            s_miny, s_maxy = ly1, ly0 - 1
            for y in range(ly0, ly1):
                ro = y * cw
                for x in range(sx0, sx1):
                    if rm[ro + x] >= ink_thr:
                        if y < s_miny:
                            s_miny = y
                        if y > s_maxy:
                            s_maxy = y
                        break
            if s_maxy < s_miny:
                continue
            segs.append((sx0, sx1, s_miny, s_maxy + 1, smass))

        if len(segs) < 2:
            continue

        r_xmin = segs[0][0]
        r_xmax = segs[-1][1]
        a_xmin, a_xmax = cw, -1
        for y in range(ly0, ly1):
            ao = y * cw
            for x in range(cw):
                if am[ao + x] >= ink_thr:
                    if x < a_xmin:
                        a_xmin = x
                    if x + 1 > a_xmax:
                        a_xmax = x + 1
        r_span = max(1, r_xmax - r_xmin)
        a_span = (a_xmax - a_xmin) if a_xmax > a_xmin else r_span
        scale_x = a_span / float(r_span)
        if not (0.92 <= scale_x <= 1.08):
            scale_x = 1.0
            a_xmin = r_xmin + bdx

        prev_matched_cx = -1e9
        max_dx_local = 2 * dpr
        max_dy_local = 1 * dpr

        for sx0, sx1, sy0, sy1, _r_mass in segs:
            px0 = max(0, sx0 - 1)
            px1 = min(cw, sx1 + 1)
            py0 = max(0, sy0 - 1)
            py1 = min(ch, sy1 + 1)
            sw_p = px1 - px0
            sh_p = py1 - py0
            if sw_p <= 1 or sh_p <= 1:
                continue

            seg_cx = 0.5 * (sx0 + sx1)
            if r_span < 30 * dpr:
                base_dx = bdx
            else:
                exp_cx = a_xmin + (seg_cx - r_xmin) * scale_x
                base_dx = int(round(exp_cx - seg_cx))

            r_vec: list[float] = []
            for y in range(py0, py1):
                ro = y * cw
                r_vec.extend(rm[ro + px0 : ro + px1])
            n = len(r_vec)
            r_patch_mass = sum(v for v in r_vec if v >= ink_thr)
            mr = sum(r_vec) / n
            r_cent = [u - mr for u in r_vec]
            dr2 = sum(u * u for u in r_cent)
            if dr2 <= 1e-9 or r_patch_mass <= 1e-9:
                continue

            best_c = -1.0
            best_mr = 0.0
            best_cx = seg_cx + base_dx
            best_a_vec: list[float] | None = None
            zeros_row = [0.0] * sw_p

            for dy in range(bdy - max_dy_local, bdy + max_dy_local + 1):
                for dx in range(base_dx - max_dx_local, base_dx + max_dx_local + 1):
                    cand_cx = seg_cx + dx
                    if cand_cx < prev_matched_cx:
                        continue
                    ax0 = px0 + dx
                    ax1 = px1 + dx
                    a_vec: list[float] = []
                    if ax0 >= 0 and ax1 <= cw:
                        for y in range(py0, py1):
                            ay = y + dy
                            if 0 <= ay < ch:
                                ao = ay * cw
                                a_vec.extend(am[ao + ax0 : ao + ax1])
                            else:
                                a_vec.extend(zeros_row)
                    else:
                        left_pad = max(0, -ax0)
                        right_pad = max(0, ax1 - cw)
                        cx0 = max(0, ax0)
                        cx1 = min(cw, ax1)
                        lp = [0.0] * left_pad
                        rp = [0.0] * right_pad
                        for y in range(py0, py1):
                            ay = y + dy
                            if 0 <= ay < ch and cx1 > cx0:
                                ao = ay * cw
                                if left_pad:
                                    a_vec.extend(lp)
                                a_vec.extend(am[ao + cx0 : ao + cx1])
                                if right_pad:
                                    a_vec.extend(rp)
                            else:
                                a_vec.extend(zeros_row)
                    ma = sum(a_vec) / n
                    num = 0.0
                    da2 = 0.0
                    for rc, v in zip(r_cent, a_vec):
                        d = v - ma
                        num += rc * d
                        da2 += d * d
                    c = num / math.sqrt(dr2 * da2) if da2 > 1e-9 else 0.0
                    if c > best_c:
                        best_c = c
                        best_mr = sum(v for v in a_vec if v >= ink_thr) / r_patch_mass
                        best_cx = cand_cx
                        best_a_vec = a_vec

            prev_matched_cx = best_cx
            seg_count += 1
            if best_c < min_seg_ncc:
                min_seg_ncc = best_c
            if best_mr < min_seg_mass_ratio:
                min_seg_mass_ratio = best_mr

            seg_w = sx1 - sx0
            seg_h = sy1 - sy0
            if best_a_vec is not None and seg_w >= 6 and seg_h >= 6:
                ox = sx0 - px0
                oy = sy0 - py0
                peak = max(max(r_vec), max(best_a_vec), 1e-9)
                cell_d = 0.0
                for gy in range(3):
                    cy0 = oy + (gy * seg_h) // 3
                    cy1 = oy + ((gy + 1) * seg_h) // 3
                    for gx in range(3):
                        cx0 = ox + (gx * seg_w) // 3
                        cx1 = ox + ((gx + 1) * seg_w) // 3
                        cnt = (cy1 - cy0) * (cx1 - cx0)
                        if cnt <= 0:
                            continue
                        rs_c = 0.0
                        as_c = 0.0
                        for yy in range(cy0, cy1):
                            ro = yy * sw_p
                            for xx in range(cx0, cx1):
                                rs_c += r_vec[ro + xx]
                                as_c += best_a_vec[ro + xx]
                        d = abs(rs_c - as_c) / (cnt * peak)
                        if d > cell_d:
                            cell_d = d
                if cell_d > max_seg_cell_diff:
                    max_seg_cell_diff = cell_d

    return {
        "ncc_2d": round(best_2d, 4),
        "ncc_col": round(best_col, 4),
        "best_shift": [best_shift[0], best_shift[1]],
        "max_run_extent_diff_px": max_run_extent_diff_px,
        "min_seg_ncc": round(min_seg_ncc, 4),
        "min_seg_mass_ratio": round(min_seg_mass_ratio, 4),
        "max_seg_cell_diff": round(max_seg_cell_diff, 4),
        "seg_count": seg_count,
    }


def _max_interior_solid_square(
    w: int,
    rgba: bytes | bytearray,
    rx: int,
    ry: int,
    rw: int,
    rh: int,
    margin: int,
) -> int:
    """Return side length (in device px) of the largest uniform (<=2 diff) non-bg interior square away from ROI edges."""
    corners = [
        (rx, ry),
        (rx + rw - 1, ry),
        (rx, ry + rh - 1),
        (rx + rw - 1, ry + rh - 1),
    ]
    cs = [
        (rgba[(cy * w + cx) * 4], rgba[(cy * w + cx) * 4 + 1], rgba[(cy * w + cx) * 4 + 2])
        for cx, cy in corners
    ]
    bg_r = sorted(c[0] for c in cs)[1]
    bg_g = sorted(c[1] for c in cs)[1]
    bg_b = sorted(c[2] for c in cs)[1]
    x0 = rx + margin
    y0 = ry + margin
    x1 = rx + rw - margin
    y1 = ry + rh - margin
    if x1 <= x0 or y1 <= y0:
        return 0
    iw = x1 - x0
    ih = y1 - y0
    prev = [0] * iw
    best = 0
    for y in range(ih):
        row = (y0 + y) * w * 4
        prow = (y0 + y - 1) * w * 4
        cur = [0] * iw
        for x in range(iw):
            i = row + (x0 + x) * 4
            r = rgba[i]
            g = rgba[i + 1]
            b = rgba[i + 2]
            if max(abs(r - bg_r), abs(g - bg_g), abs(b - bg_b)) <= 12:
                cur[x] = 0
                continue
            if x == 0 or y == 0 or cur[x - 1] == 0 or prev[x] == 0 or prev[x - 1] == 0:
                sq = 1
            else:
                li = i - 4
                ui = prow + (x0 + x) * 4
                uli = ui - 4
                if (
                    abs(r - rgba[li]) <= 2
                    and abs(g - rgba[li + 1]) <= 2
                    and abs(b - rgba[li + 2]) <= 2
                    and abs(r - rgba[ui]) <= 2
                    and abs(g - rgba[ui + 1]) <= 2
                    and abs(b - rgba[ui + 2]) <= 2
                    and abs(r - rgba[uli]) <= 2
                    and abs(g - rgba[uli + 1]) <= 2
                    and abs(b - rgba[uli + 2]) <= 2
                ):
                    m = cur[x - 1]
                    if prev[x] < m:
                        m = prev[x]
                    if prev[x - 1] < m:
                        m = prev[x - 1]
                    sq = m + 1
                else:
                    sq = 1
            cur[x] = sq
            if sq > best:
                best = sq
        prev = cur
    return best


def _switch_thumb_centroid_x(
    w: int,
    rgba: bytes | bytearray,
    rx: int,
    ry: int,
    rw: int,
    rh: int,
) -> float | None:
    """Compute horizontal centroid (relative to rx, in device px) of the bright switch thumb inside its track."""
    bg_i = (ry * w + rx) * 4
    bg_r, bg_g, bg_b = rgba[bg_i], rgba[bg_i + 1], rgba[bg_i + 2]
    tx0, ty0, tx1, ty1 = rx + rw, ry + rh, rx, ry
    for y in range(ry, ry + rh):
        row = y * w * 4
        for x in range(rx, rx + rw):
            i = row + x * 4
            if max(abs(rgba[i] - bg_r), abs(rgba[i + 1] - bg_g), abs(rgba[i + 2] - bg_b)) > 12:
                if x < tx0:
                    tx0 = x
                if y < ty0:
                    ty0 = y
                if x > tx1:
                    tx1 = x
                if y > ty1:
                    ty1 = y
    if tx1 <= tx0 + 4 or ty1 <= ty0 + 4:
        return None
    # Inset 2px from track outer edge so outer white card background is excluded
    sx = 0.0
    sw = 0.0
    for y in range(ty0 + 2, ty1 - 1):
        row = y * w * 4
        for x in range(tx0 + 2, tx1 - 1):
            i = row + x * 4
            r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
            lum = 0.299 * r + 0.587 * g + 0.114 * b
            sat = max(r, g, b) - min(r, g, b)
            if lum >= 235.0 and sat <= 20:
                wt = lum - 230.0
                sx += (x - rx) * wt
                sw += wt
    if sw <= 0.0:
        return None
    return sx / sw


def _switch_midpoint_appearance(
    w: int,
    ref_start: bytes | bytearray,
    ref_mid: bytes | bytearray,
    ref_end: bytes | bytearray,
    act_mid: bytes | bytearray,
    rx: int,
    ry: int,
    rw: int,
    rh: int,
    dpr: int,
    cx_mid: float,
    ref_cx_mid: float,
) -> dict[str, Any]:
    """Independent appearance check for switch midpoint ROIs: inner track color, thumb appearance, outer bg."""
    corners = [
        (rx, ry),
        (rx + rw - 1, ry),
        (rx, ry + rh - 1),
        (rx + rw - 1, ry + rh - 1),
    ]
    cs = [
        (ref_mid[(cy * w + cx) * 4], ref_mid[(cy * w + cx) * 4 + 1], ref_mid[(cy * w + cx) * 4 + 2])
        for cx, cy in corners
    ]
    bg_r = sorted(c[0] for c in cs)[1]
    bg_g = sorted(c[1] for c in cs)[1]
    bg_b = sorted(c[2] for c in cs)[1]

    in_track = [[False] * rw for _ in range(rh)]
    for yy in range(rh):
        row = (ry + yy) * w * 4
        for xx in range(rw):
            i = row + (rx + xx) * 4
            for img in (ref_start, ref_mid, ref_end):
                if (
                    max(abs(img[i] - bg_r), abs(img[i + 1] - bg_g), abs(img[i + 2] - bg_b))
                    > 12
                ):
                    in_track[yy][xx] = True
                    break

    def _erode_mask(dist: int) -> list[list[bool]]:
        out = [[False] * rw for _ in range(rh)]
        for yy in range(dist, rh - dist):
            for xx in range(dist, rw - dist):
                if (
                    in_track[yy][xx]
                    and in_track[yy - dist][xx]
                    and in_track[yy + dist][xx]
                    and in_track[yy][xx - dist]
                    and in_track[yy][xx + dist]
                ):
                    out[yy][xx] = True
        return out

    interior = _erode_mask(max(1, 2 * dpr))
    if not any(any(r) for r in interior):
        interior = _erode_mask(max(1, dpr))

    def _is_bright_thumb(r: int, g: int, b: int) -> bool:
        lum = 0.299 * r + 0.587 * g + 0.114 * b
        sat = max(r, g, b) - min(r, g, b)
        return lum >= 240.0 and sat <= 16

    def _thumb_pixels(img: bytes | bytearray) -> list[tuple[int, int, int, int]]:
        pts: list[tuple[int, int, int, int]] = []
        for yy in range(rh):
            row = (ry + yy) * w * 4
            for xx in range(rw):
                if not interior[yy][xx]:
                    continue
                i = row + (rx + xx) * 4
                r, g, b = img[i], img[i + 1], img[i + 2]
                if _is_bright_thumb(r, g, b):
                    pts.append((xx, r, g, b))
        return pts

    r_thumb = _thumb_pixels(ref_mid)
    a_thumb = _thumb_pixels(act_mid)
    r_txs = [p[0] for p in r_thumb]
    thumb_rad = 0.5 * (max(r_txs) - min(r_txs)) if r_txs else 0.0
    r_thumb_rgb = (
        [sorted(p[c] for p in r_thumb)[len(r_thumb) // 2] for c in (1, 2, 3)]
        if r_thumb
        else None
    )
    a_thumb_rgb = (
        [sorted(p[c] for p in a_thumb)[len(a_thumb) // 2] for c in (1, 2, 3)]
        if a_thumb
        else None
    )
    thumb_rgb_diff = (
        max(abs(r_thumb_rgb[c] - a_thumb_rgb[c]) for c in range(3))
        if (r_thumb_rgb is not None and a_thumb_rgb is not None)
        else 255
    )
    thumb_area_ratio = (len(a_thumb) / float(len(r_thumb))) if r_thumb else 0.0

    def _med_non_thumb(img: bytes | bytearray) -> list[int] | None:
        pts: list[tuple[int, int, int]] = []
        for yy in range(rh):
            row = (ry + yy) * w * 4
            for xx in range(rw):
                if not interior[yy][xx]:
                    continue
                i = row + (rx + xx) * 4
                r, g, b = img[i], img[i + 1], img[i + 2]
                if not _is_bright_thumb(r, g, b):
                    pts.append((r, g, b))
        if not pts:
            return None
        n = len(pts)
        return [sorted(p[c] for p in pts)[n // 2] for c in range(3)]

    start_track_rgb = _med_non_thumb(ref_start)
    end_track_rgb = _med_non_thumb(ref_end)

    def _sample_mid_track(pad_mult: int) -> tuple[list[tuple[int, int, int]], list[tuple[int, int, int]]]:
        excl = thumb_rad + pad_mult * dpr
        rm_pts: list[tuple[int, int, int]] = []
        am_pts: list[tuple[int, int, int]] = []
        for yy in range(rh):
            row = (ry + yy) * w * 4
            for xx in range(rw):
                if not interior[yy][xx]:
                    continue
                if abs(xx - cx_mid) <= excl or abs(xx - ref_cx_mid) <= excl:
                    continue
                i = row + (rx + xx) * 4
                rm_pts.append((ref_mid[i], ref_mid[i + 1], ref_mid[i + 2]))
                am_pts.append((act_mid[i], act_mid[i + 1], act_mid[i + 2]))
        return rm_pts, am_pts

    rm_pts, am_pts = _sample_mid_track(4)
    if len(am_pts) < 8:
        rm_pts, am_pts = _sample_mid_track(2)

    ref_track_rgb = (
        [sorted(p[c] for p in rm_pts)[len(rm_pts) // 2] for c in range(3)]
        if rm_pts
        else None
    )
    act_track_rgb = (
        [sorted(p[c] for p in am_pts)[len(am_pts) // 2] for c in range(3)]
        if am_pts
        else None
    )

    t_color: float | None = None
    perp_dist: float | None = None
    endpoints_color_diff = 0
    if start_track_rgb is not None and end_track_rgb is not None and act_track_rgb is not None:
        endpoints_color_diff = max(
            abs(end_track_rgb[c] - start_track_rgb[c]) for c in range(3)
        )
        vr = end_track_rgb[0] - start_track_rgb[0]
        vg = end_track_rgb[1] - start_track_rgb[1]
        vb = end_track_rgb[2] - start_track_rgb[2]
        v2 = vr * vr + vg * vg + vb * vb
        if v2 > 0:
            wr = act_track_rgb[0] - start_track_rgb[0]
            wg = act_track_rgb[1] - start_track_rgb[1]
            wb = act_track_rgb[2] - start_track_rgb[2]
            t_color = (wr * vr + wg * vg + wb * vb) / float(v2)
            tc = max(0.0, min(1.0, t_color))
            pr = start_track_rgb[0] + tc * vr
            pg = start_track_rgb[1] + tc * vg
            pb = start_track_rgb[2] + tc * vb
            perp_dist = math.sqrt(
                (act_track_rgb[0] - pr) ** 2
                + (act_track_rgb[1] - pg) ** 2
                + (act_track_rgb[2] - pb) ** 2
            )

    track_vs_ref_max_ch = (
        max(abs(act_track_rgb[c] - ref_track_rgb[c]) for c in range(3))
        if (act_track_rgb is not None and ref_track_rgb is not None)
        else 255
    )

    # Outer background check: corners + pixels strictly outside track (dilated by 2*dpr)
    outer_bg_max_diff = 0
    for cx, cy in corners:
        i = (cy * w + cx) * 4
        d = max(
            abs(act_mid[i] - ref_mid[i]),
            abs(act_mid[i + 1] - ref_mid[i + 1]),
            abs(act_mid[i + 2] - ref_mid[i + 2]),
        )
        if d > outer_bg_max_diff:
            outer_bg_max_diff = d
    dil = 2 * dpr
    for yy in range(rh):
        row = (ry + yy) * w * 4
        for xx in range(rw):
            near = False
            for dy in (-dil, 0, dil):
                ny = yy + dy
                if 0 <= ny < rh:
                    for dx in (-dil, 0, dil):
                        nx = xx + dx
                        if 0 <= nx < rw and in_track[ny][nx]:
                            near = True
                            break
                if near:
                    break
            if not near:
                i = row + (rx + xx) * 4
                d = max(
                    abs(act_mid[i] - ref_mid[i]),
                    abs(act_mid[i + 1] - ref_mid[i + 1]),
                    abs(act_mid[i + 2] - ref_mid[i + 2]),
                )
                if d > outer_bg_max_diff:
                    outer_bg_max_diff = d

    return {
        "n_track_px": len(am_pts),
        "start_track_rgb": start_track_rgb,
        "end_track_rgb": end_track_rgb,
        "ref_track_rgb": ref_track_rgb,
        "act_track_rgb": act_track_rgb,
        "endpoints_color_diff": endpoints_color_diff,
        "t_color": round(t_color, 4) if t_color is not None else None,
        "perp_dist": round(perp_dist, 2) if perp_dist is not None else None,
        "track_vs_ref_max_ch": track_vs_ref_max_ch,
        "ref_thumb_px": len(r_thumb),
        "act_thumb_px": len(a_thumb),
        "thumb_area_ratio": round(thumb_area_ratio, 4),
        "ref_thumb_rgb": r_thumb_rgb,
        "act_thumb_rgb": a_thumb_rgb,
        "thumb_rgb_diff": thumb_rgb_diff,
        "outer_bg_max_diff": outer_bg_max_diff,
    }


def _rect_union_area(
    req: tuple[float, float, float, float],
    pieces: list[tuple[float, float, float, float]],
) -> float:
    """Compute exact 2D union area of `pieces` clipped to `req`=(x0,y0,x1,y1) via x-sweep."""
    rx0, ry0, rx1, ry1 = req
    if rx1 <= rx0 or ry1 <= ry0 or not pieces:
        return 0.0
    clipped: list[tuple[float, float, float, float]] = []
    xs_set = {rx0, rx1}
    for px0, py0, px1, py1 in pieces:
        ix0 = max(rx0, px0)
        iy0 = max(ry0, py0)
        ix1 = min(rx1, px1)
        iy1 = min(ry1, py1)
        if ix1 > ix0 and iy1 > iy0:
            clipped.append((ix0, iy0, ix1, iy1))
            xs_set.add(ix0)
            xs_set.add(ix1)
    if not clipped:
        return 0.0
    sx = sorted(xs_set)
    area = 0.0
    for i in range(len(sx) - 1):
        x_lo = sx[i]
        x_hi = sx[i + 1]
        dx = x_hi - x_lo
        if dx <= 0.0:
            continue
        mx = 0.5 * (x_lo + x_hi)
        ys_iv: list[tuple[float, float]] = []
        for cx0, cy0, cx1, cy1 in clipped:
            if cx0 <= mx <= cx1:
                ys_iv.append((cy0, cy1))
        if not ys_iv:
            continue
        ys_iv.sort()
        cur_lo, cur_hi = ys_iv[0]
        y_cov = 0.0
        for lo, hi in ys_iv[1:]:
            if lo <= cur_hi:
                if hi > cur_hi:
                    cur_hi = hi
            else:
                y_cov += cur_hi - cur_lo
                cur_lo, cur_hi = lo, hi
        y_cov += cur_hi - cur_lo
        area += dx * y_cov
    return area


def _check_rect_covered_by_union(
    req: tuple[float, float, float, float],
    pieces: list[tuple[float, float, float, float]],
) -> bool:
    """Verify exact 2D coverage of rectangle `req`=(x0,y0,x1,y1) by the union of `pieces`."""
    rx0, ry0, rx1, ry1 = req
    if rx1 <= rx0 or ry1 <= ry0:
        return True
    req_area = (rx1 - rx0) * (ry1 - ry0)
    cov_area = _rect_union_area(req, pieces)
    return cov_area >= req_area - 1e-6 * max(1.0, req_area)


def _is_valid_perimeter_or_corner_aa(
    aroi: dict[str, Any],
    rrect: tuple[float, float, float, float],
) -> bool:
    """Return True iff `aroi` is a valid perimeter strip (min(w,h)<=4) or corner box (<=28x28) of `rrect`."""
    if str(aroi.get("kind", "solid")) != "aa":
        return False
    rx0, ry0, rx1, ry1 = rrect
    ax0 = float(aroi["x"])
    ay0 = float(aroi["y"])
    aw = float(aroi["w"])
    ah = float(aroi["h"])
    ax1 = ax0 + aw
    ay1 = ay0 + ah
    ix0 = max(rx0, ax0)
    iy0 = max(ry0, ay0)
    ix1 = min(rx1, ax1)
    iy1 = min(ry1, ay1)
    if ix1 <= ix0 or iy1 <= iy0:
        return False
    touches_left = ix0 <= rx0 + 1.0
    touches_right = ix1 >= rx1 - 1.0
    touches_top = iy0 <= ry0 + 1.0
    touches_bottom = iy1 >= ry1 - 1.0
    if not (touches_left or touches_right or touches_top or touches_bottom):
        return False
    if min(aw, ah) <= 4.0:
        return True
    if aw <= 28.0 and ah <= 28.0:
        near_left = ix0 <= rx0 + 28.0 and ix1 <= rx0 + 28.0 + 1.0
        near_right = ix1 >= rx1 - 28.0 and ix0 >= rx1 - 28.0 - 1.0
        near_top = iy0 <= ry0 + 28.0 and iy1 <= ry0 + 28.0 + 1.0
        near_bottom = iy1 >= ry1 - 28.0 and iy0 >= ry1 - 28.0 - 1.0
        if (near_left or near_right) and (near_top or near_bottom):
            return True
    return False


def _validate_required_coverage(
    manifest: dict[str, Any],
    required_manifest: dict[str, Any],
    only: str | None = None,
) -> list[str]:
    errors: list[str] = []
    actual_by_id: dict[str, dict[str, Any]] = {
        str(s.get("id", "")): s for s in manifest.get("scenes", [])
    }
    req_scenes = required_manifest.get("scenes", [])
    if only:
        req_scenes = [s for s in req_scenes if s.get("id") == only or only in str(s.get("id", ""))]

    for rsc in req_scenes:
        sid = str(rsc.get("id", ""))
        if sid not in actual_by_id:
            errors.append(f"missing required scene '{sid}'")
            continue
        asc = actual_by_id[sid]
        if asc.get("dpr") != rsc.get("dpr"):
            errors.append(f"scene '{sid}' dpr mismatch: got {asc.get('dpr')}, required {rsc.get('dpr')}")
        if asc.get("width") != rsc.get("width") or asc.get("height") != rsc.get("height"):
            errors.append(
                f"scene '{sid}' size mismatch: got {asc.get('width')}x{asc.get('height')}, "
                f"required {rsc.get('width')}x{rsc.get('height')}"
            )
        cand_rois = asc.get("rois", [])
        for rroi in rsc.get("rois", []):
            rname = str(rroi.get("name", "<unnamed>"))
            rkind = str(rroi.get("kind", "solid"))
            rx = float(rroi["x"])
            ry = float(rroi["y"])
            rw = float(rroi["w"])
            rh = float(rroi["h"])
            rrect = (rx, ry, rx + rw, ry + rh)

            def _rect_of(a: dict[str, Any]) -> tuple[float, float, float, float]:
                ax = float(a["x"])
                ay = float(a["y"])
                return (ax, ay, ax + float(a["w"]), ay + float(a["h"]))

            def _parse_mask(m: Any) -> tuple[float, float, float, float]:
                if isinstance(m, dict):
                    mx = float(m["x"])
                    my = float(m["y"])
                    return (mx, my, mx + float(m["w"]), my + float(m["h"]))
                mx = float(m[0])
                my = float(m[1])
                return (mx, my, mx + float(m[2]), my + float(m[3]))

            def _clip_to_masks(
                pieces: list[tuple[float, float, float, float]],
                masks: list[tuple[float, float, float, float]],
            ) -> list[tuple[float, float, float, float]]:
                out: list[tuple[float, float, float, float]] = []
                for px0, py0, px1, py1 in pieces:
                    for mx0, my0, mx1, my1 in masks:
                        ix0 = max(px0, mx0)
                        iy0 = max(py0, my0)
                        ix1 = min(px1, mx1)
                        iy1 = min(py1, my1)
                        if ix1 > ix0 + 1e-6 and iy1 > iy0 + 1e-6:
                            out.append((ix0, iy0, ix1, iy1))
                return out

            if rkind == "solid":
                # Strict required-solid enforcement: pixel-exact `solid` coverage is required across the
                # entire ROI EXCEPT inside explicit `text_masks` / `aa_masks` declared on the required ROI
                # (or overlapping `text` / `aa` ROIs already present in the required manifest).
                explicit_text_masks = [_parse_mask(m) for m in rroi.get("text_masks", [])]
                explicit_aa_masks = [_parse_mask(m) for m in rroi.get("aa_masks", [])]
                if (rw > 28.0 or rh > 28.0) and rw > 0.0 and rh > 0.0:
                    req_area = rw * rh
                    if explicit_text_masks:
                        tm_frac = _rect_union_area(rrect, explicit_text_masks) / req_area
                        if tm_frac >= MAX_SOLID_MASK_COVERAGE:
                            errors.append(
                                f"scene '{sid}' required solid ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                                f"has excessive text_masks coverage {tm_frac:.1%} (>= {MAX_SOLID_MASK_COVERAGE:.0%}); "
                                "full-box text_mask downgrade of solid controls is forbidden"
                            )
                    if explicit_aa_masks:
                        am_frac = _rect_union_area(rrect, explicit_aa_masks) / req_area
                        if am_frac >= MAX_SOLID_MASK_COVERAGE:
                            errors.append(
                                f"scene '{sid}' required solid ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                                f"has excessive aa_masks coverage {am_frac:.1%} (>= {MAX_SOLID_MASK_COVERAGE:.0%}); "
                                "full-box aa_mask downgrade of solid controls is forbidden"
                            )
                req_text_masks = list(explicit_text_masks)
                req_aa_masks = list(explicit_aa_masks)
                for other in rsc.get("rois", []):
                    okind = str(other.get("kind", "solid"))
                    if okind == "text":
                        req_text_masks.append(_rect_of(other))
                    elif okind == "aa":
                        req_aa_masks.append(_rect_of(other))
                solid_pieces = [_rect_of(a) for a in cand_rois if str(a.get("kind", "solid")) == "solid"]
                allowed_pieces = list(solid_pieces)
                if req_text_masks:
                    cand_text = [_rect_of(a) for a in cand_rois if str(a.get("kind", "solid")) == "text"]
                    allowed_pieces.extend(_clip_to_masks(cand_text, req_text_masks))
                if req_aa_masks:
                    cand_aa = [_rect_of(a) for a in cand_rois if str(a.get("kind", "solid")) == "aa"]
                    allowed_pieces.extend(_clip_to_masks(cand_aa, req_aa_masks))
                if not _check_rect_covered_by_union(rrect, allowed_pieces):
                    errors.append(
                        f"scene '{sid}' required solid ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                        "must be 100% covered by kind='solid' ROIs (text/aa only permitted within explicit "
                        "text_masks/aa_masks declared in required manifest)"
                    )
            elif rkind == "text":
                text_pieces = [
                    _rect_of(a) for a in cand_rois if str(a.get("kind", "solid")) in ("text", "solid")
                ]
                if not _check_rect_covered_by_union(rrect, text_pieces):
                    errors.append(
                        f"scene '{sid}' required text ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                        "must be 100% covered by kind='text' or 'solid' ROIs"
                    )
            elif rkind in ("shadow", "gradient", "skeleton"):
                if rw <= 8.0 or rh <= 8.0:
                    strict_pieces = [
                        _rect_of(a)
                        for a in cand_rois
                        if str(a.get("kind", "solid")) in ("shadow", "gradient", "skeleton", "midpoint", "solid", "text")
                        or (str(a.get("kind", "solid")) == "aa" and "max_p95_override" not in a)
                    ]
                else:
                    strict_pieces = [
                        _rect_of(a)
                        for a in cand_rois
                        if str(a.get("kind", "solid")) in ("shadow", "gradient", "skeleton", "midpoint", "solid", "text")
                        or _is_valid_perimeter_or_corner_aa(a, rrect)
                    ]
                if not _check_rect_covered_by_union(rrect, strict_pieces):
                    errors.append(
                        f"scene '{sid}' required {rkind} ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                        "cannot be downgraded to a non-perimeter AA override"
                    )
            elif rkind == "midpoint":
                mp_pieces = [_rect_of(a) for a in cand_rois if str(a.get("kind", "solid")) == "midpoint"]
                if not _check_rect_covered_by_union(rrect, mp_pieces):
                    errors.append(
                        f"scene '{sid}' required midpoint ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                        "must be 100% covered by kind='midpoint' ROIs"
                    )
            else:
                all_pieces = [_rect_of(a) for a in cand_rois]
                if not _check_rect_covered_by_union(rrect, all_pieces):
                    errors.append(
                        f"scene '{sid}' required ROI '{rname}' ({rx},{ry},{rw},{rh}) "
                        "is not fully covered by target ROIs"
                    )
    return errors


def evaluate_roi(
    w: int,
    h: int,
    dpr: int,
    ref_rgba: bytes | bytearray,
    act_rgba: bytes | bytearray,
    roi: dict[str, Any],
    *,
    ref_dir: str | None = None,
    actual_dir: str | None = None,
    scene: dict[str, Any] | None = None,
) -> dict[str, Any]:
    name = str(roi.get("name", "<unnamed>"))
    kind = str(roi.get("kind", "solid"))
    rx = int(round(float(roi["x"]) * dpr))
    ry = int(round(float(roi["y"]) * dpr))
    rw = int(round(float(roi["w"]) * dpr))
    rh = int(round(float(roi["h"]) * dpr))

    if rw <= 0 or rh <= 0 or rx < 0 or ry < 0 or rx + rw > w or ry + rh > h:
        return {
            "name": name,
            "kind": kind,
            "ok": False,
            "reason": f"ROI out of bounds or empty: ({rx},{ry},{rw},{rh}) vs image ({w}x{h}) at dpr={dpr}",
        }

    # Validate any explicit max_p95_override up front (never silently waived)
    has_override = "max_p95_override" in roi
    override_val: float | None = None
    override_reason = str(roi.get("override_reason") or roi.get("reason") or "").strip()
    if has_override:
        if kind != "aa":
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": f"max_p95_override is only permitted for kind='aa' (got kind='{kind}')",
            }
        try:
            override_val = float(roi["max_p95_override"])
        except (TypeError, ValueError):
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": f"invalid max_p95_override value: {roi['max_p95_override']!r}",
            }
        if not (0.0 < override_val <= MAX_AA_P95_OVERRIDE):
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": f"max_p95_override={override_val} exceeds hard cap (<= {MAX_AA_P95_OVERRIDE})",
            }
        if not override_reason:
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": "max_p95_override requires a non-empty 'override_reason' (or 'reason')",
            }
        solid_sq_cap = MAX_AA_OVERRIDE_SOLID_SQ_PT * dpr
        solid_margin = max(1, dpr)
        ref_solid_sq = _max_interior_solid_square(w, ref_rgba, rx, ry, rw, rh, solid_margin)
        act_solid_sq = _max_interior_solid_square(w, act_rgba, rx, ry, rw, rh, solid_margin)
        log_w = float(roi["w"])
        log_h = float(roi["h"])
        oversized_box = min(log_w, log_h) > 16.0 and max(log_w, log_h) > 28.0
        if ref_solid_sq > solid_sq_cap or act_solid_sq > solid_sq_cap or oversized_box:
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "ref_interior_solid_sq_px": ref_solid_sq,
                "actual_interior_solid_sq_px": act_solid_sq,
                "reason": (
                    f"max_p95_override forbidden on ROI containing large uniform interior solid block or whole control "
                    f"(size={log_w:.1f}x{log_h:.1f}pt, ref_sq={ref_solid_sq}px, actual_sq={act_solid_sq}px, cap={solid_sq_cap}px at dpr={dpr}); "
                    "split solid interior into kind='solid' and keep max_p95_override on narrow perimeter/corner arcs only"
                ),
            }

    # Validate any explicit min_seg_ncc_override on kind="text" up front (never silently waived)
    has_seg_override = "min_seg_ncc_override" in roi
    seg_override_val: float | None = None
    if has_seg_override:
        if kind != "text":
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": f"min_seg_ncc_override is only permitted for kind='text' (got kind='{kind}')",
            }
        try:
            seg_override_val = float(roi["min_seg_ncc_override"])
        except (TypeError, ValueError):
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": f"invalid min_seg_ncc_override value: {roi['min_seg_ncc_override']!r}",
            }
        if not (MIN_TEXT_SEG_NCC_OVERRIDE_FLOOR <= seg_override_val < MIN_TEXT_SEG_NCC):
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": (
                    f"min_seg_ncc_override={seg_override_val} out of allowed bounds "
                    f"[{MIN_TEXT_SEG_NCC_OVERRIDE_FLOOR:.2f}, {MIN_TEXT_SEG_NCC:.2f})"
                ),
            }
        if not override_reason:
            return {
                "name": name,
                "kind": kind,
                "ok": False,
                "reason": "min_seg_ncc_override requires a non-empty 'override_reason' (or 'reason')",
            }

    deltas = _roi_deltas(w, ref_rgba, act_rgba, rx, ry, rx + rw, ry + rh)
    if not deltas:
        return {"name": name, "kind": kind, "ok": False, "reason": "empty ROI"}

    deltas_sorted = sorted(deltas)
    max_delta = deltas_sorted[-1]
    mean_delta = sum(deltas) / len(deltas)
    p95_delta = _percentile(deltas_sorted, 95.0)

    res: dict[str, Any] = {
        "name": name,
        "kind": kind,
        "px_rect": [rx, ry, rw, rh],
        "max_delta": max_delta,
        "mean_delta": round(mean_delta, 4),
        "p95_delta": round(p95_delta, 4),
    }

    if kind == "solid":
        ok = max_delta <= 2
        res["ok"] = ok
        if not ok:
            res["reason"] = f"solid ROI max_delta={max_delta} > 2 (mean={mean_delta:.2f}, p95={p95_delta:.1f})"
    elif kind in ("shadow", "gradient", "aa", "skeleton"):
        p95_limit = override_val if (kind == "aa" and override_val is not None) else 6.0
        if kind == "aa" and override_val is not None:
            res["override"] = {
                "max_p95_override": override_val,
                "reason": override_reason,
            }
        ok = (mean_delta <= 3.0) and (p95_delta <= p95_limit)
        res["ok"] = ok
        if not ok:
            res["reason"] = (
                f"{kind} ROI failed: mean_delta={mean_delta:.2f} (<=3.0), "
                f"p95={p95_delta:.1f} (<={p95_limit:.1f})"
            )
    elif kind == "text":
        ref_st = _text_ink_stats(w, ref_rgba, rx, ry, rx + rw, ry + rh)
        act_st = _text_ink_stats(w, act_rgba, rx, ry, rx + rw, ry + rh)
        res["ref_bbox"] = list(ref_st["bbox"]) if ref_st else None
        res["actual_bbox"] = list(act_st["bbox"]) if act_st else None
        if ref_st is None or act_st is None:
            res["ok"] = False
            res["reason"] = f"text ROI missing ink bbox: ref={res['ref_bbox']}, actual={res['actual_bbox']}"
        else:
            ref_bb = ref_st["bbox"]
            act_bb = act_st["bbox"]
            dx0 = abs(ref_bb[0] - act_bb[0])
            dy0 = abs(ref_bb[1] - act_bb[1])
            dx1 = abs(ref_bb[2] - act_bb[2])
            dy1 = abs(ref_bb[3] - act_bb[3])
            # Ink bbox edge tolerance <= 2 device px horizontally and vertically (AA fringe),
            # plus a SEPARATE per-line vertical baseline/centroid check strictly <= 1.0 device px
            # at BOTH dpr=1 and dpr=2 (matching design_plan_v4.md:162 baseline <= 1px).
            bbox_tol = 2
            bbox_ok = (dx0 <= bbox_tol and dx1 <= bbox_tol and dy0 <= bbox_tol and dy1 <= bbox_tol)

            ref_yc = float(ref_st["y_center"])
            act_yc = float(act_st["y_center"])
            y_center_diff = abs(act_yc - ref_yc)
            ref_bands = ref_st.get("line_bands", [])
            act_bands = act_st.get("line_bands", [])
            y_center_tol = 1.0
            band_count_ok = len(ref_bands) > 0 and len(ref_bands) == len(act_bands)
            if band_count_ok:
                max_line_y_diff = max(
                    abs(float(rb["y_center"]) - float(ab["y_center"]))
                    for rb, ab in zip(ref_bands, act_bands)
                )
            else:
                max_line_y_diff = y_center_diff
            y_center_ok = band_count_ok and (max_line_y_diff <= y_center_tol)

            ref_rgb = ref_st["ink_rgb"]
            act_rgb = act_st["ink_rgb"]
            ink_rgb_diff = max(
                abs(ref_rgb[0] - act_rgb[0]),
                abs(ref_rgb[1] - act_rgb[1]),
                abs(ref_rgb[2] - act_rgb[2]),
            )
            ink_color_ok = ink_rgb_diff <= 24

            ref_cnt = ref_st["ink_count"]
            act_cnt = act_st["ink_count"]
            ink_ratio = act_cnt / float(ref_cnt) if ref_cnt > 0 else 0.0
            ref_mass = float(ref_st.get("ink_mass", 0.0))
            act_mass = float(act_st.get("ink_mass", 0.0))
            ink_mass_ratio = act_mass / ref_mass if ref_mass > 0.0 else 0.0
            # For tiny 1x single-glyph ROIs (ref_cnt < 64), a 0.2px subpixel phase difference
            # shifts ~10 low-alpha AA fringe pixels across the binary delta=20 cutoff while
            # conserving physical ink mass (sum_d); enforce the same strict [0.80, 1.25] bounds
            # on continuous ink mass there, and both pixel-count + mass guard elsewhere.
            tiny_1x_glyph = (dpr == 1 and ref_cnt < 64)
            if tiny_1x_glyph:
                ink_mass_ok = 0.80 <= ink_mass_ratio <= 1.25
            else:
                ink_mass_ok = (0.80 <= ink_ratio <= 1.25) and (0.70 <= ink_mass_ratio <= 1.40)

            res["bbox_diff"] = [dx0, dy0, dx1, dy1]
            res["ref_y_center"] = round(ref_yc, 4)
            res["actual_y_center"] = round(act_yc, 4)
            res["y_center_diff"] = round(y_center_diff, 4)
            res["line_count"] = len(ref_bands)
            res["actual_line_count"] = len(act_bands)
            res["max_line_y_center_diff"] = round(max_line_y_diff, 4)
            res["ref_ink_rgb"] = ref_rgb
            res["actual_ink_rgb"] = act_rgb
            res["ink_rgb_diff"] = ink_rgb_diff
            res["ref_ink_count"] = ref_cnt
            res["actual_ink_count"] = act_cnt
            res["ink_ratio"] = round(ink_ratio, 4)
            res["ink_mass_ratio"] = round(ink_mass_ratio, 4)
            res["ref_corner_diff"] = int(ref_st["corner_diff"])
            res["actual_corner_diff"] = int(act_st["corner_diff"])
            res["ref_max_fill_bucket_frac"] = round(float(ref_st["max_fill_bucket_frac"]), 4)
            res["actual_max_fill_bucket_frac"] = round(float(act_st["max_fill_bucket_frac"]), 4)
            res["ref_max_solid_sq"] = int(ref_st["max_solid_sq"])
            res["actual_max_solid_sq"] = int(act_st["max_solid_sq"])

            reasons: list[str] = []
            for side_lbl, st in (("ref", ref_st), ("actual", act_st)):
                cd = int(st["corner_diff"])
                if cd > MAX_TEXT_CORNER_DIFF:
                    reasons.append(
                        f"{side_lbl} text ROI corners inconsistent (max_corner_diff={cd} > {MAX_TEXT_CORNER_DIFF}); "
                        "text ROI must sit inside a uniform background without crossing control edges"
                    )
                bf = float(st["max_fill_bucket_frac"])
                if bf > MAX_TEXT_BG_FILL_FRAC:
                    reasons.append(
                        f"{side_lbl} text ROI contains dominant non-bg solid fill "
                        f"(max_fill_bucket_frac={bf:.3f} > {MAX_TEXT_BG_FILL_FRAC:.2f}); "
                        "split filled controls into solid/aa + tight inner text ROI"
                    )
                sq = int(st["max_solid_sq"])
                sq_frac = float(st["solid_sq_area_frac"])
                if sq >= MIN_SOLID_BLOCK_SIDE and sq_frac >= MAX_SOLID_BLOCK_AREA_FRAC:
                    reasons.append(
                        f"{side_lbl} text ROI contains uniform non-bg solid block "
                        f"({sq}x{sq}px, area_frac={sq_frac:.3f} >= {MAX_SOLID_BLOCK_AREA_FRAC:.3f}); "
                        "use solid/aa ROI for filled surfaces"
                    )
            if not bbox_ok:
                reasons.append(
                    f"bbox diff {res['bbox_diff']} exceeds device-px tol (<={bbox_tol}px)"
                )
            if not band_count_ok:
                reasons.append(
                    f"text line band count mismatch: ref={len(ref_bands)}, actual={len(act_bands)}"
                )
            elif not y_center_ok:
                reasons.append(
                    f"vertical line baseline centroid diff {max_line_y_diff:.3f}px > {y_center_tol:.1f}px "
                    f"(overall y_center_diff={y_center_diff:.3f}px)"
                )
            if not ink_color_ok:
                reasons.append(
                    f"ink RGB diff {ink_rgb_diff} > 24 (ref={ref_rgb}, actual={act_rgb})"
                )
            if not ink_mass_ok:
                if tiny_1x_glyph:
                    reasons.append(
                        f"continuous ink mass ratio {ink_mass_ratio:.3f} outside [0.80, 1.25] "
                        f"(ref_mass={ref_mass:.0f}, actual_mass={act_mass:.0f}, ref_cnt={ref_cnt}, actual_cnt={act_cnt})"
                    )
                else:
                    reasons.append(
                        f"ink mass ratio {ink_ratio:.3f} outside [0.80, 1.25] "
                        f"(ref={ref_cnt}, actual={act_cnt}, continuous={ink_mass_ratio:.3f})"
                    )
            glyph_ncc = _text_glyph_ncc(w, ref_rgba, act_rgba, rx, ry, rw, rh, dpr, ref_st, act_st)
            ncc_2d = float(glyph_ncc["ncc_2d"])
            ncc_col = float(glyph_ncc["ncc_col"])
            max_run_ext_px = int(glyph_ncc["max_run_extent_diff_px"])
            min_seg_ncc = float(glyph_ncc["min_seg_ncc"])
            min_seg_mass_ratio = float(glyph_ncc["min_seg_mass_ratio"])
            max_seg_cell_diff = float(glyph_ncc["max_seg_cell_diff"])
            seg_count = int(glyph_ncc["seg_count"])
            res["glyph_ncc_2d"] = ncc_2d
            res["glyph_ncc_col"] = ncc_col
            res["glyph_ncc_shift"] = glyph_ncc["best_shift"]
            res["max_run_extent_diff_px"] = max_run_ext_px
            res["min_seg_ncc"] = min_seg_ncc
            res["min_seg_mass_ratio"] = min_seg_mass_ratio
            res["max_seg_cell_diff"] = max_seg_cell_diff
            res["seg_count"] = seg_count
            if ncc_2d < MIN_TEXT_GLYPH_NCC_2D:
                reasons.append(
                    f"glyph 2D NCC {ncc_2d:.4f} < {MIN_TEXT_GLYPH_NCC_2D:.2f} "
                    f"(col_ncc={ncc_col:.4f}, shift={glyph_ncc['best_shift']})"
                )
            if ncc_col < MIN_TEXT_COL_NCC:
                reasons.append(
                    f"glyph column-projection NCC {ncc_col:.4f} < {MIN_TEXT_COL_NCC:.2f}"
                )
            max_run_ext_cap_px = max(2, int(round(MAX_TEXT_RUN_EXTENT_DIFF_PT * dpr)))
            if max_run_ext_px > max_run_ext_cap_px:
                reasons.append(
                    f"per-glyph vertical extent diff {max_run_ext_px}px > {max_run_ext_cap_px}px "
                    f"(possible character swap/substitution)"
                )
            seg_ncc_limit = seg_override_val if seg_override_val is not None else MIN_TEXT_SEG_NCC
            if seg_override_val is not None:
                res["override"] = {
                    "min_seg_ncc_override": seg_override_val,
                    "reason": override_reason,
                }
            if seg_count >= 2:
                if min_seg_ncc < seg_ncc_limit:
                    reasons.append(
                        f"per-glyph min_seg_ncc {min_seg_ncc:.4f} < {seg_ncc_limit:.2f} "
                        f"(segs={seg_count}, min_mass={min_seg_mass_ratio:.3f})"
                    )
                if min_seg_mass_ratio < MIN_TEXT_SEG_MASS_RATIO:
                    reasons.append(
                        f"per-glyph min_seg_mass_ratio {min_seg_mass_ratio:.4f} < {MIN_TEXT_SEG_MASS_RATIO:.2f} "
                        f"(missing/erased glyph in {seg_count} segments)"
                    )
                if max_seg_cell_diff > MAX_TEXT_SEG_CELL_DIFF:
                    reasons.append(
                        f"per-glyph 3x3 cell topology diff {max_seg_cell_diff:.4f} > {MAX_TEXT_SEG_CELL_DIFF:.2f} "
                        f"(possible same-height glyph swap/substitution in {seg_count} segments)"
                    )
            res["ok"] = len(reasons) == 0
            if reasons:
                res["reason"] = "text ROI failed: " + "; ".join(reasons)
    elif kind == "midpoint":
        start_file = roi.get("start_file") or (scene.get("start_file") if scene else None)
        end_file = roi.get("end_file") or (scene.get("end_file") if scene else None)
        if not start_file or not end_file or not actual_dir or not ref_dir:
            res["ok"] = False
            res["reason"] = "midpoint ROI requires 'start_file' and 'end_file' (resolved in both ref_dir and actual_dir)"
        else:
            act_start_path = os.path.join(actual_dir, str(start_file))
            act_end_path = os.path.join(actual_dir, str(end_file))
            ref_start_path = os.path.join(ref_dir, str(start_file))
            ref_end_path = os.path.join(ref_dir, str(end_file))
            require_ref_endpoints = bool(
                roi.get("verify_endpoints")
                or (os.path.isfile(ref_start_path) or os.path.isfile(ref_end_path))
            )
            missing = [p for p in (act_start_path, act_end_path) if not os.path.isfile(p)]
            if require_ref_endpoints:
                missing.extend(p for p in (ref_start_path, ref_end_path) if not os.path.isfile(p))
            if missing:
                res["ok"] = False
                res["reason"] = f"midpoint ROI missing start/end PNG(s): {', '.join(missing)}"
            else:
                sw, sh, act_start_rgba = read_png_rgba(act_start_path)
                ew, eh, act_end_rgba = read_png_rgba(act_end_path)
                ref_start_rgba: bytearray | None = None
                ref_end_rgba: bytearray | None = None
                rsw, rsh, rew, reh = w, h, w, h
                if require_ref_endpoints:
                    rsw, rsh, ref_start_rgba = read_png_rgba(ref_start_path)
                    rew, reh, ref_end_rgba = read_png_rgba(ref_end_path)
                if (sw, sh) != (w, h) or (ew, eh) != (w, h) or (rsw, rsh) != (w, h) or (rew, reh) != (w, h):
                    res["ok"] = False
                    res["reason"] = (
                        f"midpoint start/end size mismatch: act_start={sw}x{sh}, act_end={ew}x{eh}, "
                        f"ref_start={rsw}x{rsh}, ref_end={rew}x{reh}, mid={w}x{h}"
                    )
                else:
                    # 1. Verify endpoints differ from each other & mid differs from both endpoints
                    d_se = _roi_deltas(w, act_start_rgba, act_end_rgba, rx, ry, rx + rw, ry + rh)
                    ds = _roi_deltas(w, act_start_rgba, act_rgba, rx, ry, rx + rw, ry + rh)
                    de = _roi_deltas(w, act_end_rgba, act_rgba, rx, ry, rx + rw, ry + rh)
                    max_start_vs_end = max(d_se) if d_se else 0
                    max_vs_start = max(ds) if ds else 0
                    max_vs_end = max(de) if de else 0

                    res["max_delta_start_vs_end"] = max_start_vs_end
                    res["max_delta_vs_start"] = max_vs_start
                    res["max_delta_vs_end"] = max_vs_end

                    mp_reasons: list[str] = []
                    if require_ref_endpoints and ref_start_rgba is not None and ref_end_rgba is not None:
                        d_start_ref = sorted(_roi_deltas(w, ref_start_rgba, act_start_rgba, rx, ry, rx + rw, ry + rh))
                        d_end_ref = sorted(_roi_deltas(w, ref_end_rgba, act_end_rgba, rx, ry, rx + rw, ry + rh))
                        start_mean = sum(d_start_ref) / len(d_start_ref)
                        start_p95 = _percentile(d_start_ref, 95.0)
                        end_mean = sum(d_end_ref) / len(d_end_ref)
                        end_p95 = _percentile(d_end_ref, 95.0)
                        res["start_vs_ref_mean"] = round(start_mean, 4)
                        res["start_vs_ref_p95"] = round(start_p95, 4)
                        res["end_vs_ref_mean"] = round(end_mean, 4)
                        res["end_vs_ref_p95"] = round(end_p95, 4)
                        if start_mean > 3.0 or start_p95 > 6.0:
                            mp_reasons.append(
                                f"start frame vs ref failed: mean={start_mean:.2f} (<=3.0), p95={start_p95:.1f} (<=6.0)"
                            )
                        if end_mean > 3.0 or end_p95 > 6.0:
                            mp_reasons.append(
                                f"end frame vs ref failed: mean={end_mean:.2f} (<=3.0), p95={end_p95:.1f} (<=6.0)"
                            )
                        if max_start_vs_end < MIDPOINT_MIN_ENDPOINT_DELTA:
                            mp_reasons.append(
                                f"start and end frames are identical/too close (max_delta={max_start_vs_end} < {MIDPOINT_MIN_ENDPOINT_DELTA})"
                            )
                    elif not require_ref_endpoints:
                        pass
                    if max_vs_start == 0 or max_vs_end == 0:
                        mp_reasons.append(
                            f"midpoint ROI is static (max_vs_start={max_vs_start}, max_vs_end={max_vs_end}); "
                            "midpoint frame must differ from both start and end frames"
                        )

                    # 3. Directional thumb monotonicity & transition-curve centroid check (when ref endpoints exist):
                    # Chromium CSS ease (t=48ms) and Aura knob_spring (t=48ms) follow slightly different
                    # easing curves (~1.51pt thumb centroid delta at 48ms), so when a distinct moving thumb
                    # is detected we enforce:
                    #   (a) strict directional monotonicity: cx_start + 1.5pt < cx_mid < cx_end - 1.5pt (LTR),
                    #   (b) |cx_mid - ref_cx_mid| <= 6.0pt (documented transition-curve tolerance; Linux-vs-Metal <= 1px).
                    # For non-thumb midpoint ROIs (e.g. Wave 0 periodic skeleton shimmer), fall back to mean<=3, p95<=6 vs ref_mid.
                    cx_start = _switch_thumb_centroid_x(w, act_start_rgba, rx, ry, rw, rh) if require_ref_endpoints else None
                    cx_mid = _switch_thumb_centroid_x(w, act_rgba, rx, ry, rw, rh) if require_ref_endpoints else None
                    cx_end = _switch_thumb_centroid_x(w, act_end_rgba, rx, ry, rw, rh) if require_ref_endpoints else None
                    ref_cx_mid = _switch_thumb_centroid_x(w, ref_rgba, rx, ry, rw, rh) if require_ref_endpoints else None
                    direction = str(roi.get("direction") or (scene.get("dir") if scene else "ltr") or "ltr").lower()

                    if cx_start is not None and cx_mid is not None and cx_end is not None and ref_cx_mid is not None:
                        margin_px = MIDPOINT_MIN_TRAVEL_MARGIN_PT * dpr
                        ref_tol_px = MIDPOINT_MAX_REF_CX_DIFF_PT * dpr
                        res["thumb_cx_px"] = {
                            "start": round(cx_start, 2),
                            "mid": round(cx_mid, 2),
                            "end": round(cx_end, 2),
                            "ref_mid": round(ref_cx_mid, 2),
                            "direction": direction,
                        }
                        if direction == "rtl":
                            if not (cx_end + margin_px < cx_mid < cx_start - margin_px):
                                mp_reasons.append(
                                    f"RTL thumb centroid not strictly interior/monotonic: "
                                    f"end={cx_end:.2f} < mid={cx_mid:.2f} < start={cx_start:.2f} (margin={margin_px:.1f}px)"
                                )
                        else:
                            if not (cx_start + margin_px < cx_mid < cx_end - margin_px):
                                mp_reasons.append(
                                    f"LTR thumb centroid not strictly interior/monotonic: "
                                    f"start={cx_start:.2f} < mid={cx_mid:.2f} < end={cx_end:.2f} (margin={margin_px:.1f}px)"
                                )
                        if abs(cx_mid - ref_cx_mid) > ref_tol_px:
                            mp_reasons.append(
                                f"mid thumb centroid {cx_mid:.2f}px deviates from ref_mid {ref_cx_mid:.2f}px "
                                f"by {abs(cx_mid - ref_cx_mid):.2f}px (> {ref_tol_px:.1f}px)"
                            )
                        # Independent appearance checks after thumb centroid passes:
                        # (1) inner track color along start->end line segment & strictly interior progress,
                        # (2) thumb white appearance & area,
                        # (3) outer background integrity.
                        if ref_start_rgba is not None and ref_end_rgba is not None:
                            app = _switch_midpoint_appearance(
                                w,
                                ref_start_rgba,
                                ref_rgba,
                                ref_end_rgba,
                                act_rgba,
                                rx,
                                ry,
                                rw,
                                rh,
                                dpr,
                                cx_mid,
                                ref_cx_mid,
                            )
                            res["midpoint_appearance"] = app
                            if app["n_track_px"] <= 0 or app["act_track_rgb"] is None:
                                mp_reasons.append(
                                    "midpoint switch inner track could not be sampled away from thumb"
                                )
                            else:
                                if app["endpoints_color_diff"] >= 25:
                                    if (
                                        app["perp_dist"] is None
                                        or app["perp_dist"] > MIDPOINT_TRACK_MAX_PERP_DIST
                                    ):
                                        mp_reasons.append(
                                            f"midpoint track color {app['act_track_rgb']} deviates from "
                                            f"start {app['start_track_rgb']}->end {app['end_track_rgb']} segment "
                                            f"(perp_dist={app['perp_dist']} > {MIDPOINT_TRACK_MAX_PERP_DIST:.1f})"
                                        )
                                    if app["t_color"] is None or not (
                                        MIDPOINT_TRACK_MIN_PROGRESS
                                        <= app["t_color"]
                                        <= MIDPOINT_TRACK_MAX_PROGRESS
                                    ):
                                        mp_reasons.append(
                                            f"midpoint track color progress t={app['t_color']} outside "
                                            f"[{MIDPOINT_TRACK_MIN_PROGRESS:.2f}, {MIDPOINT_TRACK_MAX_PROGRESS:.2f}] "
                                            f"(act={app['act_track_rgb']}, ref={app['ref_track_rgb']})"
                                        )
                                if app["track_vs_ref_max_ch"] > MIDPOINT_TRACK_MAX_REF_CH_DIFF:
                                    mp_reasons.append(
                                        f"midpoint track color {app['act_track_rgb']} differs from ref_mid "
                                        f"{app['ref_track_rgb']} by {app['track_vs_ref_max_ch']} "
                                        f"(> {MIDPOINT_TRACK_MAX_REF_CH_DIFF})"
                                    )
                            if app["ref_thumb_px"] > 0:
                                if app["thumb_rgb_diff"] > MIDPOINT_THUMB_MAX_RGB_DIFF:
                                    mp_reasons.append(
                                        f"midpoint thumb RGB {app['act_thumb_rgb']} differs from ref_mid "
                                        f"{app['ref_thumb_rgb']} by {app['thumb_rgb_diff']} "
                                        f"(> {MIDPOINT_THUMB_MAX_RGB_DIFF})"
                                    )
                                if not (
                                    MIDPOINT_THUMB_MIN_AREA_RATIO
                                    <= app["thumb_area_ratio"]
                                    <= MIDPOINT_THUMB_MAX_AREA_RATIO
                                ):
                                    mp_reasons.append(
                                        f"midpoint thumb area ratio {app['thumb_area_ratio']:.3f} outside "
                                        f"[{MIDPOINT_THUMB_MIN_AREA_RATIO:.2f}, {MIDPOINT_THUMB_MAX_AREA_RATIO:.2f}] "
                                        f"(act={app['act_thumb_px']}px, ref={app['ref_thumb_px']}px)"
                                    )
                            if app["outer_bg_max_diff"] > MIDPOINT_OUTER_BG_MAX_DIFF:
                                mp_reasons.append(
                                    f"midpoint outer background max diff {app['outer_bg_max_diff']} "
                                    f"> {MIDPOINT_OUTER_BG_MAX_DIFF}"
                                )
                    else:
                        if mean_delta > 3.0 or p95_delta > 6.0:
                            mp_reasons.append(
                                f"midpoint ROI vs ref failed: mean_delta={mean_delta:.2f} (<=3.0), "
                                f"p95={p95_delta:.1f} (<=6.0)"
                            )

                    res["ok"] = len(mp_reasons) == 0
                    if mp_reasons:
                        res["reason"] = "midpoint ROI failed: " + "; ".join(mp_reasons)
    else:
        res["ok"] = False
        res["reason"] = f"unknown ROI kind: {kind}"

    return res


def verify_manifest(
    manifest_path: str,
    ref_dir: str,
    actual_dir: str,
    sbs_dir: str | None = None,
    only: str | None = None,
    required_manifest_path: str | None = None,
) -> tuple[bool, dict[str, Any]]:
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    scenes = manifest.get("scenes", [])
    if not scenes:
        return False, {"ok": False, "error": "manifest contains no scenes"}

    coverage_errors: list[str] = []
    if required_manifest_path:
        with open(required_manifest_path, "r", encoding="utf-8") as f:
            req_manifest = json.load(f)
        coverage_errors.extend(_validate_required_coverage(manifest, req_manifest, only=only))
    elif not only and isinstance(manifest.get("required_scenes"), list):
        present_ids = {str(s.get("id", "")) for s in scenes}
        for req_id in manifest["required_scenes"]:
            if str(req_id) not in present_ids:
                coverage_errors.append(f"missing required scene '{req_id}'")

    if only:
        scenes = [s for s in scenes if s.get("id") == only or only in str(s.get("id", ""))]
        if not scenes:
            return False, {"ok": False, "error": f"no scenes matched --only={only}"}

    report_scenes: list[dict[str, Any]] = []
    overrides_used: list[dict[str, Any]] = []
    all_ok = len(coverage_errors) == 0

    for sc in scenes:
        sid = str(sc.get("id", ""))
        fname = str(sc.get("file", f"{sid}.png"))
        if "dpr" not in sc:
            all_ok = False
            report_scenes.append({"id": sid, "ok": False, "error": "missing scene-level 'dpr' field"})
            continue
        raw_dpr = sc["dpr"]
        if isinstance(raw_dpr, bool) or raw_dpr not in ALLOWED_DPRS:
            all_ok = False
            report_scenes.append({
                "id": sid,
                "dpr": raw_dpr,
                "ok": False,
                "error": f"invalid scene dpr={raw_dpr!r}: must be 1 or 2",
            })
            continue
        dpr = int(raw_dpr)
        rois = sc.get("rois", [])
        if not rois:
            all_ok = False
            report_scenes.append({"id": sid, "ok": False, "error": "scene has no ROIs"})
            continue

        ref_path = os.path.join(ref_dir, fname)
        act_path = os.path.join(actual_dir, fname)
        if not os.path.isfile(ref_path):
            all_ok = False
            report_scenes.append({"id": sid, "ok": False, "error": f"missing ref PNG: {ref_path}"})
            continue
        if not os.path.isfile(act_path):
            all_ok = False
            report_scenes.append({"id": sid, "ok": False, "error": f"missing actual PNG: {act_path}"})
            continue

        rw, rh, ref_rgba = read_png_rgba(ref_path)
        aw, ah, act_rgba = read_png_rgba(act_path)
        same_size = (rw == aw) and (rh == ah)
        if not same_size:
            all_ok = False
            report_scenes.append({
                "id": sid,
                "dpr": dpr,
                "same_size": False,
                "ref_size": [rw, rh],
                "actual_size": [aw, ah],
                "ok": False,
                "error": f"size mismatch: ref={rw}x{rh} vs actual={aw}x{ah}",
            })
            continue

        expected_w = int(sc.get("width", rw // dpr)) * dpr
        expected_h = int(sc.get("height", rh // dpr)) * dpr
        if rw != expected_w or rh != expected_h:
            all_ok = False
            report_scenes.append({
                "id": sid,
                "dpr": dpr,
                "same_size": True,
                "actual_size": [aw, ah],
                "expected_size": [expected_w, expected_h],
                "ok": False,
                "error": f"image dimensions {rw}x{rh} != manifest {expected_w}x{expected_h} (dpr={dpr})",
            })
            continue

        if sbs_dir:
            sbs_path = os.path.join(sbs_dir, f"{sid}_sbs.png")
            write_sbs_png(sbs_path, rw, rh, ref_rgba, act_rgba)

        roi_results: list[dict[str, Any]] = []
        sc_ok = True
        for roi in rois:
            r = evaluate_roi(
                rw, rh, dpr, ref_rgba, act_rgba, roi, ref_dir=ref_dir, actual_dir=actual_dir, scene=sc
            )
            roi_results.append(r)
            if "override" in r:
                ov_entry: dict[str, Any] = {
                    "scene": sid,
                    "roi": r["name"],
                    "reason": r["override"]["reason"],
                }
                if "max_p95_override" in r["override"]:
                    ov_entry["p95_delta"] = r["p95_delta"]
                    ov_entry["max_p95_override"] = r["override"]["max_p95_override"]
                if "min_seg_ncc_override" in r["override"]:
                    ov_entry["min_seg_ncc"] = r.get("min_seg_ncc")
                    ov_entry["min_seg_ncc_override"] = r["override"]["min_seg_ncc_override"]
                overrides_used.append(ov_entry)
            if not r["ok"]:
                sc_ok = False

        if not sc_ok:
            all_ok = False
        report_scenes.append({
            "id": sid,
            "dpr": dpr,
            "same_size": True,
            "size": [rw, rh],
            "ok": sc_ok,
            "rois": roi_results,
        })

    report: dict[str, Any] = {
        "ok": all_ok,
        "manifest": os.path.basename(manifest_path),
        "scene_count": len(report_scenes),
        "passed_scenes": sum(1 for s in report_scenes if s.get("ok")),
        "override_count": len(overrides_used),
        "overrides": overrides_used,
        "scenes": report_scenes,
    }
    if coverage_errors:
        report["coverage_errors"] = coverage_errors
    return all_ok, report


# ---------------------------------------------------------------------------
# Built-in mandatory negative & positive self-tests
# ---------------------------------------------------------------------------

def _fill_rect(
    buf: bytearray,
    w: int,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    rgba: tuple[int, int, int, int],
) -> None:
    r, g, b, a = rgba
    for y in range(y0, y1):
        row = y * w * 4
        for x in range(x0, x1):
            i = row + x * 4
            buf[i] = r
            buf[i + 1] = g
            buf[i + 2] = b
            buf[i + 3] = a


def run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="aura_verify_selftest_") as td:
        ref_dir = os.path.join(td, "ref")
        act_dir = os.path.join(td, "act")
        os.makedirs(ref_dir)
        os.makedirs(act_dir)

        # Case 1 (Astra 10x10 negative): per-pixel P95=16 > 6 while MAE=0.96 <= 3.0 -> MUST FAIL
        w, h = 10, 10
        ref_px = bytearray([100, 100, 100, 255] * (w * h))
        act_px = bytearray([100, 100, 100, 255] * (w * h))
        for idx in (0, 1, 2):
            for c in range(3):
                ref_px[idx * 4 + c] = 116
        for idx in (3, 4, 5):
            for c in range(3):
                act_px[idx * 4 + c] = 116
        write_png_rgba(os.path.join(ref_dir, "neg1.png"), w, h, ref_px)
        write_png_rgba(os.path.join(act_dir, "neg1.png"), w, h, act_px)
        m1 = os.path.join(td, "m1.json")
        with open(m1, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg1", "file": "neg1.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "r1", "kind": "shadow", "x": 0, "y": 0, "w": 10, "h": 10}],
                }]
            }, f)
        ok1, rep1 = verify_manifest(m1, ref_dir, act_dir)
        if ok1 or rep1["scenes"][0]["rois"][0]["p95_delta"] != 16.0:
            sys.stderr.write(f"SELF-TEST FAIL: Case 1 (Astra 10x10 P95=16): {rep1}\n")
            return 1

        # Case 2: 1x1 vs 2x1 size mismatch -> MUST FAIL
        write_png_rgba(os.path.join(ref_dir, "neg2.png"), 1, 1, bytearray([100, 100, 100, 255]))
        write_png_rgba(os.path.join(act_dir, "neg2.png"), 2, 1, bytearray([100, 100, 100, 255] * 2))
        m2 = os.path.join(td, "m2.json")
        with open(m2, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg2", "file": "neg2.png", "width": 1, "height": 1, "dpr": 1,
                    "rois": [{"name": "r1", "kind": "solid", "x": 0, "y": 0, "w": 1, "h": 1}],
                }]
            }, f)
        ok2, rep2 = verify_manifest(m2, ref_dir, act_dir)
        if ok2 or rep2["scenes"][0].get("same_size") is not False:
            sys.stderr.write(f"SELF-TEST FAIL: Case 2 (1x1 vs 2x1): {rep2}\n")
            return 1

        # Case 3a: missing ROI list -> MUST FAIL
        m3a = os.path.join(td, "m3a.json")
        with open(m3a, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg1", "file": "neg1.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [],
                }]
            }, f)
        ok3a, _ = verify_manifest(m3a, ref_dir, act_dir)
        if ok3a:
            sys.stderr.write("SELF-TEST FAIL: Case 3a (empty ROI list) unexpectedly passed\n")
            return 1

        # Case 3b: OOB ROI -> MUST FAIL
        write_png_rgba(os.path.join(act_dir, "neg3.png"), 10, 10, ref_px)
        write_png_rgba(os.path.join(ref_dir, "neg3.png"), 10, 10, ref_px)
        m3b = os.path.join(td, "m3b.json")
        with open(m3b, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg3", "file": "neg3.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "oob", "kind": "solid", "x": 8, "y": 8, "w": 10, "h": 10}],
                }]
            }, f)
        ok3b, _ = verify_manifest(m3b, ref_dir, act_dir)
        if ok3b:
            sys.stderr.write("SELF-TEST FAIL: Case 3b (OOB ROI) unexpectedly passed\n")
            return 1

        # Case 4: Identical image positive check -> MUST PASS
        m4 = os.path.join(td, "m4.json")
        with open(m4, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg3", "file": "neg3.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "ok", "kind": "solid", "x": 0, "y": 0, "w": 10, "h": 10}],
                }]
            }, f)
        ok4, rep4 = verify_manifest(m4, ref_dir, act_dir)
        if not ok4:
            sys.stderr.write(f"SELF-TEST FAIL: Case 4 (identical image) failed: {rep4}\n")
            return 1

        # Case 5 (new negative a): text ROI same bbox solid black block vs hollow red box -> MUST FAIL
        tw, th = 24, 24
        txt_ref = bytearray([255, 255, 255, 255] * (tw * th))
        txt_act = bytearray([255, 255, 255, 255] * (tw * th))
        _fill_rect(txt_ref, tw, 4, 4, 20, 20, (0, 0, 0, 255))
        _fill_rect(txt_act, tw, 4, 4, 20, 20, (220, 0, 0, 255))
        _fill_rect(txt_act, tw, 6, 6, 18, 18, (255, 255, 255, 255))
        write_png_rgba(os.path.join(ref_dir, "txt_blk.png"), tw, th, txt_ref)
        write_png_rgba(os.path.join(act_dir, "txt_blk.png"), tw, th, txt_act)
        m5 = os.path.join(td, "m5.json")
        with open(m5, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "txt_blk", "file": "txt_blk.png", "width": tw, "height": th, "dpr": 1,
                    "rois": [{"name": "t1", "kind": "text", "x": 0, "y": 0, "w": tw, "h": th}],
                }]
            }, f)
        ok5, rep5 = verify_manifest(m5, ref_dir, act_dir)
        if ok5:
            sys.stderr.write(f"SELF-TEST FAIL: Case 5 (solid black vs hollow red text ROI) unexpectedly passed: {rep5}\n")
            return 1

        # Case 6 (new negative b): text ROI wrong font weight (bold vs thin, same bbox & color) -> MUST FAIL
        bold_ref = bytearray([255, 255, 255, 255] * (tw * th))
        thin_act = bytearray([255, 255, 255, 255] * (tw * th))
        # Bold "H" (thick strokes, bbox 4..20 x 4..20)
        _fill_rect(bold_ref, tw, 4, 4, 9, 20, (20, 20, 20, 255))
        _fill_rect(bold_ref, tw, 15, 4, 20, 20, (20, 20, 20, 255))
        _fill_rect(bold_ref, tw, 4, 10, 20, 14, (20, 20, 20, 255))
        # Thin "H" (1px strokes, identical bbox 4..20 x 4..20 and identical color)
        _fill_rect(thin_act, tw, 4, 4, 5, 20, (20, 20, 20, 255))
        _fill_rect(thin_act, tw, 19, 4, 20, 20, (20, 20, 20, 255))
        _fill_rect(thin_act, tw, 4, 12, 20, 13, (20, 20, 20, 255))
        write_png_rgba(os.path.join(ref_dir, "txt_wt.png"), tw, th, bold_ref)
        write_png_rgba(os.path.join(act_dir, "txt_wt.png"), tw, th, thin_act)
        m6 = os.path.join(td, "m6.json")
        with open(m6, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "txt_wt", "file": "txt_wt.png", "width": tw, "height": th, "dpr": 1,
                    "rois": [{"name": "t_wt", "kind": "text", "x": 0, "y": 0, "w": tw, "h": th}],
                }]
            }, f)
        ok6, rep6 = verify_manifest(m6, ref_dir, act_dir)
        if ok6:
            sys.stderr.write(f"SELF-TEST FAIL: Case 6 (bold vs thin weight) unexpectedly passed: {rep6}\n")
            return 1

        # Case 7 (new negative c): dpr=3 -> MUST FAIL
        write_png_rgba(os.path.join(ref_dir, "dpr3.png"), 30, 30, bytearray([100, 100, 100, 255] * 900))
        write_png_rgba(os.path.join(act_dir, "dpr3.png"), 30, 30, bytearray([100, 100, 100, 255] * 900))
        m7 = os.path.join(td, "m7.json")
        with open(m7, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "dpr3", "file": "dpr3.png", "width": 10, "height": 10, "dpr": 3,
                    "rois": [{"name": "r", "kind": "solid", "x": 0, "y": 0, "w": 10, "h": 10}],
                }]
            }, f)
        ok7, rep7 = verify_manifest(m7, ref_dir, act_dir)
        if ok7:
            sys.stderr.write(f"SELF-TEST FAIL: Case 7 (dpr=3) unexpectedly passed: {rep7}\n")
            return 1

        # Case 8 (new negative d): static midpoint (mid == start) -> MUST FAIL
        start_px = bytearray([40, 40, 40, 255] * 100)
        end_px = bytearray([200, 200, 200, 255] * 100)
        mid_real_px = bytearray([120, 120, 120, 255] * 100)
        write_png_rgba(os.path.join(act_dir, "mp_start.png"), 10, 10, start_px)
        write_png_rgba(os.path.join(act_dir, "mp_end.png"), 10, 10, end_px)
        write_png_rgba(os.path.join(ref_dir, "mp_start.png"), 10, 10, start_px)
        write_png_rgba(os.path.join(ref_dir, "mp_end.png"), 10, 10, end_px)
        write_png_rgba(os.path.join(act_dir, "mp_fake.png"), 10, 10, start_px)
        write_png_rgba(os.path.join(ref_dir, "mp_fake.png"), 10, 10, start_px)
        m8 = os.path.join(td, "m8.json")
        with open(m8, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "mp_fake", "file": "mp_fake.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{
                        "name": "mp", "kind": "midpoint", "x": 0, "y": 0, "w": 10, "h": 10,
                        "start_file": "mp_start.png", "end_file": "mp_end.png",
                    }],
                }]
            }, f)
        ok8, rep8 = verify_manifest(m8, ref_dir, act_dir)
        if ok8:
            sys.stderr.write(f"SELF-TEST FAIL: Case 8 (static fake midpoint) unexpectedly passed: {rep8}\n")
            return 1

        # Case 8b (positive midpoint): mid != start and mid != end and matches ref -> MUST PASS
        write_png_rgba(os.path.join(act_dir, "mp_real.png"), 10, 10, mid_real_px)
        write_png_rgba(os.path.join(ref_dir, "mp_real.png"), 10, 10, mid_real_px)
        m8b = os.path.join(td, "m8b.json")
        with open(m8b, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "mp_real", "file": "mp_real.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{
                        "name": "mp", "kind": "midpoint", "x": 0, "y": 0, "w": 10, "h": 10,
                        "start_file": "mp_start.png", "end_file": "mp_end.png",
                    }],
                }]
            }, f)
        ok8b, rep8b = verify_manifest(m8b, ref_dir, act_dir)
        if not ok8b:
            sys.stderr.write(f"SELF-TEST FAIL: Case 8b (real midpoint) failed: {rep8b}\n")
            return 1

        # Case 9: --required-manifest catches deleted sub-ROI -> MUST FAIL
        m9_req = os.path.join(td, "m9_req.json")
        m9_partial = os.path.join(td, "m9_partial.json")
        with open(m9_req, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg3", "file": "neg3.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "full", "kind": "solid", "x": 0, "y": 0, "w": 10, "h": 10}],
                }]
            }, f)
        with open(m9_partial, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg3", "file": "neg3.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "half", "kind": "solid", "x": 0, "y": 0, "w": 5, "h": 10}],
                }]
            }, f)
        ok9, rep9 = verify_manifest(m9_partial, ref_dir, act_dir, required_manifest_path=m9_req)
        if ok9 or not rep9.get("coverage_errors"):
            sys.stderr.write(f"SELF-TEST FAIL: Case 9 (deleted ROI coverage check) unexpectedly passed: {rep9}\n")
            return 1

        # Case 10: aa max_p95_override capped at 18.0 with required reason (narrow arc strip, no large solid interior)
        aa_ref = bytearray([100, 100, 100, 255] * (10 * 4))
        aa_act = bytearray([100, 100, 100, 255] * (10 * 4))
        for idx in (0, 1):
            for c in range(3):
                aa_ref[idx * 4 + c] = 116
        for idx in (2, 3):
            for c in range(3):
                aa_act[idx * 4 + c] = 116
        write_png_rgba(os.path.join(ref_dir, "aa_arc.png"), 10, 4, aa_ref)
        write_png_rgba(os.path.join(act_dir, "aa_arc.png"), 10, 4, aa_act)
        m10_ok = os.path.join(td, "m10_ok.json")
        with open(m10_ok, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "aa_arc", "file": "aa_arc.png", "width": 10, "height": 4, "dpr": 1,
                    "rois": [{
                        "name": "aa_ok", "kind": "aa", "x": 0, "y": 0, "w": 10, "h": 4,
                        "max_p95_override": 18.0,
                        "override_reason": "pill button corner arc (Skia cubic AA vs circular SDF)",
                    }],
                }]
            }, f)
        ok10, rep10 = verify_manifest(m10_ok, ref_dir, act_dir)
        if not ok10 or rep10.get("override_count") != 1:
            sys.stderr.write(f"SELF-TEST FAIL: Case 10a (valid aa override) failed: {rep10}\n")
            return 1

        m10_bad = os.path.join(td, "m10_bad.json")
        with open(m10_bad, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg1", "file": "neg1.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{
                        "name": "aa_bad", "kind": "aa", "x": 0, "y": 0, "w": 10, "h": 10,
                        "max_p95_override": 20.0,
                        "override_reason": "exceeds cap",
                    }],
                }]
            }, f)
        ok10_bad, _ = verify_manifest(m10_bad, ref_dir, act_dir)
        if ok10_bad:
            sys.stderr.write("SELF-TEST FAIL: Case 10b (override > 18.0) unexpectedly passed\n")
            return 1

        # Case 11 (Astra P1-5): shifting the exact same text block down by 2 device px MUST FAIL at BOTH dpr=1 and dpr=2
        for shift_dpr in (1, 2):
            sw = 24 * shift_dpr
            sh = 24 * shift_dpr
            s_ref = bytearray([255, 255, 255, 255] * (sw * sh))
            s_act = bytearray([255, 255, 255, 255] * (sw * sh))
            _fill_rect(s_ref, sw, 4 * shift_dpr, 4 * shift_dpr, 18 * shift_dpr, 12 * shift_dpr, (20, 20, 20, 255))
            # Shift down by exactly 2 device pixels
            _fill_rect(s_act, sw, 4 * shift_dpr, 4 * shift_dpr + 2, 18 * shift_dpr, 12 * shift_dpr + 2, (20, 20, 20, 255))
            fname = f"txt_shift_{shift_dpr}x.png"
            write_png_rgba(os.path.join(ref_dir, fname), sw, sh, s_ref)
            write_png_rgba(os.path.join(act_dir, fname), sw, sh, s_act)
            m11 = os.path.join(td, f"m11_{shift_dpr}x.json")
            with open(m11, "w", encoding="utf-8") as f:
                json.dump({
                    "scenes": [{
                        "id": f"txt_shift_{shift_dpr}x", "file": fname, "width": 24, "height": 24, "dpr": shift_dpr,
                        "rois": [{"name": "t_shift", "kind": "text", "x": 0, "y": 0, "w": 24, "h": 24}],
                    }]
                }, f)
            ok11, rep11 = verify_manifest(m11, ref_dir, act_dir)
            if ok11:
                sys.stderr.write(f"SELF-TEST FAIL: Case 11 (2-dev-px shift at dpr={shift_dpr}) unexpectedly passed: {rep11}\n")
                return 1

        # Case 12 (Astra P1-6): 10x10 image with 1 pixel changed by +255 inside a required 2x2 solid ROI
        # MUST FAIL when candidate covers it with a 10x10 aa ROI
        s12_ref = bytearray([0, 0, 0, 255] * 100)
        s12_act = bytearray([0, 0, 0, 255] * 100)
        s12_act[0] = 255
        s12_act[1] = 255
        s12_act[2] = 255
        write_png_rgba(os.path.join(ref_dir, "solid_aa.png"), 10, 10, s12_ref)
        write_png_rgba(os.path.join(act_dir, "solid_aa.png"), 10, 10, s12_act)
        m12_req = os.path.join(td, "m12_req.json")
        m12_cand = os.path.join(td, "m12_cand.json")
        with open(m12_req, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "solid_aa", "file": "solid_aa.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "tiny_solid", "kind": "solid", "x": 0, "y": 0, "w": 2, "h": 2}],
                }]
            }, f)
        with open(m12_cand, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "solid_aa", "file": "solid_aa.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{"name": "wide_aa", "kind": "aa", "x": 0, "y": 0, "w": 10, "h": 10}],
                }]
            }, f)
        ok12, rep12 = verify_manifest(m12_cand, ref_dir, act_dir, required_manifest_path=m12_req)
        if ok12 or not rep12.get("coverage_errors"):
            sys.stderr.write(f"SELF-TEST FAIL: Case 12 (solid->aa downgrade) unexpectedly passed: {rep12}\n")
            return 1

        # Case 13 (Opus P2-3): tightened ink_ratio window [0.80, 1.25] rejects 0.75 ratio
        s13_ref = bytearray([255, 255, 255, 255] * (tw * th))
        s13_act = bytearray([255, 255, 255, 255] * (tw * th))
        _fill_rect(s13_ref, tw, 4, 4, 20, 12, (20, 20, 20, 255))  # 16*8 = 128 px
        _fill_rect(s13_act, tw, 4, 4, 20, 12, (20, 20, 20, 255))
        _fill_rect(s13_act, tw, 6, 6, 14, 10, (255, 255, 255, 255))  # punch out 8*4 = 32 px -> 96/128 = 0.75
        write_png_rgba(os.path.join(ref_dir, "txt_ratio.png"), tw, th, s13_ref)
        write_png_rgba(os.path.join(act_dir, "txt_ratio.png"), tw, th, s13_act)
        m13 = os.path.join(td, "m13.json")
        with open(m13, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "txt_ratio", "file": "txt_ratio.png", "width": tw, "height": th, "dpr": 1,
                    "rois": [{"name": "t_ratio", "kind": "text", "x": 0, "y": 0, "w": tw, "h": th}],
                }]
            }, f)
        ok13, rep13 = verify_manifest(m13, ref_dir, act_dir)
        if ok13:
            sys.stderr.write(f"SELF-TEST FAIL: Case 13 (ink_ratio=0.75 outside [0.80, 1.25]) unexpectedly passed: {rep13}\n")
            return 1

        # Case 14 (Astra R3 P1-2): 16x16 required solid ROI with 1 pixel changed by 255 MUST FAIL
        # when candidate downgrades the entire solid ROI to kind='text' without explicit text_masks
        s14_ref = bytearray([255, 255, 255, 255] * (16 * 16))
        s14_act = bytearray([255, 255, 255, 255] * (16 * 16))
        _fill_rect(s14_ref, 16, 4, 4, 12, 12, (0, 0, 0, 255))
        _fill_rect(s14_act, 16, 4, 4, 12, 12, (0, 0, 0, 255))
        # Flip 1 center pixel to white (delta=255)
        p14 = (8 * 16 + 8) * 4
        s14_act[p14] = 255
        s14_act[p14 + 1] = 255
        s14_act[p14 + 2] = 255
        write_png_rgba(os.path.join(ref_dir, "solid16.png"), 16, 16, s14_ref)
        write_png_rgba(os.path.join(act_dir, "solid16.png"), 16, 16, s14_act)
        m14_req = os.path.join(td, "m14_req.json")
        m14_cand = os.path.join(td, "m14_cand.json")
        with open(m14_req, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "solid16", "file": "solid16.png", "width": 16, "height": 16, "dpr": 1,
                    "rois": [{"name": "roi", "kind": "solid", "x": 0, "y": 0, "w": 16, "h": 16}],
                }]
            }, f)
        with open(m14_cand, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "solid16", "file": "solid16.png", "width": 16, "height": 16, "dpr": 1,
                    "rois": [{"name": "roi", "kind": "text", "x": 0, "y": 0, "w": 16, "h": 16}],
                }]
            }, f)
        ok14, rep14 = verify_manifest(m14_cand, ref_dir, act_dir, required_manifest_path=m14_req)
        if ok14 or not rep14.get("coverage_errors"):
            sys.stderr.write(f"SELF-TEST FAIL: Case 14 (16x16 solid->text downgrade) unexpectedly passed: {rep14}\n")
            return 1

        # Case 15 (Astra R3 P1-3): 1.5 device-px pure vertical shift at BOTH dpr=1 and dpr=2 MUST FAIL
        for s15_dpr in (1, 2):
            sw15 = 24 * s15_dpr
            sh15 = 24 * s15_dpr
            r15 = bytearray([255, 255, 255, 255] * (sw15 * sh15))
            a15 = bytearray([255, 255, 255, 255] * (sw15 * sh15))
            # Solid 16px-tall block on [y0..y0+16) in ref; shifted down by 1.5 dev px via half-coverage edges in act
            y0 = 4 * s15_dpr
            x0 = 4 * s15_dpr
            x1 = 18 * s15_dpr
            bh = 8 * s15_dpr
            _fill_rect(r15, sw15, x0, y0, x1, y0 + bh, (15, 15, 15, 255))
            _fill_rect(a15, sw15, x0, y0 + 1, x1, y0 + 2, (135, 135, 135, 255))
            _fill_rect(a15, sw15, x0, y0 + 2, x1, y0 + bh + 1, (15, 15, 15, 255))
            _fill_rect(a15, sw15, x0, y0 + bh + 1, x1, y0 + bh + 2, (135, 135, 135, 255))
            fn15 = f"shift15_{s15_dpr}x.png"
            write_png_rgba(os.path.join(ref_dir, fn15), sw15, sh15, r15)
            write_png_rgba(os.path.join(act_dir, fn15), sw15, sh15, a15)
            m15 = os.path.join(td, f"m15_{s15_dpr}x.json")
            with open(m15, "w", encoding="utf-8") as f:
                json.dump({
                    "scenes": [{
                        "id": f"shift15_{s15_dpr}x", "file": fn15, "width": 24, "height": 24, "dpr": s15_dpr,
                        "rois": [{"name": "t_shift15", "kind": "text", "x": 0, "y": 0, "w": 24, "h": 24}],
                    }]
                }, f)
            ok15, rep15 = verify_manifest(m15, ref_dir, act_dir)
            if ok15:
                sys.stderr.write(f"SELF-TEST FAIL: Case 15 (1.5-dev-px shift at dpr={s15_dpr}) unexpectedly passed: {rep15}\n")
                return 1

        # Helper: draw a synthetic 85x36 blue button with white glyph strokes on white page bg
        def _make_btn_img(fill_rgba: tuple[int, int, int, int], with_text: bool) -> bytearray:
            bw, bh = 100, 48
            buf = bytearray([245, 245, 245, 255] * (bw * bh))
            # Blue button body at (6, 6)..(91, 42) => 85x36
            _fill_rect(buf, bw, 6, 6, 91, 42, fill_rgba)
            if with_text:
                # 6 crisp 2px-wide vertical glyph bars + 1px crossbars inside (22, 16)..(74, 30)
                for gx in range(24, 72, 8):
                    _fill_rect(buf, bw, gx, 17, gx + 2, 29, (255, 255, 255, 255))
                    _fill_rect(buf, bw, gx, 22, gx + 5, 24, (255, 255, 255, 255))
            return buf

        btn_ref = _make_btn_img((4, 133, 247, 255), with_text=True)
        btn_erased = _make_btn_img((4, 133, 247, 255), with_text=False)
        btn_r20 = _make_btn_img((24, 133, 247, 255), with_text=True)
        write_png_rgba(os.path.join(ref_dir, "btn_adv.png"), 100, 48, btn_ref)
        write_png_rgba(os.path.join(act_dir, "btn_adv_erased.png"), 100, 48, btn_erased)
        write_png_rgba(os.path.join(act_dir, "btn_adv_r20.png"), 100, 48, btn_r20)
        write_png_rgba(os.path.join(act_dir, "btn_adv_ok.png"), 100, 48, btn_ref)
        write_png_rgba(os.path.join(ref_dir, "btn_adv_erased.png"), 100, 48, btn_ref)
        write_png_rgba(os.path.join(ref_dir, "btn_adv_r20.png"), 100, 48, btn_ref)
        write_png_rgba(os.path.join(ref_dir, "btn_adv_ok.png"), 100, 48, btn_ref)

        # Case 16 (Astra r2-erased-label mode): erasing white label inside blue button -> MUST FAIL
        m16 = os.path.join(td, "m16.json")
        with open(m16, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "btn_erased", "file": "btn_adv_erased.png", "width": 100, "height": 48, "dpr": 1,
                    "rois": [
                        {"name": "btn_fill_top", "kind": "solid", "x": 10, "y": 8, "w": 77, "h": 6},
                        {"name": "btn_label", "kind": "text", "x": 20, "y": 15, "w": 56, "h": 16},
                    ],
                }]
            }, f)
        ok16, rep16 = verify_manifest(m16, ref_dir, act_dir)
        if ok16:
            sys.stderr.write(f"SELF-TEST FAIL: Case 16 (erased button label) unexpectedly passed: {rep16}\n")
            return 1

        # Case 17 (Opus README_fault_injection mode): button fill R+20 -> MUST FAIL (both proper split & spoofed full text)
        m17_split = os.path.join(td, "m17_split.json")
        with open(m17_split, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "btn_r20", "file": "btn_adv_r20.png", "width": 100, "height": 48, "dpr": 1,
                    "rois": [
                        {"name": "btn_fill_top", "kind": "solid", "x": 10, "y": 8, "w": 77, "h": 6},
                        {"name": "btn_label", "kind": "text", "x": 20, "y": 15, "w": 56, "h": 16},
                    ],
                }]
            }, f)
        ok17a, rep17a = verify_manifest(m17_split, ref_dir, act_dir)
        if ok17a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 17a (button fill R+20 split) unexpectedly passed: {rep17a}\n")
            return 1

        # Case 18a: marking entire filled button (incl. outer bg corners) as kind="text" -> MUST FAIL even when act==ref!
        m18a = os.path.join(td, "m18a.json")
        with open(m18a, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "btn_spoof_text", "file": "btn_adv_ok.png", "width": 100, "height": 48, "dpr": 1,
                    "rois": [{"name": "whole_btn_as_text", "kind": "text", "x": 4, "y": 4, "w": 89, "h": 40}],
                }]
            }, f)
        ok18a, rep18a = verify_manifest(m18a, ref_dir, act_dir)
        if ok18a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 18a (whole filled button as kind=text) unexpectedly passed: {rep18a}\n")
            return 1

        # Case 18b: full-box text_mask (>=75%) on required solid ROI (w>28 or h>28) -> MUST FAIL coverage check!
        m18b_req = os.path.join(td, "m18b_req.json")
        m18b_cand = os.path.join(td, "m18b_cand.json")
        with open(m18b_req, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "btn_full_mask", "file": "btn_adv_ok.png", "width": 100, "height": 48, "dpr": 1,
                    "rois": [{
                        "name": "btn_solid", "kind": "solid", "x": 6, "y": 6, "w": 85, "h": 36,
                        "text_masks": [[6.0, 6.0, 85.0, 36.0]],
                    }],
                }]
            }, f)
        with open(m18b_cand, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "btn_full_mask", "file": "btn_adv_ok.png", "width": 100, "height": 48, "dpr": 1,
                    "rois": [{"name": "btn_text", "kind": "text", "x": 6, "y": 6, "w": 85, "h": 36}],
                }]
            }, f)
        ok18b, rep18b = verify_manifest(m18b_cand, ref_dir, act_dir, required_manifest_path=m18b_req)
        if ok18b or not rep18b.get("coverage_errors"):
            sys.stderr.write(f"SELF-TEST FAIL: Case 18b (full-box text_mask >=75%) unexpectedly passed: {rep18b}\n")
            return 1

        # Case 19: midpoint replaced with end OR missing end_file PNG -> MUST FAIL
        write_png_rgba(os.path.join(act_dir, "mp_fake_end.png"), 10, 10, end_px)
        write_png_rgba(os.path.join(ref_dir, "mp_fake_end.png"), 10, 10, end_px)
        m19a = os.path.join(td, "m19a.json")
        with open(m19a, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "mp_fake_end", "file": "mp_fake_end.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{
                        "name": "mp", "kind": "midpoint", "x": 0, "y": 0, "w": 10, "h": 10,
                        "start_file": "mp_start.png", "end_file": "mp_end.png",
                    }],
                }]
            }, f)
        ok19a, rep19a = verify_manifest(m19a, ref_dir, act_dir)
        if ok19a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 19a (midpoint==end) unexpectedly passed: {rep19a}\n")
            return 1

        m19b = os.path.join(td, "m19b.json")
        with open(m19b, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "mp_missing_end", "file": "mp_real.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{
                        "name": "mp", "kind": "midpoint", "x": 0, "y": 0, "w": 10, "h": 10,
                        "start_file": "mp_start.png", "end_file": "mp_deleted.png",
                    }],
                }]
            }, f)
        ok19b, rep19b = verify_manifest(m19b, ref_dir, act_dir)
        if ok19b:
            sys.stderr.write(f"SELF-TEST FAIL: Case 19b (missing end_file PNG) unexpectedly passed: {rep19b}\n")
            return 1

        # Case 20 (Opus P1-1): Horizontally mirrored asymmetric text ("Secondary"-like) -> MUST FAIL
        gw, gh = 64, 24
        g_ref = bytearray([245, 245, 245, 255] * (gw * gh))
        # Asymmetric strokes: tall bar on left, short crossbar in middle, descending tail on right
        _fill_rect(g_ref, gw, 8, 4, 11, 18, (20, 20, 20, 255))
        _fill_rect(g_ref, gw, 11, 4, 18, 6, (20, 20, 20, 255))
        _fill_rect(g_ref, gw, 24, 10, 34, 12, (20, 20, 20, 255))
        _fill_rect(g_ref, gw, 24, 10, 26, 18, (20, 20, 20, 255))
        _fill_rect(g_ref, gw, 42, 8, 44, 21, (20, 20, 20, 255))
        _fill_rect(g_ref, gw, 44, 14, 54, 16, (20, 20, 20, 255))
        g_mir = bytearray([245, 245, 245, 255] * (gw * gh))
        for y in range(gh):
            for x in range(gw):
                si = (y * gw + (gw - 1 - x)) * 4
                di = (y * gw + x) * 4
                g_mir[di : di + 4] = g_ref[si : si + 4]
        write_png_rgba(os.path.join(ref_dir, "txt_mir.png"), gw, gh, g_ref)
        write_png_rgba(os.path.join(act_dir, "txt_mir.png"), gw, gh, g_mir)
        m20 = os.path.join(td, "m20.json")
        with open(m20, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "txt_mir", "file": "txt_mir.png", "width": gw, "height": gh, "dpr": 1,
                    "rois": [{"name": "t_mir", "kind": "text", "x": 0, "y": 0, "w": gw, "h": gh}],
                }]
            }, f)
        ok20, rep20 = verify_manifest(m20, ref_dir, act_dir)
        if ok20:
            sys.stderr.write(f"SELF-TEST FAIL: Case 20 (mirrored text) unexpectedly passed: {rep20}\n")
            return 1

        # Case 21 (Opus P1-1): Character-swapped "Copy" -> "Cpoy" (o and p swapped) -> MUST FAIL
        cw21, ch21 = 64, 28
        def _draw_copy_word(swap_op: bool) -> bytearray:
            buf = bytearray([255, 255, 255, 255] * (cw21 * ch21))
            # 'C' at x=6..15, y=4..18
            _fill_rect(buf, cw21, 6, 4, 8, 18, (20, 20, 20, 255))
            _fill_rect(buf, cw21, 6, 4, 15, 6, (20, 20, 20, 255))
            _fill_rect(buf, cw21, 6, 16, 15, 18, (20, 20, 20, 255))
            # 'o' (y=9..18) vs 'p' (y=9..24 with left descender stem)
            o_x = 34 if swap_op else 20
            p_x = 20 if swap_op else 34
            # draw 'o'
            _fill_rect(buf, cw21, o_x, 9, o_x + 9, 11, (20, 20, 20, 255))
            _fill_rect(buf, cw21, o_x, 16, o_x + 9, 18, (20, 20, 20, 255))
            _fill_rect(buf, cw21, o_x, 9, o_x + 2, 18, (20, 20, 20, 255))
            _fill_rect(buf, cw21, o_x + 7, 9, o_x + 9, 18, (20, 20, 20, 255))
            # draw 'p'
            _fill_rect(buf, cw21, p_x, 9, p_x + 9, 11, (20, 20, 20, 255))
            _fill_rect(buf, cw21, p_x, 16, p_x + 9, 18, (20, 20, 20, 255))
            _fill_rect(buf, cw21, p_x, 9, p_x + 2, 24, (20, 20, 20, 255))
            _fill_rect(buf, cw21, p_x + 7, 9, p_x + 9, 18, (20, 20, 20, 255))
            # 'y' at x=48..57, y=9..24
            _fill_rect(buf, cw21, 48, 9, 50, 18, (20, 20, 20, 255))
            _fill_rect(buf, cw21, 55, 9, 57, 24, (20, 20, 20, 255))
            _fill_rect(buf, cw21, 48, 16, 57, 18, (20, 20, 20, 255))
            return buf
        write_png_rgba(os.path.join(ref_dir, "txt_copy.png"), cw21, ch21, _draw_copy_word(False))
        write_png_rgba(os.path.join(act_dir, "txt_copy.png"), cw21, ch21, _draw_copy_word(True))
        m21 = os.path.join(td, "m21.json")
        with open(m21, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "txt_copy", "file": "txt_copy.png", "width": cw21, "height": ch21, "dpr": 1,
                    "rois": [{"name": "t_copy", "kind": "text", "x": 0, "y": 0, "w": cw21, "h": ch21}],
                }]
            }, f)
        ok21, rep21 = verify_manifest(m21, ref_dir, act_dir)
        if ok21:
            sys.stderr.write(f"SELF-TEST FAIL: Case 21 (Copy -> Cpoy swap) unexpectedly passed: {rep21}\n")
            return 1

        # Case 22 (Opus P1-1): RTL punctuation reversed (".org" vs "org.") -> MUST FAIL
        rw22, rh22 = 48, 22
        def _draw_dot_org(dot_on_right: bool) -> bytearray:
            buf = bytearray([255, 255, 255, 255] * (rw22 * rh22))
            if dot_on_right:
                # "org." : glyphs at x=6..34, dot at x=38..41
                _fill_rect(buf, rw22, 6, 8, 14, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 8, 10, 12, 14, (255, 255, 255, 255))
                _fill_rect(buf, rw22, 17, 8, 19, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 19, 8, 23, 10, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 26, 8, 34, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 28, 10, 32, 14, (255, 255, 255, 255))
                _fill_rect(buf, rw22, 32, 14, 34, 19, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 38, 14, 41, 16, (20, 20, 20, 255))
            else:
                # ".org" : dot at x=6..9, glyphs at x=12..40
                _fill_rect(buf, rw22, 6, 14, 9, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 12, 8, 20, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 14, 10, 18, 14, (255, 255, 255, 255))
                _fill_rect(buf, rw22, 23, 8, 25, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 25, 8, 29, 10, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 32, 8, 40, 16, (20, 20, 20, 255))
                _fill_rect(buf, rw22, 34, 10, 38, 14, (255, 255, 255, 255))
                _fill_rect(buf, rw22, 38, 14, 40, 19, (20, 20, 20, 255))
            return buf
        write_png_rgba(os.path.join(ref_dir, "txt_rtl.png"), rw22, rh22, _draw_dot_org(True))
        write_png_rgba(os.path.join(act_dir, "txt_rtl.png"), rw22, rh22, _draw_dot_org(False))
        m22 = os.path.join(td, "m22.json")
        with open(m22, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "txt_rtl", "file": "txt_rtl.png", "width": rw22, "height": rh22, "dpr": 1,
                    "rois": [{"name": "t_rtl", "kind": "text", "x": 0, "y": 0, "w": rw22, "h": rh22}],
                }]
            }, f)
        ok22, rep22 = verify_manifest(m22, ref_dir, act_dir)
        if ok22:
            sys.stderr.write(f"SELF-TEST FAIL: Case 22 (RTL punctuation reversed) unexpectedly passed: {rep22}\n")
            return 1

        # Case 23 (Astra P1-6): Whole 44x24 Switch stamped kind="aa" with max_p95_override=18 -> MUST FAIL
        sw23, sh23 = 48, 28
        sw_img = bytearray([255, 255, 255, 255] * (sw23 * sh23))
        _fill_rect(sw_img, sw23, 2, 2, 46, 26, (4, 133, 247, 255))
        _fill_rect(sw_img, sw23, 24, 5, 43, 23, (255, 255, 255, 255))
        write_png_rgba(os.path.join(ref_dir, "whole_sw.png"), sw23, sh23, sw_img)
        write_png_rgba(os.path.join(act_dir, "whole_sw.png"), sw23, sh23, sw_img)
        m23 = os.path.join(td, "m23.json")
        with open(m23, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "whole_sw", "file": "whole_sw.png", "width": sw23, "height": sh23, "dpr": 1,
                    "rois": [{
                        "name": "whole_switch_aa", "kind": "aa", "x": 2, "y": 2, "w": 44, "h": 24,
                        "max_p95_override": 18.0,
                        "override_reason": "switch track/thumb circular arc",
                    }],
                }]
            }, f)
        ok23, rep23 = verify_manifest(m23, ref_dir, act_dir)
        if ok23:
            sys.stderr.write(f"SELF-TEST FAIL: Case 23 (whole Switch aa override) unexpectedly passed: {rep23}\n")
            return 1

        # Case 24a/24b/24c (Astra P1-7): Switch midpoint negative cases
        def _draw_switch(track_rgba: tuple[int, int, int, int], thumb_x: int) -> bytearray:
            buf = bytearray([255, 255, 255, 255] * (48 * 28))
            _fill_rect(buf, 48, 4, 4, 44, 24, track_rgba)
            _fill_rect(buf, 48, thumb_x, 6, thumb_x + 16, 22, (252, 252, 252, 255))
            return buf

        sw_start = _draw_switch((212, 212, 216, 255), 6)
        sw_mid = _draw_switch((90, 160, 240, 255), 16)
        sw_end = _draw_switch((4, 133, 247, 255), 26)
        sw_wrong_dir = _draw_switch((90, 160, 240, 255), 5)  # left of start!
        write_png_rgba(os.path.join(ref_dir, "sw_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(ref_dir, "sw_mid.png"), 48, 28, sw_mid)
        write_png_rgba(os.path.join(ref_dir, "sw_end.png"), 48, 28, sw_end)
        write_png_rgba(os.path.join(act_dir, "sw_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(act_dir, "sw_mid.png"), 48, 28, sw_mid)
        write_png_rgba(os.path.join(act_dir, "sw_end.png"), 48, 28, sw_end)
        write_png_rgba(os.path.join(act_dir, "sw_end_is_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(act_dir, "sw_wrong_dir.png"), 48, 28, sw_wrong_dir)

        # 24a: end_file replaced by start_file -> MUST FAIL
        m24a = os.path.join(td, "m24a.json")
        with open(m24a, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "sw_mid", "file": "sw_mid.png", "width": 48, "height": 28, "dpr": 1,
                    "rois": [{
                        "name": "sw_mp", "kind": "midpoint", "x": 0, "y": 0, "w": 48, "h": 28,
                        "start_file": "sw_start.png", "end_file": "sw_end_is_start.png",
                    }],
                }]
            }, f)
        # Also place ref copy of sw_end_is_start.png as sw_end.png? No, test with real ref sw_end.png by overwriting act_dir/sw_end in subdir
        act_24a = os.path.join(td, "act_24a")
        os.makedirs(act_24a)
        write_png_rgba(os.path.join(act_24a, "sw_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(act_24a, "sw_mid.png"), 48, 28, sw_mid)
        write_png_rgba(os.path.join(act_24a, "sw_end.png"), 48, 28, sw_start)
        m24_common = os.path.join(td, "m24_common.json")
        with open(m24_common, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "sw_mid", "file": "sw_mid.png", "width": 48, "height": 28, "dpr": 1,
                    "rois": [{
                        "name": "sw_mp", "kind": "midpoint", "x": 0, "y": 0, "w": 48, "h": 28,
                        "start_file": "sw_start.png", "end_file": "sw_end.png",
                    }],
                }]
            }, f)
        ok24a, rep24a = verify_manifest(m24_common, ref_dir, act_24a)
        if ok24a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 24a (midpoint end replaced by start) unexpectedly passed: {rep24a}\n")
            return 1

        # 24b: start and end swapped -> MUST FAIL
        act_24b = os.path.join(td, "act_24b")
        os.makedirs(act_24b)
        write_png_rgba(os.path.join(act_24b, "sw_start.png"), 48, 28, sw_end)
        write_png_rgba(os.path.join(act_24b, "sw_mid.png"), 48, 28, sw_mid)
        write_png_rgba(os.path.join(act_24b, "sw_end.png"), 48, 28, sw_start)
        ok24b, rep24b = verify_manifest(m24_common, ref_dir, act_24b)
        if ok24b:
            sys.stderr.write(f"SELF-TEST FAIL: Case 24b (midpoint swapped start/end) unexpectedly passed: {rep24b}\n")
            return 1

        # 24c: wrong direction (mid thumb left of start) -> MUST FAIL
        act_24c = os.path.join(td, "act_24c")
        os.makedirs(act_24c)
        write_png_rgba(os.path.join(act_24c, "sw_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(act_24c, "sw_mid.png"), 48, 28, sw_wrong_dir)
        write_png_rgba(os.path.join(act_24c, "sw_end.png"), 48, 28, sw_end)
        ok24c, rep24c = verify_manifest(m24_common, ref_dir, act_24c)
        if ok24c:
            sys.stderr.write(f"SELF-TEST FAIL: Case 24c (midpoint wrong direction) unexpectedly passed: {rep24c}\n")
            return 1

        # Case 31: 1x single thin glyph (ref_cnt < 64) conserving continuous ink mass under
        # subpixel AA phase shift passes, while thinning to 0.70x continuous ink mass fails.
        tw31, th31 = 12, 15
        g31_ref = bytearray([255, 255, 255, 255] * (tw31 * th31))
        g31_shift = bytearray([255, 255, 255, 255] * (tw31 * th31))
        g31_thin = bytearray([255, 255, 255, 255] * (tw31 * th31))
        for yy in range(2, 12):
            # ref: 3-column AA stem (col 2 faint fringe d=28, col 3 full d=200, col 4 d=110) + foot
            for xx, val in ((2, 227), (3, 55), (4, 145)):
                o = (yy * tw31 + xx) * 4
                g31_ref[o:o + 4] = bytes((val, val, val, 255))
            # shift: same energy concentrated into 2 columns (col 3 d=170, col 4 d=168 -> 0% mass loss)
            for xx, val in ((3, 85), (4, 87)):
                o = (yy * tw31 + xx) * 4
                g31_shift[o:o + 4] = bytes((val, val, val, 255))
            # thin: 0.65x continuous ink mass -> must fail
            for xx, val in ((3, 140), (4, 155)):
                o = (yy * tw31 + xx) * 4
                g31_thin[o:o + 4] = bytes((val, val, val, 255))
        for xx in range(5, 9):
            for yy in (10, 11):
                o = (yy * tw31 + xx) * 4
                g31_ref[o:o + 4] = bytes((70, 70, 70, 255))
                g31_shift[o:o + 4] = bytes((70, 70, 70, 255))
                g31_thin[o:o + 4] = bytes((145, 145, 145, 255))
        write_png_rgba(os.path.join(ref_dir, "g31.png"), tw31, th31, g31_ref)
        write_png_rgba(os.path.join(act_dir, "g31.png"), tw31, th31, g31_shift)
        m31 = os.path.join(td, "m31.json")
        with open(m31, "w", encoding="utf-8") as f:
            json.dump({
                "version": 2,
                "scenes": [{
                    "id": "g31", "file": "g31.png", "width": tw31, "height": th31, "dpr": 1,
                    "rois": [{"name": "t31", "kind": "text", "x": 0, "y": 0, "w": tw31, "h": th31}],
                }],
            }, f)
        ok31a, rep31a = verify_manifest(m31, ref_dir, act_dir)
        if not ok31a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 31a (1x single glyph conserved ink mass) unexpectedly failed: {rep31a}\n")
            return 1
        write_png_rgba(os.path.join(act_dir, "g31.png"), tw31, th31, g31_thin)
        ok31b, rep31b = verify_manifest(m31, ref_dir, act_dir)
        if ok31b:
            sys.stderr.write(f"SELF-TEST FAIL: Case 31b (1x single glyph thinned 0.65x) unexpectedly passed: {rep31b}\n")
            return 1

        # Case 32 (Opus P1-1): Real Inter "Secondary" (2x, 154x36) positive + "Seocndary" swap + "Sec ndary" erasure
        _real_sec_ref_b64 = (
            "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"
            "AAAASUVORK5CYII="
        )
        _real_sec_act_b64 = (
            "iVBORw0KGgoAAAANSUhEUgAAAJoAAAAkCAYAAABxPyMSAAAKBElEQVR4nO2b93dVVRbH/UscsevYx4IFl21ZRkdnLMuyVGYUQcSC"
            "osKCkWYDLIgFUQQbFhBJAUIIJAFCEiCEJKSRhCSEhISE+R/OnM+Vc99+N+ec917ySJbk/vBd5N1T7infs/d373M548SJ/6kYMU41"
            "zhjtAcQYG4iJFmNEEBMtxoggJlqMEUHaRDvc1ae+29qo5q+pUi+tqFBvrtqrPlxfq8pqj4z6JE43bKxoU6sLGwM0HD426uPJBlIS"
            "rf3ocTVjZaU6+7kcdeazG6y4bc5WVbi3fdQnc7rg0UWl4dpu2NU66uPJBrxEaz3SpybM2uIkmMRZk3LUj9uaRn1CpwPGHNEeXZyY"
            "8IUv5Kn3fq1W5Qe7AgLubTiqVhU2qFtmF4Z1zp6cq2pbekZ9Un92jCmiob1CAmm3ucuhxbqO9at75m0L677+zZ5Rn9SfHWOKaF9t"
            "bggn+9jiHd5Ofi1tVuM0GcF9C7d76yJucbEf6UDi87w6lV/epnp6BzIadOmBziAwoQ/G6ToELhw41KN+KWlWSzfUqlVbGgJ9ebz/"
            "hLcN48wrb03SogRIa0sPhf1UNR1NewxtRxJtv9xYryrruzImWpt+/287W9SnuXVqWU6tWqf7O9jq9yg7azqDeYCunv7gGW2+1uu4"
            "aN2BYG15VlzdEdarb/P3iYczdbfsOZwZ0Zb8VhNO9okP/ERLd2Gf+aRM/WXSYH331xfz1aK1B1Ju9rodLerW2VutGvHmWYUqb7f/"
            "9LOZyAHbGK6dsVkt1xvuajvuZDD0t1c3Bb8hyEXT8gb1M3HpLtWhAyhXP316jnO+36fOfz53UNtnl5WpY30DKYnW0tmnJn+2O5Aq"
            "trV4+P0SVd3cbX0/ZaYeMmixJpcM9NgH6pFVMM94l29d5/1YFdb9z9KyzIjGaTONz5mSq4qqOoZMsjp9Ym58syBlQPH4klIn2Rav"
            "q7ESRILyL/IPWtuTMkBnphrDtOXlamDATbQrp28K3uHr4xFNFFsfzI1Dm+r9PqJhfSC7LRiTvy/Rh7fOYt0k0dDc0X4M0SCheXbB"
            "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"
        )
        sec_ref_path = os.path.join(ref_dir, "real_inter_sec.png")
        sec_act_path = os.path.join(act_dir, "real_inter_sec.png")
        with open(sec_ref_path, "wb") as f:
            f.write(base64.b64decode(_real_sec_ref_b64))
        with open(sec_act_path, "wb") as f:
            f.write(base64.b64decode(_real_sec_act_b64))
        m32 = os.path.join(td, "m32.json")
        with open(m32, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "real_inter_sec", "file": "real_inter_sec.png", "width": 77, "height": 18, "dpr": 2,
                    "rois": [{"name": "m01-btn-secondary-text-0", "kind": "text", "x": 0, "y": 0, "w": 77, "h": 18}],
                }]
            }, f)
        ok32a, rep32a = verify_manifest(m32, ref_dir, act_dir)
        if not ok32a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 32a (real Inter Secondary positive) failed: {rep32a}\n")
            return 1

        # Extract actual glyph segments from real Inter "Secondary" to construct "Seocndary" & "Sec ndary"
        sw32, sh32, sec_act_rgba = read_png_rgba(sec_act_path)
        s32_st = _text_ink_stats(sw32, sec_act_rgba, 0, 0, sw32, sh32)
        assert s32_st is not None
        bg32 = s32_st["bg_rgb"]
        cols32 = [
            any(
                max(
                    abs(sec_act_rgba[(yy * sw32 + xx) * 4] - bg32[0]),
                    abs(sec_act_rgba[(yy * sw32 + xx) * 4 + 1] - bg32[1]),
                    abs(sec_act_rgba[(yy * sw32 + xx) * 4 + 2] - bg32[2]),
                ) >= 24
                for yy in range(sh32)
            )
            for xx in range(sw32)
        ]
        segs32: list[tuple[int, int]] = []
        in_s32 = False
        s0_32 = 0
        for xx, has in enumerate(cols32):
            if has and not in_s32:
                in_s32 = True
                s0_32 = xx
            elif not has and in_s32:
                in_s32 = False
                segs32.append((s0_32, xx))
        if in_s32:
            segs32.append((s0_32, sw32))
        if len(segs32) < 9:
            sys.stderr.write(f"SELF-TEST FAIL: Case 32 expected 9 segments in real Inter Secondary, got {len(segs32)}\n")
            return 1
        c0_32, c1_32 = segs32[2]
        o0_32, o1_32 = segs32[3]
        gap_32 = o0_32 - c1_32

        # 32b: "Seocndary" (adjacent same-height swap of 'c' and 'o') -> MUST FAIL
        sec_swap = bytearray(sec_act_rgba)
        for yy in range(sh32):
            for xx in range(c0_32, o1_32):
                di = (yy * sw32 + xx) * 4
                sec_swap[di : di + 4] = bytes((bg32[0], bg32[1], bg32[2], 255))
            for dx in range(o1_32 - o0_32):
                si = (yy * sw32 + o0_32 + dx) * 4
                di = (yy * sw32 + c0_32 + dx) * 4
                sec_swap[di : di + 4] = sec_act_rgba[si : si + 4]
            for dx in range(c1_32 - c0_32):
                si = (yy * sw32 + c0_32 + dx) * 4
                di = (yy * sw32 + c0_32 + (o1_32 - o0_32) + gap_32 + dx) * 4
                sec_swap[di : di + 4] = sec_act_rgba[si : si + 4]
        write_png_rgba(sec_act_path, sw32, sh32, sec_swap)
        ok32b, rep32b = verify_manifest(m32, ref_dir, act_dir)
        if ok32b:
            sys.stderr.write(f"SELF-TEST FAIL: Case 32b (real Inter Seocndary swap) unexpectedly passed: {rep32b}\n")
            return 1

        # 32c: "Sec ndary" (single glyph 'o' erased) -> MUST FAIL
        sec_del = bytearray(sec_act_rgba)
        for yy in range(sh32):
            for xx in range(o0_32, o1_32):
                di = (yy * sw32 + xx) * 4
                sec_del[di : di + 4] = bytes((bg32[0], bg32[1], bg32[2], 255))
        write_png_rgba(sec_act_path, sw32, sh32, sec_del)
        ok32c, rep32c = verify_manifest(m32, ref_dir, act_dir)
        if ok32c:
            sys.stderr.write(f"SELF-TEST FAIL: Case 32c (real Inter Sec ndary erasure) unexpectedly passed: {rep32c}\n")
            return 1

        # Case 33 (Astra P1-6): Switch midpoint track appearance independent gate
        # 33a: Red track RGB(205, 27, 32) with valid white thumb & start/end -> MUST FAIL
        sw_red_track = _draw_switch((205, 27, 32, 255), 16)
        act_33a = os.path.join(td, "act_33a")
        os.makedirs(act_33a)
        write_png_rgba(os.path.join(act_33a, "sw_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(act_33a, "sw_mid.png"), 48, 28, sw_red_track)
        write_png_rgba(os.path.join(act_33a, "sw_end.png"), 48, 28, sw_end)
        ok33a, rep33a = verify_manifest(m24_common, ref_dir, act_33a)
        if ok33a:
            sys.stderr.write(f"SELF-TEST FAIL: Case 33a (midpoint red track) unexpectedly passed: {rep33a}\n")
            return 1

        # 33b: Prematurely jumped to ON deep-blue track (t_color ~ 1.0) with valid mid thumb -> MUST FAIL
        sw_deep_blue = _draw_switch((4, 133, 247, 255), 16)
        act_33b = os.path.join(td, "act_33b")
        os.makedirs(act_33b)
        write_png_rgba(os.path.join(act_33b, "sw_start.png"), 48, 28, sw_start)
        write_png_rgba(os.path.join(act_33b, "sw_mid.png"), 48, 28, sw_deep_blue)
        write_png_rgba(os.path.join(act_33b, "sw_end.png"), 48, 28, sw_end)
        ok33b, rep33b = verify_manifest(m24_common, ref_dir, act_33b)
        if ok33b:
            sys.stderr.write(f"SELF-TEST FAIL: Case 33b (midpoint uninterpolated ON track) unexpectedly passed: {rep33b}\n")
            return 1

        # 33c: Valid interpolated mid track + valid mid thumb -> MUST PASS
        ok33c, rep33c = verify_manifest(m24_common, ref_dir, act_dir)
        if not ok33c:
            sys.stderr.write(f"SELF-TEST FAIL: Case 33c (valid interpolated midpoint) failed: {rep33c}\n")
            return 1

        # Case 34 (Opus R5 P1-2): Real 2x Inter "start/end" -> "start/edn" adjacent n/d swap -> MUST FAIL (>=0.70)
        _real_edn_ref_b64 = (
            "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"
        )
        _real_edn_act_b64 = (
            "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"
        )
        edn_ref_path = os.path.join(ref_dir, "real_inter_edn.png")
        edn_act_path = os.path.join(act_dir, "real_inter_edn.png")
        with open(edn_ref_path, "wb") as f:
            f.write(base64.b64decode(_real_edn_ref_b64))
        with open(edn_act_path, "wb") as f:
            f.write(base64.b64decode(_real_edn_act_b64))
        m34 = os.path.join(td, "m34.json")
        with open(m34, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "real_inter_edn", "file": "real_inter_edn.png", "width": 82, "height": 20, "dpr": 2,
                    "rois": [{"name": "rtl-alert-text-2", "kind": "text", "x": 0, "y": 0, "w": 82, "h": 20}],
                }]
            }, f)
        ok34, rep34 = verify_manifest(m34, ref_dir, act_dir)
        if ok34:
            sys.stderr.write(f"SELF-TEST FAIL: Case 34 (real Inter end->edn swap) unexpectedly passed: {rep34}\n")
            return 1

        # Case 35 (Opus R5 P1-2): min_seg_ncc_override requires reason, respects floor >= 0.45, text-only
        m35_noreason = os.path.join(td, "m35_noreason.json")
        with open(m35_noreason, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "real_inter_edn", "file": "real_inter_edn.png", "width": 82, "height": 20, "dpr": 2,
                    "rois": [{"name": "kbd", "kind": "text", "x": 0, "y": 0, "w": 82, "h": 20, "min_seg_ncc_override": 0.46}],
                }]
            }, f)
        ok35a, _ = verify_manifest(m35_noreason, ref_dir, act_dir)
        if ok35a:
            sys.stderr.write("SELF-TEST FAIL: Case 35a (min_seg_ncc_override missing reason) unexpectedly passed\n")
            return 1

        m35_floor = os.path.join(td, "m35_floor.json")
        with open(m35_floor, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "real_inter_edn", "file": "real_inter_edn.png", "width": 82, "height": 20, "dpr": 2,
                    "rois": [{
                        "name": "kbd", "kind": "text", "x": 0, "y": 0, "w": 82, "h": 20,
                        "min_seg_ncc_override": 0.40, "override_reason": "too low below 0.45 floor",
                    }],
                }]
            }, f)
        ok35b, _ = verify_manifest(m35_floor, ref_dir, act_dir)
        if ok35b:
            sys.stderr.write("SELF-TEST FAIL: Case 35b (min_seg_ncc_override < 0.45 floor) unexpectedly passed\n")
            return 1

        m35_valid = os.path.join(td, "m35_valid.json")
        with open(m35_valid, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "real_inter_sec_ov", "file": "real_inter_sec.png", "width": 77, "height": 18, "dpr": 2,
                    "rois": [{
                        "name": "kbd-vec", "kind": "text", "x": 0, "y": 0, "w": 77, "h": 18,
                        "min_seg_ncc_override": 0.46, "override_reason": "vector U+2318 glyph vs font glyph",
                    }],
                }]
            }, f)
        # Restore real_inter_sec.png actual to positive before validating override reporting
        with open(sec_act_path, "wb") as f:
            f.write(base64.b64decode(_real_sec_act_b64))
        ok35c, rep35c = verify_manifest(m35_valid, ref_dir, act_dir)
        if not ok35c or rep35c.get("override_count") != 1:
            sys.stderr.write(f"SELF-TEST FAIL: Case 35c (valid min_seg_ncc_override) failed: {rep35c}\n")
            return 1

    print(json.dumps({
        "self_test": "ok",
        "cases": [
            "astra_10x10_p95_16_rejected",
            "size_mismatch_1x1_vs_2x1_rejected",
            "missing_roi_rejected",
            "oob_roi_rejected",
            "identical_image_passed",
            "text_solid_black_vs_hollow_red_rejected",
            "text_wrong_weight_rejected",
            "invalid_dpr_3_rejected",
            "static_midpoint_rejected",
            "real_midpoint_passed",
            "required_manifest_missing_roi_rejected",
            "aa_override_capped_and_reported",
            "text_2px_vertical_shift_rejected_1x_and_2x",
            "required_solid_to_aa_downgrade_rejected",
            "text_ink_ratio_0_75_rejected",
            "required_16x16_solid_to_text_downgrade_rejected",
            "text_1_5px_vertical_shift_rejected_1x_and_2x",
            "erased_button_label_rejected",
            "button_fill_r_plus_20_rejected",
            "whole_filled_button_as_text_rejected",
            "full_box_text_mask_coverage_rejected",
            "midpoint_equals_end_rejected",
            "midpoint_missing_end_png_rejected",
            "mirrored_text_rejected",
            "swapped_copy_cpoy_rejected",
            "rtl_punctuation_reversed_rejected",
            "whole_switch_aa_override_rejected",
            "midpoint_end_replaced_by_start_rejected",
            "midpoint_swapped_start_end_rejected",
            "midpoint_wrong_direction_rejected",
            "tiny_1x_glyph_subpixel_conserved_and_weight_rejected",
            "real_inter_secondary_positive_passed",
            "real_inter_seocndary_swap_rejected",
            "real_inter_sec_ndary_erasure_rejected",
            "midpoint_red_track_rejected",
            "midpoint_uninterpolated_track_rejected",
            "midpoint_valid_interp_passed",
            "real_inter_end_to_edn_swap_rejected",
            "text_min_seg_ncc_override_validated",
        ],
    }))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Aura vs HeroUI pixel-level verifier")
    parser.add_argument("--self-test", action="store_true", help="Run built-in negative/positive self-tests")
    parser.add_argument("--manifest", help="Path to manifest JSON")
    parser.add_argument("--required-manifest", help="Optional base manifest JSON to verify scene & ROI coverage")
    parser.add_argument("--ref-dir", help="Directory containing HeroUI reference PNGs")
    parser.add_argument("--actual-dir", help="Directory containing Aura actual PNGs")
    parser.add_argument("--sbs-dir", help="Optional directory to emit 3-panel side-by-side PNGs")
    parser.add_argument("--only", help="Optional substring filter on scene id")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if not args.manifest or not args.ref_dir or not args.actual_dir:
        parser.error("--manifest, --ref-dir, and --actual-dir are required unless --self-test is given")

    ok, report = verify_manifest(
        args.manifest,
        args.ref_dir,
        args.actual_dir,
        sbs_dir=args.sbs_dir,
        only=args.only,
        required_manifest_path=args.required_manifest,
    )
    print(json.dumps(report, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
