package dev.invar;

import android.content.ContentResolver;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ColorSpace;
import android.graphics.ImageDecoder;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.opengl.GLES30;
import android.opengl.GLUtils;
import android.os.Build;
import android.util.Size;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Asynchronous image decoding (ImageDecoder / BitmapFactory with EXIF
 * orientation handling) on background worker pools and premultiplied RGBA
 * texture upload on the GL thread.
 */
final class Images {
    static volatile int textureMost;

    static void textureMostRead() {
        if (textureMost > 0) return;
        int[] v = new int[1];
        GLES30.glGetIntegerv(GLES30.GL_MAX_TEXTURE_SIZE, v, 0);
        if (v[0] > 0) textureMost = v[0];
    }

    private static ThreadFactory bgFactory(String prefix) {
        AtomicInteger n = new AtomicInteger();
        return r -> {
            Thread t = new Thread(() -> {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
                r.run();
            }, prefix + "-" + n.incrementAndGet());
            t.setDaemon(true);
            return t;
        };
    }

    static final ExecutorService pool = Executors.newFixedThreadPool(3, bgFactory("invar-img"));
    static final ExecutorService thumbs = Executors.newFixedThreadPool(2, bgFactory("invar-thumb"));
    static final ExecutorService remote = Executors.newFixedThreadPool(2, bgFactory("invar-remote"));

    static final class Req {
        final String src;
        final byte[] data;
        final int maxPx;

        Req(String src, byte[] data, int maxPx) {
            this.src = src;
            this.data = data;
            this.maxPx = maxPx;
        }
    }

    static final ConcurrentHashMap<Integer, Req> live = new ConcurrentHashMap<>();

    static ExecutorService poolFor(Req r) {
        if (r.data != null) return pool;
        if (r.src.startsWith("content://")) return thumbs;
        if (r.src.startsWith("http://") || r.src.startsWith("https://")) return remote;
        return pool;
    }

    static void submit(int id, Req r) {
        poolFor(r).execute(() -> decode(id, r));
    }

    static void load(int id, String src, byte[] data, int maxPx) {
        Req r = new Req(src, data, maxPx);
        live.put(id, r);
        submit(id, r);
    }

    static volatile boolean held;
    static final ConcurrentLinkedQueue<Runnable> heldUploads = new ConcurrentLinkedQueue<>();

    static void hold() {
        held = true;
    }

    static void pump() {
        for (Runnable r; (r = heldUploads.poll()) != null; ) r.run();
    }

    static void toRender(Runnable r) {
        if (held) {
            heldUploads.add(r);
            return;
        }
        MBView.toRender(r);
    }

    static void free(int id) {
        live.remove(id);
        MBView.toRetire(() -> Native.imageGone(id));
    }

    static void reload() {
        for (Map.Entry<Integer, Req> e : live.entrySet()) {
            submit(e.getKey(), e.getValue());
        }
    }

    static Bitmap decodeSource(ImageDecoder.Source src, int maxPx, int[] orig) throws IOException {
        Bitmap b = ImageDecoder.decodeBitmap(src, (d, info, s) -> {
            int w = info.getSize().getWidth(), h = info.getSize().getHeight();
            orig[0] = w;
            orig[1] = h;
            double k = Math.min(1.0, maxPx / (double) Math.max(w, h));
            if (k < 1) {
                d.setTargetSize(Math.max(1, (int) Math.round(w * k)),
                        Math.max(1, (int) Math.round(h * k)));
            }
            d.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
            d.setTargetColorSpace(ColorSpace.get(ColorSpace.Named.SRGB));
        });
        if (b.getConfig() != Bitmap.Config.ARGB_8888) {
            Bitmap c = b.copy(Bitmap.Config.ARGB_8888, false);
            b.recycle();
            b = c;
        }
        return b;
    }

    static int cappedMaxPx(int maxPx) {
        int most = textureMost > 0 ? textureMost : 4096;
        return maxPx > 0 ? Math.min(maxPx, most) : most;
    }

    static void decode(int id, Req r) {
        if (live.get(id) != r) return;
        int maxPx = cappedMaxPx(r.maxPx);
        int[] orig = new int[2];
        Bitmap b = null;
        try {
            byte[] data = r.data;
            if (data == null && r.src.startsWith("content://") && Host.activity != null) {
                ContentResolver cr = Host.activity.getContentResolver();
                Uri uri = Uri.parse(r.src);
                try {
                    Bitmap t = Build.VERSION.SDK_INT >= 29 ? cr.loadThumbnail(uri, new Size(maxPx, maxPx), null) : null;
                    if (t != null) {
                        orig[0] = t.getWidth();
                        orig[1] = t.getHeight();
                        b = t.getConfig() == Bitmap.Config.ARGB_8888 ? t : t.copy(Bitmap.Config.ARGB_8888, false);
                        if (b != t) t.recycle();
                    }
                } catch (Exception ignored) {
                }
                if (b == null && Build.VERSION.SDK_INT >= 28) {
                    b = decodeSource(ImageDecoder.createSource(cr, uri), maxPx, orig);
                }
            } else {
                if (data == null && (r.src.startsWith("http://") || r.src.startsWith("https://"))) {
                    data = fetch(r.src);
                    if (live.get(id) != r) return;
                }
                String path = r.src.startsWith("file://") ? r.src.substring(7) : r.src;
                if (Build.VERSION.SDK_INT >= 28) {
                    try {
                        b = decodeSource(data != null
                                ? ImageDecoder.createSource(ByteBuffer.wrap(data))
                                : ImageDecoder.createSource(new File(path)), maxPx, orig);
                    } catch (IOException e) {
                        b = withFactory(data, path, maxPx, orig);
                        if (b == null) throw e;
                    }
                } else {
                    b = withFactory(data, path, maxPx, orig);
                }
                if (b != null && b.getConfig() != Bitmap.Config.ARGB_8888) {
                    Bitmap c = b.copy(Bitmap.Config.ARGB_8888, false);
                    b.recycle();
                    b = c;
                }
            }
        } catch (Throwable e) {
            Host.log("image " + r.src + ": " + e);
            b = null;
        }
        Bitmap bmp = b;
        toRender(() -> upload(id, bmp, orig[0], orig[1]));
    }

    static Bitmap withFactory(byte[] data, String path, int maxPx, int[] orig) throws IOException {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        if (data != null) BitmapFactory.decodeByteArray(data, 0, data.length, o);
        else BitmapFactory.decodeFile(path, o);
        int w = o.outWidth, h = o.outHeight;
        if (w <= 0 || h <= 0) return null;
        o.inJustDecodeBounds = false;
        o.inSampleSize = 1;
        while (Math.max(w, h) / (o.inSampleSize * 2) >= maxPx) o.inSampleSize *= 2;
        Bitmap b = data != null ? BitmapFactory.decodeByteArray(data, 0, data.length, o)
                : BitmapFactory.decodeFile(path, o);
        if (b == null) return null;
        ExifInterface ex = data != null ? new ExifInterface(new ByteArrayInputStream(data))
                : new ExifInterface(path);
        int turn = ex.getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        float k = (float) Math.min(1.0, maxPx / (double) Math.max(b.getWidth(), b.getHeight()));
        Matrix m = new Matrix();
        m.setScale(k, k);
        switch (turn) {
            case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: m.postScale(-1, 1); break;
            case ExifInterface.ORIENTATION_ROTATE_180: m.postRotate(180); break;
            case ExifInterface.ORIENTATION_FLIP_VERTICAL: m.postScale(1, -1); break;
            case ExifInterface.ORIENTATION_TRANSPOSE: m.postRotate(90); m.postScale(-1, 1); break;
            case ExifInterface.ORIENTATION_ROTATE_90: m.postRotate(90); break;
            case ExifInterface.ORIENTATION_TRANSVERSE: m.postRotate(-90); m.postScale(-1, 1); break;
            case ExifInterface.ORIENTATION_ROTATE_270: m.postRotate(-90); break;
            default: break;
        }
        boolean swap = turn >= ExifInterface.ORIENTATION_TRANSPOSE;
        orig[0] = swap ? h : w;
        orig[1] = swap ? w : h;
        if (m.isIdentity()) return b;
        Bitmap t = Bitmap.createBitmap(b, 0, 0, b.getWidth(), b.getHeight(), m, true);
        if (t != b) b.recycle();
        return t;
    }

    static byte[] fetch(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15_000);
        c.setReadTimeout(30_000);
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[64 * 1024];
            for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toByteArray();
        } finally {
            c.disconnect();
        }
    }

    static void upload(int id, Bitmap b, int ow, int oh) {
        if (!live.containsKey(id)) {
            if (b != null) b.recycle();
            return;
        }
        textureMostRead();
        int most = textureMost;
        if (b != null && most > 0 && (b.getWidth() > most || b.getHeight() > most)) {
            double k = most / (double) Math.max(b.getWidth(), b.getHeight());
            int nw = Math.max(1, Math.min(most, (int) Math.round(b.getWidth() * k)));
            int nh = Math.max(1, Math.min(most, (int) Math.round(b.getHeight() * k)));
            Bitmap s = Bitmap.createScaledBitmap(b, nw, nh, true);
            if (s != b) b.recycle();
            b = s;
        }
        int[] t = {0};
        if (b != null) {
            GLES30.glGenTextures(1, t, 0);
            GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, t[0]);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE);
            GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE);
            GLUtils.texImage2D(GLES30.GL_TEXTURE_2D, 0, b, 0);
        }
        int w = b != null ? b.getWidth() : 0, h = b != null ? b.getHeight() : 0;
        if (b != null) b.recycle();
        boolean ok = t[0] != 0;
        if (!Native.imageDone(id, t[0])) return;
        if (MBView.ONE) Native.imageTold(id, ok, w, h, ow, oh);
        else MBView.onApp(() -> Native.imageTold(id, ok, w, h, ow, oh));
    }
}
