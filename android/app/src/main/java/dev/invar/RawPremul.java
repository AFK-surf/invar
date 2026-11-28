package dev.invar;

/**
 * Pure Java helpers for the MBRAWPRE premultiplied RGBA8888 binary wire format.
 * Kept free of Android SDK dependencies so it can be compiled and tested directly on any JVM.
 */
final class RawPremul {
    static final int HEADER_BYTES = 16;
    static final int MAX_DIM = 16384;

    private RawPremul() {}

    static boolean isRawPremul(byte[] data) {
        return data != null && data.length >= 8
                && data[0] == 'M' && data[1] == 'B' && data[2] == 'R' && data[3] == 'A'
                && data[4] == 'W' && data[5] == 'P' && data[6] == 'R' && data[7] == 'E';
    }

    static int[] parseRawPremulHeader(byte[] data) {
        if (!isRawPremul(data) || data.length < HEADER_BYTES) return null;
        long w = (data[8] & 0xFFL) | ((data[9] & 0xFFL) << 8)
                | ((data[10] & 0xFFL) << 16) | ((data[11] & 0xFFL) << 24);
        long h = (data[12] & 0xFFL) | ((data[13] & 0xFFL) << 8)
                | ((data[14] & 0xFFL) << 16) | ((data[15] & 0xFFL) << 24);
        if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) return null;
        if (w * h * 4L != (long) (data.length - HEADER_BYTES)) return null;
        return new int[]{(int) w, (int) h};
    }

    static int[] targetSize(int w, int h, int maxPx) {
        if (maxPx <= 0 || (w <= maxPx && h <= maxPx)) return new int[]{w, h};
        double k = (double) maxPx / (double) Math.max(w, h);
        int tw = Math.max(1, Math.min(maxPx, (int) Math.round(w * k)));
        int th = Math.max(1, Math.min(maxPx, (int) Math.round(h * k)));
        return new int[]{tw, th};
    }

    /**
     * Exact box / area averaging premultiplied RGBA8888 downscaler matching native raw_premul.h.
     * Prevents aliasing and vanishing 1px lines at >2x downscale ratios while clamping rgb <= a.
     */
    static byte[] downscale(byte[] src, int srcOffset, int sw, int sh, int dw, int dh) {
        byte[] out = new byte[dw * dh * 4];
        double sxScale = (double) sw / (double) dw;
        double syScale = (double) sh / (double) dh;
        if (dw <= sw && dh <= sh) {
            long sw64 = sw;
            long sh64 = sh;
            long dw64 = dw;
            long dh64 = dh;
            long totalW = sw64 * sh64;
            long den = totalW * 2L;
            for (int y = 0; y < dh; y++) {
                long sy0 = y * sh64;
                long sy1 = (y + 1) * sh64;
                int iy0 = (int) (sy0 / dh64);
                int iy1 = (int) ((sy1 - 1L) / dh64);
                int dstRow = y * dw * 4;
                for (int x = 0; x < dw; x++) {
                    long sx0 = x * sw64;
                    long sx1 = (x + 1) * sw64;
                    int ix0 = (int) (sx0 / dw64);
                    int ix1 = (int) ((sx1 - 1L) / dw64);
                    long wx0 = (ix0 == ix1) ? sw64 : ((ix0 + 1L) * dw64 - sx0);
                    long wx1 = (ix0 == ix1) ? 0L : (sx1 - ix1 * dw64);
                    long sr = 0L, sg = 0L, sb = 0L, sa = 0L;
                    for (int iy = iy0; iy <= iy1; iy++) {
                        long yLo = Math.max(iy * dh64, sy0);
                        long yHi = Math.min((iy + 1L) * dh64, sy1);
                        long wy = yHi - yLo;
                        int row = srcOffset + iy * sw * 4;
                        int p0 = row + ix0 * 4;
                        long rr = (src[p0] & 0xFFL) * wx0;
                        long rg = (src[p0 + 1] & 0xFFL) * wx0;
                        long rb = (src[p0 + 2] & 0xFFL) * wx0;
                        long ra = (src[p0 + 3] & 0xFFL) * wx0;
                        if (ix0 + 1 < ix1) {
                            int ir = 0, ig = 0, ib = 0, ia = 0;
                            for (int ix = ix0 + 1; ix < ix1; ix++) {
                                int p = row + ix * 4;
                                ir += src[p] & 0xFF;
                                ig += src[p + 1] & 0xFF;
                                ib += src[p + 2] & 0xFF;
                                ia += src[p + 3] & 0xFF;
                            }
                            rr += (long) ir * dw64;
                            rg += (long) ig * dw64;
                            rb += (long) ib * dw64;
                            ra += (long) ia * dw64;
                        }
                        if (ix1 > ix0) {
                            int p1 = row + ix1 * 4;
                            rr += (src[p1] & 0xFFL) * wx1;
                            rg += (src[p1 + 1] & 0xFFL) * wx1;
                            rb += (src[p1 + 2] & 0xFFL) * wx1;
                            ra += (src[p1 + 3] & 0xFFL) * wx1;
                        }
                        sr += rr * wy;
                        sg += rg * wy;
                        sb += rb * wy;
                        sa += ra * wy;
                    }
                    int a = (int) ((sa * 2L + totalW) / den);
                    if (a > 255) a = 255;
                    int r = (int) ((sr * 2L + totalW) / den);
                    int g = (int) ((sg * 2L + totalW) / den);
                    int b = (int) ((sb * 2L + totalW) / den);
                    if (r > a) r = a;
                    if (g > a) g = a;
                    if (b > a) b = a;
                    int dst = dstRow + x * 4;
                    out[dst] = (byte) r;
                    out[dst + 1] = (byte) g;
                    out[dst + 2] = (byte) b;
                    out[dst + 3] = (byte) a;
                }
            }
            return out;
        }
        for (int y = 0; y < dh; y++) {
            double fy = (y + 0.5) * syScale - 0.5;
            int y0 = fy < 0.0 ? 0 : Math.min(sh - 1, (int) Math.floor(fy));
            int y1 = Math.min(sh - 1, y0 + 1);
            double wy = (fy < 0.0 || fy >= sh - 1) ? 0.0 : (fy - y0);
            double wy0 = 1.0 - wy;
            int row0 = srcOffset + y0 * sw * 4;
            int row1 = srcOffset + y1 * sw * 4;
            int dstRow = y * dw * 4;
            for (int x = 0; x < dw; x++) {
                double fx = (x + 0.5) * sxScale - 0.5;
                int x0 = fx < 0.0 ? 0 : Math.min(sw - 1, (int) Math.floor(fx));
                int x1 = Math.min(sw - 1, x0 + 1);
                double wx = (fx < 0.0 || fx >= sw - 1) ? 0.0 : (fx - x0);
                double wx0 = 1.0 - wx;
                int p00 = row0 + x0 * 4, p10 = row0 + x1 * 4;
                int p01 = row1 + x0 * 4, p11 = row1 + x1 * 4;
                double w00 = wx0 * wy0, w10 = wx * wy0, w01 = wx0 * wy, w11 = wx * wy;
                int a = (int) Math.round((src[p00 + 3] & 0xFF) * w00 + (src[p10 + 3] & 0xFF) * w10
                        + (src[p01 + 3] & 0xFF) * w01 + (src[p11 + 3] & 0xFF) * w11);
                if (a < 0) a = 0;
                else if (a > 255) a = 255;
                int dst = dstRow + x * 4;
                for (int c = 0; c < 3; c++) {
                    int v = (int) Math.round((src[p00 + c] & 0xFF) * w00 + (src[p10 + c] & 0xFF) * w10
                            + (src[p01 + c] & 0xFF) * w01 + (src[p11 + c] & 0xFF) * w11);
                    if (v < 0) v = 0;
                    else if (v > a) v = a;
                    out[dst + c] = (byte) v;
                }
                out[dst + 3] = (byte) a;
            }
        }
        return out;
    }
}
