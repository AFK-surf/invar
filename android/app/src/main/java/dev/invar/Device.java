package dev.invar;

import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Build;
import android.util.DisplayMetrics;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Device capabilities beyond the window: screen metrics & density, safe-area
 * insets, key catcher mode, soft keyboard inset, clipboard read/write,
 * keep-awake, IME action hints, and haptic feedback.
 */
final class Device {
    static volatile boolean catching;
    static volatile float safeTop, safeBottom, safeLeft, safeRight;

    static final int PNG_MAX = 1 << 30;
    static final int SOURCE_MAX = 32 * 1024 * 1024;
    static final long PIXELS_MAX = 16L * 1024 * 1024;
    static final int NONE = 0, PNG = 1, TOO_LARGE = 2, UNREADABLE = 3;

    static float density(Context c) {
        DisplayMetrics dm = c.getResources().getDisplayMetrics();
        return dm.density > 0 ? dm.density : 1f;
    }

    static int screenWidthPx(Context c) {
        return c.getResources().getDisplayMetrics().widthPixels;
    }

    static int screenHeightPx(Context c) {
        return c.getResources().getDisplayMetrics().heightPixels;
    }

    static void updateSafeInsets(float top, float bottom, float left, float right) {
        safeTop = top;
        safeBottom = bottom;
        safeLeft = left;
        safeRight = right;
    }

    static void keyCatch(boolean on) {
        if (catching == on) return;
        catching = on;
        if (Host.imeActive) {
            Host.main.post(() -> {
                MBView v = Host.view;
                if (v == null) return;
                InputMethodManager imm = (InputMethodManager) v.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) imm.restartInput(v);
            });
        }
    }

    static double imeInset() {
        MBView v = Host.view;
        return v == null ? 0 : Host.imeBottom / v.density;
    }

    static boolean copyShown() {
        return Build.VERSION.SDK_INT >= 33;
    }

    static int keyMods(KeyEvent e) {
        int meta = e.getMetaState();
        return ((meta & KeyEvent.META_SHIFT_ON) != 0 ? 1 : 0)
                | ((meta & KeyEvent.META_CTRL_ON) != 0 ? 2 : 0)
                | ((meta & KeyEvent.META_ALT_ON) != 0 ? 4 : 0)
                | ((meta & KeyEvent.META_META_ON) != 0 ? 8 : 0);
    }

    static int[] catchKey(KeyEvent e) {
        int mods = keyMods(e);
        int code;
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_DEL: code = 1; break;
            case KeyEvent.KEYCODE_FORWARD_DEL: code = 2; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: code = 3; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: code = 4; break;
            case KeyEvent.KEYCODE_DPAD_UP: code = 5; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: code = 6; break;
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: code = 7; break;
            case KeyEvent.KEYCODE_MOVE_HOME: code = 8; break;
            case KeyEvent.KEYCODE_MOVE_END: code = 9; break;
            case KeyEvent.KEYCODE_PAGE_UP: code = 15; break;
            case KeyEvent.KEYCODE_PAGE_DOWN: code = 16; break;
            case KeyEvent.KEYCODE_ESCAPE: code = 17; break;
            case KeyEvent.KEYCODE_TAB: code = 18; break;
            case KeyEvent.KEYCODE_SPACE: code = 1032; break;
            default: code = 0; break;
        }
        if (code != 0) return new int[] {code, mods};
        if ((mods & (2 | 4 | 8)) == 0) return null;
        int k = e.getKeyCode();
        if (k >= KeyEvent.KEYCODE_A && k <= KeyEvent.KEYCODE_Z) return new int[] {1000 + 'a' + (k - KeyEvent.KEYCODE_A), mods};
        if (k >= KeyEvent.KEYCODE_0 && k <= KeyEvent.KEYCODE_9) return new int[] {1000 + '0' + (k - KeyEvent.KEYCODE_0), mods};
        int ch = e.getUnicodeChar();
        return ch > 0 ? new int[] {1000 + ch, mods} : null;
    }

    // ---- Clipboard read ----

    static final class Read {
        volatile int state;
        String text = "";
        int picture = NONE;
        byte[] png = new byte[0];
    }

    static final ConcurrentHashMap<Integer, Read> reads = new ConcurrentHashMap<>();
    private static final AtomicInteger nextRead = new AtomicInteger(1);

    static int clipRead() {
        int id = nextRead.getAndIncrement();
        Read r = new Read();
        reads.put(id, r);
        Host.main.post(() -> {
            Context c = Host.activity;
            ClipData clip = null;
            try {
                ClipboardManager cm = c == null ? null : (ClipboardManager) c.getSystemService(Context.CLIPBOARD_SERVICE);
                clip = cm == null ? null : cm.getPrimaryClip();
            } catch (Exception e) {
                Host.log("clipboard: " + e);
            }
            final ClipData got = clip != null && clip.getItemCount() > 0 ? clip : null;
            new Thread(() -> {
                if (got != null && c != null) readClip(c, got, r);
                r.state = 1;
                Native.tell(Native.OUTSIDE_CLIP, id, 1);
            }, "invar-clip-read").start();
        });
        return id;
    }

    private static void readClip(Context c, ClipData clip, Read r) {
        ClipData.Item item = clip.getItemAt(0);
        ClipDescription d = clip.getDescription();
        boolean described = false;
        if (d != null) {
            for (int i = 0; i < d.getMimeTypeCount(); i++) described |= isImage(d.getMimeType(i));
        }
        ContentResolver cr = c.getContentResolver();
        Uri uri = item.getUri();
        Uri image = null;
        if (uri != null) {
            String type = null;
            try {
                type = cr.getType(uri);
            } catch (Exception ignored) {
            }
            if (described || isImage(type)) image = uri;
        }
        CharSequence text = item.getText();
        if (text == null && image == null) {
            try {
                text = item.coerceToText(c);
            } catch (Exception ignored) {
            }
        }
        r.text = text == null ? "" : text.toString();
        if (image != null) readyImage(cr, image, r);
    }

    private static boolean isImage(String type) {
        return type != null && type.trim().toLowerCase(Locale.ROOT).startsWith("image/");
    }

    private static void readyImage(ContentResolver cr, Uri uri, Read r) {
        byte[] bytes;
        try (InputStream in = cr.openInputStream(uri)) {
            if (in == null) {
                r.picture = UNREADABLE;
                return;
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[64 * 1024];
            boolean kindRead = false;
            int max = SOURCE_MAX;
            for (int n; (n = in.read(buf)) >= 0; ) {
                out.write(buf, 0, n);
                if (!kindRead && out.size() >= 8) {
                    kindRead = true;
                    if (isPng(out.toByteArray())) max = PNG_MAX;
                }
                if (out.size() > max) {
                    r.picture = TOO_LARGE;
                    return;
                }
            }
            bytes = out.toByteArray();
        } catch (Exception e) {
            r.picture = UNREADABLE;
            return;
        }
        if (bytes.length == 0) {
            r.picture = UNREADABLE;
            return;
        }
        if (isPng(bytes)) {
            r.picture = bytes.length <= PNG_MAX ? PNG : TOO_LARGE;
            if (r.picture == PNG) r.png = bytes;
            return;
        }
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            r.picture = UNREADABLE;
            return;
        }
        if ((long) bounds.outWidth * bounds.outHeight > PIXELS_MAX) {
            r.picture = TOO_LARGE;
            return;
        }
        Bitmap b = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
        if (b == null) {
            r.picture = UNREADABLE;
            return;
        }
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            if (!b.compress(Bitmap.CompressFormat.PNG, 100, out)) {
                r.picture = UNREADABLE;
            } else if (out.size() > PNG_MAX) {
                r.picture = TOO_LARGE;
            } else {
                r.picture = PNG;
                r.png = out.toByteArray();
            }
        } finally {
            b.recycle();
        }
    }

    private static final byte[] SIGNATURE = {(byte) 0x89, 'P', 'N', 'G', 13, 10, 26, 10};

    static boolean isPng(byte[] b) {
        return b.length >= SIGNATURE.length && Arrays.equals(Arrays.copyOf(b, SIGNATURE.length), SIGNATURE);
    }

    static int clipState(int id) {
        Read r = reads.get(id);
        return r == null ? -1 : r.state;
    }

    static int clipPicture(int id) {
        Read r = reads.get(id);
        return r == null ? NONE : r.picture;
    }

    static String clipText(int id) {
        Read r = reads.get(id);
        return r == null ? "" : r.text;
    }

    static byte[] clipPng(int id) {
        Read r = reads.get(id);
        return r == null ? new byte[0] : r.png;
    }

    static void clipEnd(int id) {
        reads.remove(id);
    }

    static void clipSet(String text, byte[] png, String label) {
        Context c = Host.activity;
        if (c == null) return;
        Host.main.post(() -> {
            try {
                ClipboardManager cm = (ClipboardManager) c.getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) cm.setPrimaryClip(ClipData.newPlainText(label != null ? label : "text", text != null ? text : ""));
            } catch (Exception e) {
                Host.log("clipboard: " + e);
            }
        });
    }

    static void keepAwake(boolean on) {
        Host.main.post(() -> {
            MainActivity a = Host.activity;
            if (a == null) return;
            if (on) a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        });
    }

    static volatile int imeAction;

    static void imeAction(int kind) {
        if (kind == imeAction) return;
        imeAction = kind;
        Host.main.post(() -> {
            MBView v = Host.view;
            if (v == null) return;
            InputMethodManager imm = (InputMethodManager) v.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.restartInput(v);
        });
    }

    static void haptic(int kind) {
        int constant;
        switch (kind) {
            case 0: constant = HapticFeedbackConstants.LONG_PRESS; break;
            case 1: constant = HapticFeedbackConstants.CONTEXT_CLICK; break;
            case 2: constant = HapticFeedbackConstants.TEXT_HANDLE_MOVE; break;
            case 3: constant = Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.CONFIRM : HapticFeedbackConstants.VIRTUAL_KEY; break;
            case 4: constant = Build.VERSION.SDK_INT >= 30 ? HapticFeedbackConstants.REJECT : HapticFeedbackConstants.LONG_PRESS; break;
            case 5: constant = HapticFeedbackConstants.KEYBOARD_TAP; break;
            default: return;
        }
        Host.main.post(() -> {
            MBView v = Host.view;
            if (v != null) v.performHapticFeedback(constant);
        });
    }
}
