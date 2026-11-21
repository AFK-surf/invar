// Zero-copy bridge between Pion wasm32 (`packages/host_web`) and `web/host.js` (`globalThis.__mbhost`).
// Attaches `mb_web_*` imports onto `rt.imports.pion_rt` and routes `H.run` callbacks to Wasm exports.

const INST = 60;

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

  let firstFrameReported = false;
  const frameTimes = [];

  const bridge = {
    mb_web_run(titlePtr, titleLen) {
      const H = host();
      const title = rt.readUtf8(titlePtr, titleLen);
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
      if (H && H.dirty) H.dirty(x, y, w, h);
    },
    mb_web_glass_light_angle(rad) {
      const H = host();
      if (H && H.glassLightAngle) H.glassLightAngle(rad);
    },

    mb_web_set_ime(active, x, y, w, h, textPtr, textLen, selStart, selEnd) {
      const H = host();
      if (H && H.setIme) {
        H.setIme(!!active, x, y, w, h, rt.readUtf8(textPtr, textLen), selStart | 0, selEnd | 0);
      }
    },
    mb_web_clipboard_set(sPtr, sLen) {
      const H = host();
      if (H && H.clipboardSet) H.clipboardSet(rt.readUtf8(sPtr, sLen));
    },
    mb_web_log(sPtr, sLen) {
      const H = host();
      const s = rt.readUtf8(sPtr, sLen);
      if (H && H.log) H.log(s);
      else console.log(s);
    },
    mb_web_set_cursor(kind) {
      const H = host();
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
  return { frameTimes };
}
