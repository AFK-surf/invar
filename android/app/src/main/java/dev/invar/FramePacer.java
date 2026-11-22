package dev.invar;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Choreographer + IME animation frame pacer and display refresh-rate controller
 * (kept independent of Android framework types so FramePacerTest can run it on
 * a plain JVM with a deterministic simulated clock).
 */
final class FramePacer {
    interface Platform {
        long nanoTime();
        boolean onMain();
        void postMain(Runnable r);
        void postVsync();
        void atNextVsyncOnDrawing(Runnable r);
        void requestRender();
        boolean setRate(float rate);
        boolean typing();
    }

    private final Platform p;

    FramePacer(Platform p) {
        this.p = p;
    }

    volatile long framePeriod = 16_666_667L;
    volatile float topRate;
    volatile boolean touching;

    private volatile long[] vsyncAt = {0, 0};
    private final AtomicBoolean wanted = new AtomicBoolean();
    private final AtomicBoolean ahead = new AtomicBoolean();
    final AtomicBoolean appWanted = new AtomicBoolean(), picturesOnly = new AtomicBoolean();

    private double lastFrame;
    private long lastVsync;
    private boolean askedAtEnd;

    // Main-thread state for coalescing CALLBACK_ANIMATION (vsyncCame) with
    // CALLBACK_INSETS_ANIMATION (frameNow) into one frame per vsync.
    private boolean imeMoving;
    private boolean deferredVsync;
    private long deferredVsyncT, deferredVsyncShows;
    private long lastFrameNowAt;

    private void armImeVsync() {
        if (imeMoving && wanted.compareAndSet(false, true)) {
            if (p.onMain()) p.postVsync();
            else p.postMain(p::postVsync);
        }
    }

    /** Whether the soft keyboard inset animation is currently running (main thread). */
    void setImeMoving(boolean moving) {
        imeMoving = moving;
        if (moving) armImeVsync();
        else if (deferredVsync) flushDeferredVsync();
    }

    long[] vsyncAt() {
        return vsyncAt;
    }

    private void dispatchVsync(long t, long shows) {
        vsyncAt = new long[] {t, shows};
        if (appWanted.get() || picturesOnly.get()) {
            wanted.set(false);
            p.requestRender();
            return;
        }
        ahead.set(true);
        wanted.set(false);
        if ((appWanted.get() || picturesOnly.get()) && ahead.getAndSet(false)) p.requestRender();
    }

    private void flushDeferredVsync() {
        if (!deferredVsync) return;
        deferredVsync = false;
        dispatchVsync(deferredVsyncT, deferredVsyncShows);
    }

    void vsyncCame(long t, long shows) {
        if (lastFrameNowAt != 0 && t <= lastFrameNowAt && lastFrameNowAt - t < framePeriod / 2) {
            wanted.set(false);
            return;
        }
        if (imeMoving) {
            deferredVsyncT = t;
            deferredVsyncShows = shows;
            deferredVsync = true;
            p.postMain(this::flushDeferredVsync);
            return;
        }
        dispatchVsync(t, shows);
    }

    private void askAhead() {
        ahead.set(false);
        if (wanted.compareAndSet(false, true)) p.postMain(p::postVsync);
    }

    private void ask() {
        if (ahead.getAndSet(false) && p.nanoTime() - vsyncAt[0] < framePeriod) {
            p.requestRender();
            return;
        }
        if (!wanted.compareAndSet(false, true)) return;
        if (p.onMain()) p.postVsync();
        else p.postMain(p::postVsync);
    }

    void wantFrame() {
        appWanted.set(true);
        ask();
    }

    void pictureFrame() {
        picturesOnly.set(true);
        ask();
    }

    void frameNow() {
        appWanted.set(true);
        ahead.set(false);
        long now = p.nanoTime();
        lastFrameNowAt = now;
        if (deferredVsync) {
            deferredVsync = false;
            vsyncAt = new long[] {deferredVsyncT, deferredVsyncShows};
            wanted.set(false);
        } else {
            vsyncAt = new long[] {now, 0};
        }
        p.requestRender();
        armImeVsync();
    }

    void redrawn() {
        askedAtEnd = false;
    }

    static final class Begun {
        final long vsync, shows;
        final double now, late;
        final boolean following;

        Begun(long vsync, long shows, double now, double late, boolean following) {
            this.vsync = vsync;
            this.shows = shows;
            this.now = now;
            this.late = late;
            this.following = following;
        }
    }

    Begun begin(long[] at, long began) {
        if (askedAtEnd) askAhead();
        long v = at[0];
        double now = v != 0 ? v / 1e6 : began / 1e6;
        if (now < lastFrame) now = lastFrame;
        lastFrame = now;
        double late = 0;
        if (v != 0 && lastVsync != 0 && askedAtEnd) {
            long frames = Math.round((double) (v - lastVsync) / framePeriod);
            if (frames > 1) late = (frames - 1) * framePeriod / 1e6;
        }
        boolean following = v > lastVsync && lastVsync != 0 && v - lastVsync < framePeriod * 3 / 2;
        lastVsync = v;
        return new Begun(v, at[1], now, late, following);
    }

    void ended(boolean more, boolean later, boolean following, float paced) {
        askedAtEnd = more || later;
        drawn++;
        if (paced > 0 && !more && !later && !touching && !p.typing()) {
            moved = drawn;
            sayRate(paced);
        } else if (more || later || following) {
            moved = drawn;
            sayRate(true);
        } else if (p.typing()) {
            sayRate(true);
        } else if (rateSaid != 0) {
            stillAfter(moved);
        }
        if (more || later) wantFrame();
        else ahead.set(false);
    }

    private float rateSaid;
    private int drawn;
    private int moved;
    private int stillFrom = -1;

    float rateSaid() {
        return rateSaid;
    }

    void sayRate(boolean on) {
        if (on && topRate <= 0) return;
        sayRate(on ? topRate : 0);
    }

    private void sayRate(float rate) {
        if (rate == rateSaid) return;
        if (p.setRate(rate)) rateSaid = rate;
    }

    private void stillAfter(int at) {
        if (stillFrom == at) return;
        stillFrom = at;
        stillFor(at, Math.max(1, Math.round(topRate)));
    }

    private void stillFor(int at, int left) {
        p.atNextVsyncOnDrawing(() -> {
            if (moved != at || appWanted.get() || touching || p.typing() || rateSaid == 0) {
                if (stillFrom == at) stillFrom = -1;
                return;
            }
            if (left > 1) {
                stillFor(at, left - 1);
                return;
            }
            stillFrom = -1;
            sayRate(false);
        });
    }

    void typingChanged() {
        if (p.typing()) sayRate(true);
        else if (rateSaid != 0) stillAfter(moved);
    }
}
