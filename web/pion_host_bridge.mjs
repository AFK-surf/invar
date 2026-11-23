// Zero-copy bridge between Pion wasm32 (`packages/host_web`) and `web/host.js` (`globalThis.__mbhost`).
// Attaches `mb_web_*` imports onto `rt.imports.pion_rt` and routes `H.run` callbacks + browser events
// (modifier state, keyup, async clipboard, picker, downloads, keep-awake, drag-and-drop, native views,
// WebCodecs/WebAudio streams, terminal, WebAuthn, Pointer Lock, Gamepads, a11y, perf recording) to Wasm.

const INST = 60;
const KEYMAP = {
  Backspace: 1, Delete: 2, ArrowLeft: 3, ArrowRight: 4, ArrowUp: 5, ArrowDown: 6,
  Enter: 7, Home: 8, End: 9, PageUp: 15, PageDown: 16, Escape: 17, Tab: 18,
};
const MOD_KEYS = new Set(["Shift", "Control", "Alt", "Meta", "OS"]);

function b64urlToU8(s) {
  const pad = s.length % 4 ? "=".repeat(4 - (s.length % 4)) : "";
  const bin = atob(s.replace(/-/g, "+").replace(/_/g, "/") + pad);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

function u8ToB64url(buf) {
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let bin = "";
  for (let i = 0; i < u8.length; i++) bin += String.fromCharCode(u8[i]);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export function attachHostBridge(rt, perfHooks = {}) {
  const host = () => globalThis.__mbhost;
  const f64View = (ptr, count) => {
    const mem = rt.getMemory();
    const p = Number(ptr);
    if (!mem || !p || count <= 0) return new Float64Array(0);
    return new Float64Array(mem.buffer, p, count);
  };
  const u8Slice = (ptr, len) => {
    const mem = rt.getMemory();
    const p = Number(ptr), n = Number(len);
    if (!mem || !p || n <= 0) return new Uint8Array(0);
    return new Uint8Array(mem.buffer, p, n).slice();
  };

  let lastBytesLen = 0;
  const allocRawBytes = (src) => {
    const u8 = src instanceof Uint8Array ? src : new Uint8Array(src || 0);
    lastBytesLen = u8.length;
    if (!u8.length) return 0n;
    const ptr = rt.imports.pion_rt.pion_ptr_alloc(BigInt(u8.length));
    const mem = rt.getMemory();
    new Uint8Array(mem.buffer, Number(ptr), u8.length).set(u8);
    return ptr;
  };

  let firstFrameReported = false;
  const frameTimes = [];
  let modsHeld = 0;
  let desktopListenersAttached = false;
  const keyUpEvents = [];
  let lastFrameWall = 0;
  let framesDropped = 0;
  let cpuMsTotal = 0;

  // Pointer lock delta accumulator
  let pointerDx = 0;
  let pointerDy = 0;

  // In-memory performance log recorder
  let perfRecording = false;
  const perfLines = [];
  let perfBytes = 0;

  // Accessibility live DOM mirror (lazily created when enabled)
  let a11yEl = null;

  // WebAuthn / Passkeys async request table
  let waNextId = 1;
  const waSlots = new Map();

  // WebAudio PCM playback per stream id
  const audioStreams = new Map();
  function getAudioStream(id) {
    let s = audioStreams.get(id);
    if (!s) {
      s = { ctx: null, gain: null, nextTime: 0, playedSec: 0, queuedSec: 0, volume: 1.0 };
      audioStreams.set(id, s);
    }
    return s;
  }

  const state = {
    lastCursor: 0,
    lastClipboardText: "",
    lastTitle: "",
    keepAwake: false,
    lastIme: null,
    dark: false,
    badge: 0,
  };

  const modsFromEvent = (e) =>
    (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);

  const keyCodeOf = (e) => {
    if (KEYMAP[e.key]) return KEYMAP[e.key];
    if (MOD_KEYS.has(e.key)) return 0;
    if (e.key && e.key.length === 1) return 1000 + e.key.toLowerCase().charCodeAt(0);
    return 0;
  };

  function triggerDownload(name, data, mime = "application/octet-stream") {
    if (typeof document === "undefined" || typeof Blob === "undefined" || typeof URL === "undefined") return false;
    try {
      const blob = new Blob([data], { type: mime || "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = name || "download";
      a.style.display = "none";
      document.body.appendChild(a);
      a.click();
      setTimeout(() => {
        try { a.remove(); } catch {}
        try { URL.revokeObjectURL(url); } catch {}
      }, 1000);
      return true;
    } catch {
      return false;
    }
  }

  function attachDesktopListeners() {
    if (desktopListenersAttached || typeof window === "undefined") return;
    desktopListenersAttached = true;

    window.addEventListener("keydown", (e) => {
      modsHeld = modsFromEvent(e);
    }, true);

    window.addEventListener("keyup", (e) => {
      modsHeld = modsFromEvent(e);
      const code = keyCodeOf(e);
      keyUpEvents.push({ code, mods: modsHeld, key: e.key });
      if (keyUpEvents.length > 64) keyUpEvents.shift();
      const exp = rt.getExports();
      if (exp && exp.web_key_up) {
        exp.web_key_up(code | 0, modsHeld | 0);
        const H = host();
        if (H && H.requestFrame) H.requestFrame();
      }
    }, true);

    window.addEventListener("mousemove", (e) => {
      if (typeof document !== "undefined" && document.pointerLockElement) {
        pointerDx += e.movementX || 0;
        pointerDy += e.movementY || 0;
      }
    }, true);

    window.addEventListener("blur", () => {
      modsHeld = 0;
    });

    // File drag-and-drop import -> forwarded to host.js external post queue
    window.addEventListener("dragover", (e) => {
      if (e.dataTransfer && Array.from(e.dataTransfer.types || []).includes("Files")) {
        e.preventDefault();
      }
    });
    window.addEventListener("drop", async (e) => {
      const files = Array.from(e.dataTransfer?.files || []);
      if (!files.length) return;
      e.preventDefault();
      const H = host();
      if (!H || !H.post) return;
      for (const f of files) {
        try {
          const bytes = new Uint8Array(await f.arrayBuffer());
          H.post("drop", JSON.stringify({ name: f.name, type: f.type || "", size: f.size }), bytes);
        } catch {
          // ignore unreadable dropped entry
        }
      }
    });
  }

  const bridge = {
    mb_web_last_bytes_len() {
      return lastBytesLen | 0;
    },

    mb_web_run(titlePtr, titleLen) {
      const H = host();
      const title = rt.readUtf8(titlePtr, titleLen);
      state.lastTitle = title;
      attachDesktopListeners();
      H.run(
        title,
        (w, h, scale, now) => {
          const exp = rt.getExports();
          if (!exp || !exp.web_frame) return;
          const t0 = performance.now();
          if (lastFrameWall > 0 && t0 - lastFrameWall > 34 && t0 - lastFrameWall < 1000) {
            framesDropped += Math.max(0, Math.floor((t0 - lastFrameWall) / 16.67) - 1);
          }
          lastFrameWall = t0;
          exp.web_frame(w, h, scale, now);
          const dt = performance.now() - t0;
          cpuMsTotal += dt;
          frameTimes.push(dt);
          if (frameTimes.length > 240) frameTimes.shift();
          if (!firstFrameReported) {
            firstFrameReported = true;
            if (perfHooks.onFirstFrame) perfHooks.onFirstFrame(dt);
          }
          if (perfHooks.onFrame) perfHooks.onFrame(dt);
        },
        (kind, x, y, btn) => {
          const exp = rt.getExports();
          if (!exp || !exp.web_pointer) return;
          exp.web_pointer(kind | 0, x, y, btn | 0);
          H.requestFrame();
        },
        (x, y, dx, dy, phase) => {
          const exp = rt.getExports();
          if (!exp || !exp.web_wheel) return;
          exp.web_wheel(x, y, dx, dy, phase | 0);
          H.requestFrame();
        },
        (code, mods) => {
          modsHeld = mods | 0;
          const exp = rt.getExports();
          if (!exp || !exp.web_key) return;
          exp.web_key(code | 0, mods | 0);
          H.requestFrame();
        },
        (kind, text, a, b) => {
          const exp = rt.getExports();
          if (!exp || !exp.web_text) return;
          const cptr = rt.mkCStr(text);
          try {
            exp.web_text(kind | 0, cptr, a | 0, b | 0);
          } finally {
            rt.rawFree(Number(cptr));
          }
          H.requestFrame();
        },
      );
    },

    mb_web_now() {
      const H = host();
      return H && H.now ? H.now() : performance.now();
    },
    mb_web_wall_ms() {
      const H = host();
      return H && H.wallMs ? H.wallMs() : Date.now();
    },
    mb_web_utc_offset_min(ms) {
      const H = host();
      return (H && H.utcOffsetMin ? H.utcOffsetMin(ms) : -new Date(ms).getTimezoneOffset()) | 0;
    },
    mb_web_event_time() {
      const H = host();
      return H && H.eventTime ? H.eventTime() : -1;
    },
    mb_web_request_frame() {
      const H = host();
      if (H && H.requestFrame) H.requestFrame();
    },
    mb_web_wake_now() {
      const H = host();
      if (H && H.requestFrame) H.requestFrame();
    },
    invar_hostapi_set_native_wake() {},
    invar_hostapi_call_native_wake() {},
    mb_web_frames_paced(hz) {
      const H = host();
      if (!H) return;
      if (H.framesPaced) H.framesPaced(hz);
      else if (H.requestFrame) H.requestFrame();
    },
    mb_web_wake_after(ms) {
      const H = host();
      if (H && H.wakeAfter) H.wakeAfter(ms);
    },
    mb_web_own_clock() {
      const H = host();
      return H && H.ownClock && H.ownClock() ? 1 : 0;
    },
    mb_web_wheel_kept(kept) {
      const H = host();
      if (H) H.wheelKept = !!kept;
    },
    mb_web_mods_held() {
      const H = host();
      const hm = H && H.modsHeld ? (H.modsHeld() | 0) : 0;
      return (modsHeld | hm) | 0;
    },

    // App callbacks, page lifecycle, appearance & badge
    mb_web_app_said(namePtr, nameLen) {
      const H = host();
      const name = rt.readUtf8(namePtr, nameLen);
      if (name === "mbui.page.location" || name === "invar.page.location") {
        const v = H && H.historyLocation ? H.historyLocation() : (typeof location !== "undefined" ? location.pathname + location.search + location.hash : "");
        return rt.mkCStr(String(v || ""));
      }
      if (name === "mbui.page.loaded_title" || name === "invar.page.loaded_title") {
        const v = H && H.loadedTitle ? H.loadedTitle() : (typeof document !== "undefined" ? document.title : "");
        return rt.mkCStr(String(v || ""));
      }
      const c = H && H.appCalls;
      const v = c && c.said ? c.said(name) : "";
      return rt.mkCStr(typeof v === "string" ? v : "");
    },
    mb_web_app_tell(namePtr, nameLen, argPtr, argLen) {
      const H = host();
      const name = rt.readUtf8(namePtr, nameLen);
      const arg = rt.readUtf8(argPtr, argLen);
      if (name === "mbui.page.title" || name === "invar.page.title") {
        state.lastTitle = arg;
        if (H && H.setPageTitle) H.setPageTitle(arg);
        else if (typeof document !== "undefined") document.title = arg;
        return;
      }
      if (name === "mbui.page.theme_color" || name === "invar.page.theme_color") {
        if (H && H.setThemeColor) H.setThemeColor(arg);
        return;
      }
      if (name === "mbui.page.cjk_face" || name === "invar.page.cjk_face") {
        if (H && H.setCjkFace) {
          try {
            const o = JSON.parse(arg);
            H.setCjkFace(o.face, o.step, o.families);
          } catch {}
        }
        return;
      }
      if (name === "mbui.page.history_push" || name === "invar.page.history_push") {
        if (H && H.historyPush) H.historyPush(arg);
        else if (typeof history !== "undefined") history.pushState(null, "", arg);
        return;
      }
      if (name === "mbui.page.history_replace" || name === "invar.page.history_replace") {
        if (H && H.historyReplace) H.historyReplace(arg);
        else if (typeof history !== "undefined") history.replaceState(null, "", arg);
        return;
      }
      if (name === "mbui.page.history_back" || name === "invar.page.history_back") {
        if (H && H.historyBack) H.historyBack();
        else if (typeof history !== "undefined") history.back();
        return;
      }
      const c = H && H.appCalls;
      if (c && c.tell) c.tell(name, arg);
    },
    mb_web_set_site_name(namePtr, nameLen) {
      const H = host();
      if (H) H.siteName = rt.readUtf8(namePtr, nameLen);
    },
    mb_web_set_dark(dark) {
      state.dark = !!dark;
      const H = host();
      if (H && H.setDark) H.setDark(!!dark);
      if (typeof document !== "undefined" && document.documentElement) {
        document.documentElement.style.colorScheme = dark ? "dark" : "light";
      }
    },
    mb_web_set_badge(n) {
      state.badge = n | 0;
      if (typeof navigator === "undefined") return;
      try {
        if ((n | 0) > 0 && typeof navigator.setAppBadge === "function") {
          navigator.setAppBadge(n | 0).catch(() => {});
        } else if ((n | 0) <= 0 && typeof navigator.clearAppBadge === "function") {
          navigator.clearAppBadge().catch(() => {});
        }
      } catch {}
    },
    mb_web_full_screen() {
      return typeof document !== "undefined" && !!document.fullscreenElement ? 1 : 0;
    },
    mb_web_set_full_screen(on) {
      if (typeof document === "undefined") return;
      try {
        if (on) {
          if (!document.fullscreenElement && document.documentElement && document.documentElement.requestFullscreen) {
            document.documentElement.requestFullscreen().catch(() => {});
          }
        } else if (document.fullscreenElement && document.exitFullscreen) {
          document.exitFullscreen().catch(() => {});
        }
      } catch {}
    },
    mb_web_window_inset() {
      if (typeof navigator !== "undefined" && navigator.windowControlsOverlay && navigator.windowControlsOverlay.visible) {
        return 1;
      }
      return 0;
    },
    mb_web_font_scale() {
      if (typeof window === "undefined" || typeof document === "undefined" || !document.documentElement) return 1.0;
      try {
        const px = parseFloat(window.getComputedStyle(document.documentElement).fontSize);
        if (Number.isFinite(px) && px > 4 && px < 128) return px / 16.0;
      } catch {}
      return 1.0;
    },
    mb_web_a11y_on() {
      const H = host();
      if (H && H.a11yOn) return H.a11yOn() ? 1 : 0;
      return a11yEl ? 1 : 0;
    },
    mb_web_a11y_tree(sPtr, sLen) {
      const s = rt.readUtf8(sPtr, sLen);
      const H = host();
      if (H && H.a11yTree) {
        H.a11yTree(s);
        return;
      }
      if (typeof document === "undefined" || !document.body) return;
      if (!a11yEl) {
        a11yEl = document.createElement("div");
        a11yEl.setAttribute("role", "region");
        a11yEl.setAttribute("aria-live", "polite");
        a11yEl.style.cssText = "position:fixed;width:1px;height:1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0;pointer-events:none;";
        document.body.appendChild(a11yEl);
      }
      a11yEl.dataset.treeLen = String(s.length);
    },
    mb_web_over_done(openApp) {
      const s = globalThis.__mbshare;
      if (s && s.done) s.done(!!openApp);
    },
    mb_web_update_apply(_saidPtr, _saidLen) {
      if (typeof location !== "undefined" && typeof location.reload === "function") {
        location.reload();
      }
    },
    mb_web_post(chPtr, chLen, jsonPtr, jsonLen) {
      const H = host();
      if (H && H.post) H.post(rt.readUtf8(chPtr, chLen), rt.readUtf8(jsonPtr, jsonLen));
    },
    mb_web_drive(kind) {
      const H = host();
      return H && H.drive && H.drive(kind | 0) ? 1 : 0;
    },
    mb_web_word_at(textPtr, textLen, off) {
      const H = host();
      if (!H || !H.wordAt) return -1;
      return H.wordAt(rt.readUtf8(textPtr, textLen), off | 0);
    },

    // Multi-window & title
    mb_web_windows(op, wid, whatPtr, whatLen) {
      const H = host();
      if (!H || !H.windows) return 0;
      return H.windows(op | 0, wid | 0, rt.readUtf8(whatPtr, whatLen)) | 0;
    },
    mb_web_window_set_title(wid, titlePtr, titleLen) {
      const H = host();
      const t = rt.readUtf8(titlePtr, titleLen);
      state.lastTitle = t;
      if ((wid | 0) <= 0) {
        if (H && H.setPageTitle) H.setPageTitle(t);
        else if (typeof document !== "undefined") document.title = t;
      } else if (H && H.windows) {
        H.windows(5, wid | 0, t);
      }
    },

    // Text shaping & glyph atlas
    mb_web_font_metrics(size, weight, style, outPtr) {
      const H = host();
      const out = f64View(outPtr, 6);
      H.fontMetrics(size, weight | 0, style | 0, out);
    },
    mb_web_shape(textPtr, textLen, size, weight, style, outPtr, outLen) {
      const H = host();
      const s = rt.readUtf8(textPtr, textLen);
      const out = f64View(outPtr, outLen | 0);
      return H.shape(s, size, weight | 0, style | 0, out) | 0;
    },
    mb_web_text_key(textPtr, textLen, size, weight, style) {
      const H = host();
      const s = rt.readUtf8(textPtr, textLen);
      return H.textKey(s, size, weight | 0, style | 0) | 0;
    },
    mb_web_glyph(key, outPtr) {
      const H = host();
      const out = f64View(outPtr, 8);
      H.glyph(key | 0, out);
    },
    mb_web_glyph_put(key, x, y) {
      const H = host();
      H.glyphPut(key | 0, x | 0, y | 0);
    },
    mb_web_glyphs_ahead(keysPtr, n) {
      const H = host();
      const mem = rt.getMemory();
      const p = Number(keysPtr);
      const count = n | 0;
      if (!H || !H.glyph || !mem || !p || count <= 0) return;
      const dv = new DataView(mem.buffer);
      const tmp = new Float64Array(8);
      for (let i = 0; i < count; i++) {
        const key = dv.getInt32(p + i * 8, true);
        if (key > 0) H.glyph(key, tmp);
      }
    },
    mb_web_atlas_size(colour) {
      const H = host();
      return H.atlasSize(!!colour) | 0;
    },
    mb_web_atlas_generation() {
      const H = host();
      return H.atlasGeneration() | 0;
    },
    mb_web_face_add(face, pathPtr, pathLen, weight) {
      const H = host();
      if (!H || !H.faceAdd) return 0;
      return H.faceAdd(face | 0, rt.readUtf8(pathPtr, pathLen), weight | 0) ? 1 : 0;
    },
    mb_web_face_add_bytes(face, dataPtr, dataLen, weight) {
      const H = host();
      if (!H || !H.faceAddBytes) return 0;
      return H.faceAddBytes(face | 0, u8Slice(dataPtr, dataLen), weight | 0) ? 1 : 0;
    },
    mb_web_face_clear(face) {
      const H = host();
      if (H && H.faceClear) H.faceClear(face | 0);
    },

    // Zero-copy direct Float64Array view over Pion Array[Double] linear memory buffer!
    mb_web_present(bufPtr, count, r, g, b) {
      const H = host();
      const n = count | 0;
      const view = f64View(bufPtr, n * INST);
      H.present(view, n, r, g, b);
    },
    mb_web_dirty(x, y, w, h) {
      const H = host();
      if (H && H.renderDirty) H.renderDirty(x, y, w, h);
    },
    mb_web_glass_light_angle(rad) {
      const H = host();
      if (H && H.glassLightAngle) H.glassLightAngle(rad);
    },
    mb_web_layer_supported() {
      const H = host();
      return H && H.layerSupported ? (H.layerSupported() | 0) : 0;
    },
    mb_web_layer_render(id, physW, physH, scale, ptr, count) {
      const H = host();
      if (!H || !H.layerRender) return 0;
      const n = count | 0;
      const view = f64View(ptr, n * INST);
      return H.layerRender(id | 0, physW | 0, physH | 0, scale, view, n) ? 1 : 0;
    },
    mb_web_layer_destroy(id) {
      const H = host();
      if (H && H.layerDestroy) H.layerDestroy(id | 0);
    },

    // IME, text menu & clipboard
    mb_web_set_ime(active, x, y, w, h, textPtr, textLen, selStart, selEnd) {
      const H = host();
      const text = rt.readUtf8(textPtr, textLen);
      state.lastIme = { active: !!active, x, y, w, h, text, selStart: selStart | 0, selEnd: selEnd | 0 };
      if (H && H.setIme) {
        H.setIme(!!active, x, y, w, h, text, selStart | 0, selEnd | 0);
      }
    },
    mb_web_text_menu(shown, x, y, w, h, can) {
      const H = host();
      if (H && H.textMenu) H.textMenu(shown | 0, x, y, w, h, can | 0);
    },
    mb_web_text_menu_own(wordsPtr, wordsLen) {
      const H = host();
      if (H && H.textMenuOwn) H.textMenuOwn(rt.readUtf8(wordsPtr, wordsLen));
    },
    mb_web_clipboard_set(sPtr, sLen) {
      const H = host();
      const s = rt.readUtf8(sPtr, sLen);
      state.lastClipboardText = s;
      if (H && H.clipboardSet) H.clipboardSet(s);
    },
    mb_web_cl_read() {
      const H = host();
      return H && H.clipRead ? (H.clipRead() | 0) : 0;
    },
    mb_web_cl_state(id) {
      const H = host();
      return H && H.clipState ? (H.clipState(id | 0) | 0) : -1;
    },
    mb_web_cl_picture(id) {
      const H = host();
      return H && H.clipPicture ? (H.clipPicture(id | 0) | 0) : 0;
    },
    mb_web_cl_text(id) {
      const H = host();
      return rt.mkCStr(H && H.clipText ? H.clipText(id | 0) : "");
    },
    mb_web_cl_png(id) {
      const H = host();
      return allocRawBytes(H && H.clipPng ? H.clipPng(id | 0) : new Uint8Array(0));
    },
    mb_web_cl_files(id) {
      const H = host();
      return H && H.clipFiles ? (H.clipFiles(id | 0) | 0) : 0;
    },
    mb_web_cl_file_name(id, i) {
      const H = host();
      return rt.mkCStr(H && H.clipFileName ? H.clipFileName(id | 0, i | 0) : "");
    },
    mb_web_cl_file(id, i) {
      const H = host();
      return allocRawBytes(H && H.clipFile ? H.clipFile(id | 0, i | 0) : new Uint8Array(0));
    },
    mb_web_cl_end(id) {
      const H = host();
      if (H && H.clipEnd) H.clipEnd(id | 0);
    },
    mb_web_cl_set(textPtr, textLen, pngPtr, pngLen, labelPtr, labelLen) {
      const H = host();
      const text = rt.readUtf8(textPtr, textLen);
      const png = u8Slice(pngPtr, pngLen);
      const label = rt.readUtf8(labelPtr, labelLen);
      if (text) state.lastClipboardText = text;
      if (H && H.clipSet) H.clipSet(text, png, label);
    },

    // File picker
    mb_web_pk_start(kind) {
      const H = host();
      return H && H.pickStart ? (H.pickStart(kind | 0) | 0) : 0;
    },
    mb_web_pk_state(id) {
      const H = host();
      return H && H.pickState ? (H.pickState(id | 0) | 0) : -1;
    },
    mb_web_pk_count(id) {
      const H = host();
      return H && H.pickCount ? (H.pickCount(id | 0) | 0) : 0;
    },
    mb_web_pk_name(id, i) {
      const H = host();
      return rt.mkCStr(H && H.pickName ? H.pickName(id | 0, i | 0) : "");
    },
    mb_web_pk_bytes(id, i) {
      const H = host();
      return allocRawBytes(H && H.pickBytes ? H.pickBytes(id | 0, i | 0) : new Uint8Array(0));
    },
    mb_web_pk_info(id, i) {
      const H = host();
      return rt.mkCStr(H && H.pickInfo ? H.pickInfo(id | 0, i | 0) : "");
    },
    mb_web_pk_end(id) {
      const H = host();
      if (H && H.pickEnd) H.pickEnd(id | 0);
    },

    // Keep / download / save & share
    mb_web_kp_begin(namePtr, nameLen, mimePtr, mimeLen, way) {
      const H = host();
      return H && H.keepBegin
        ? (H.keepBegin(rt.readUtf8(namePtr, nameLen), rt.readUtf8(mimePtr, mimeLen), way | 0) | 0)
        : 0;
    },
    mb_web_kp_append(id, dataPtr, dataLen) {
      const H = host();
      return H && H.keepAppend ? (H.keepAppend(id | 0, u8Slice(dataPtr, dataLen)) | 0) : 0;
    },
    mb_web_kp_finish(id) {
      const H = host();
      if (H && H.keepFinish) H.keepFinish(id | 0);
    },
    mb_web_kp_state(id) {
      const H = host();
      return H && H.keepState ? (H.keepState(id | 0) | 0) : 3;
    },
    mb_web_kp_where(id) {
      const H = host();
      return rt.mkCStr(H && H.keepWhere ? H.keepWhere(id | 0) : "");
    },
    mb_web_kp_end(id) {
      const H = host();
      if (H && H.keepEnd) H.keepEnd(id | 0);
    },
    mb_web_save_file(namePtr, nameLen, dataPtr, dataLen) {
      const name = rt.readUtf8(namePtr, nameLen);
      const data = u8Slice(dataPtr, dataLen);
      const H = host();
      if (H && H.saveFile) return H.saveFile(name, data) ? 1 : 0;
      return triggerDownload(name, data) ? 1 : 0;
    },
    mb_web_share(textPtr, textLen, namePtr, nameLen, mimePtr, mimeLen, dataPtr, dataLen) {
      if (typeof navigator === "undefined" || typeof navigator.share !== "function") return 0;
      const text = rt.readUtf8(textPtr, textLen);
      const name = rt.readUtf8(namePtr, nameLen);
      const mime = rt.readUtf8(mimePtr, mimeLen);
      const data = u8Slice(dataPtr, dataLen);
      const payload = {};
      if (text) payload.text = text;
      if (data.length > 0 && typeof File !== "undefined") {
        try {
          const f = new File([data], name || "file", { type: mime || "application/octet-stream" });
          if (!navigator.canShare || navigator.canShare({ files: [f] })) {
            payload.files = [f];
          }
        } catch {}
      }
      if (!payload.text && !payload.files) return 0;
      try {
        navigator.share(payload).catch(() => {});
        return 1;
      } catch {
        return 0;
      }
    },
    mb_web_shared_in() {
      const s = globalThis.__mbshare;
      const v = s && s.take ? s.take() : "";
      return rt.mkCStr(typeof v === "string" ? v : "");
    },
    mb_web_shared_in_file(i) {
      const s = globalThis.__mbshare;
      const v = s && s.file ? s.file(i | 0) : new Uint8Array(0);
      return allocRawBytes(v);
    },

    // Charset decoding (TextDecoder)
    mb_web_cs_known(csPtr, csLen) {
      try {
        new TextDecoder(rt.readUtf8(csPtr, csLen));
        return 1;
      } catch {
        return 0;
      }
    },
    mb_web_cs_decode(dataPtr, dataLen, csPtr, csLen) {
      try {
        const dec = new TextDecoder(rt.readUtf8(csPtr, csLen));
        return rt.mkCStr(dec.decode(u8Slice(dataPtr, dataLen)));
      } catch {
        return rt.mkCStr("");
      }
    },

    // WebAuthn / Passkeys
    mb_web_wa_able() {
      return typeof navigator !== "undefined" && !!navigator.credentials && typeof PublicKeyCredential !== "undefined" ? 1 : 0;
    },
    mb_web_wa_get(askPtr, askLen) {
      if (typeof navigator === "undefined" || !navigator.credentials || typeof PublicKeyCredential === "undefined") {
        return 0;
      }
      const askStr = rt.readUtf8(askPtr, askLen);
      const id = waNextId++;
      const slot = { done: false, out: "" };
      waSlots.set(id, slot);
      const finish = (obj) => {
        slot.done = true;
        slot.out = JSON.stringify(obj);
        const H = host();
        if (H && H.requestFrame) H.requestFrame();
      };
      (async () => {
        try {
          const req = JSON.parse(askStr);
          const pk = req.publicKey || req;
          const opts = { ...pk };
          if (typeof opts.challenge === "string") opts.challenge = b64urlToU8(opts.challenge);
          if (Array.isArray(opts.allowCredentials)) {
            opts.allowCredentials = opts.allowCredentials.map((c) => ({
              ...c,
              id: typeof c.id === "string" ? b64urlToU8(c.id) : c.id,
            }));
          }
          const cred = await navigator.credentials.get({ publicKey: opts });
          if (!cred) {
            finish({ error: "cancelled" });
            return;
          }
          const resp = cred.response;
          finish({
            id: cred.id,
            rawId: u8ToB64url(cred.rawId),
            type: cred.type,
            response: {
              clientDataJSON: u8ToB64url(resp.clientDataJSON),
              authenticatorData: resp.authenticatorData ? u8ToB64url(resp.authenticatorData) : "",
              signature: resp.signature ? u8ToB64url(resp.signature) : "",
              userHandle: resp.userHandle ? u8ToB64url(resp.userHandle) : "",
            },
          });
        } catch (err) {
          finish({ error: String(err && err.message ? err.message : err) });
        }
      })();
      return id | 0;
    },
    mb_web_wa_out(id) {
      const slot = waSlots.get(id | 0);
      return rt.mkCStr(slot && slot.done ? slot.out : "");
    },
    mb_web_wa_end(id) {
      waSlots.delete(id | 0);
    },

    // External posts
    mb_web_post_take() {
      const H = host();
      return H && H.postTake ? (H.postTake() | 0) : 0;
    },
    mb_web_post_next() {
      const H = host();
      return rt.mkCStr(H && H.postNext ? H.postNext() : "");
    },
    mb_web_post_json() {
      const H = host();
      return rt.mkCStr(H && H.postJson ? H.postJson() : "");
    },
    mb_web_post_bytes() {
      const H = host();
      return allocRawBytes(H && H.postBytes ? H.postBytes() : new Uint8Array(0));
    },

    // Device & notifications
    mb_web_keep_awake(on) {
      const H = host();
      state.keepAwake = !!on;
      if (H && H.keepAwake) H.keepAwake(!!on);
    },
    mb_web_key_catch(on) {
      const H = host();
      if (H && H.keyCatch) H.keyCatch(!!on);
    },
    mb_web_ime_inset() {
      const H = host();
      return H && H.imeInset ? H.imeInset() : 0;
    },
    mb_web_ime_action(k) {
      const H = host();
      if (H && H.imeAction) H.imeAction(k | 0);
    },
    mb_web_haptic(k) {
      const H = host();
      if (H && H.haptic) H.haptic(k | 0);
    },
    mb_web_nt_show(chP, chL, tagP, tagL, titleP, titleL, bodyP, bodyL, opensP, opensL) {
      const H = host();
      if (H && H.noticeShow) {
        H.noticeShow(
          rt.readUtf8(chP, chL),
          rt.readUtf8(tagP, tagL),
          rt.readUtf8(titleP, titleL),
          rt.readUtf8(bodyP, bodyL),
          rt.readUtf8(opensP, opensL),
        );
      }
    },
    mb_web_nt_cancel(tagP, tagL) {
      const H = host();
      if (H && H.noticeCancel) H.noticeCancel(rt.readUtf8(tagP, tagL));
    },
    mb_web_nt_state(what) {
      const H = host();
      return H && H.noticeState ? (H.noticeState(what | 0) | 0) : 0;
    },
    mb_web_nt_text(what) {
      const H = host();
      return rt.mkCStr(H && H.noticeText ? H.noticeText(what | 0) : "");
    },

    // Native views (iframe / video / canvas / site bridge)
    mb_web_view_open(kind, argPtr, argLen) {
      const H = host();
      return H && H.viewOpen ? (H.viewOpen(kind | 0, rt.readUtf8(argPtr, argLen)) | 0) : 0;
    },
    mb_web_view_place(id, x, y, w, h, cx0, cy0, cx1, cy1, r0, r1, r2, r3, shown) {
      const H = host();
      if (H && H.viewPlace) {
        H.viewPlace(id | 0, x, y, w, h, cx0, cy0, cx1, cy1, !!shown, r0, r1, r2, r3);
      }
    },
    mb_web_view_close(id) {
      const H = host();
      if (H && H.viewClose) H.viewClose(id | 0);
    },
    mb_web_view_control(id, op, value) {
      const H = host();
      if (H && H.viewControl) H.viewControl(id | 0, op | 0, value);
    },
    mb_web_view_info(id, outPtr) {
      const H = host();
      if (!H || !H.viewInfo) return -1;
      return H.viewInfo(id | 0, f64View(outPtr, 8)) | 0;
    },
    mb_web_view_read_next(id, outPtr) {
      const H = host();
      if (!H || !H.viewReadNext) return 0;
      return H.viewReadNext(id | 0, f64View(outPtr, 4)) | 0;
    },
    mb_web_view_read_answer(id, req, dataPtr, dataLen, total, more) {
      const H = host();
      if (H && H.viewReadAnswer) {
        H.viewReadAnswer(id | 0, req | 0, u8Slice(dataPtr, dataLen), total, !!more);
      }
    },
    mb_web_view_site_next(id) {
      const H = host();
      return rt.mkCStr((H && H.viewSiteNext ? H.viewSiteNext(id | 0) : "") || "");
    },
    mb_web_view_site_body(id, req) {
      const H = host();
      return allocRawBytes(H && H.viewSiteBody ? H.viewSiteBody(id | 0, req | 0) : new Uint8Array(0));
    },
    mb_web_view_site_head(id, req, status, headersPtr, headersLen) {
      const H = host();
      if (H && H.viewSiteHead) H.viewSiteHead(id | 0, req | 0, status | 0, rt.readUtf8(headersPtr, headersLen));
    },
    mb_web_view_site_chunk(id, req, dataPtr, dataLen) {
      const H = host();
      if (H && H.viewSiteChunk) H.viewSiteChunk(id | 0, req | 0, u8Slice(dataPtr, dataLen));
    },
    mb_web_view_site_end(id, req, errPtr, errLen) {
      const H = host();
      if (H && H.viewSiteEnd) H.viewSiteEnd(id | 0, req | 0, rt.readUtf8(errPtr, errLen));
    },
    mb_web_view_site_state(id) {
      const H = host();
      return rt.mkCStr((H && H.viewSiteState ? H.viewSiteState(id | 0) : "") || "");
    },
    mb_web_view_site_load(id, pathPtr, pathLen) {
      const H = host();
      if (H && H.viewSiteLoad) H.viewSiteLoad(id | 0, rt.readUtf8(pathPtr, pathLen));
    },
    mb_web_view_tell(id, what, textPtr, textLen) {
      const H = host();
      if (H && H.viewTell) H.viewTell(id | 0, what | 0, rt.readUtf8(textPtr, textLen));
    },
    mb_web_view_text(id, what) {
      const H = host();
      return rt.mkCStr((H && H.viewText ? H.viewText(id | 0, what | 0) : "") || "");
    },
    mb_web_view_still(id) {
      const H = host();
      return allocRawBytes(H && H.viewStill ? H.viewStill(id | 0) : new Uint8Array(0));
    },

    // Streaming media (WebCodecs / MediaSource / WebAudio)
    mb_web_st_open() {
      const H = host();
      return H && H.streamOpen ? (H.streamOpen() | 0) : 0;
    },
    mb_web_st_h264(id, avccPtr, avccLen, w, h) {
      const H = host();
      if (H && H.streamH264) H.streamH264(id | 0, u8Slice(avccPtr, avccLen), w | 0, h | 0);
    },
    mb_web_st_hevc(id, hvccPtr, hvccLen, w, h) {
      const H = host();
      const data = u8Slice(hvccPtr, hvccLen);
      if (H && H.streamHevc) H.streamHevc(id | 0, data, w | 0, h | 0);
      else if (H && H.streamCodec) H.streamCodec(id | 0, "hevc", data);
    },
    mb_web_st_av1(id, av1cPtr, av1cLen, w, h) {
      const H = host();
      const data = u8Slice(av1cPtr, av1cLen);
      if (H && H.streamAv1) H.streamAv1(id | 0, data, w | 0, h | 0);
      else if (H && H.streamCodec) H.streamCodec(id | 0, "av1", data);
    },
    mb_web_st_frame(id, dataPtr, dataLen, key, ts) {
      const H = host();
      if (H && H.streamFrame) H.streamFrame(id | 0, u8Slice(dataPtr, dataLen), key | 0, ts);
    },
    mb_web_st_jpeg(id, dataPtr, dataLen, ts) {
      const H = host();
      if (H && H.streamJpeg) H.streamJpeg(id | 0, u8Slice(dataPtr, dataLen), ts);
    },
    mb_web_st_next(id) {
      const H = host();
      return rt.mkCStr((H && H.streamNext ? H.streamNext(id | 0) : "") || "");
    },
    mb_web_st_show(id, ts) {
      const H = host();
      if (H && H.streamShow) H.streamShow(id | 0, ts);
    },
    mb_web_st_close(id) {
      const H = host();
      if (H && H.streamClose) H.streamClose(id | 0);
      const a = audioStreams.get(id | 0);
      if (a && a.ctx) {
        try { a.ctx.close(); } catch {}
      }
      audioStreams.delete(id | 0);
    },
    mb_web_st_h264_here() {
      const H = host();
      return H && H.h264Here ? (H.h264Here() | 0) : (typeof VideoDecoder !== "undefined" ? 1 : 0);
    },
    mb_web_st_h265_here() {
      const H = host();
      return H && H.h265Here ? (H.h265Here() | 0) : 0;
    },
    mb_web_st_av1_here() {
      const H = host();
      return H && H.av1Here ? (H.av1Here() | 0) : 0;
    },
    mb_web_st_feeds() {
      const H = host();
      return H && H.streamFeeds ? (H.streamFeeds() | 0) : 0;
    },
    mb_web_st_codec(id, cmdPtr, cmdLen, dataPtr, dataLen) {
      const H = host();
      if (H && H.streamCodec) {
        H.streamCodec(id | 0, rt.readUtf8(cmdPtr, cmdLen), u8Slice(dataPtr, dataLen));
      }
    },
    mb_web_st_audio_here() {
      return typeof AudioContext !== "undefined" || typeof webkitAudioContext !== "undefined" ? 1 : 0;
    },
    mb_web_st_audio(id, dataPtr, dataLen) {
      const AC = typeof AudioContext !== "undefined" ? AudioContext : (typeof webkitAudioContext !== "undefined" ? webkitAudioContext : null);
      if (!AC) return;
      const bytes = u8Slice(dataPtr, dataLen);
      if (bytes.length < 4) return;
      const s = getAudioStream(id | 0);
      if (!s.ctx) {
        s.ctx = new AC({ sampleRate: 48000 });
        s.gain = s.ctx.createGain();
        s.gain.gain.value = s.volume;
        s.gain.connect(s.ctx.destination);
      }
      if (s.ctx.state === "suspended") s.ctx.resume().catch(() => {});
      // Interpret payload as interleaved 16-bit signed LE stereo PCM @ 48kHz
      const frames = Math.floor(bytes.length / 4);
      if (frames <= 0) return;
      const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const ab = s.ctx.createBuffer(2, frames, 48000);
      const ch0 = ab.getChannelData(0);
      const ch1 = ab.getChannelData(1);
      for (let i = 0; i < frames; i++) {
        ch0[i] = dv.getInt16(i * 4, true) / 32768;
        ch1[i] = dv.getInt16(i * 4 + 2, true) / 32768;
      }
      const src = s.ctx.createBufferSource();
      src.buffer = ab;
      src.connect(s.gain);
      const now = s.ctx.currentTime;
      const startAt = Math.max(now + 0.01, s.nextTime);
      src.start(startAt);
      const dur = frames / 48000;
      s.nextTime = startAt + dur;
      s.playedSec = now;
      s.queuedSec = Math.max(0, s.nextTime - now);
    },
    mb_web_st_audio_played(id) {
      const s = audioStreams.get(id | 0);
      return s && s.ctx ? s.ctx.currentTime * 1000 : 0;
    },
    mb_web_st_audio_queued(id) {
      const s = audioStreams.get(id | 0);
      return s && s.ctx ? Math.max(0, s.nextTime - s.ctx.currentTime) * 1000 : 0;
    },
    mb_web_st_audio_wanted(_id) {
      return 40.0;
    },
    mb_web_st_audio_volume(id, vol) {
      const s = getAudioStream(id | 0);
      s.volume = Math.max(0, Math.min(2, vol));
      if (s.gain) s.gain.gain.value = s.volume;
    },
    mb_web_st_flush(id, drain) {
      const H = host();
      if (H && H.streamFlush) H.streamFlush(id | 0, drain | 0);
      const s = audioStreams.get(id | 0);
      if (s && s.ctx) {
        s.nextTime = s.ctx.currentTime;
        s.queuedSec = 0;
      }
    },
    mb_web_st_layers() {
      const H = host();
      return H && H.streamLayers ? (H.streamLayers() | 0) : 0;
    },
    mb_web_st_pointer(id, pngPtr, pngLen, w, h, hx, hy) {
      const H = host();
      if (H && H.streamPointer) H.streamPointer(id | 0, u8Slice(pngPtr, pngLen), w, h, hx, hy);
    },
    mb_web_st_pointer_at(id, x, y, shown) {
      const H = host();
      if (H && H.streamPointerAt) H.streamPointerAt(id | 0, x, y, shown | 0);
    },
    mb_web_st_direct(id, route, after, what) {
      const H = host();
      if (H && H.streamDirect) H.streamDirect(id | 0, route, after, what | 0);
    },

    // Terminal bridge (ghostty-vt.wasm via host.js)
    mb_web_tm_open(cols, rows) {
      const H = host();
      return H && H.termOpen ? (H.termOpen(cols | 0, rows | 0) | 0) : 0;
    },
    mb_web_tm_write(id, dataPtr, dataLen) {
      const H = host();
      if (H && H.termWrite) H.termWrite(id | 0, u8Slice(dataPtr, dataLen));
    },
    mb_web_tm_resize(id, cols, rows, cw, ch) {
      const H = host();
      if (H && H.termResize) H.termResize(id | 0, cols | 0, rows | 0, cw | 0, ch | 0);
    },
    mb_web_tm_frame(id, flags) {
      const H = host();
      return allocRawBytes(H && H.termFrame ? H.termFrame(id | 0, flags | 0) : new Uint8Array(0));
    },
    mb_web_tm_modes(id) {
      const H = host();
      return H && H.termModes ? (H.termModes(id | 0) | 0) : 0;
    },
    mb_web_tm_scroll(id, kind, value) {
      const H = host();
      if (H && H.termScroll) H.termScroll(id | 0, kind | 0, value | 0);
    },
    mb_web_tm_select(id, kind, x0, y0, x1, y1) {
      const H = host();
      return H && H.termSelect ? (H.termSelect(id | 0, kind | 0, x0 | 0, y0 | 0, x1 | 0, y1 | 0) | 0) : 0;
    },
    mb_web_tm_text(id, sel) {
      const H = host();
      return rt.mkCStr((H && H.termText ? H.termText(id | 0, sel | 0) : "") || "");
    },
    mb_web_tm_mouse(id, action, button, mods, x, y, w, h, cw, ch) {
      const H = host();
      return allocRawBytes(
        H && H.termMouse
          ? H.termMouse(id | 0, action | 0, button | 0, mods | 0, x | 0, y | 0, w | 0, h | 0, cw | 0, ch | 0)
          : new Uint8Array(0),
      );
    },
    mb_web_tm_close(id) {
      const H = host();
      if (H && H.termClose) H.termClose(id | 0);
    },

    // Native bridge calls
    mb_web_ncall_call(namePtr, nameLen, paramsPtr, paramsLen) {
      const H = host();
      return H && H.nativeCall ? (H.nativeCall(rt.readUtf8(namePtr, nameLen), rt.readUtf8(paramsPtr, paramsLen)) | 0) : 0;
    },
    mb_web_ncall_state(id) {
      const H = host();
      return H && H.nativeState ? (H.nativeState(id | 0) | 0) : -1;
    },
    mb_web_ncall_result(id) {
      const H = host();
      return rt.mkCStr((H && H.nativeResult ? H.nativeResult(id | 0) : "") || "");
    },
    mb_web_ncall_end(id) {
      const H = host();
      if (H && H.nativeEnd) H.nativeEnd(id | 0);
    },

    // Pointer lock & gamepads (W3C Standard Gamepad layout: 9 doubles per pad)
    mb_web_pointer_lock(on) {
      if (typeof document === "undefined") return;
      try {
        if (on) {
          const target = document.querySelector("canvas") || document.documentElement;
          if (target && target.requestPointerLock) target.requestPointerLock();
        } else if (document.pointerLockElement && document.exitPointerLock) {
          document.exitPointerLock();
        }
      } catch {}
    },
    mb_web_pointer_locked() {
      return typeof document !== "undefined" && !!document.pointerLockElement ? 1 : 0;
    },
    mb_web_pointer_delta(outPtr) {
      const out = f64View(outPtr, 2);
      if (out.length >= 2) {
        out[0] = pointerDx;
        out[1] = pointerDy;
      }
      pointerDx = 0;
      pointerDy = 0;
    },
    mb_web_gamepads(outPtr, maxFields) {
      if (typeof navigator === "undefined" || typeof navigator.getGamepads !== "function") return 0;
      const pads = navigator.getGamepads() || [];
      const out = f64View(outPtr, maxFields | 0);
      const maxPads = Math.min(4, Math.floor(out.length / 9));
      let count = 0;
      for (let i = 0; i < pads.length && count < maxPads; i++) {
        const gp = pads[i];
        if (!gp || !gp.connected) continue;
        let bits = 0;
        const btns = gp.buttons || [];
        for (let b = 0; b < Math.min(17, btns.length); b++) {
          if (btns[b] && btns[b].pressed) bits |= 1 << b;
        }
        const axes = gp.axes || [];
        const o = count * 9;
        out[o] = gp.index | 0;
        out[o + 1] = 1;
        out[o + 2] = bits;
        out[o + 3] = axes[0] || 0;
        out[o + 4] = axes[1] || 0;
        out[o + 5] = axes[2] || 0;
        out[o + 6] = axes[3] || 0;
        out[o + 7] = btns[6] ? (btns[6].value || 0) : 0;
        out[o + 8] = btns[7] ? (btns[7].value || 0) : 0;
        count++;
      }
      return count | 0;
    },

    mb_web_log(sPtr, sLen) {
      const H = host();
      const s = rt.readUtf8(sPtr, sLen);
      if (H && H.log) H.log(s);
      else console.log(s);
    },
    mb_web_bench_mode() {
      const H = host();
      return H && H.benchMode ? (H.benchMode() | 0) : 0;
    },
    mb_web_exit(code) {
      const H = host();
      if (H && H.exit) H.exit(code | 0);
    },
    mb_web_set_cursor(kind) {
      const H = host();
      state.lastCursor = kind | 0;
      if (H && H.setCursor) H.setCursor(kind | 0);
    },
    mb_web_env(namePtr, nameLen) {
      const H = host();
      const name = rt.readUtf8(namePtr, nameLen);
      const val = H && H.env ? H.env(name) : (typeof location !== "undefined" ? (new URLSearchParams(location.search).get(name) || "") : "");
      return rt.mkCStr(val);
    },
    mb_web_system_dark() {
      return typeof matchMedia === "function" && matchMedia("(prefers-color-scheme: dark)").matches ? 1 : 0;
    },
    mb_web_reduce_motion() {
      return typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches ? 1 : 0;
    },
    mb_web_reduce_transparency() {
      const H = host();
      return H && H.reduceTransparency && H.reduceTransparency() ? 1 : 0;
    },
    mb_web_increase_contrast() {
      const H = host();
      if (H && H.increaseContrast) return H.increaseContrast() ? 1 : 0;
      return typeof matchMedia === "function" && matchMedia("(prefers-contrast: more)").matches ? 1 : 0;
    },
    mb_web_keys_platform() {
      if (typeof navigator === "undefined") return -1;
      const p = (navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || "";
      return /mac|iphone|ipad|ipod|ios/i.test(p) ? 0 : /android/i.test(p) ? 2 : /win/i.test(p) ? 3 : /linux|cros|x11/i.test(p) ? 5 : -1;
    },
    mb_web_native_pointing() {
      if (typeof matchMedia !== "function") return 0;
      return matchMedia("(pointer: coarse)").matches ? 1 : matchMedia("(pointer: fine)").matches ? 2 : 0;
    },
    mb_web_frame_awaiting(n, whatPtr, whatLen) {
      const H = host();
      if (H && H.frameAwaiting) H.frameAwaiting(n | 0, rt.readUtf8(whatPtr, whatLen));
    },
    mb_web_set_outside() {
      const H = host();
      if (!H || !H.setOutside) return 0;
      H.setOutside((kind, id, st) => {
        const exp = rt.getExports();
        if (exp && exp.web_outside) exp.web_outside(kind | 0, id | 0, st | 0);
      });
      return 1;
    },
    mb_web_set_back() {
      const H = host();
      if (!H || !H.setBack) return;
      H.setBack((kind, x, p, e) => {
        const exp = rt.getExports();
        if (exp && exp.web_back) exp.web_back(kind | 0, x, p, e | 0);
      });
    },
    mb_web_back_enabled(on) {
      const H = host();
      if (H && H.backEnabled) H.backEnabled(!!on);
    },
    mb_web_set_app_state() {
      const H = host();
      if (!H || !H.setAppState) return;
      H.setAppState((st) => {
        const exp = rt.getExports();
        if (exp && exp.web_app_state) exp.web_app_state(st | 0);
      });
    },

    // Images, URLs & performance metrics
    mb_web_image_load(srcPtr, srcLen, maxPx) {
      const H = host();
      return H && H.imageLoad ? (H.imageLoad(rt.readUtf8(srcPtr, srcLen), maxPx | 0) | 0) : 0;
    },
    mb_web_image_load_bytes(dataPtr, dataLen, maxPx) {
      const H = host();
      return H && H.imageLoadBytes ? (H.imageLoadBytes(u8Slice(dataPtr, dataLen), maxPx | 0) | 0) : 0;
    },
    mb_web_image_info(id, outPtr) {
      const H = host();
      if (!H || !H.imageInfo) return 0;
      return H.imageInfo(id | 0, f64View(outPtr, 4)) | 0;
    },
    mb_web_image_free(id) {
      const H = host();
      if (H && H.imageFree) H.imageFree(id | 0);
    },
    mb_web_image_pixels(id, outPtr, outLen) {
      const H = host();
      const mem = rt.getMemory();
      const p = Number(outPtr);
      const n = outLen | 0;
      if (!H || !H.imagePixels || !mem || !p || n <= 0) return 0;
      const tmp = new Uint8Array(n);
      const ok = H.imagePixels(id | 0, tmp);
      if (ok !== 1) return 0;
      const dv = new DataView(mem.buffer);
      for (let i = 0; i < n; i++) {
        dv.setUint32(p + i * 8, tmp[i], true);
        dv.setUint32(p + i * 8 + 4, 0, true);
      }
      return 1;
    },
    mb_web_open_url(urlPtr, urlLen) {
      const H = host();
      const url = rt.readUtf8(urlPtr, urlLen);
      if (H && H.openUrl) return H.openUrl(url) ? 1 : 0;
      window.open(url, "_blank", "noopener");
      return 1;
    },
    mb_web_gpu_ms() {
      const H = host();
      return H && H.gpuMs ? H.gpuMs() : -1;
    },
    mb_web_mem_bytes() {
      const mem = rt.getMemory();
      return mem ? mem.buffer.byteLength : 0;
    },
    mb_web_frames_dropped() {
      return framesDropped | 0;
    },
    mb_web_cpu_ms() {
      return cpuMsTotal;
    },
    mb_web_perf_state() {
      return perfRecording ? 1 : 0;
    },
    mb_web_set_perf(on) {
      perfRecording = !!on;
      if (on) {
        perfLines.length = 0;
        perfBytes = 0;
      }
    },
    mb_web_perf_line(linePtr, lineLen) {
      if (!perfRecording) return;
      const line = rt.readUtf8(linePtr, lineLen) + "\n";
      perfLines.push(line);
      perfBytes += line.length;
      if (perfLines.length > 10000) {
        const dropped = perfLines.shift();
        perfBytes -= dropped.length;
      }
    },
    mb_web_perf_where() {
      return rt.mkCStr("memory://invar-perf.log");
    },
    mb_web_perf_size() {
      return perfBytes;
    },
    mb_web_perf_read() {
      const text = perfLines.join("");
      return allocRawBytes(new TextEncoder().encode(text));
    },
    mb_web_perf_open(go) {
      if (!perfLines.length) return 0;
      if (!go) return 1;
      const bytes = new TextEncoder().encode(perfLines.join(""));
      return triggerDownload("invar-perf.log", bytes, "text/plain") ? 1 : 0;
    },
    getenv() {
      return 0n;
    },
  };

  Object.assign(rt.imports.pion_rt, bridge);
  return {
    frameTimes,
    keyUpEvents,
    state,
    getModsHeld: () => bridge.mb_web_mods_held(),
    bridge,
  };
}
