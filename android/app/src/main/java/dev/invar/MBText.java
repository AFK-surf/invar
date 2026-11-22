package dev.invar;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.FontFamily;
import android.graphics.fonts.FontStyle;
import android.icu.text.Bidi;
import android.icu.text.BreakIterator;
import android.opengl.GLES30;
import android.opengl.GLUtils;
import android.os.Build;
import android.text.Layout;
import android.text.StaticLayout;
import android.text.TextPaint;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Android text shaping (Paint.getTextRunAdvances + ICU BreakIterator/Bidi)
 * and glyph rasterization into GLES3 alpha/colour texture atlases with
 * background pre-rasterization for sub-pixel phases.
 */
final class MBText {
    static final class Entry {
        final String text;
        final float size;
        final int weight;
        final int style;
        final boolean mayColour;
        volatile GlyphBounds bounds;
        int want, drawing, have, first, skip;
        boolean inFirst, inRest, listed;
        final Drawn[] drawn = new Drawn[SUB_COUNT];
        float askScale;
        int askFaces;
        int subsQueued;
        float queuedScale;
        int queuedFaces;
        int queuedGen;

        Entry(String text, float size, int weight, int style) {
            this.text = text;
            this.size = size;
            this.weight = weight;
            this.style = style;
            this.mayColour = mayBeColour(text);
        }
    }

    static final class GlyphBounds {
        final Rect rect = new Rect();
        final float scale;
        final int faces;

        GlyphBounds(Entry e, float scale, int faces, Paint p) {
            this.scale = scale;
            this.faces = faces;
            p.getTextBounds(e.text, 0, e.text.length(), rect);
        }
    }

    static final class Drawn {
        Bitmap bmp;
        byte[] bytes;
        int w, h;
        int x0, y0;
        int color;
        float scale;
        int faces;

        void release() {
            if (bmp != null) bmp.recycle();
            bmp = null;
            bytes = null;
        }
    }

    static final int SUB_COUNT = 4, SUB_ALL = 0x0F;

    static float subDx(int sub) {
        switch (sub) {
            case 1: return 0.25f;
            case 2: return -0.5f;
            case 3: return -0.25f;
            default: return 0f;
        }
    }

    static int glyphSub(int fullKey) {
        int sub = fullKey >= 0 ? (fullKey >>> 26) & 7 : 0;
        return sub < SUB_COUNT ? sub : 0;
    }

    static final class Placed {
        final Bitmap bmp;
        byte[] bytes;
        final int w, h, x, y;
        final boolean colour;

        Placed(byte[] bytes, Bitmap bmp, int w, int h, int x, int y, boolean colour) {
            this.bytes = bytes;
            this.bmp = bmp;
            this.w = w;
            this.h = h;
            this.x = x;
            this.y = y;
            this.colour = colour;
        }

        void release() {
            if (bmp != null) bmp.recycle();
            bytes = null;
        }
    }

    static final int COLOUR_U = 16384;

    static final class Clusters {
        final HashMap<String, Integer> map = new HashMap<>();
        final int[][] units = new int[256][];
        final float[][] unitAdv = new float[256][];
        float advScale;
        int advFaces;
    }

    static final HashMap<Long, Clusters> keys = new HashMap<>();
    static final ArrayList<Entry> entries = new ArrayList<>();
    static final HashMap<Long, Paint> paints = new HashMap<>();
    static final ArrayList<Placed> placed = new ArrayList<>();
    static int generation;
    static int heldKey = -1;
    static Bitmap held;
    static byte[] heldBytes;
    static int heldW, heldH;
    static boolean heldColour;
    static volatile int faces;
    static float[] adv = new float[1024];
    static char[] chars = new char[1024];
    static final BreakIterator graphemes = BreakIterator.getCharacterInstance();

    static final class Scratch {
        static final int ALPHA_SIZE = 96;
        final Canvas canvas = new Canvas();
        int[] px = new int[1024];
        Bitmap alphaBmp;
        ByteBuffer alphaBuf;
        int alphaStride;
        Paint clearPaint;

        void initAlpha() {
            Bitmap bmp = Bitmap.createBitmap(ALPHA_SIZE, ALPHA_SIZE, Bitmap.Config.ALPHA_8);
            alphaStride = bmp.getRowBytes();
            alphaBuf = ByteBuffer.allocate(alphaStride * ALPHA_SIZE);
            clearPaint = new Paint();
            clearPaint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.CLEAR));
            alphaBmp = bmp;
        }
    }

    static final Scratch mine = new Scratch();

    static void reset() {
        generation++;
        dropHeld();
        dropPlaced();
    }

    static long fontKey(float size, int weight, int style) {
        return ((long) Float.floatToIntBits(size) << 32) | ((long) (weight & 0xFFFF) << 16) | (style & 0xFFFF);
    }

    static Typeface face(int style) {
        return (style & 2) != 0 ? Typeface.MONOSPACE : (style & 4) != 0 ? Typeface.SERIF : Typeface.DEFAULT;
    }

    static final int FACES = 16;
    @SuppressWarnings("unchecked")
    static final ArrayList<Object>[] faceFiles = new ArrayList[FACES];
    @SuppressWarnings("unchecked")
    static final ArrayList<Integer>[] faceWeights = new ArrayList[FACES];

    static int faceId(int style) {
        int f = (style >> 8) & 15;
        if (f > 0) return given(f) ? f : 0;
        if ((style & 2) != 0) return given(3) ? 3 : 0;
        if ((style & 16) != 0 && given(2)) return 2;
        if ((style & 4) != 0) return 0;
        return given(1) ? 1 : 0;
    }

    static boolean given(int f) {
        return faceFiles[f] != null && !faceFiles[f].isEmpty();
    }

    static Font font(Object src, int weight, int fileWeight, boolean italic) throws IOException {
        Font.Builder b = src instanceof File
                ? new Font.Builder((File) src)
                : new Font.Builder(((ByteBuffer) src).duplicate());
        if (fileWeight == 0) b.setFontVariationSettings("'wght' " + weight);
        return b.setWeight(fileWeight == 0 ? weight : fileWeight)
                .setSlant(italic ? FontStyle.FONT_SLANT_ITALIC : FontStyle.FONT_SLANT_UPRIGHT)
                .build();
    }

    static Typeface faceTypeface(int f, int weight, boolean italic) {
        if (Build.VERSION.SDK_INT < 29) return null;
        int best = 0, bestd = Integer.MAX_VALUE;
        for (int i = 0; i < faceWeights[f].size(); i++) {
            int w = faceWeights[f].get(i);
            int d = w == 0 ? 0 : Math.abs(w - weight);
            if (d < bestd) {
                bestd = d;
                best = i;
            }
        }
        try {
            Font font = font(faceFiles[f].get(best), weight, faceWeights[f].get(best), italic);
            FontFamily fam = new FontFamily.Builder(font).build();
            return new Typeface.CustomFallbackBuilder(fam)
                    .setStyle(new FontStyle(weight, italic ? FontStyle.FONT_SLANT_ITALIC : FontStyle.FONT_SLANT_UPRIGHT))
                    .setSystemFallback((f == 3) ? "monospace" : "sans-serif")
                    .build();
        } catch (Exception e) {
            return null;
        }
    }

    static final int TYPEFACE_STYLE = 0xF17;

    static long typefaceKey(int facesGen, int style, int weight) {
        return ((long) facesGen << 32) | ((long) (style & TYPEFACE_STYLE) << 16) | (weight & 0xFFFF);
    }

    static final ConcurrentHashMap<Long, Typeface> typefaces = new ConcurrentHashMap<>();

    static Typeface typeface(int style, int weight) {
        Typeface t = typefaces.get(typefaceKey(faces, style, weight));
        return t != null ? t : typefaceMade(style, weight);
    }

    static synchronized Typeface typefaceMade(int style, int weight) {
        long k = typefaceKey(faces, style, weight);
        Typeface t = typefaces.get(k);
        if (t != null) return t;
        int f = faceId(style);
        Typeface given = f > 0 ? faceTypeface(f, weight, (style & 1) != 0) : null;
        t = given != null ? given : Typeface.create(face(style), weight, (style & 1) != 0);
        typefaces.put(k, t);
        return t;
    }

    static synchronized boolean take(int f, Object src, int weight) {
        if (f <= 0 || f >= FACES || Build.VERSION.SDK_INT < 29) return false;
        try {
            font(src, weight == 0 ? 400 : weight, weight, false);
        } catch (Exception e) {
            return false;
        }
        if (faceFiles[f] == null) {
            faceFiles[f] = new ArrayList<>();
            faceWeights[f] = new ArrayList<>();
        }
        faceFiles[f].add(src);
        faceWeights[f].add(weight);
        facesChanged();
        return true;
    }

    static boolean faceAdd(int f, String path, int weight) {
        return take(f, new File(path), weight);
    }

    static boolean faceAddBytes(int f, byte[] data, int weight) {
        ByteBuffer b = ByteBuffer.allocateDirect(data.length);
        b.put(data);
        b.flip();
        return take(f, b, weight);
    }

    static synchronized void faceClear(int f) {
        if (f <= 0 || f >= FACES || !given(f)) return;
        faceFiles[f].clear();
        faceWeights[f].clear();
        facesChanged();
    }

    static void facesChanged() {
        paints.clear();
        typefaces.clear();
        faces++;
        reset();
        keysStale = true;
    }

    static Paint paint(float size, int weight, int style) {
        return paintIn(paints, size, weight, style);
    }

    static Paint paintIn(HashMap<Long, Paint> paints, float size, int weight, int style) {
        long k = fontKey(size, weight, style);
        Paint p = paints.get(k);
        if (p == null) {
            p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
            p.setTypeface(typeface(style, weight));
            p.setTextSize(size);
            StringBuilder fs = new StringBuilder();
            if ((style & 8) != 0) fs.append(", 'liga' 0, 'clig' 0");
            if ((style & 32) != 0) fs.append(", 'tnum' 1");
            if ((style & 64) != 0) fs.append(", 'pwid' 1");
            String features = fs.length() > 0 ? fs.substring(2) : "";
            if (!features.isEmpty()) p.setFontFeatureSettings(features);
            p.setColor(Color.WHITE);
            paints.put(k, p);
        }
        return p;
    }

    static int atlasGeneration() {
        keysSweep();
        return generation;
    }

    static final int KEYS_MOST = 65536;
    static boolean keysStale;

    static void keysSweep() {
        if (!keysStale && entries.size() <= KEYS_MOST) return;
        keysStale = false;
        synchronized (lock) {
            int drawing = 0;
            for (Entry e : entries) {
                if (e.drawing != 0) {
                    e.skip |= e.drawing;
                    drawing++;
                }
                dropDrawn(e);
                e.want = 0;
                e.first = 0;
                e.inFirst = false;
                e.inRest = false;
                e.listed = false;
            }
            askedFirst.clear();
            askedRest.clear();
            drawnAhead.clear();
            aheadCount = drawing;
        }
        keys.clear();
        entries.clear();
        entries.trimToSize();
        dropHeld();
        generation++;
    }

    static float density() {
        return scale > 0 ? scale : Host.view != null ? Host.view.density : 1;
    }

    static float scale;

    static void setScale(float s) {
        scale = s;
        reset();
    }

    private static final String HAN = "\u56fd";

    static void fontMetrics(double size, int weight, int style, double[] out) {
        float d = density();
        Paint p = paint((float) size * d, weight, style);
        Paint.FontMetricsInt m = p.getFontMetricsInt();
        out[0] = -m.ascent / d;
        out[1] = m.descent / d;
        out[2] = Math.round(size * 1.42);
        if (Build.VERSION.SDK_INT >= 34) {
            Paint.FontMetricsInt c = new Paint.FontMetricsInt();
            p.getFontMetricsInt(HAN, 0, HAN.length(), 0, HAN.length(), false, c);
            out[3] = -c.ascent / d;
            out[4] = c.descent / d;
        } else {
            StaticLayout l = StaticLayout.Builder.obtain(HAN, 0, HAN.length(), new TextPaint(p), 1 << 20)
                    .setIncludePad(false).setUseLineSpacingFromFallbacks(true).build();
            out[3] = -l.getLineAscent(0) / d;
            out[4] = l.getLineDescent(0) / d;
        }
        out[5] = 1;
    }

    static int shape(String text, double size, int weight, int style, double[] out) {
        float d = density();
        Paint p = paint((float) size * d, weight, style);
        int n = text.length();
        if (adv.length < n) adv = new float[n * 2];
        if (MBText.chars.length < n) MBText.chars = new char[n * 2];
        char[] chars = MBText.chars;
        text.getChars(0, n, chars, 0);
        Clusters font = clusters((float) size, weight, style);
        boolean units = unitClusters(chars, n);
        if (!units && Bidi.requiresBidi(chars, 0, n)) {
            Bidi b = new Bidi(chars, 0, null, 0, n, Bidi.DIRECTION_DEFAULT_LEFT_TO_RIGHT);
            for (int r = 0; r < b.getRunCount(); r++) {
                int rs = b.getRunStart(r), rl = b.getRunLimit(r) - rs;
                p.getTextRunAdvances(chars, rs, rl, rs, rl, (b.getRunLevel(r) & 1) != 0, adv, rs);
            }
        } else if (units) {
            measureUnits(p, font, chars, n, d);
        } else {
            p.getTextRunAdvances(chars, 0, n, 0, n, false, adv, 0);
        }
        float x = 0;
        for (int i = 0; i < n; i++) {
            if (chars[i] == '\t') adv[i] = (float) Math.floor((x + 20) / 20) * 20 - x;
            x += adv[i];
        }
        for (int i = 0; i < n; i++) adv[i] /= d;
        if (!units) graphemes.setText(text);
        int cap = out.length / 4;
        int k = 0;
        int start = units ? 0 : graphemes.first();
        for (int end = units ? (n > 0 ? 1 : BreakIterator.DONE) : graphemes.next();
                end != BreakIterator.DONE && k < cap;
                start = end, end = units ? (end < n ? end + 1 : BreakIterator.DONE) : graphemes.next()) {
            double a = 0;
            for (int i = start; i < end; i++) a += adv[i];
            char c = chars[start];
            int key;
            if (c == '\n') {
                a = 0;
                key = -1;
            } else if (c == ' ' || c == '\t') {
                key = -1;
            } else if (a == 0 && k > 0 && out[(k - 1) * 4 + 3] != -1) {
                key = -2;
            } else if (end - start == 1) {
                key = unitKey(font, c, (float) size, weight, style);
            } else {
                key = intern(font.map, text.substring(start, end), (float) size, weight, style);
            }
            out[k * 4] = start;
            out[k * 4 + 1] = end - start;
            out[k * 4 + 2] = a;
            out[k * 4 + 3] = key;
            k++;
        }
        return k;
    }

    static boolean isHan(char c) {
        return c >= 0x3400 && c <= 0x9FFF;
    }

    static boolean isolatedHan(char[] text, int i, int n) {
        if (!isHan(text[i])) return false;
        if (i + 1 == n) return true;
        char next = text[i + 1];
        return isHan(next) || next == ' ' || next == 0x3000;
    }

    static void measureUnits(Paint p, Clusters font, char[] text, int n, float scale) {
        if (font.advScale != scale || font.advFaces != faces) {
            Arrays.fill(font.unitAdv, null);
            font.advScale = scale;
            font.advFaces = faces;
        }
        int start = 0;
        for (int i = 0; i < n; i++) {
            if (!isolatedHan(text, i, n)) continue;
            char c = text[i];
            float[] page = font.unitAdv[c >>> 8];
            if (page == null || Float.isNaN(page[c & 255])) continue;
            measureAndRemember(p, font, text, start, i, n);
            adv[i] = page[c & 255];
            start = i + 1;
        }
        measureAndRemember(p, font, text, start, n, n);
    }

    static void measureAndRemember(Paint p, Clusters font, char[] text, int start, int end, int n) {
        int count = end - start;
        if (count == 0) return;
        p.getTextRunAdvances(text, start, count, start, count, false, adv, start);
        for (int i = start; i < end; i++) {
            if (!isolatedHan(text, i, n)) continue;
            char c = text[i];
            int index = c >>> 8;
            float[] page = font.unitAdv[index];
            if (page == null) {
                page = new float[256];
                Arrays.fill(page, Float.NaN);
                font.unitAdv[index] = page;
            }
            page[c & 255] = adv[i];
        }
    }

    static boolean unitClusters(char[] chars, int n) {
        for (int i = 0; i < n; i++) {
            char c = chars[i];
            if (c >= 0x20 && c <= 0x7E) continue;
            if (c < 0x3000) {
                if (c == '\n' || c == '\t'
                        || c == 0x00A5 || c == 0x00A7 || c == 0x00B0 || c == 0x00B1
                        || c == 0x00B7 || c == 0x00D7 || c == 0x00F7
                        || c == 0x2013 || c == 0x2014 || (c >= 0x2018 && c <= 0x201D)
                        || c == 0x2022 || c == 0x2026 || c == 0x2030
                        || c == 0x2032 || c == 0x2033 || c == 0x20AC || c == 0x2103) {
                    continue;
                }
                return false;
            }
            if (c <= 0x3029 || (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FF)
                    || isHan(c) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xFF01 && c <= 0xFF60)) {
                continue;
            }
            return false;
        }
        return true;
    }

    static Clusters clusters(float size, int weight, int style) {
        long k = fontKey(size, weight, style);
        Clusters m = keys.get(k);
        if (m == null) {
            m = new Clusters();
            keys.put(k, m);
        }
        return m;
    }

    static int unitKey(Clusters font, char c, float size, int weight, int style) {
        int[] page = font.units[c >>> 8];
        if (page == null) {
            page = new int[256];
            Arrays.fill(page, -1);
            font.units[c >>> 8] = page;
        }
        int key = page[c & 255];
        if (key < 0) {
            key = intern(font.map, String.valueOf(c), size, weight, style);
            page[c & 255] = key;
        }
        return key;
    }

    static int intern(HashMap<String, Integer> font, String cluster, float size, int weight, int style) {
        Integer id = font.get(cluster);
        if (id != null) return id;
        entries.add(new Entry(cluster, size, weight, style));
        font.put(cluster, entries.size() - 1);
        return entries.size() - 1;
    }

    static int textKey(String text, double size, int weight, int style) {
        return intern(clusters((float) size, weight, style).map, text, (float) size, weight, style);
    }

    static boolean isColor(Bitmap b, Scratch sc) {
        int w = b.getWidth(), h = b.getHeight(), n = w * h;
        if (sc.px.length < n) sc.px = new int[Math.max(n, sc.px.length * 2)];
        int[] px = sc.px;
        b.getPixels(px, 0, w, 0, 0, w, h);
        for (int i = 0; i < n; i++) {
            int v = px[i];
            int a = v >>> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, bl = v & 255;
            if (a > 64 && Math.min(r, Math.min(g, bl)) < 224) return true;
        }
        return false;
    }

    static void glyph(int fullKey, double[] out) {
        for (int i = 0; i < 7; i++) out[i] = 0;
        dropHeld();
        int key = fullKey >= 0 ? fullKey & 0x03FFFFFF : fullKey;
        int sub = glyphSub(fullKey);
        if (key < 0 || key >= entries.size()) return;
        Entry e = entries.get(key);
        float sc = density();
        Drawn d = drawnAhead(e, sub, sc);
        if (d == null) d = draw(e, sc, subDx(sub), paint(e.size * sc, e.weight, e.style), faces, mine);
        if (e.queuedScale != sc || e.queuedFaces != faces || e.queuedGen != generation) {
            e.subsQueued = 0;
            e.queuedScale = sc;
            e.queuedFaces = faces;
            e.queuedGen = generation;
        }
        int bit = 1 << sub;
        boolean moved = e.subsQueued != 0 && (e.subsQueued & bit) == 0;
        e.subsQueued |= bit;
        if (moved) {
            int rest = SUB_ALL & ~e.subsQueued;
            if (rest != 0) aheadOne(e, rest, sc);
        }
        if (d.bytes == null && d.bmp == null) return;
        heldKey = fullKey;
        held = d.bmp;
        heldBytes = d.bytes;
        heldW = d.w;
        heldH = d.h;
        heldColour = d.color != 0;
        out[2] = d.w / sc;
        out[3] = d.h / sc;
        out[4] = d.x0 / sc;
        out[5] = d.y0 / sc;
        out[6] = d.color;
    }

    static void glyphPut(int key, int x, int y) {
        boolean colour = x >= COLOUR_U;
        if (key != heldKey || (heldBytes == null && held == null) || colour != heldColour) return;
        placed.add(new Placed(heldBytes, held, heldW, heldH, colour ? x - COLOUR_U : x, y, colour));
        held = null;
        heldBytes = null;
        heldKey = -1;
    }

    static void dropHeld() {
        if (held != null) held.recycle();
        held = null;
        heldBytes = null;
        heldKey = -1;
    }

    static Drawn draw(Entry e, float sc, float subDx, Paint p, int faces, Scratch scratch) {
        Drawn d = new Drawn();
        d.scale = sc;
        d.faces = faces;
        GlyphBounds bounds = e.bounds;
        if (bounds == null || bounds.scale != sc || bounds.faces != faces) {
            bounds = new GlyphBounds(e, sc, faces, p);
            e.bounds = bounds;
        }
        Rect b = bounds.rect;
        int pad = 1;
        if (b.width() <= 0 || b.height() <= 0) return d;
        int x0 = (int) Math.floor(b.left + subDx) - pad, x1 = (int) Math.ceil(b.right + subDx) + pad;
        int pw = x1 - x0, ph = b.height() + 2 * pad;
        if (pw > 512 || ph > 512) return d;
        boolean reuseAlpha = !e.mayColour && pw <= Scratch.ALPHA_SIZE && ph <= Scratch.ALPHA_SIZE;
        Bitmap bmp;
        if (reuseAlpha) {
            if (scratch.alphaBmp == null) scratch.initAlpha();
            bmp = scratch.alphaBmp;
        } else {
            bmp = Bitmap.createBitmap(pw, ph, e.mayColour ? Bitmap.Config.ARGB_8888 : Bitmap.Config.ALPHA_8);
        }
        Canvas c = scratch.canvas;
        c.setBitmap(bmp);
        try {
            if (reuseAlpha) c.drawRect(0, 0, pw, ph, scratch.clearPaint);
            c.drawText(e.text, -x0 + subDx, pad - b.top, p);
        } finally {
            c.setBitmap(null);
        }
        if (reuseAlpha) {
            ByteBuffer pixels = scratch.alphaBuf;
            pixels.clear();
            bmp.copyPixelsToBuffer(pixels);
            byte[] src = pixels.array();
            d.bytes = new byte[pw * ph];
            for (int y = 0; y < ph; y++) System.arraycopy(src, y * scratch.alphaStride, d.bytes, y * pw, pw);
        } else {
            d.color = e.mayColour && isColor(bmp, scratch) ? 1 : 0;
            d.bmp = bmp;
        }
        d.w = pw;
        d.h = ph;
        d.x0 = x0;
        d.y0 = b.top - pad;
        return d;
    }

    static boolean mayBeColour(String s) {
        for (int i = 0, n = s.length(); i < n; i++) {
            char c = s.charAt(i);
            boolean plain = (c >= 0x20 && c <= 0x7E)
                    || (c >= 0xA0 && c <= 0x24F && c != 0xA9 && c != 0xAE)
                    || (c >= 0x300 && c <= 0x4FF)
                    || (c >= 0x2010 && c <= 0x2027)
                    || (c >= 0x2030 && c <= 0x2033)
                    || c == 0x20AC
                    || c == 0x2103
                    || (c >= 0x3000 && c <= 0x30FF && c != 0x3030 && c != 0x303D)
                    || (c >= 0x3400 && c <= 0x4DBF)
                    || (c >= 0x4E00 && c <= 0x9FFF)
                    || (c >= 0xAC00 && c <= 0xD7A3)
                    || (c >= 0xFF01 && c <= 0xFF5E);
            if (!plain) return true;
        }
        return false;
    }

    static void flush() {
        upload(placed);
    }

    static void upload(ArrayList<Placed> placed) {
        int n = placed.size();
        if (n == 0) return;
        int bound = -1;
        int i = 0;
        while (i < n) {
            Placed first = placed.get(i);
            int j = i, right = first.x - 1, h = 0;
            while (j < n && placed.get(j).colour == first.colour && placed.get(j).y == first.y
                    && placed.get(j).x == right + 1) {
                Placed g = placed.get(j);
                right = g.x + g.w;
                h = Math.max(h, g.h);
                j++;
            }
            int tex = first.colour ? Native.colourAtlasTexture() : Native.atlasTexture();
            if (tex != bound) {
                GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, tex);
                bound = tex;
            }
            int w = right - first.x;
            if (first.colour) {
                if (j - i == 1) {
                    GLUtils.texSubImage2D(GLES30.GL_TEXTURE_2D, 0, first.x, first.y, first.bmp);
                } else {
                    Bitmap row = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    Canvas c = new Canvas(row);
                    for (int k = i; k < j; k++) {
                        Placed g = placed.get(k);
                        c.drawBitmap(g.bmp, g.x - first.x, 0, null);
                    }
                    GLUtils.texSubImage2D(GLES30.GL_TEXTURE_2D, 0, first.x, first.y, row);
                    row.recycle();
                }
            } else {
                GLES30.glPixelStorei(GLES30.GL_UNPACK_ALIGNMENT, 1);
                if (j - i == 1 && first.bytes != null) {
                    ByteBuffer one = glyphBuffer(first.bytes.length);
                    one.put(first.bytes);
                    one.flip();
                    GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, first.x, first.y, first.w, first.h,
                            GLES30.GL_RED, GLES30.GL_UNSIGNED_BYTE, one);
                } else if (j - i == 1 && first.bmp.getConfig() == Bitmap.Config.ALPHA_8
                        && first.bmp.getRowBytes() == first.w) {
                    ByteBuffer one = coverOf(first.bmp);
                    GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, first.x, first.y, first.w, first.h,
                            GLES30.GL_RED, GLES30.GL_UNSIGNED_BYTE, one);
                } else {
                    if (cover == null || cover.capacity() < w * h) {
                        cover = ByteBuffer.allocateDirect(Math.max(w * h, 1 << 16)).order(ByteOrder.nativeOrder());
                    }
                    cover.clear();
                    for (int b = 0; b < w * h; b += ZEROS.length) {
                        cover.position(b);
                        cover.put(ZEROS, 0, Math.min(ZEROS.length, w * h - b));
                    }
                    for (int k = i; k < j; k++) {
                        Placed g = placed.get(k);
                        int gw = g.w, gh = g.h;
                        int ox = g.x - first.x;
                        if (g.bytes != null) {
                            for (int y = 0; y < gh; y++) {
                                cover.position(y * w + ox);
                                cover.put(g.bytes, y * gw, gw);
                            }
                        } else if (g.bmp.getConfig() == Bitmap.Config.ALPHA_8) {
                            ByteBuffer src = coverOf(g.bmp);
                            int rb = g.bmp.getRowBytes();
                            for (int y = 0; y < gh; y++) {
                                src.limit(y * rb + gw);
                                src.position(y * rb);
                                cover.position(y * w + ox);
                                cover.put(src);
                            }
                        } else {
                            if (px.length < gw * gh) px = new int[gw * gh * 2];
                            g.bmp.getPixels(px, 0, gw, 0, 0, gw, gh);
                            for (int y = 0; y < gh; y++)
                                for (int x = 0; x < gw; x++) cover.put(y * w + ox + x, (byte) (px[y * gw + x] >>> 24));
                        }
                    }
                    cover.position(0);
                    GLES30.glTexSubImage2D(GLES30.GL_TEXTURE_2D, 0, first.x, first.y, w, h, GLES30.GL_RED,
                            GLES30.GL_UNSIGNED_BYTE, cover);
                }
                GLES30.glPixelStorei(GLES30.GL_UNPACK_ALIGNMENT, 4);
            }
            for (int k = i; k < j; k++) placed.get(k).release();
            i = j;
        }
        placed.clear();
    }

    static int[] px = new int[4096];
    static ByteBuffer glyphBytes;
    static ByteBuffer cover;
    static final byte[] ZEROS = new byte[4096];

    static ByteBuffer glyphBuffer(int n) {
        if (glyphBytes == null || glyphBytes.capacity() < n) {
            glyphBytes = ByteBuffer.allocateDirect(Math.max(n, 1 << 16)).order(ByteOrder.nativeOrder());
        }
        glyphBytes.clear();
        return glyphBytes;
    }

    static ByteBuffer coverOf(Bitmap bmp) {
        ByteBuffer bytes = glyphBuffer(bmp.getRowBytes() * bmp.getHeight());
        bmp.copyPixelsToBuffer(bytes);
        bytes.flip();
        return bytes;
    }

    static void dropPlaced() {
        for (Placed g : placed) g.release();
        placed.clear();
    }

    // ---- Background glyph pre-rasterization ----

    static final int AHEAD_MOST = 1024;
    static final Object lock = new Object();
    static final ArrayDeque<Entry> askedFirst = new ArrayDeque<>();
    static final ArrayDeque<Entry> askedRest = new ArrayDeque<>();
    static final LinkedHashSet<Entry> drawnAhead = new LinkedHashSet<>();
    static int aheadCount;
    static Thread[] aheadThreads;

    static int aheadThreadCount() {
        return Math.min(2, Math.max(1, Runtime.getRuntime().availableProcessors() / 3));
    }

    static boolean asked(Entry e) {
        return (e.want | e.drawing | e.have) != 0;
    }

    static void ahead(int[] ks, int n) {
        float sc = density();
        boolean any = false;
        synchronized (lock) {
            for (int i = 0; i < n; i++) {
                int k = ks[i] >= 0 ? ks[i] & 0x03FFFFFF : -1;
                if (k < 0 || k >= entries.size()) continue;
                Entry e = entries.get(k);
                int bit = 1 << glyphSub(ks[i]);
                if (e.queuedScale != sc || e.queuedFaces != faces || e.queuedGen != generation) {
                    e.subsQueued = 0;
                    e.queuedScale = sc;
                    e.queuedFaces = faces;
                    e.queuedGen = generation;
                }
                boolean moved = e.subsQueued != 0 && (e.subsQueued & bit) == 0;
                int want = moved ? (SUB_ALL & ~e.subsQueued) : (bit & ~e.subsQueued);
                if (asked(e)) {
                    if (want != 0 && e.askScale == sc && e.askFaces == faces) {
                        int add = want & ~(e.want | e.drawing | e.have);
                        if (add != 0) {
                            e.want |= add;
                            e.subsQueued |= add;
                            int addFirst = add & bit;
                            if (addFirst != 0) {
                                e.first |= addFirst;
                                if (!e.inFirst) {
                                    e.inFirst = true;
                                    askedFirst.add(e);
                                }
                            }
                            if ((add & ~addFirst) != 0 && !e.inRest) {
                                e.inRest = true;
                                askedRest.add(e);
                            }
                            any = true;
                        }
                    } else if ((e.want & bit) != 0 && (e.first & bit) == 0) {
                        e.first |= bit;
                        if (!e.inFirst) {
                            e.inFirst = true;
                            askedFirst.add(e);
                        }
                        any = true;
                    }
                    continue;
                }
                if (want == 0) continue;
                if (!roomAhead()) break;
                ask(e, want, want & bit, sc);
                any = true;
            }
            if (any) lock.notifyAll();
        }
        if (any) startAhead();
    }

    static void ask(Entry e, int want, int first, float sc) {
        e.want = want;
        e.first = first;
        e.askScale = sc;
        e.askFaces = faces;
        e.subsQueued |= want;
        if (first != 0 && !e.inFirst) {
            e.inFirst = true;
            askedFirst.add(e);
        }
        if ((want & ~first) != 0 && !e.inRest) {
            e.inRest = true;
            askedRest.add(e);
        }
        aheadCount++;
    }

    static boolean roomAhead() {
        while (aheadCount >= AHEAD_MOST) {
            if (!dropOldestDrawn()) return false;
        }
        return true;
    }

    static void aheadOne(Entry e, int want, float sc) {
        boolean any = false;
        synchronized (lock) {
            if (asked(e)) {
                if (e.askScale == sc && e.askFaces == faces) {
                    int add = want & ~(e.want | e.drawing | e.have);
                    if (add != 0) {
                        e.want |= add;
                        e.subsQueued |= add;
                        if (!e.inRest) {
                            e.inRest = true;
                            askedRest.add(e);
                        }
                        lock.notify();
                        any = true;
                    }
                }
            } else if (roomAhead()) {
                ask(e, want, 0, sc);
                lock.notify();
                any = true;
            }
        }
        if (any) startAhead();
    }

    static void startAhead() {
        if (aheadThreads != null) return;
        aheadThreads = new Thread[aheadThreadCount()];
        for (int i = 0; i < aheadThreads.length; i++) {
            Thread t = new Thread(MBText::drawAhead, "invar-glyphs-" + i);
            t.setDaemon(true);
            t.start();
            aheadThreads[i] = t;
        }
    }

    static void dropDrawn(Entry e) {
        for (int s = 0; s < SUB_COUNT; s++) {
            if (e.drawn[s] != null) e.drawn[s].release();
            e.drawn[s] = null;
        }
        e.have = 0;
    }

    static boolean dropOldestDrawn() {
        Iterator<Entry> it = drawnAhead.iterator();
        if (!it.hasNext()) return false;
        Entry e = it.next();
        it.remove();
        e.listed = false;
        e.subsQueued &= ~(e.have | e.want);
        dropDrawn(e);
        e.want = 0;
        e.first = 0;
        if (!asked(e)) aheadCount--;
        return true;
    }

    static Drawn drawnAhead(Entry e, int sub, float sc) {
        int bit = 1 << sub;
        synchronized (lock) {
            if (!asked(e)) return null;
            if ((e.want & bit) != 0) {
                e.want &= ~bit;
                e.first &= ~bit;
                if (!asked(e)) aheadCount--;
                return null;
            }
            if ((e.drawing & bit) != 0) {
                e.skip |= bit;
                return null;
            }
            if ((e.have & bit) == 0) return null;
            Drawn d = e.drawn[sub];
            e.drawn[sub] = null;
            e.have &= ~bit;
            boolean stale = d.scale != sc || d.faces != faces;
            if (stale) {
                dropDrawn(e);
                e.want = 0;
                e.first = 0;
            }
            if (e.have == 0 && e.listed) {
                drawnAhead.remove(e);
                e.listed = false;
            }
            if (!asked(e)) aheadCount--;
            if (stale) {
                d.release();
                return null;
            }
            return d;
        }
    }

    static void drawAhead() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DEFAULT);
        HashMap<Long, Paint> paints = new HashMap<>();
        int paintsFaces = -1;
        Scratch scratch = new Scratch();
        while (true) {
            Entry e;
            int bit;
            float sc;
            int f;
            synchronized (lock) {
                while (true) {
                    e = askedFirst.poll();
                    if (e != null) {
                        e.inFirst = false;
                        int firsts = e.want & e.first;
                        bit = Integer.lowestOneBit(firsts);
                        if ((firsts & ~bit) != 0) {
                            e.inFirst = true;
                            askedFirst.addFirst(e);
                        }
                    } else {
                        e = askedRest.poll();
                        if (e == null) {
                            try {
                                lock.wait();
                            } catch (InterruptedException x) {
                                return;
                            }
                            continue;
                        }
                        e.inRest = false;
                        int rest = e.want & ~e.first;
                        bit = Integer.lowestOneBit(rest);
                        if ((rest & ~bit) != 0) {
                            e.inRest = true;
                            askedRest.addLast(e);
                        }
                    }
                    if (bit != 0) break;
                }
                e.want &= ~bit;
                e.first &= ~bit;
                e.drawing |= bit;
                sc = e.askScale;
                f = e.askFaces;
            }
            int s = Integer.numberOfTrailingZeros(bit);
            Drawn d = null;
            try {
                if (f != paintsFaces) {
                    paints.clear();
                    paintsFaces = f;
                }
                d = draw(e, sc, subDx(s), paintIn(paints, e.size * sc, e.weight, e.style), f, scratch);
            } catch (Throwable x) {
                Host.log("glyph ahead: " + x);
            }
            synchronized (lock) {
                e.drawing &= ~bit;
                boolean skipped = (e.skip & bit) != 0;
                e.skip &= ~bit;
                if (d != null) {
                    if (skipped) {
                        d.release();
                    } else {
                        e.drawn[s] = d;
                        e.have |= bit;
                        if (!e.listed) {
                            e.listed = true;
                            drawnAhead.add(e);
                        }
                    }
                }
                if (!asked(e)) aheadCount--;
            }
        }
    }

    // ---- Reference text rendering (textdiff) ----

    static Bitmap refBitmap;
    static Canvas refCanvas;
    static float refScale;
    static String frameName;

    static File refFile(String name) {
        File d = new File(Host.activity.getFilesDir(), "textdiff");
        d.mkdirs();
        return new File(d, name + ".png");
    }

    static boolean save(Bitmap b, String name) {
        try (FileOutputStream o = new FileOutputStream(refFile(name))) {
            return b.compress(Bitmap.CompressFormat.PNG, 100, o);
        } catch (Exception e) {
            Host.log("textdiff: " + name + ": " + e);
            return false;
        }
    }

    static void refBegin(double w, double h, double scale, int bg) {
        refScale = (float) scale;
        refBitmap = Bitmap.createBitmap((int) Math.round(w * scale), (int) Math.round(h * scale), Bitmap.Config.ARGB_8888);
        refCanvas = new Canvas(refBitmap);
        refCanvas.drawColor(0xFF000000 | bg);
    }

    static void refText(String text, double size, int weight, int style, double x, double base, int color, double[] out) {
        for (int i = 0; i < 5; i++) out[i] = 0;
        if (refCanvas == null) return;
        float d = refScale;
        TextPaint p = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        p.setTypeface(typeface(style, weight));
        p.setTextSize((float) size * d);
        p.setColor(0xFF000000 | color);
        StaticLayout l = StaticLayout.Builder.obtain(text, 0, text.length(), p, 1 << 20)
                .setIncludePad(false).setAlignment(Layout.Alignment.ALIGN_NORMAL).build();
        float left = l.getLineLeft(0);
        refCanvas.save();
        refCanvas.translate((float) x * d - left, (float) base * d - l.getLineBaseline(0));
        l.draw(refCanvas);
        refCanvas.restore();
        out[0] = l.getLineWidth(0) / d;
        Rect b = new Rect();
        p.getTextBounds(text, 0, text.length(), b);
        out[1] = b.left / d;
        out[2] = b.top / d;
        out[3] = b.right / d;
        out[4] = b.bottom / d;
    }

    static boolean refEnd(String name) {
        if (refBitmap == null) return false;
        boolean ok = save(refBitmap, name);
        refBitmap.recycle();
        refBitmap = null;
        refCanvas = null;
        return ok;
    }

    static boolean refFrame(String name) {
        frameName = name;
        return true;
    }

    static String takeFrameName() {
        String name = frameName;
        frameName = null;
        return name;
    }

    static void frameDrawn(String name) {
        MBView v = Host.view;
        if (v == null) return;
        int w = v.getWidth(), h = v.getHeight();
        if (w <= 0 || h <= 0) return;
        ByteBuffer buf = ByteBuffer.allocateDirect(w * h * 4).order(ByteOrder.nativeOrder());
        GLES30.glReadPixels(0, 0, w, h, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, buf);
        Bitmap up = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        up.copyPixelsFromBuffer(buf);
        Matrix m = new Matrix();
        m.preScale(1, -1);
        Bitmap b = Bitmap.createBitmap(up, 0, 0, w, h, m, false);
        save(b, name);
        up.recycle();
        b.recycle();
    }
}
