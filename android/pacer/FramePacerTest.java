package dev.invar;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.PriorityQueue;

/**
 * Pure-JDK deterministic unit tests for FramePacer: simulates display vsyncs,
 * main-thread Choreographer callbacks, drawing thread, GPU queue, and
 * SurfaceFlinger buffer presentation, including IME inset animation coalescing.
 */
public final class FramePacerTest {
    interface App {
        double work(int frame, double now);
        double gpu(int frame, double now);
        boolean more(int frame, double now);

        default float paced(int frame, double now) {
            return 0;
        }
    }

    static final class Event implements Comparable<Event> {
        final long at;
        final long seq;
        final Runnable run;

        Event(long at, long seq, Runnable run) {
            this.at = at;
            this.seq = seq;
            this.run = run;
        }

        @Override
        public int compareTo(Event o) {
            return at != o.at ? Long.compare(at, o.at) : Long.compare(seq, o.seq);
        }
    }

    static final class Phone implements FramePacer.Platform {
        final long period;
        final int buffers;
        final boolean swapWaitsGpu;
        final long wake;
        final FramePacer pacer;
        App app;
        boolean typing;

        long now;
        long seq;
        final PriorityQueue<Event> events = new PriorityQueue<>();
        boolean onMain;
        List<Runnable> nextVsync = new ArrayList<>();
        Runnable onInsets;
        int vsyncPosts;
        long drawingFree;
        final ArrayDeque<Runnable> drawingQueue = new ArrayDeque<>();
        boolean renderRequested;
        boolean drawingScheduled;
        long gpuFree;
        long gpuLastEnd;
        final ArrayDeque<long[]> queued = new ArrayDeque<>();
        int frames;
        final List<Long> shownAt = new ArrayList<>();
        final List<double[]> drawn = new ArrayList<>();
        final List<double[]> rates = new ArrayList<>();
        float rate;

        Phone(double hz, int buffers, boolean swapWaitsGpu, long wake) {
            this.period = Math.round(1e9 / hz);
            this.buffers = buffers;
            this.swapWaitsGpu = swapWaitsGpu;
            this.wake = wake;
            this.pacer = new FramePacer(this);
            pacer.framePeriod = period;
            pacer.topRate = (float) hz;
            at(period, this::vsync);
        }

        void at(long t, Runnable r) {
            events.add(new Event(t, seq++, r));
        }

        void main(long t, Runnable r) {
            at(t, () -> {
                onMain = true;
                r.run();
                onMain = false;
            });
        }

        void runUntil(long end) {
            while (!events.isEmpty() && events.peek().at <= end) {
                Event e = events.poll();
                now = e.at;
                e.run.run();
            }
            now = end;
        }

        void vsync() {
            long v = now;
            if (!queued.isEmpty()) {
                long[] b = queued.peek();
                if (b[0] <= v && b[1] <= v) {
                    queued.poll();
                    shownAt.add(v / period);
                    bufferFreed();
                }
            }
            List<Runnable> cbs = nextVsync;
            nextVsync = new ArrayList<>();
            onMain = true;
            for (Runnable r : cbs) r.run();
            if (onInsets != null) onInsets.run();
            onMain = false;
            at(v + period, this::vsync);
        }

        long vsyncTime() {
            return (now / period) * period;
        }

        @Override public long nanoTime() { return now; }
        @Override public boolean onMain() { return onMain; }
        @Override public void postMain(Runnable r) { main(now, r); }

        @Override public void postVsync() {
            vsyncPosts++;
            nextVsync.add(() -> pacer.vsyncCame(vsyncTime(), vsyncTime() + 2 * period));
        }

        @Override public void atNextVsyncOnDrawing(Runnable r) {
            main(now, () -> nextVsync.add(() -> {
                drawingQueue.add(r);
                kickDrawing();
            }));
        }

        @Override public void requestRender() {
            renderRequested = true;
            kickDrawing();
        }

        @Override public boolean setRate(float r) {
            rate = r;
            rates.add(new double[] {now, r});
            return true;
        }

        @Override public boolean typing() { return typing; }

        void kickDrawing() {
            if (drawingScheduled || drawingFree == BLOCKED) return;
            if (drawingQueue.isEmpty() && !renderRequested) return;
            drawingScheduled = true;
            at(Math.max(now + wake, drawingFree), this::drawingStep);
        }

        void drawingStep() {
            drawingScheduled = false;
            if (drawingFree > now) {
                kickDrawing();
                return;
            }
            while (!drawingQueue.isEmpty()) drawingQueue.poll().run();
            if (renderRequested) {
                renderRequested = false;
                drawFrame();
            }
            kickDrawing();
        }

        static final long BLOCKED = Long.MAX_VALUE / 4;

        void drawFrame() {
            boolean wantedApp = pacer.appWanted.getAndSet(false), pictured = pacer.picturesOnly.getAndSet(false);
            long[] vat = pacer.vsyncAt();
            if (pictured && !wantedApp) {
                pacer.redrawn();
                return;
            }
            long began = now;
            FramePacer.Begun b = pacer.begin(vat, began);
            int f = frames++;
            double work = app.work(f, b.now);
            boolean more = app.more(f, b.now);
            float paced = app.paced(f, b.now);
            long cpuEnd = began + Math.round(work * 1e6);
            drawingFree = cpuEnd;
            at(cpuEnd, () -> {
                pacer.ended(more, false, b.following, paced);
                if (paced > 0) main(Math.round(b.now * 1e6 + 1e9 / paced) - period, pacer::wantFrame);
                long gpuStart = Math.max(cpuEnd, gpuFree);
                long gpuEnd = gpuStart + Math.round(app.gpu(f, b.now) * 1e6);
                gpuFree = gpuEnd;
                long swapDone = swapWaitsGpu ? Math.max(cpuEnd, gpuLastEnd) : cpuEnd;
                gpuLastEnd = gpuEnd;
                long showFrom = b.shows != 0 ? b.shows - period / 2 : 0;
                queued.add(new long[] {gpuEnd, showFrom});
                drawn.add(new double[] {b.now, work});
                drawingFree = queued.size() >= buffers - 1 ? BLOCKED : swapDone;
                kickDrawing();
            });
        }

        void bufferFreed() {
            if (drawingFree == BLOCKED && queued.size() < buffers - 1) {
                drawingFree = now;
                kickDrawing();
            }
        }
    }

    static List<Long> missed(Phone ph, long from, long to) {
        List<Long> out = new ArrayList<>();
        long first = from / ph.period + 1, last = to / ph.period;
        int i = 0;
        for (long k = first; k <= last; k++) {
            while (i < ph.shownAt.size() && ph.shownAt.get(i) < k) i++;
            if (i >= ph.shownAt.size() || ph.shownAt.get(i) != k) out.add(k);
        }
        return out;
    }

    static Phone motion(Phone ph, double startMs, double ms, App app) {
        ph.app = app;
        long start = Math.round(startMs * 1e6);
        ph.main(start, ph.pacer::wantFrame);
        ph.runUntil(start + Math.round((ms + 200) * 1e6));
        return ph;
    }

    static App steady(double work, double gpu, double untilMs) {
        return new App() {
            @Override public double work(int f, double now) { return work; }
            @Override public double gpu(int f, double now) { return gpu; }
            @Override public boolean more(int f, double now) { return now < untilMs; }
        };
    }

    static int failures;

    static void check(String what, boolean ok, String said) {
        System.out.println((ok ? "ok   " : "FAIL ") + what + (said.isEmpty() ? "" : ": " + said));
        if (!ok) failures++;
    }

    static void motionCase(String what, double work, double gpu, boolean swapWaitsGpu) {
        Phone ph = new Phone(120, 3, swapWaitsGpu, 200_000);
        double start = 100, ms = 1000;
        motion(ph, start, ms, steady(work, gpu, start + ms));
        List<Long> miss = missed(ph, Math.round((start + 4 * 8.34) * 1e6), Math.round((start + ms) * 1e6));
        check(what, miss.isEmpty(), miss.size() + " of " + Math.round(ms / 8.333) + " refreshes with no new frame"
                + (miss.isEmpty() ? "" : ", first at vsync " + miss.get(0)) + "; frames drawn " + ph.drawn.size());
    }

    static void typingCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        ph.typing = true;
        ph.app = steady(1.0, 1.0, -1);
        ph.main(Math.round(50e6), ph.pacer::typingChanged);
        for (int k = 0; k < 10; k++) ph.main(Math.round((100 + 120 * k) * 1e6), ph.pacer::wantFrame);
        ph.runUntil(Math.round(1400e6));
        check("typing holds top refresh rate", ph.rate == 120f,
                "rate " + ph.rate + ", said " + ph.rates.size() + " times");
    }

    static void pacedCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        ph.app = new App() {
            @Override public double work(int f, double now) { return 1; }
            @Override public double gpu(int f, double now) { return 1; }
            @Override public boolean more(int f, double now) { return false; }
            @Override public float paced(int f, double now) { return now < 1100 ? 30 : 0; }
        };
        ph.main(Math.round(100e6), ph.pacer::wantFrame);
        ph.runUntil(Math.round(1050e6));
        float during = ph.rate;
        int frames = ph.drawn.size();
        check("30 Hz paced motion sets 30 Hz rate and draws ~30 fps",
                during == 30f && frames >= 28 && frames <= 30, "rate " + during + ", " + frames + " frames in 0.95 s");
    }

    static void frameNowAfterPictureCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        ph.app = steady(1.0, 1.0, -1);
        final boolean[] wantedAfterFrameNow = {false};
        ph.main(Math.round(50e6), () -> {
            ph.pacer.pictureFrame();
            ph.pacer.frameNow();
            wantedAfterFrameNow[0] = ph.pacer.appWanted.get();
        });
        ph.runUntil(Math.round(120e6));
        check("frameNow after pictureFrame sets appWanted and draws an app frame",
                wantedAfterFrameNow[0] && ph.drawn.size() == 1,
                "appWanted=" + wantedAfterFrameNow[0] + ", frames drawn=" + ph.drawn.size());
    }

    static void imeWarmupRateCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        ph.app = steady(1.0, 1.0, -1);
        final float[] rateBeforeFirstFrame = {0f};
        ph.at(Math.round(50e6), () -> {
            ph.drawingQueue.add(() -> ph.pacer.sayRate(true));
            ph.kickDrawing();
        });
        ph.main(Math.round(60e6), () -> rateBeforeFirstFrame[0] = ph.rate);
        long firstStep = Math.round(60e6);
        for (int k = 0; k < 10; k++) {
            ph.main(firstStep + k * ph.period, ph.pacer::frameNow);
        }
        long afterSteps = firstStep + 10 * ph.period;
        ph.runUntil(afterSteps);
        float rateDuring = ph.rate;
        int framesDuring = ph.drawn.size();
        ph.at(afterSteps + ph.period, () -> {
            ph.typing = false;
            ph.drawingQueue.add(ph.pacer::typingChanged);
            ph.kickDrawing();
        });
        ph.runUntil(afterSteps + Math.round(1300e6));
        check("IME warmup holds topRate before frame 1 and releases after end",
                rateBeforeFirstFrame[0] == 120f && rateDuring == 120f && framesDuring == 10 && ph.rate == 0f,
                "before=" + rateBeforeFirstFrame[0] + ", during=" + rateDuring
                        + ", frames=" + framesDuring + ", after=" + ph.rate);
    }

    static void imeMovingWithAnimationCoalescesCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        long firstV = 12;
        long imeSteps = 25;
        long postSteps = 10;
        long endV = firstV + imeSteps + postSteps;
        ph.app = steady(3.0, 3.0, (endV * ph.period) / 1e6 - 0.1);
        ph.main(firstV * ph.period - ph.period / 2, () -> {
            ph.pacer.setImeMoving(true);
            ph.pacer.wantFrame();
        });
        ph.onInsets = () -> {
            long k = ph.now / ph.period;
            if (k >= firstV && k < firstV + imeSteps) {
                ph.pacer.frameNow();
                if (k == firstV + imeSteps - 1) ph.pacer.setImeMoving(false);
            }
        };
        ph.runUntil((endV + 6) * ph.period);
        int imeFrames = 0;
        double imeStartMs = (firstV * ph.period) / 1e6 - 0.1;
        double imeEndMs = ((firstV + imeSteps - 1) * ph.period) / 1e6 + 0.1;
        for (double[] d : ph.drawn) {
            if (d[0] >= imeStartMs && d[0] <= imeEndMs) imeFrames++;
        }
        List<Long> miss = missed(ph, (firstV + 2) * ph.period - 1, (endV + 1) * ph.period);
        check("IME moving with animation coalesces to 1 frame/vsync with 0 missed refreshes",
                miss.isEmpty() && imeFrames == imeSteps,
                "imeFrames=" + imeFrames + "/" + imeSteps + ", missed=" + miss.size()
                        + (miss.isEmpty() ? "" : " (first=" + miss.get(0) + ")") + ", totalDrawn=" + ph.drawn.size());
    }

    static void imeMovingWithoutProgressFallsBackCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        long firstV = 12;
        long steps = 25;
        long endV = firstV + steps;
        ph.app = steady(3.0, 3.0, (endV * ph.period) / 1e6 - 0.1);
        ph.main(firstV * ph.period - ph.period / 2, () -> {
            ph.pacer.setImeMoving(true);
            ph.pacer.wantFrame();
        });
        ph.onInsets = () -> {
            long k = ph.now / ph.period;
            if (k >= firstV && k < endV && (k % 2 == 0)) {
                ph.pacer.frameNow();
            }
            if (k == endV - 1) ph.pacer.setImeMoving(false);
        };
        ph.runUntil((endV + 6) * ph.period);
        List<Long> miss = missed(ph, (firstV + 2) * ph.period - 1, (endV + 1) * ph.period);
        check("IME moving without onProgress on some vsyncs falls back with 0 missed refreshes",
                miss.isEmpty() && ph.drawn.size() == steps + 1,
                "drawn=" + ph.drawn.size() + "/" + (steps + 1) + ", missed=" + miss.size()
                        + (miss.isEmpty() ? "" : " (first=" + miss.get(0) + ")"));
    }

    static void frameNowDoesNotDuplicatePendingVsyncCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        long nextV = 12;
        ph.app = new App() {
            @Override public double work(int f, double now) { return 1.0; }
            @Override public double gpu(int f, double now) { return 1.0; }
            @Override public boolean more(int f, double now) { return f == 0; }
        };
        final int[] postsBeforeNextVsync = {0};
        final int[] callbacksAtNextVsync = {0};
        ph.main(11 * ph.period + 1_000_000, ph.pacer::wantFrame);
        ph.main(11 * ph.period + 2_000_000, ph.pacer::frameNow);
        ph.at(nextV * ph.period - 1, () -> {
            postsBeforeNextVsync[0] = ph.vsyncPosts;
            callbacksAtNextVsync[0] = ph.nextVsync.size();
        });
        ph.runUntil((nextV + 4) * ph.period);
        check("frameNow with pending postVsync does not duplicate next vsync callback",
                postsBeforeNextVsync[0] == 1 && callbacksAtNextVsync[0] == 1 && ph.drawn.size() == 2,
                "postsBefore=" + postsBeforeNextVsync[0] + ", cbsAtNextVsync=" + callbacksAtNextVsync[0]
                        + ", drawn=" + ph.drawn.size());
    }

    static void imeMovingPureSlideKeepsTimelineCase() {
        Phone ph = new Phone(120, 3, true, 200_000);
        long firstV = 12;
        long totalSteps = 20;
        long endV = firstV + totalSteps;
        ph.app = new App() {
            @Override public double work(int f, double now) { return f == 4 ? 5.0 : 2.0; }
            @Override public double gpu(int f, double now) { return f == 4 ? 4.5 : 2.0; }
            @Override public boolean more(int f, double now) { return f >= 10 && f < totalSteps - 1; }
        };
        ph.main(firstV * ph.period - ph.period / 2, () -> ph.pacer.setImeMoving(true));
        ph.onInsets = () -> {
            long k = ph.now / ph.period;
            if (k >= firstV && k < endV) {
                ph.pacer.frameNow();
                if (k == endV - 1) ph.pacer.setImeMoving(false);
            }
        };
        ph.runUntil((endV + 6) * ph.period);
        List<Long> miss = missed(ph, (firstV + 2) * ph.period - 1, (endV + 1) * ph.period);
        check("IME pure slide keeps timeline across >8.33ms frame and transition to more=true with 0 missed refreshes",
                miss.isEmpty() && ph.drawn.size() == totalSteps,
                "drawn=" + ph.drawn.size() + "/" + totalSteps + ", missed=" + miss.size()
                        + (miss.isEmpty() ? "" : " (first=" + miss.get(0) + ")"));
    }

    public static void main(String[] args) {
        motionCase("1 ms CPU + 1 ms GPU at 120 Hz shows a new frame every refresh", 1, 1, true);
        motionCase("0.5 ms CPU + 7 ms GPU at 120 Hz (swap waits GPU) shows a new frame every refresh", 0.5, 7, true);
        motionCase("0.5 ms CPU + 7 ms GPU at 120 Hz (swap async) shows a new frame every refresh", 0.5, 7, false);
        motionCase("6 ms CPU + 1 ms GPU at 120 Hz shows a new frame every refresh", 6, 1, true);
        typingCase();
        pacedCase();
        frameNowAfterPictureCase();
        imeWarmupRateCase();
        imeMovingWithAnimationCoalescesCase();
        imeMovingWithoutProgressFallsBackCase();
        frameNowDoesNotDuplicatePendingVsyncCase();
        imeMovingPureSlideKeepsTimelineCase();
        if (failures > 0) {
            System.out.println(failures + " failed");
            System.exit(1);
        }
        System.out.println("ALL FramePacerTest assertions PASSED");
    }
}
