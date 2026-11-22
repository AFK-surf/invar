package dev.invar;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.icu.text.BreakIterator;
import android.opengl.EGL14;
import android.opengl.EGLExt;
import android.opengl.GLSurfaceView;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.PerformanceHintManager;
import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.util.Log;
import android.view.ActionMode;
import android.view.Choreographer;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * Invar Android GLSurfaceView host: runs Pion logic on a dedicated logic
 * HandlerThread while the GL thread draws double-buffered frames via FrameMail
 * and paces refreshes via FramePacer (including IME inset animation sync).
 */
public class MBView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private static boolean started;
    private static int setUp;
    private int mine;
    final float density;

    static boolean ONE;
    private static boolean decided;

    private static HandlerThread logicThread;
    private static Handler logic;
    private static int logicTid;

    public MBView(Context c) {
        super(c);
        density = Device.density(c);
        if (!decided) {
            decided = true;
            ONE = Host.shotAt >= 0 || System.getenv("INVAR_DL") != null;
            if (!ONE) {
                logicThread = new HandlerThread("invar-logic", android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
                logicThread.start();
                logic = new Handler(logicThread.getLooper());
                logic.post(() -> logicTid = android.os.Process.myTid());
            }
        }
        setEGLContextClientVersion(3);
        setEGLConfigChooser(8, 8, 8, 8, 0, 0);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        setZOrderOnTop(true);
        setPreserveEGLContextOnPause(true);
        setRenderer(this);
        setRenderMode(RENDERMODE_WHEN_DIRTY);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    private A11y a11y;

    A11y a11y() {
        if (a11y == null) a11y = new A11y(this);
        return a11y;
    }

    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        if (!A11y.isScreenReaderOn()) return null;
        return a11y != null && a11y.any() ? a11y : super.getAccessibilityNodeProvider();
    }

    @Override
    protected boolean dispatchHoverEvent(MotionEvent e) {
        if (A11y.isScreenReaderOn() && a11y != null && a11y.any() && a11y.hover(e)) return true;
        return super.dispatchHoverEvent(e);
    }

    private volatile boolean glAlive, paused;

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        glAlive = false;
        super.surfaceDestroyed(holder);
    }

    @Override
    public void onPause() {
        paused = true;
        lastSettleNs = 0L;
        if (settleScheduled.getAndSet(false)) {
            Host.main.post(() -> Choreographer.getInstance().removeFrameCallback(settleVsync));
        }
        pacer.setImeMoving(false);
        trim();
        if (!ONE && logic != null) logic.post(this::closeHints);
        else queueEvent(this::closeHints);
        super.onPause();
    }

    @Override
    public void onResume() {
        super.onResume();
        paused = false;
        if (!ONE && logic != null) logic.post(this::wakeHints);
        else queueEvent(this::wakeHints);
        wantFrame();
    }

    void trim() {
        queueEvent(() -> {
            if (glCurrent()) Native.trim();
        });
    }

    static void onApp(Runnable r) {
        if (logic != null) {
            logic.post(r);
            return;
        }
        MBView v = Host.view;
        if (v != null) v.queueEvent(r);
    }

    void toApp(Runnable r) {
        onApp(r);
        wantFrame();
    }

    private static final FrameMail<MBText.Placed> mail = new FrameMail<>(new FrameMail.Frames<MBText.Placed>() {
        @Override public boolean publish() { return Native.publish(); }
        @Override public boolean take(boolean shown) { return Native.take(shown); }
        @Override public void dropPending() { Native.dropPending(); }
        @Override public String frameName() { return MBText.takeFrameName(); }
        @Override public void dropped(List<MBText.Placed> glyphs) {
            for (MBText.Placed g : glyphs) g.release();
        }
        @Override public void failed(RuntimeException e) {
            Host.log("gl: " + e);
            Log.e("invar", "gl task", e);
        }
    });

    static void toRender(Runnable r) {
        MBView v = Host.view;
        if (ONE) {
            if (v != null) v.queueEvent(r);
            return;
        }
        mail.task(r);
        if (v != null) v.queueEvent(MBView::glTasksDue);
    }

    static void toRetire(Runnable r) {
        if (ONE) {
            MBView v = Host.view;
            if (v != null) v.queueEvent(r);
            return;
        }
        mail.retire(r);
    }

    private static void glTasksDue() {
        if (!glCurrent()) return;
        mail.runTasks(mail.pendingSeqNow());
    }

    private static boolean glCurrent() {
        return EGL14.eglGetCurrentContext() != EGL14.EGL_NO_CONTEXT;
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        drawingThread();
        boolean lost = Native.surfaceCreated();
        Images.textureMostRead();
        mine = ++setUp;
        glAlive = true;
        if (ONE) {
            MBText.reset();
            if (!started) {
                started = true;
                if (Host.shotAt >= 0) Native.ownClock();
                Native.start();
            }
            return;
        }
        contextMade(lost);
        if (!started) {
            started = true;
            Native.twoThreads();
            logic.post(Native::start);
        }
        fresh = 1000;
    }

    private void contextMade(boolean lost) {
        long gen = mail.contextMade();
        logic.post(() -> {
            MBText.reset();
            if (lost) {
                Native.imagesUntold();
                Images.reload();
            }
            mail.contextTold(gen);
            pacer.appWanted.set(true);
            wantFrame();
        });
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int w, int h) {
        glAlive = true;
        Native.resize(w, h);
        if (ONE) {
            pacer.sayRate(false);
            return;
        }
        logic.post(() -> pacer.sayRate(false));
        fresh = Math.max(fresh, 100);
        wantFrame();
    }

    private int fresh;

    private final FramePacer pacer = new FramePacer(new FramePacer.Platform() {
        @Override public long nanoTime() { return System.nanoTime(); }
        @Override public boolean onMain() { return Looper.myLooper() == Looper.getMainLooper(); }
        @Override public void postMain(Runnable r) { Host.main.post(r); }
        @Override public void postVsync() { postVsync.run(); }
        @Override public void atNextVsyncOnDrawing(Runnable r) {
            Host.main.post(() -> Choreographer.getInstance().postFrameCallback(t -> onApp(r)));
        }
        @Override public void requestRender() { makeFrame(); }
        @Override public boolean setRate(float rate) { return surfaceRate(rate); }
        @Override public boolean typing() { return MBView.typing(); }
    });

    private final AtomicBoolean making = new AtomicBoolean();

    private void makeFrame() {
        if (ONE) {
            requestRender();
            return;
        }
        if (making.compareAndSet(false, true)) logic.post(this::frameOnApp);
    }

    private double shotClock;
    private int shotFrames;
    private volatile boolean shotTaken;

    private void shotFrame(float w, float h) {
        double t = shotClock;
        takeBacks(Long.MAX_VALUE);
        Native.frame(w, h, density, t, 0);
        if (shotTaken) return;
        shotFrames++;
        if (Native.awaiting() > 0) return;
        if (t >= Host.shotAt) {
            shotTaken = true;
            final String said = "SHOT " + shotFrames + " " + t;
            Runnable say = () -> Choreographer.getInstance().postFrameCallback(a ->
                    Choreographer.getInstance().postFrameCallback(b -> Host.log(said)));
            Host.main.post(() -> {
                if (hasWindowFocus()) {
                    say.run();
                    return;
                }
                getViewTreeObserver().addOnWindowFocusChangeListener(new ViewTreeObserver.OnWindowFocusChangeListener() {
                    @Override
                    public void onWindowFocusChanged(boolean focused) {
                        if (!focused) return;
                        getViewTreeObserver().removeOnWindowFocusChangeListener(this);
                        say.run();
                    }
                });
            });
            return;
        }
        shotClock = shotFrames * (1000.0 / 60.0);
        wantFrame();
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        if (Host.view != this) return;
        if (!ONE) {
            drawOnGl();
            return;
        }
        boolean app = pacer.appWanted.getAndSet(false), pictured = pacer.picturesOnly.getAndSet(false);
        if (mine != setUp) {
            Native.surfaceCreated();
            MBText.reset();
            Native.resize(getWidth(), getHeight());
            mine = ++setUp;
            app = true;
        }
        float w = getWidth() / density, h = Math.max(1, getHeight() - Host.imeBottom) / density;
        if (Host.shotAt >= 0) {
            shotFrame(w, h);
            return;
        }
        long[] at = pacer.vsyncAt();
        if (pictured && !app) {
            Native.redraw();
            showAt(at[1]);
            pacer.redrawn();
            return;
        }
        long began = System.nanoTime();
        FramePacer.Begun b = pacer.begin(at, began);
        boolean later = takeBacks(b.vsync != 0 ? b.vsync : began);
        wakeFor = -1;
        buildingFor = b.now;
        pacedRate = 0;
        boolean more = Native.frame(w, h, density, b.now, b.late);
        buildingFor = -1;
        showAt(b.shows);
        pacer.ended(more, later, b.following, pacedRate);
        frameWorked(System.nanoTime() - began, 0);
    }

    private void frameOnApp() {
        making.set(false);
        if (Host.view != this || paused) return;
        boolean app = pacer.appWanted.getAndSet(false), pictured = pacer.picturesOnly.getAndSet(false);
        final int imeUsed = Host.imeBottom;
        float w = getWidth() / density, h = Math.max(1, getHeight() - imeUsed) / density;
        long[] at = pacer.vsyncAt();
        if (pictured && !app) {
            if (mail.redrawAsked(at[1])) requestRender();
            pacer.redrawn();
            return;
        }
        long began = System.nanoTime();
        FramePacer.Begun b = pacer.begin(at, began);
        boolean later = takeBacks(b.vsync != 0 ? b.vsync : began);
        wakeFor = -1;
        buildingFor = b.now;
        pacedRate = 0;
        boolean more = Native.frame(w, h, density, b.now, b.late);
        buildingFor = -1;
        handOver(b.shows);
        pacer.ended(more, later, b.following, pacedRate);
        frameWorked(System.nanoTime() - began, glWorked);
        if (Host.imeBottom != imeUsed && Host.view == this && !paused) {
            pacer.appWanted.set(true);
            makeFrame();
        }
    }

    private void handOver(long shows) {
        if (mail.handOver(shows, MBText.placed)) requestRender();
    }

    /** VSync-paced settle wakeup for adaptive glass (main thread Choreographer -> GLThread requestRender, capped <= 60 Hz). */
    private final AtomicBoolean settleScheduled = new AtomicBoolean();
    private volatile long lastSettleNs;
    private final Choreographer.FrameCallback settleVsync = this::onSettleVsync;

    private void onSettleVsync(long t) {
        if (paused || Host.view != this || !Native.adaptiveSettling()) {
            settleScheduled.set(false);
            return;
        }
        if (t - lastSettleNs < 15_000_000L) {
            Choreographer.getInstance().postFrameCallback(settleVsync);
            return;
        }
        settleScheduled.set(false);
        lastSettleNs = t;
        requestRender();
    }

    private void scheduleAdaptiveSettle() {
        if (paused) return;
        if (settleScheduled.compareAndSet(false, true)) {
            Host.main.post(() -> {
                if (paused || Host.view != this) {
                    settleScheduled.set(false);
                    return;
                }
                Choreographer.getInstance().postFrameCallback(settleVsync);
            });
        }
    }

    private volatile long glWorked;
    private boolean swapPending;
    private long swapT0, swapD0, swapEnd;
    private int swapDropped, swapCtx;
    private final Runnable afterSwap = this::finishSwap;

    private void finishSwap() {
        if (!swapPending) return;
        swapPending = false;
        if (paused || mine != swapCtx || !glCurrent()) return;
        long s1 = System.nanoTime();
        glWorked = s1 - swapT0;
        Native.glDrawn((s1 - swapD0) / 1e6, swapDropped);
    }

    private void drawOnGl() {
        if (swapPending) {
            swapPending = false;
            if (!paused && mine == swapCtx) {
                glWorked = swapEnd - swapT0;
                Native.glDrawn((swapEnd - swapD0) / 1e6, swapDropped);
            }
        }
        if (mine != setUp) {
            Native.surfaceCreated();
            Native.resize(getWidth(), getHeight());
            mine = ++setUp;
            contextMade(true);
            fresh = Math.max(fresh, 100);
        }
        long t0 = System.nanoTime();
        mail.awaitFresh(fresh);
        fresh = 0;
        long d0 = System.nanoTime();
        FrameMail.Taken<MBText.Placed> t = mail.take();
        if (t.took) mail.runTasks(t.seq);
        if (t.took) {
            MBText.upload(t.glyphs);
            Native.draw();
        } else {
            Native.redraw();
        }
        if (Native.adaptiveSettling()) {
            if (t.took) lastSettleNs = System.nanoTime();
            scheduleAdaptiveSettle();
        }
        mail.runAll(t.retire);
        showAt(t.shows);
        if (t.ref != null) MBText.frameDrawn(t.ref);
        mail.runTasks(mail.pendingSeqNow());
        swapT0 = t0;
        swapD0 = d0;
        swapEnd = System.nanoTime();
        swapDropped = mail.takeDropped();
        swapCtx = mine;
        swapPending = true;
        queueEvent(afterSwap);
    }

    static boolean glSync() {
        MBView v = Host.view;
        if (v == null || !v.glAlive || v.paused) return false;
        final Object done = new Object();
        final boolean[] state = {false, false};
        v.queueEvent(() -> {
            synchronized (done) {
                if (state[1]) return;
                if (!glCurrent()) {
                    state[0] = true;
                    done.notify();
                    return;
                }
                FrameMail.Taken<MBText.Placed> t = mail.takeForSync();
                if (t.took) {
                    mail.runTasks(t.seq);
                    MBText.upload(t.glyphs);
                }
                Native.glSync();
                state[0] = true;
                done.notify();
            }
            v.requestRender();
        });
        synchronized (done) {
            long until = System.nanoTime() + 2_000_000_000L;
            for (long left = 2000; !state[0] && left > 0; left = (until - System.nanoTime()) / 1_000_000L) {
                try {
                    done.wait(left);
                } catch (InterruptedException e) {
                    break;
                }
            }
            if (!state[0]) state[1] = true;
            return state[0];
        }
    }

    // ---- ADPF thread hints & display rate ----

    private void drawingThread() {
        try {
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY);
        } catch (RuntimeException e) {
            Host.log("drawing thread priority: " + e);
        }
        int tid = android.os.Process.myTid();
        lastGlTid = tid;
        if (ONE) {
            hintsFor(new int[] {tid});
            return;
        }
        logic.post(() -> hintsFor(new int[] {logicTid, tid}));
    }

    private void closeHints() {
        if (Build.VERSION.SDK_INT >= 31 && hints != null) {
            hints.close();
            hints = null;
        }
        hintTids = null;
    }

    private void wakeHints() {
        if (ONE) {
            if (lastGlTid > 0) hintsFor(new int[] {lastGlTid});
        } else if (logicTid > 0) {
            int gl = lastGlTid;
            hintsFor(gl > 0 && glAlive ? new int[] {logicTid, gl} : new int[] {logicTid});
        }
        if (Build.VERSION.SDK_INT >= 31 && hints != null) {
            long target = pacer.framePeriod * 7 / 10;
            if (target > 0 && target != hintTarget) {
                hintTarget = target;
                hints.updateTargetWorkDuration(target);
            }
            if (hintTarget > 0) hints.reportActualWorkDuration(hintTarget);
        }
    }

    private void hintsFor(int[] tids) {
        if (Build.VERSION.SDK_INT < 31 || Arrays.equals(tids, hintTids)) return;
        if (hints != null) hints.close();
        hints = null;
        hintTids = tids;
        PerformanceHintManager pm = getContext().getSystemService(PerformanceHintManager.class);
        long target = pacer.framePeriod * 7 / 10;
        try {
            hints = pm == null ? null : pm.createHintSession(tids, target);
        } catch (RuntimeException e) {
            Host.log("drawing thread hints: " + e);
        }
        hintTarget = target;
    }

    private PerformanceHintManager.Session hints;
    private int[] hintTids;
    private long hintTarget;
    private volatile int lastGlTid;

    double framePeriodMs() {
        return pacer.framePeriod / 1e6;
    }

    private final DisplayManager.DisplayListener displays = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) {}
        @Override public void onDisplayRemoved(int id) {}
        @Override public void onDisplayChanged(int id) { readFramePeriod(); }
    };

    private void readFramePeriod() {
        Display d = getDisplay();
        if (d != null && d.getRefreshRate() > 1) pacer.framePeriod = (long) (1e9 / d.getRefreshRate());
        if (d == null) return;
        Display.Mode m = d.getMode();
        float top = 0;
        for (Display.Mode o : d.getSupportedModes()) {
            if (o.getPhysicalWidth() == m.getPhysicalWidth() && o.getPhysicalHeight() == m.getPhysicalHeight()) {
                top = Math.max(top, o.getRefreshRate());
            }
        }
        pacer.topRate = top;
    }

    private boolean surfaceRate(float rate) {
        if (Build.VERSION.SDK_INT < 30) return false;
        Surface s = getHolder().getSurface();
        if (s == null || !s.isValid()) return false;
        try {
            s.setFrameRate(rate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            return true;
        } catch (RuntimeException e) {
            Host.log("display rate: " + e);
            return false;
        }
    }

    private static boolean typing() {
        return Host.imeActive || Host.imeVisible;
    }

    void typingChanged() {
        onApp(pacer::typingChanged);
    }

    void setImeMoving(boolean moving) {
        pacer.setImeMoving(moving);
    }

    void imeMovingStarted() {
        if (paused) return;
        if (!ONE && logic != null) {
            logic.post(() -> {
                wakeHints();
                pacer.sayRate(true);
            });
        } else {
            queueEvent(() -> {
                wakeHints();
                pacer.sayRate(true);
            });
        }
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        readFramePeriod();
        DisplayManager dm = getContext().getSystemService(DisplayManager.class);
        if (dm != null) dm.registerDisplayListener(displays, Host.main);
    }

    @Override
    protected void onDetachedFromWindow() {
        DisplayManager dm = getContext().getSystemService(DisplayManager.class);
        if (dm != null) dm.unregisterDisplayListener(displays);
        super.onDetachedFromWindow();
    }

    private void frameWorked(long nanos, long gl) {
        if (Build.VERSION.SDK_INT < 31 || hints == null) return;
        nanos = Math.max(nanos, gl);
        long target = pacer.framePeriod * 7 / 10;
        if (target != hintTarget) {
            hintTarget = target;
            hints.updateTargetWorkDuration(target);
        }
        if (nanos > 0) hints.reportActualWorkDuration(nanos);
    }

    // ---- Predictive back ----

    private final ArrayDeque<double[]> backs = new ArrayDeque<>();

    void back(int kind, float x, float progress, int edge) {
        synchronized (backs) {
            backs.add(new double[] {System.nanoTime(), kind, x, progress, edge});
        }
        wantFrame();
    }

    private boolean takeBacks(long before) {
        while (true) {
            double[] b;
            synchronized (backs) {
                b = backs.peek();
                if (b == null) return false;
                if (b[0] >= before) return true;
                backs.poll();
            }
            Native.back((int) b[1], (float) b[2], (float) b[3], (int) b[4]);
        }
    }

    // ---- Vsync ----

    private final Choreographer.FrameCallback vsync = t -> pacer.vsyncCame(t, 0);
    private Object vsyncTimed;
    private final Runnable postVsync = () -> {
        if (Build.VERSION.SDK_INT >= 33) Timelines.post(this);
        else Choreographer.getInstance().postFrameCallback(vsync);
    };

    private static final class Timelines {
        static void post(MBView v) {
            if (v.vsyncTimed == null) {
                v.vsyncTimed = (Choreographer.VsyncCallback) d -> {
                    long shows = d.getPreferredFrameTimeline().getExpectedPresentationTimeNanos();
                    Choreographer.FrameTimeline[] tl = d.getFrameTimelines();
                    long period = tl.length > 1
                            ? tl[1].getExpectedPresentationTimeNanos() - tl[0].getExpectedPresentationTimeNanos() : 0;
                    if (period > 0 && Math.abs(period - v.pacer.framePeriod) * 16 > v.pacer.framePeriod) {
                        v.pacer.framePeriod = period;
                    }
                    v.pacer.vsyncCame(d.getFrameTimeNanos(), shows);
                };
            }
            Choreographer.getInstance().postVsyncCallback((Choreographer.VsyncCallback) v.vsyncTimed);
        }
    }

    private void showAt(long shows) {
        if (Build.VERSION.SDK_INT < 33) return;
        long at = shows != 0 ? shows - pacer.framePeriod / 2 : System.nanoTime();
        EGLExt.eglPresentationTimeANDROID(EGL14.eglGetCurrentDisplay(),
                EGL14.eglGetCurrentSurface(EGL14.EGL_DRAW), at);
    }

    void frameNow() {
        if (shotTaken) return;
        pacer.frameNow();
    }

    volatile double wakeFor = -1;
    volatile double buildingFor = -1;
    private float pacedRate;

    boolean framesPaced(double hz) {
        float top = pacer.topRate > 0 ? pacer.topRate : (float) (1000.0 / framePeriodMs());
        long every = hz > 0 ? Math.round(top / hz) : 1;
        if (every <= 1 || buildingFor < 0) return false;
        pacedRate = top / every;
        Host.wakeAfter((every - 0.5) * 1000.0 / top);
        return true;
    }

    void wakeAt(double at) {
        if (wakeFor != at) return;
        Choreographer.getInstance().postFrameCallback(new Choreographer.FrameCallback() {
            @Override
            public void doFrame(long t) {
                if (wakeFor != at) return;
                if ((t + pacer.framePeriod) / 1e6 < at - 0.5) {
                    Choreographer.getInstance().postFrameCallback(this);
                    return;
                }
                wakeFor = -1;
                wantFrame();
            }
        });
    }

    void wantFrame() {
        if (shotTaken) return;
        pacer.wantFrame();
    }

    void pictureFrame() {
        if (shotTaken) return;
        pacer.pictureFrame();
    }

    // ---- Touch, wheel & keyboard input ----

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_CLASS_POINTER) != 0
                && e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            final float x = e.getX() / density, y = e.getY() / density;
            final float dx = -e.getAxisValue(MotionEvent.AXIS_HSCROLL) * 48f;
            final float dy = -e.getAxisValue(MotionEvent.AXIS_VSCROLL) * 48f;
            toApp(() -> Native.wheel(x, y, dx, dy, 0));
            return true;
        }
        return super.onGenericMotionEvent(e);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int a = e.getActionMasked();
        final double t = e.getEventTime();
        switch (a) {
            case MotionEvent.ACTION_DOWN:
                pacer.touching = true;
                onApp(() -> pacer.sayRate(true));
                requestFocus();
                break;
            case MotionEvent.ACTION_UP:
                Host.lifts++;
                pacer.touching = false;
                break;
            case MotionEvent.ACTION_CANCEL:
                pacer.touching = false;
                break;
            default:
                break;
        }
        switch (a) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                final int i = e.getActionIndex();
                final int id = e.getPointerId(i);
                final float x = e.getX(i) / density, y = e.getY(i) / density;
                final int phase = (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) ? 10 : 12;
                toApp(() -> Native.touch(phase, id, x, y, t));
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                final int n = e.getPointerCount();
                final int[] ids = new int[n];
                final float[] xs = new float[n], ys = new float[n];
                for (int p = 0; p < n; p++) {
                    ids[p] = e.getPointerId(p);
                    xs[p] = e.getX(p) / density;
                    ys[p] = e.getY(p) / density;
                }
                toApp(() -> {
                    for (int p = 0; p < n; p++) Native.touch(13, ids[p], xs[p], ys[p], t);
                });
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                final int n = e.getPointerCount(), h = e.getHistorySize();
                final int[] ids = new int[n];
                final float[] xs = new float[n * (h + 1)], ys = new float[n * (h + 1)];
                final double[] ts = new double[h + 1];
                for (int k = 0; k < h; k++) ts[k] = e.getHistoricalEventTime(k);
                ts[h] = t;
                for (int p = 0; p < n; p++) {
                    ids[p] = e.getPointerId(p);
                    for (int k = 0; k < h; k++) {
                        xs[p * (h + 1) + k] = e.getHistoricalX(p, k) / density;
                        ys[p * (h + 1) + k] = e.getHistoricalY(p, k) / density;
                    }
                    xs[p * (h + 1) + h] = e.getX(p) / density;
                    ys[p * (h + 1) + h] = e.getY(p) / density;
                }
                toApp(() -> {
                    for (int k = 0; k <= h; k++) {
                        for (int p = 0; p < n; p++) {
                            final int kind = (k == h && p == n - 1) ? 11 : 14;
                            Native.touch(kind, ids[p], xs[p * (h + 1) + k], ys[p * (h + 1) + k], ts[k]);
                        }
                    }
                });
                return true;
            }
            default:
                return true;
        }
    }

    // ---- Floating text selection menu ----

    private ActionMode textMenu;
    private final Rect textMenuRect = new Rect();
    private int textMenuCan;
    String[] textMenuOwn = new String[0];
    private static final int TEXT_MENU_OWN = 0x696e0001;

    void textMenu(boolean shown, double x, double y, double w, double h, int can) {
        if (!shown) {
            if (textMenu != null) textMenu.finish();
            textMenu = null;
            return;
        }
        textMenuRect.set((int) Math.floor(x * density), (int) Math.floor(y * density),
                (int) Math.ceil((x + w) * density), (int) Math.ceil((y + h) * density));
        textMenuCan = can;
        if (textMenu != null) {
            textMenu.invalidate();
            textMenu.invalidateContentRect();
            return;
        }
        textMenu = startActionMode(new ActionMode.Callback2() {
            private void items(Menu m) {
                m.clear();
                if ((textMenuCan & 1) != 0) m.add(0, android.R.id.cut, 0, android.R.string.cut);
                if ((textMenuCan & 2) != 0) m.add(0, android.R.id.copy, 1, android.R.string.copy);
                if ((textMenuCan & 4) != 0 && clipHasText()) m.add(0, android.R.id.paste, 2, android.R.string.paste);
                if ((textMenuCan & 8) != 0) m.add(0, android.R.id.selectAll, 3, android.R.string.selectAll);
                if ((textMenuCan & 64) != 0) {
                    for (int k = 0; k < textMenuOwn.length; k++) {
                        if (!textMenuOwn[k].isEmpty()) m.add(0, TEXT_MENU_OWN + k, 4 + k, textMenuOwn[k]);
                    }
                }
            }

            @Override public boolean onCreateActionMode(ActionMode mode, Menu m) { items(m); return true; }
            @Override public boolean onPrepareActionMode(ActionMode mode, Menu m) { items(m); return true; }

            @Override public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
                int id = item.getItemId();
                if (id == android.R.id.paste) paste();
                else if (id == android.R.id.selectAll) toApp(() -> Native.key(12, 0));
                else if (id == android.R.id.copy) toApp(() -> Native.key(13, 0));
                else if (id == android.R.id.cut) toApp(() -> Native.key(14, 0));
                else if (id >= TEXT_MENU_OWN && id < TEXT_MENU_OWN + 8) {
                    int k = id - TEXT_MENU_OWN;
                    toApp(() -> Native.key(60 + k, 0));
                }
                mode.finish();
                return true;
            }

            @Override public void onDestroyActionMode(ActionMode mode) {
                if (textMenu == mode) textMenu = null;
            }

            @Override public void onGetContentRect(ActionMode mode, View v, Rect out) {
                out.set(textMenuRect);
            }
        }, ActionMode.TYPE_FLOATING);
    }

    private boolean clipHasText() {
        ClipboardManager cm = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        return cm != null && cm.hasPrimaryClip();
    }

    void paste() {
        ClipboardManager cm = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) return;
        CharSequence t = clip.getItemAt(0).coerceToText(getContext());
        if (t == null || t.length() == 0) return;
        String s = t.toString();
        toApp(() -> Native.text(8, s, 0, 0));
    }

    boolean catchKey(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.KEYCODE_BACK) return false;
        int[] k = Device.catchKey(e);
        if (k != null) {
            toApp(() -> Native.key(k[0], k[1]));
            return true;
        }
        int ch = e.getUnicodeChar();
        if (ch <= 0) return false;
        String s = new String(Character.toChars(ch));
        toApp(() -> Native.text(0, s, 0, 0));
        return true;
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        if (Device.catching && Host.imeActive && catchKey(e)) return true;
        if (code == KeyEvent.KEYCODE_PASTE || (code == KeyEvent.KEYCODE_V && e.isCtrlPressed())) {
            if (Host.imeActive) paste();
            return true;
        }
        int sk = shortcutKey(e);
        if (sk != 0) {
            int mods = Device.keyMods(e);
            toApp(() -> Native.key(sk, mods));
            return true;
        }
        int k = keyCode(code);
        if (k != 0) {
            int mods = Device.keyMods(e);
            toApp(() -> Native.key(k, mods));
            return true;
        }
        int ch = e.getUnicodeChar();
        if (ch != 0 && Host.imeActive) {
            String s = new String(Character.toChars(ch));
            toApp(() -> Native.text(0, s, 0, 0));
            return true;
        }
        return super.onKeyDown(code, e);
    }

    static int shortcutKey(KeyEvent e) {
        if (!e.isCtrlPressed() && !e.isMetaPressed()) return 0;
        int k = e.getKeyCode();
        int ch;
        if (k >= KeyEvent.KEYCODE_A && k <= KeyEvent.KEYCODE_Z) ch = 'a' + (k - KeyEvent.KEYCODE_A);
        else if (k >= KeyEvent.KEYCODE_0 && k <= KeyEvent.KEYCODE_9) ch = '0' + (k - KeyEvent.KEYCODE_0);
        else return 0;
        if (e.isCtrlPressed()) {
            if (ch == 'a') return 12;
            if (ch == 'c') return 13;
            if (ch == 'x') return 14;
        }
        return 1000 + ch;
    }

    static int keyCode(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DEL: return 1;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 2;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 3;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 4;
            case KeyEvent.KEYCODE_DPAD_UP: return 5;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 6;
            case KeyEvent.KEYCODE_ENTER: return 7;
            case KeyEvent.KEYCODE_MOVE_HOME: return 8;
            case KeyEvent.KEYCODE_MOVE_END: return 9;
            default: return 0;
        }
    }

    @Override
    public boolean onCheckIsTextEditor() {
        return Host.imeActive;
    }

    MBInput input;

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        if (Device.catching) {
            out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
            out.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_NONE;
            out.initialSelStart = 0;
            out.initialSelEnd = 0;
            input = null;
            return new CatchInput(this);
        }
        out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        out.imeOptions = EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_ENTER_ACTION
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        if (Device.imeAction == 1) {
            out.inputType = InputType.TYPE_CLASS_TEXT;
            out.imeOptions = EditorInfo.IME_ACTION_SEARCH | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        } else if (Device.imeAction == 2) {
            out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI;
            out.imeOptions = EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        } else if (Device.imeAction == 3) {
            out.inputType = InputType.TYPE_CLASS_NUMBER;
            out.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        } else if (Device.imeAction == 4) {
            out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
            out.imeOptions = EditorInfo.IME_ACTION_NONE | EditorInfo.IME_FLAG_NO_ENTER_ACTION
                    | EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
                    | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        } else if (Device.imeAction == 5) {
            out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS;
        } else if (Device.imeAction == 6) {
            out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        }
        out.initialSelStart = Host.selStart;
        out.initialSelEnd = Host.selEnd;
        input = new MBInput(this);
        return input;
    }

    static class CatchInput extends BaseInputConnection {
        final MBView view;
        volatile boolean composing;
        volatile String composingText = "";

        CatchInput(MBView v) {
            super(v, false);
            view = v;
        }

        @Override
        public boolean commitText(CharSequence text, int pos) {
            String s = text.toString();
            composing = false;
            composingText = "";
            if (s.equals("\n")) view.toApp(() -> Native.key(7, 0));
            else view.toApp(() -> Native.text(0, s, 0, 0));
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int pos) {
            String s = text.toString();
            composingText = s;
            composing = s.length() > 0;
            view.toApp(() -> Native.text(1, s, s.length(), 0));
            return true;
        }

        @Override
        public boolean finishComposingText() {
            if (composing) {
                composing = false;
                composingText = "";
                view.toApp(() -> Native.text(2, "", 0, 0));
            }
            return true;
        }

        @Override
        public boolean setSelection(int start, int end) {
            view.toApp(() -> Native.text(3, "", start, end));
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int before, int after) {
            if (before > 0 || after > 0) view.toApp(() -> Native.text(4, "", before, after));
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent e) {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
            if (Device.catching) {
                view.catchKey(e);
                return true;
            }
            if (e.getKeyCode() == KeyEvent.KEYCODE_PASTE) {
                view.paste();
                return true;
            }
            int k = keyCode(e.getKeyCode());
            if (k == 0) k = shortcutKey(e);
            if (k != 0) {
                int kk = k, mods = Device.keyMods(e);
                view.toApp(() -> Native.key(kk, mods));
                return true;
            }
            int ch = e.getUnicodeChar();
            if (ch != 0) {
                String s = new String(Character.toChars(ch));
                view.toApp(() -> Native.text(0, s, 0, 0));
            }
            return true;
        }

        @Override
        public boolean performContextMenuAction(int id) {
            if (id == android.R.id.paste || id == android.R.id.pasteAsPlainText) {
                view.paste();
                return true;
            }
            if (Device.catching) return false;
            if (id == android.R.id.selectAll) view.toApp(() -> Native.key(12, 0));
            else if (id == android.R.id.copy) view.toApp(() -> Native.key(13, 0));
            else if (id == android.R.id.cut) view.toApp(() -> Native.key(14, 0));
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            view.toApp(() -> Native.key(7, 0));
            return true;
        }

        @Override
        public CharSequence getTextBeforeCursor(int n, int flags) {
            String t = Host.imeText;
            int a = Math.max(0, Math.min(Host.selStart, t.length()));
            return t.substring(Math.max(0, a - n), a);
        }

        @Override
        public CharSequence getTextAfterCursor(int n, int flags) {
            String t = Host.imeText;
            int b = Math.max(0, Math.min(Host.selEnd, t.length()));
            return t.substring(b, Math.min(t.length(), b + n));
        }

        @Override
        public CharSequence getSelectedText(int flags) {
            String t = Host.imeText;
            int a = Math.max(0, Math.min(Host.selStart, t.length()));
            int b = Math.max(a, Math.min(Host.selEnd, t.length()));
            return a == b ? null : t.substring(a, b);
        }
    }

    static final class MBInput extends BaseInputConnection {
        final MBView view;
        final Editable ed;
        private int batch;
        String saidText;
        int saidA, saidB, saidCs = -1, saidCe = -1;

        MBInput(MBView v) {
            super(v, true);
            view = v;
            String t = Host.imeText;
            ed = Editable.Factory.getInstance().newEditable(t);
            int a = Math.max(0, Math.min(Host.selStart, t.length()));
            int b = Math.max(0, Math.min(Host.selEnd, t.length()));
            Selection.setSelection(ed, a, b);
            saidText = t;
            saidA = a;
            saidB = b;
        }

        @Override public Editable getEditable() { return ed; }

        @Override
        public boolean beginBatchEdit() {
            batch++;
            return true;
        }

        @Override
        public boolean endBatchEdit() {
            if (batch > 0) batch--;
            if (batch == 0) edited();
            return batch > 0;
        }

        void edited() {
            if (batch > 0) return;
            String t = ed.toString();
            int a = Selection.getSelectionStart(ed), b = Selection.getSelectionEnd(ed);
            int cs = getComposingSpanStart(ed), ce = getComposingSpanEnd(ed);
            if (cs > ce) { int x = cs; cs = ce; ce = x; }
            if (t.equals(saidText) && a == saidA && b == saidB && cs == saidCs && ce == saidCe) return;
            saidText = t;
            saidA = a;
            saidB = b;
            saidCs = cs;
            saidCe = ce;
            final int seq = ++Host.imeSent;
            final int fa = a, fb = b, fcs = cs, fce = ce;
            view.toApp(() -> {
                Native.text(6, "", fcs, fce);
                Native.text(5, t, fa, fb);
                Host.imeTaken = seq;
            });
            InputMethodManager imm = imm();
            if (imm != null) imm.updateSelection(view, a, b, cs, ce);
        }

        InputMethodManager imm() {
            return (InputMethodManager) view.getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        }

        boolean took(String text, int a, int b) {
            int cs = saidCs, ce = saidCe;
            String has = saidText;
            int ha = saidA, hb = saidB;
            boolean comp = cs >= 0 && ce > cs && ha == hb && ha >= cs && ha <= ce;
            if (comp) {
                has = saidText.substring(0, cs) + saidText.substring(ce);
                ha = cs;
                hb = cs;
            }
            if (text.equals(has) && Math.min(a, b) == Math.min(ha, hb) && Math.max(a, b) == Math.max(ha, hb)) return false;
            removeComposingSpans(ed);
            ed.replace(0, ed.length(), text);
            int n = text.length();
            Selection.setSelection(ed, Math.max(0, Math.min(a, n)), Math.max(0, Math.min(b, n)));
            saidText = text;
            saidA = Selection.getSelectionStart(ed);
            saidB = Selection.getSelectionEnd(ed);
            saidCs = -1;
            saidCe = -1;
            return true;
        }

        @Override
        public boolean commitText(CharSequence text, int pos) {
            if (text.toString().equals("\n")) {
                finishComposingText();
                view.toApp(() -> Native.key(7, 0));
                return true;
            }
            return super.commitText(text, pos);
        }

        @Override
        public boolean finishComposingText() {
            boolean r = super.finishComposingText();
            edited();
            return r;
        }

        @Override
        public boolean setComposingRegion(int start, int end) {
            boolean r = super.setComposingRegion(start, end);
            edited();
            return r;
        }

        @Override
        public boolean setSelection(int start, int end) {
            boolean r = super.setSelection(start, end);
            edited();
            return r;
        }

        @Override
        public boolean deleteSurroundingText(int before, int after) {
            boolean r = super.deleteSurroundingText(before, after);
            edited();
            return r;
        }

        @Override
        public boolean deleteSurroundingTextInCodePoints(int before, int after) {
            boolean r = super.deleteSurroundingTextInCodePoints(before, after);
            edited();
            return r;
        }

        void deleteKey(boolean back) {
            removeComposingSpans(ed);
            int a = Selection.getSelectionStart(ed), b = Selection.getSelectionEnd(ed);
            if (a > b) { int x = a; a = b; b = x; }
            if (a == b) {
                BreakIterator g = BreakIterator.getCharacterInstance();
                g.setText(ed.toString());
                if (back) {
                    if (a == 0) { edited(); return; }
                    a = g.preceding(a);
                } else {
                    if (b >= ed.length()) { edited(); return; }
                    b = g.following(b);
                }
            }
            ed.delete(a, b);
            Selection.setSelection(ed, a);
            edited();
        }

        @Override
        public boolean sendKeyEvent(KeyEvent e) {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
            if (e.getKeyCode() == KeyEvent.KEYCODE_PASTE) {
                view.paste();
                return true;
            }
            if ((e.getKeyCode() == KeyEvent.KEYCODE_DEL || e.getKeyCode() == KeyEvent.KEYCODE_FORWARD_DEL)
                    && !e.isShiftPressed() && !e.isCtrlPressed() && !e.isAltPressed()) {
                deleteKey(e.getKeyCode() == KeyEvent.KEYCODE_DEL);
                return true;
            }
            int k = keyCode(e.getKeyCode());
            if (k != 0) {
                int mods = Device.keyMods(e);
                view.toApp(() -> Native.key(k, mods));
                return true;
            }
            int ch = e.getUnicodeChar();
            if (ch != 0) super.commitText(new String(Character.toChars(ch)), 1);
            return true;
        }

        @Override
        public boolean performContextMenuAction(int id) {
            if (id == android.R.id.paste || id == android.R.id.pasteAsPlainText) view.paste();
            else if (id == android.R.id.selectAll) view.toApp(() -> Native.key(12, 0));
            else if (id == android.R.id.copy) view.toApp(() -> Native.key(13, 0));
            else if (id == android.R.id.cut) view.toApp(() -> Native.key(14, 0));
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            view.toApp(() -> Native.key(7, 0));
            return true;
        }

        @Override
        public ExtractedText getExtractedText(ExtractedTextRequest req, int flags) {
            ExtractedText x = new ExtractedText();
            x.text = ed.toString();
            x.startOffset = 0;
            x.partialStartOffset = -1;
            x.partialEndOffset = -1;
            x.selectionStart = Selection.getSelectionStart(ed);
            x.selectionEnd = Selection.getSelectionEnd(ed);
            x.flags = 0;
            return x;
        }
    }
}
