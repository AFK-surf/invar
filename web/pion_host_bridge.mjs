// Zero-copy bridge between Pion wasm32 (`packages/host_web`) and `web/host.js` (`globalThis.__mbhost`).
// Attaches `mb_web_*` imports onto `rt.imports.pion_rt` and routes `H.run` callbacks + desktop events
// (modifier state, keyup, async clipboard, picker, downloads, keep-awake, drag-and-drop) to Wasm exports.

const INST = 60;
const KEYMAP = {
  Backspace: 1, Delete: 2, ArrowLeft: 3, ArrowRight: 4, ArrowUp: 5, ArrowDown: 6,
  Enter: 7, Home: 8, End: 9, PageUp: 15, PageDown: 16, Escape: 17, Tab: 18,
};
const MOD_KEYS = new Set(["Shift", "Control", "Alt", "Meta", "OS"]);

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
  const state = {
    lastCursor: 0,
    lastClipboardText: "",
    lastTitle: "",
    keepAwake: false,
    lastIme: null,
  };

  const modsFromEvent = (e) =>
    (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);

  const keyCodeOf = (e) => {
    if (KEYMAP[e.key]) return KEYMAP[e.key];
    if (MOD_KEYS.has(e.key)) return 0;
    if (e.key && e.key.length === 1) return 1000 + e.key.toLowerCase().charCodeAt(0);
    return 0;
  };

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
          exp.web_frame(w, h, scale, now);
          const dt = performance.now() - t0;
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

    // Keep / download
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
      const val = H && H.env ? H.env(name) : (new URLSearchParams(location.search).get(name) || "");
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
