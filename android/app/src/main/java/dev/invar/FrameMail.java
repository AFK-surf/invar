package dev.invar;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/**
 * Synchronized handoff between the application logic thread and the GL render
 * thread: pending frame slot, ordered GL task queue, and deferred resource
 * retirement once a newer frame is drawn on screen.
 */
final class FrameMail<G> {
    interface Frames<G> {
        boolean publish();
        boolean take(boolean shown);
        void dropPending();
        String frameName();
        void dropped(List<G> glyphs);
        void failed(RuntimeException e);
    }

    static final class Taken<G> {
        boolean took;
        boolean redraw;
        long seq = -1;
        long shows;
        ArrayList<G> glyphs;
        String ref;
        final ArrayList<Runnable> retire = new ArrayList<>();
    }

    private static final class Task {
        final long seq;
        final Runnable r;

        Task(long seq, Runnable r) {
            this.seq = seq;
            this.r = r;
        }
    }

    private final Frames<G> f;

    FrameMail(Frames<G> f) {
        this.f = f;
    }

    private long seq;
    private final ArrayDeque<Task> tasks = new ArrayDeque<>();
    private long pendingSeq = -1;
    private ArrayList<G> pendingGlyphs = new ArrayList<>();
    private long pendingShows;
    private String pendingRef;
    private boolean redrawOwed;
    private long redrawShows;
    private String redrawRef;
    private final ArrayList<Runnable> retiring = new ArrayList<>();
    private final ArrayList<Runnable> pendingRetire = new ArrayList<>();
    private final ArrayList<Runnable> owedRetire = new ArrayList<>();
    private long contextGen, appGen;
    private long made;
    private int dropped;

    synchronized void task(Runnable r) {
        tasks.add(new Task(++seq, r));
    }

    synchronized void retire(Runnable r) {
        retiring.add(r);
    }

    synchronized boolean handOver(long shows, List<G> placed) {
        if (appGen != contextGen) {
            if (f.publish()) f.dropPending();
            f.dropped(new ArrayList<>(placed));
            placed.clear();
            return false;
        }
        made++;
        notifyAll();
        if (!f.publish()) return false;
        if (pendingSeq >= 0) dropped++;
        pendingSeq = ++seq;
        pendingShows = shows;
        pendingGlyphs.addAll(placed);
        placed.clear();
        String ref = f.frameName();
        if (ref != null) pendingRef = ref;
        pendingRetire.addAll(retiring);
        retiring.clear();
        return true;
    }

    synchronized boolean redrawAsked(long shows) {
        if (appGen != contextGen) return false;
        redrawOwed = true;
        redrawShows = shows;
        made++;
        notifyAll();
        return true;
    }

    synchronized void contextTold(long gen) {
        appGen = gen;
        notifyAll();
    }

    synchronized long contextMade() {
        if (pendingSeq >= 0) {
            f.dropPending();
            f.dropped(pendingGlyphs);
            pendingGlyphs = new ArrayList<>();
            pendingSeq = -1;
            pendingRef = null;
        }
        redrawOwed = false;
        return ++contextGen;
    }

    synchronized void awaitFresh(int ms) {
        if (ms <= 0 || pendingSeq >= 0 || redrawOwed) return;
        long until = System.nanoTime() + ms * 1_000_000L, from = made;
        for (long left = ms; pendingSeq < 0 && made == from && left > 0; left = (until - System.nanoTime()) / 1_000_000L) {
            try {
                wait(left);
            } catch (InterruptedException e) {
                break;
            }
        }
    }

    synchronized Taken<G> take() {
        Taken<G> t = new Taken<>();
        t.took = pendingSeq >= 0 && f.take(true);
        if (t.took) {
            t.seq = pendingSeq;
            pendingSeq = -1;
            t.glyphs = pendingGlyphs;
            pendingGlyphs = new ArrayList<>();
            t.shows = pendingShows;
            t.ref = pendingRef != null ? pendingRef : redrawRef;
            pendingRef = null;
        }
        t.redraw = redrawOwed;
        redrawOwed = false;
        if (!t.took && t.redraw) {
            t.shows = redrawShows;
            t.ref = redrawRef;
        }
        redrawRef = null;
        t.retire.addAll(owedRetire);
        owedRetire.clear();
        if (t.took) {
            t.retire.addAll(pendingRetire);
            pendingRetire.clear();
        }
        return t;
    }

    synchronized Taken<G> takeForSync() {
        Taken<G> t = new Taken<>();
        t.took = pendingSeq >= 0 && f.take(false);
        if (!t.took) return t;
        t.seq = pendingSeq;
        pendingSeq = -1;
        t.glyphs = pendingGlyphs;
        pendingGlyphs = new ArrayList<>();
        redrawOwed = true;
        redrawShows = pendingShows;
        if (pendingRef != null) redrawRef = pendingRef;
        pendingRef = null;
        owedRetire.addAll(pendingRetire);
        pendingRetire.clear();
        return t;
    }

    synchronized int takeDropped() {
        int n = dropped;
        dropped = 0;
        return n;
    }

    synchronized long pendingSeqNow() {
        return pendingSeq >= 0 ? pendingSeq : Long.MAX_VALUE;
    }

    void runTasks(long upto) {
        while (true) {
            Task t;
            synchronized (this) {
                t = tasks.peek();
                if (t == null || t.seq >= upto) return;
                tasks.poll();
            }
            run(t.r);
        }
    }

    void runAll(List<Runnable> rs) {
        for (Runnable r : rs) run(r);
    }

    private void run(Runnable r) {
        try {
            r.run();
        } catch (RuntimeException e) {
            f.failed(e);
        }
    }

    synchronized long made() { return made; }
    synchronized boolean pending() { return pendingSeq >= 0; }
    synchronized boolean redrawOwed() { return redrawOwed; }
}
