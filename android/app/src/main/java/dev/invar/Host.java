package dev.invar;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.icu.text.BreakIterator;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;
import android.provider.Settings;
import android.system.Os;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;

/** Static callbacks invoked by android_host.c on the application logic thread. */
final class Host {
    static MainActivity activity;
    static MBView view;
    static int bench;
    static int shotAt = -1;
    static final Handler main = new Handler(Looper.getMainLooper());

    static volatile boolean imeActive;
    static volatile String imeText = "";
    static volatile int selStart, selEnd;
    static volatile int imeBottom;

    static boolean imeLog() {
        return "1".equals(Os.getenv("INVAR_IME_LOG"));
    }

    static volatile boolean resumed, focused;

    static int appState() {
        return (resumed ? 1 : 0) | (resumed && focused ? 2 : 0);
    }

    private static int toldAppState = -1;

    static synchronized void tellAppState() {
        int s = appState();
        if (s == toldAppState || view == null) return;
        toldAppState = s;
        view.toApp(() -> Native.appState(s));
    }

    static volatile boolean systemDark;
    static volatile float fontScale = 1f;

    static float fontScale() {
        return fontScale;
    }

    static boolean systemDark() {
        return systemDark;
    }

    static int nativePointing() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            if (d.supportsSource(InputDevice.SOURCE_MOUSE) || d.supportsSource(InputDevice.SOURCE_TOUCHPAD)) return 2;
        }
        return 1;
    }

    static int keyboard() {
        watchDevices();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            if (d.supportsSource(InputDevice.SOURCE_KEYBOARD)
                    && d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC) {
                return 1;
            }
        }
        return 0;
    }

    private static boolean watching;

    private static void watchDevices() {
        if (watching || view == null) return;
        watching = true;
        InputManager im = view.getContext().getSystemService(InputManager.class);
        if (im == null) return;
        main.post(() -> im.registerInputDeviceListener(new InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) { requestFrame(); }
            @Override public void onInputDeviceRemoved(int id) { requestFrame(); }
            @Override public void onInputDeviceChanged(int id) { requestFrame(); }
        }, main));
    }

    static boolean a11yOn() {
        return A11y.on();
    }

    static void a11yTree(String lines) {
        main.post(() -> {
            MBView v = view;
            if (v != null) v.a11y().set(lines);
        });
    }

    static int groundLight = Color.rgb(245, 245, 247);
    static int groundDark = 0xFF1B1C1F;

    static void setDark(boolean dark) {
        main.post(() -> {
            if (activity == null) return;
            if (Build.VERSION.SDK_INT >= 30) {
                WindowInsetsController c = activity.getWindow().getInsetsController();
                if (c != null) {
                    int light = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                            | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                    c.setSystemBarsAppearance(dark ? 0 : light, light);
                }
            }
            ViewGroup content = activity.findViewById(android.R.id.content);
            if (content != null && content.getChildCount() > 0) {
                content.getChildAt(0).setBackgroundColor(dark ? groundDark : groundLight);
            }
        });
    }

    static boolean sideways;
    static boolean turnable;

    static void setSideways(boolean on) {
        main.post(() -> {
            if (activity == null) return;
            if (Build.VERSION.SDK_INT >= 30) {
                WindowInsetsController c = activity.getWindow().getInsetsController();
                if (c != null) {
                    c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    if (on) c.hide(WindowInsets.Type.systemBars());
                    else c.show(WindowInsets.Type.systemBars());
                }
            }
            sideways = on;
            if (on) activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            else holdUpright(activity);
        });
    }

    static boolean setTurnable(boolean on) {
        Activity a = activity;
        boolean phone = a != null && upright(a.getResources().getConfiguration())
                == ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
        main.post(() -> {
            turnable = on;
            if (activity != null) holdUpright(activity);
        });
        return phone;
    }

    static int upright(Configuration c) {
        return c.smallestScreenWidthDp < 600
                ? ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
                : ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
    }

    static void holdUpright(Activity a) {
        if (sideways) return;
        a.setRequestedOrientation(turnable
                ? ActivityInfo.SCREEN_ORIENTATION_USER
                : upright(a.getResources().getConfiguration()));
    }

    private static boolean imeOwed;
    static volatile boolean imeVisible;
    static volatile boolean imeAwaited;
    static volatile int lifts;
    private static volatile int liftsSeen;
    private static boolean imeControllable;
    private static View imeWatched;

    static void windowFocused(boolean has) {
        if (!has) return;
        showOwedIme();
    }

    private static void showOwedIme() {
        boolean owed = imeOwed;
        if (!owed || view == null || activity == null) return;
        int api = Build.VERSION.SDK_INT;
        boolean multiWindow = activity.isInMultiWindowMode();
        if (api < 30 || multiWindow) {
            if (!ImeRequest.showNow(api, view.hasWindowFocus(), multiWindow, imeControllable, owed)) return;
            imeOwed = false;
            InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.showSoftInput(view, 0);
            return;
        }
        WindowInsetsController c = view.getWindowInsetsController();
        if (c == null) return;
        if (imeWatched != view) {
            imeWatched = view;
            imeControllable = false;
            c.addOnControllableInsetsChangedListener((ctl, types) -> {
                imeControllable = (types & WindowInsets.Type.ime()) != 0;
                showOwedIme();
            });
        }
        if (!ImeRequest.showNow(api, view.hasWindowFocus(), multiWindow, imeControllable, owed)) return;
        imeOwed = false;
        c.show(WindowInsets.Type.ime());
    }

    static void requestFrame() {
        MBView v = view;
        if (v != null) v.wantFrame();
    }

    private static final AtomicBoolean postedOwed = new AtomicBoolean();

    static void posted() {
        MBView v = view;
        if (v == null) return;
        if (!postedOwed.compareAndSet(false, true)) return;
        MBView.onApp(() -> {
            postedOwed.set(false);
            Native.posted();
        });
    }

    static void wakeAfter(double ms) {
        MBView v = view;
        if (v == null) return;
        double base = v.buildingFor >= 0 ? v.buildingFor : System.nanoTime() / 1e6;
        double at = base + ms;
        if (v.wakeFor >= 0 && v.wakeFor <= at) return;
        v.wakeFor = at;
        double lead = at - System.nanoTime() / 1e6 - 2 * v.framePeriodMs();
        main.postDelayed(() -> v.wakeAt(at), (long) Math.max(0, lead));
    }

    static boolean framesPaced(double hz) {
        MBView v = view;
        return v != null && v.framesPaced(hz);
    }

    static int imeSent;
    static volatile int imeTaken;
    private static int imeTakenSeen;

    static void setIme(boolean active, double x, double y, double w, double h,
                       String text, int a, int b) {
        boolean was = imeActive;
        final int taken = imeTaken;
        if (!active && !was) {
            imeText = text;
            selStart = a;
            selEnd = b;
            imeTakenSeen = taken;
            return;
        }
        if (active == was && lifts == liftsSeen && taken == imeTakenSeen
                && a == selStart && b == selEnd && Objects.equals(text, imeText)) {
            return;
        }
        boolean cleared = text.isEmpty() && !imeText.isEmpty();
        imeActive = active;
        if (active != was) imeAwaited = active;
        imeText = text;
        selStart = a;
        selEnd = b;
        imeTakenSeen = taken;
        main.post(() -> {
            if (activity == null || view == null) return;
            InputMethodManager imm = (InputMethodManager)
                    activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm == null) return;
            boolean lifted = lifts != liftsSeen;
            liftsSeen = lifts;
            if (active && !was) {
                view.requestFocus();
                imm.restartInput(view);
                view.imeMovingStarted();
                imeOwed = true;
                showOwedIme();
            } else if (!active && was) {
                imeOwed = false;
                if (imeVisible) view.imeMovingStarted();
                imm.hideSoftInputFromWindow(view.getWindowToken(), 0);
            } else if (active) {
                if (ImeRequest.retryOnTap(lifted, imeVisible, imeOwed, activity.isInMultiWindowMode())) {
                    view.imeMovingStarted();
                    imeOwed = true;
                    imeAwaited = true;
                    showOwedIme();
                }
                MBView.MBInput in = view.input;
                if (in == null || taken != imeSent || !in.took(text, a, b)) return;
                if (cleared) {
                    imm.restartInput(view);
                } else {
                    if (imeLog()) log("ime: updateSelection " + a + "," + b);
                    imm.updateSelection(view, a, b, -1, -1);
                }
            }
        });
    }

    static void textMenu(boolean shown, double x, double y, double w, double h, int can) {
        main.post(() -> {
            if (view != null) view.textMenu(shown, x, y, w, h, can);
        });
    }

    static void textMenuOwn(String words) {
        main.post(() -> {
            if (view != null) view.textMenuOwn = words.isEmpty() ? new String[0] : words.split("\n", 8);
        });
    }

    static void clipboardSet(String s) {
        main.post(() -> {
            if (activity == null) return;
            ClipboardManager cm = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("text", s));
        });
    }

    static String secretCopied;

    static void clipboardSecret(String s, double clearMs) {
        main.post(() -> {
            if (activity == null) return;
            ClipboardManager cm = (ClipboardManager)
                    activity.getApplicationContext().getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null) return;
            ClipData clip = ClipData.newPlainText("text", s);
            PersistableBundle extras = new PersistableBundle();
            extras.putBoolean("android.content.extra.IS_SENSITIVE", true);
            clip.getDescription().setExtras(extras);
            cm.setPrimaryClip(clip);
            secretCopied = s;
        });
    }

    static void clipboardSecretOff() {
        main.post(() -> {
            String s = secretCopied;
            secretCopied = null;
            if (s == null || activity == null) return;
            ClipboardManager cm = (ClipboardManager)
                    activity.getApplicationContext().getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null) return;
            String now = null;
            try {
                ClipData c = cm.getPrimaryClip();
                if (c != null && c.getItemCount() > 0 && c.getItemAt(0).getText() != null) {
                    now = c.getItemAt(0).getText().toString();
                }
            } catch (RuntimeException ignored) {
            }
            if (now == null || now.equals(s)) {
                try {
                    if (Build.VERSION.SDK_INT >= 28) cm.clearPrimaryClip();
                    else cm.setPrimaryClip(ClipData.newPlainText("", ""));
                } catch (RuntimeException e) {
                    log("clipboard: clear failed: " + e);
                }
            }
        });
    }

    static double wordAt(String text, int off) {
        if (text == null || text.isEmpty() || off < 0 || off >= text.length()) return -1;
        BreakIterator words = BreakIterator.getWordInstance();
        words.setText(text);
        int end = words.following(off);
        int status = words.getRuleStatus();
        int start = words.previous();
        if (end == BreakIterator.DONE || start == BreakIterator.DONE) return -1;
        if (status < BreakIterator.WORD_NUMBER) return -1;
        return start * 4294967296.0 + end;
    }

    static boolean openUrl(String url) {
        if (activity == null || url == null || url.isEmpty()) return false;
        Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
        i.addCategory(Intent.CATEGORY_BROWSABLE);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        main.post(() -> {
            try {
                activity.startActivity(i);
            } catch (ActivityNotFoundException e) {
                log("openUrl: no handler for " + url);
            }
        });
        return true;
    }

    static volatile boolean backWanted;

    static void backEnabled(boolean on) {
        backWanted = on;
        main.post(() -> {
            if (activity != null) activity.setBackEnabled(backWanted);
        });
    }

    static volatile Writer logFile;

    static void log(String s) {
        Writer f = logFile;
        if (f != null) {
            synchronized (Host.class) {
                try {
                    f.write(s);
                    f.write('\n');
                    f.flush();
                } catch (IOException e) {
                    logFile = null;
                }
            }
        }
        Log.i("invar", s);
    }

    static void openLogFile(Context c) {
        String name = Os.getenv("INVAR_LOG_FILE");
        if (name == null || name.isEmpty() || logFile != null) return;
        try {
            logFile = new BufferedWriter(new OutputStreamWriter(
                    new FileOutputStream(new File(c.getFilesDir(), name)), StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.i("invar", "INVAR_LOG_FILE " + name + ": " + e);
        }
    }

    static int benchMode() {
        return bench;
    }

    static int reduceMotion() {
        MainActivity a = activity;
        if (a == null) return 0;
        float scale = Settings.Global.getFloat(a.getContentResolver(), Settings.Global.ANIMATOR_DURATION_SCALE, 1f);
        return scale == 0f ? 1 : 0;
    }

    static int drive(int kind, String text) {
        if (kind == 4 || kind > 5) return 0;
        main.post(() -> {
            MBView v = view;
            if (v == null) return;
            InputConnection ic = v.input;
            if (ic == null) return;
            switch (kind) {
                case 0: ic.commitText(text, 1); break;
                case 1: ic.setComposingText(text, 1); break;
                case 2: ic.sendKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)); break;
                case 3: v.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_PASTE)); break;
                case 5: ic.deleteSurroundingText(1, 0); break;
                default: break;
            }
        });
        return 1;
    }

    static void exit(int code) {
        main.post(() -> {
            if (activity != null) activity.finishAndRemoveTask();
            System.exit(code);
        });
    }

    static boolean charsetKnown(String name) {
        try {
            return Charset.isSupported(name);
        } catch (Exception e) {
            return false;
        }
    }

    static String charsetDecode(byte[] data, String name) {
        try {
            return new String(data, Charset.forName(name));
        } catch (Exception e) {
            return "";
        }
    }
}
