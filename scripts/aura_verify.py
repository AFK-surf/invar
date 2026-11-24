#!/usr/bin/env python3
"""Strict pixel-level verification gate for packages/aura against HeroUI v3.2.8 ground truth.

Usage:
  python3 scripts/aura_verify.py --self-test
  python3 scripts/aura_verify.py --manifest ref_w0_manifest.json --ref-dir ../ref_suite --actual-dir out/shots [--required-manifest base.json] [--sbs-dir out/sbs] [--only <id>]
"""

from __future__ import annotations

import argparse
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
    bg_r = sorted(rgba[(cy * w + cx) * 4] for cx, cy in corners)[1]
    bg_g = sorted(rgba[(cy * w + cx) * 4 + 1] for cx, cy in corners)[1]
    bg_b = sorted(rgba[(cy * w + cx) * 4 + 2] for cx, cy in corners)[1]

    min_x = x1
    min_y = y1
    max_x = x0 - 1
    max_y = y0 - 1
    thresh = 24
    ink_pixels: list[tuple[int, int, int, int]] = []  # (dist, r, g, b)
    max_dist = 0
    sum_yd = 0.0
    sum_d = 0.0
    row_w: list[float] = [0.0] * (y1 - y0)
    for y in range(y0, y1):
        row = y * w * 4
        ry_rel = y - y0
        rw_acc = 0.0
        for x in range(x0, x1):
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
        row_w[ry_rel] = rw_acc
    if max_x < min_x or max_y < min_y or not ink_pixels or sum_d <= 0.0:
        return None

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
        "ink_rgb": ink_rgb,
    }


def _check_rect_covered_by_union(
    req: tuple[float, float, float, float],
    pieces: list[tuple[float, float, float, float]],
) -> bool:
    """Verify exact 2D coverage of rectangle `req`=(x0,y0,x1,y1) by the union of `pieces`."""
    rx0, ry0, rx1, ry1 = req
    if rx1 <= rx0 or ry1 <= ry0:
        return True
    clipped: list[tuple[float, float, float, float]] = []
    xs = {rx0, rx1}
    ys = {ry0, ry1}
    for px0, py0, px1, py1 in pieces:
        ix0 = max(rx0, px0)
        iy0 = max(ry0, py0)
        ix1 = min(rx1, px1)
        iy1 = min(ry1, py1)
        if ix1 > ix0 and iy1 > iy0:
            clipped.append((ix0, iy0, ix1, iy1))
            xs.add(ix0)
            xs.add(ix1)
            ys.add(iy0)
            ys.add(iy1)
    if not clipped:
        return False
    sx = sorted(xs)
    sy = sorted(ys)
    for i in range(len(sx) - 1):
        mx = 0.5 * (sx[i] + sx[i + 1])
        for j in range(len(sy) - 1):
            my = 0.5 * (sy[j] + sy[j + 1])
            covered = False
            for cx0, cy0, cx1, cy1 in clipped:
                if cx0 <= mx <= cx1 and cy0 <= my <= cy1:
                    covered = True
                    break
            if not covered:
                return False
    return True


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
                req_text_masks = [_parse_mask(m) for m in rroi.get("text_masks", [])]
                req_aa_masks = [_parse_mask(m) for m in rroi.get("aa_masks", [])]
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
            ink_mass_ok = 0.80 <= ink_ratio <= 1.25

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

            reasons: list[str] = []
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
                reasons.append(
                    f"ink mass ratio {ink_ratio:.3f} outside [0.80, 1.25] (ref={ref_cnt}, actual={act_cnt})"
                )
            res["ok"] = len(reasons) == 0
            if reasons:
                res["reason"] = "text ROI failed: " + "; ".join(reasons)
    elif kind == "midpoint":
        start_file = roi.get("start_file") or (scene.get("start_file") if scene else None)
        end_file = roi.get("end_file") or (scene.get("end_file") if scene else None)
        if not start_file or not end_file or not actual_dir:
            res["ok"] = False
            res["reason"] = "midpoint ROI requires 'start_file' and 'end_file' (resolved in actual_dir)"
        else:
            start_path = os.path.join(actual_dir, str(start_file))
            end_path = os.path.join(actual_dir, str(end_file))
            if not os.path.isfile(start_path) or not os.path.isfile(end_path):
                res["ok"] = False
                res["reason"] = f"midpoint ROI missing start/end PNG: start={start_path}, end={end_path}"
            else:
                sw, sh, start_rgba = read_png_rgba(start_path)
                ew, eh, end_rgba = read_png_rgba(end_path)
                if (sw, sh) != (w, h) or (ew, eh) != (w, h):
                    res["ok"] = False
                    res["reason"] = f"midpoint start/end size mismatch: start={sw}x{sh}, end={ew}x{eh}, mid={w}x{h}"
                else:
                    ds = _roi_deltas(w, start_rgba, act_rgba, rx, ry, rx + rw, ry + rh)
                    de = _roi_deltas(w, end_rgba, act_rgba, rx, ry, rx + rw, ry + rh)
                    max_vs_start = max(ds) if ds else 0
                    max_vs_end = max(de) if de else 0
                    res["max_delta_vs_start"] = max_vs_start
                    res["max_delta_vs_end"] = max_vs_end
                    if max_vs_start == 0 or max_vs_end == 0:
                        res["ok"] = False
                        res["reason"] = (
                            f"midpoint ROI is static (max_vs_start={max_vs_start}, max_vs_end={max_vs_end}); "
                            "midpoint frame must differ from both start and end frames"
                        )
                    else:
                        ok = (mean_delta <= 3.0) and (p95_delta <= 6.0)
                        res["ok"] = ok
                        if not ok:
                            res["reason"] = (
                                f"midpoint ROI vs ref failed: mean_delta={mean_delta:.2f} (<=3.0), "
                                f"p95={p95_delta:.1f} (<=6.0)"
                            )
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
            r = evaluate_roi(rw, rh, dpr, ref_rgba, act_rgba, roi, actual_dir=actual_dir, scene=sc)
            roi_results.append(r)
            if "override" in r:
                overrides_used.append({
                    "scene": sid,
                    "roi": r["name"],
                    "p95_delta": r["p95_delta"],
                    "max_p95_override": r["override"]["max_p95_override"],
                    "reason": r["override"]["reason"],
                })
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

        # Case 10: aa max_p95_override capped at 18.0 with required reason
        m10_ok = os.path.join(td, "m10_ok.json")
        with open(m10_ok, "w", encoding="utf-8") as f:
            json.dump({
                "scenes": [{
                    "id": "neg1", "file": "neg1.png", "width": 10, "height": 10, "dpr": 1,
                    "rois": [{
                        "name": "aa_ok", "kind": "aa", "x": 0, "y": 0, "w": 10, "h": 10,
                        "max_p95_override": 18.0,
                        "override_reason": "Skia cubic AA bias on pill arc",
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
