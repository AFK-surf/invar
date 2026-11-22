package dev.invar;

import java.nio.charset.StandardCharsets;

/**
 * JNI bridge to libinvar_android.so (the Pion application + Invar GLES3 host).
 * Calls into Pion (start, frame, input, outside notifications) run on the
 * application logic thread (MBView's logic thread, or the GL thread in ONE mode);
 * rendering calls run on the GL thread.
 */
final class Native {
    static {
        System.loadLibrary("invar_android");
    }

    static native void start();
    static native void twoThreads();
    static native boolean surfaceCreated();
    static native void resize(int w, int h);
    static native boolean frame(float w, float h, float scale, double now, double late);
    static native void redraw();
    static native boolean publish();
    static native boolean take(boolean shown);
    static native void dropPending();
    static native void draw();
    /** Whether adaptive liquid glass is still settling temporally on the GL thread. */
    static native boolean adaptiveSettling();
    static native void trim();
    static native void glSync();
    static native void glDrawn(double ms, int dropped);
    static native void pointer(int kind, float x, float y, double time);
    static native void touch(int kind, int id, float x, float y, double time);
    static native void wheel(float x, float y, float dx, float dy, int phase);
    static native void key(int code, int mods);
    static native void text(int kind, String s, int a, int b);
    static native void back(int kind, float x, float progress, int edge);
    static native void appState(int state);
    static native void overOther(boolean on);
    static native void thermal(boolean hot);
    static native void a11yEnabled(boolean enabled);
    static native void postBytes(String channel, byte[] json);

    static void post(String channel, String json) {
        postBytes(channel, json.getBytes(StandardCharsets.UTF_8));
    }

    static native int atlasTexture();
    static native int colourAtlasTexture();
    static native boolean snapshot(String path);
    static native boolean imageDone(int id, int tex);
    static native void imageTold(int id, boolean ok, int w, int h, int ow, int oh);
    static native void imagesUntold();
    static native void imageGone(int id);
    static native void outside(int kind, int id, int state);
    static native void posted();
    static native void ownClock();
    static native int awaiting();

    static final int OUTSIDE_IMAGE = 1;
    static final int OUTSIDE_CLIP = 9;
    static final int OUTSIDE_POSTED = 11;

    static void tell(int kind, int id, int state) {
        MBView v = Host.view;
        if (v != null) v.toApp(() -> outside(kind, id, state));
        Host.requestFrame();
    }
}
