package dev.invar;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.view.WindowInsets;
import android.view.WindowInsetsAnimation;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.FrameLayout;
import android.window.BackEvent;
import android.window.OnBackAnimationCallback;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.util.List;
import java.util.Locale;

/** Edge-to-edge Android Activity hosting an Invar MBView with IME animation sync. */
public class MainActivity extends Activity {
    private boolean imeMoving;
    private boolean imeShown;
    private static boolean edge = true;
    private MBView mbView;
    private static Insets saidBars;
    static volatile long hiddenAt;
    private PowerManager.OnThermalStatusChangedListener thermal;
    private boolean backOn;
    private Object backCallback;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Host.activity = this;
        Host.holdUpright(this);

        try {
            Bundle meta = getPackageManager()
                    .getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA).metaData;
            if (meta != null) {
                edge = meta.getBoolean("dev.invar.edge", true);
                String env = meta.getString("dev.invar.env");
                if (env != null) {
                    for (String kv : env.split(";")) {
                        int eq = kv.indexOf('=');
                        if (eq <= 0) continue;
                        try {
                            Os.setenv(kv.substring(0, eq), kv.substring(eq + 1), true);
                        } catch (ErrnoException e) {
                            Host.log("env " + kv + ": " + e);
                        }
                    }
                }
                String ground = meta.getString("dev.invar.ground");
                if (ground != null && ground.indexOf(',') > 0) {
                    try {
                        Host.groundLight = Color.parseColor(ground.substring(0, ground.indexOf(',')).trim());
                        Host.groundDark = Color.parseColor(ground.substring(ground.indexOf(',') + 1).trim());
                    } catch (IllegalArgumentException e) {
                        Host.log("dev.invar.ground " + ground + ": " + e);
                    }
                }
            }
        } catch (PackageManager.NameNotFoundException ignored) {
        }

        Host.openLogFile(this);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(night(getResources().getConfiguration()) ? Host.groundDark : Host.groundLight);
        MBView view = new MBView(this);
        mbView = view;
        root.addView(view, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        if (Build.VERSION.SDK_INT >= 30) {
            root.setOnApplyWindowInsetsListener((v, insets) -> {
                Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                if (edge) {
                    sayBars(bars);
                    bars = Insets.NONE;
                }
                FrameLayout.LayoutParams p = (FrameLayout.LayoutParams) view.getLayoutParams();
                if (p.leftMargin != bars.left || p.topMargin != bars.top
                        || p.rightMargin != bars.right || p.bottomMargin != bars.bottom) {
                    p.setMargins(bars.left, bars.top, bars.right, bars.bottom);
                    view.setLayoutParams(p);
                }
                if (!imeMoving) keyboardAt(insets);
                boolean shown = insets.isVisible(WindowInsets.Type.ime());
                if (imeShown && !shown && Host.imeActive && !Host.imeAwaited && mbView.hasWindowFocus()) {
                    mbView.toApp(() -> Native.text(7, "", 0, 0));
                }
                if (!imeShown && shown && !Host.imeActive && !Host.imeAwaited && root.findFocus() == mbView) {
                    InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                    if (imm != null) imm.hideSoftInputFromWindow(mbView.getWindowToken(), 0);
                }
                boolean turned = shown != imeShown;
                imeShown = shown;
                Host.imeVisible = shown;
                if (turned) mbView.typingChanged();
                if (shown) Host.imeAwaited = false;
                return WindowInsets.CONSUMED;
            });

            root.setWindowInsetsAnimationCallback(new WindowInsetsAnimation.Callback(
                    WindowInsetsAnimation.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                @Override
                public void onPrepare(WindowInsetsAnimation a) {
                    if ((a.getTypeMask() & WindowInsets.Type.ime()) != 0) {
                        imeMoving = true;
                        if (mbView != null) {
                            mbView.setImeMoving(true);
                            mbView.imeMovingStarted();
                        }
                    }
                }

                @Override
                public WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> running) {
                    for (WindowInsetsAnimation a : running) {
                        if ((a.getTypeMask() & WindowInsets.Type.ime()) != 0) {
                            keyboardAt(insets);
                            break;
                        }
                    }
                    return insets;
                }

                @Override
                public void onEnd(WindowInsetsAnimation a) {
                    if ((a.getTypeMask() & WindowInsets.Type.ime()) == 0) return;
                    WindowInsets now = root.getRootWindowInsets();
                    if (now != null) keyboardAt(now);
                    imeMoving = false;
                    if (mbView != null) mbView.setImeMoving(false);
                    if (now != null && now.isVisible(WindowInsets.Type.ime())) Host.imeAwaited = false;
                }
            });
        }

        setContentView(root);

        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            if (edge) {
                WindowManager.LayoutParams a = getWindow().getAttributes();
                a.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                getWindow().setAttributes(a);
            }
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                int bars = WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
                        | WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
                c.setSystemBarsAppearance(night(getResources().getConfiguration()) ? 0 : bars, bars);
            }
        }

        Host.activity = this;
        Host.view = view;
        Host.systemDark = night(getResources().getConfiguration());
        Host.fontScale = getResources().getConfiguration().fontScale;
        A11y.attach(this);
    }

    private static boolean thermalHot(int status) {
        return Build.VERSION.SDK_INT >= 29 && status >= PowerManager.THERMAL_STATUS_MODERATE;
    }

    private static void thermalSaid(int status) {
        Native.thermal(thermalHot(status));
        Host.requestFrame();
    }

    @Override
    protected void onStart() {
        super.onStart();
        A11y.refresh(this);
        if (Build.VERSION.SDK_INT >= 29) {
            PowerManager power = getSystemService(PowerManager.class);
            if (power != null && thermal == null) {
                thermal = MainActivity::thermalSaid;
                power.addThermalStatusListener(getMainExecutor(), thermal);
                thermalSaid(power.getCurrentThermalStatus());
            }
        }
        if (hiddenAt != 0) {
            long away = Math.max(0, SystemClock.elapsedRealtime() - hiddenAt);
            hiddenAt = 0;
            Native.post("system", "{\"shown\":{\"away\":" + away + "}}");
        }
    }

    @Override
    protected void onStop() {
        if (mbView != null) mbView.onPause();
        if (Build.VERSION.SDK_INT >= 29) {
            PowerManager power = getSystemService(PowerManager.class);
            if (power != null && thermal != null) power.removeThermalStatusListener(thermal);
            thermal = null;
        }
        if (hiddenAt == 0) hiddenAt = SystemClock.elapsedRealtime();
        super.onStop();
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (level >= TRIM_MEMORY_RUNNING_LOW && mbView != null) mbView.trim();
    }

    private void sayBars(Insets bars) {
        if (bars.equals(saidBars)) return;
        saidBars = bars;
        float d = mbView.density;
        float top = bars.top / d, bottom = bars.bottom / d, left = bars.left / d, right = bars.right / d;
        Device.updateSafeInsets(top, bottom, left, right);
        Native.post("system", String.format(Locale.ROOT,
                "{\"bars\":{\"top\":%.2f,\"bottom\":%.2f,\"left\":%.2f,\"right\":%.2f}}",
                top, bottom, left, right));
    }

    private void keyboardAt(WindowInsets insets) {
        if (Build.VERSION.SDK_INT < 30) return;
        int ime = insets.getInsets(WindowInsets.Type.ime()).bottom;
        int bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout()).bottom;
        int over = Math.max(0, ime - bars);
        if (over != Host.imeBottom) {
            Host.imeBottom = over;
            if (Host.view != null) {
                if (imeMoving) Host.view.frameNow();
                else Host.view.wantFrame();
            }
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        Host.requestFrame();
    }

    private void back(int kind, float x, float progress, int edge) {
        MBView v = Host.view;
        if (v == null) return;
        v.back(kind, x / v.density, progress, edge);
    }

    void setBackEnabled(boolean on) {
        if (on == backOn) return;
        backOn = on;
        if (Build.VERSION.SDK_INT < 33) return;
        OnBackInvokedDispatcher dispatcher = getOnBackInvokedDispatcher();
        if (on) {
            if (Build.VERSION.SDK_INT >= 34) {
                backCallback = new OnBackAnimationCallback() {
                    @Override public void onBackStarted(BackEvent e) {
                        back(0, e.getTouchX(), e.getProgress(), e.getSwipeEdge() == BackEvent.EDGE_RIGHT ? 1 : 0);
                    }
                    @Override public void onBackProgressed(BackEvent e) {
                        back(1, e.getTouchX(), e.getProgress(), e.getSwipeEdge() == BackEvent.EDGE_RIGHT ? 1 : 0);
                    }
                    @Override public void onBackInvoked() { back(2, 0, 1, 0); }
                    @Override public void onBackCancelled() { back(3, 0, 0, 0); }
                };
            } else {
                backCallback = (OnBackInvokedCallback) () -> back(2, 0, 1, 0);
            }
            dispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    (OnBackInvokedCallback) backCallback);
        } else if (backCallback != null) {
            dispatcher.unregisterOnBackInvokedCallback((OnBackInvokedCallback) backCallback);
            backCallback = null;
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (Host.backWanted) back(2, 0, 1, 0);
        else super.onBackPressed();
    }

    private static boolean night(Configuration c) {
        return (c.uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        Host.holdUpright(this);
        if (c.fontScale != Host.fontScale) {
            Host.fontScale = c.fontScale;
            Host.requestFrame();
        }
        boolean dark = night(c);
        if (dark != Host.systemDark) {
            Host.systemDark = dark;
            Host.requestFrame();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        Host.resumed = false;
        Host.tellAppState();
    }

    @Override
    protected void onResume() {
        super.onResume();
        Host.activity = this;
        Host.view = mbView;
        Native.overOther(false);
        if (mbView != null) mbView.onResume();
        setBackEnabled(Host.backWanted);
        Host.resumed = true;
        Host.tellAppState();
    }

    @Override
    public void onWindowFocusChanged(boolean has) {
        super.onWindowFocusChanged(has);
        Host.focused = has;
        Host.windowFocused(has);
        Host.tellAppState();
    }
}
