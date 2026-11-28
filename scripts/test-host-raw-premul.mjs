#!/usr/bin/env node
// Tests for Web host MBRAWPRE premultiplied RGBA fast path, validation, downscale, readback,
// context-lost / null texture guards, and secondary window slotTex behavior.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../web/host.js', import.meta.url), 'utf8');

function makeRawPremul(w, h, fill = [100, 50, 25, 128], payloadLen = null) {
  const n = payloadLen === null ? w * h * 4 : payloadLen;
  const buf = new Uint8Array(16 + n);
  buf.set([0x4d, 0x42, 0x52, 0x41, 0x57, 0x50, 0x52, 0x45], 0);
  const dv = new DataView(buf.buffer);
  dv.setUint32(8, w >>> 0, true);
  dv.setUint32(12, h >>> 0, true);
  if (payloadLen === null) {
    for (let i = 0; i < w * h; i++) {
      buf[16 + i * 4 + 0] = fill[0];
      buf[16 + i * 4 + 1] = fill[1];
      buf[16 + i * 4 + 2] = fill[2];
      buf[16 + i * 4 + 3] = fill[3];
    }
  }
  return buf;
}

function makeMockGl({ maxTex = 4096, glErrorOnUpload = 0, contextLost = false, nullCreateTex = false, layerFill = null } = {}) {
  const pixelStoreCalls = [];
  const texUploads = [];
  const deletedTexs = [];
  const textures = new Map();
  let nextTex = 1;
  let boundTex = null;
  let fbTex = null;
  let pendingError = 0;
  const state = { maxTex, glErrorOnUpload, contextLost, nullCreateTex };

  const gl = new Proxy(
    {
      NO_ERROR: 0,
      MAX_TEXTURE_SIZE: 0x0d33,
      UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241,
      UNPACK_ALIGNMENT: 0x0cf5,
      FRAMEBUFFER_BINDING: 0x8ca6,
      FRAMEBUFFER_COMPLETE: 0x8cd5,
      TEXTURE_2D: 0x0de1,
      RGBA: 0x1908,
      UNSIGNED_BYTE: 0x1401,
    },
    {
      get(target, name) {
        if (name in target) return target[name];
        if (name === 'isContextLost') return () => state.contextLost;
        if (name === 'getShaderParameter' || name === 'getProgramParameter') return () => true;
        if (name === 'getParameter') {
          return (p) => (p === target.MAX_TEXTURE_SIZE ? state.maxTex : null);
        }
        if (name === 'getError') {
          return () => {
            const e = pendingError;
            pendingError = 0;
            return e;
          };
        }
        if (name === 'createTexture') {
          return () => (state.nullCreateTex ? null : { id: nextTex++ });
        }
        if (name === 'bindTexture') return (_, t) => { boundTex = t; };
        if (name === 'deleteTexture') {
          return (t) => {
            deletedTexs.push(t);
            textures.delete(t);
          };
        }
        if (name === 'pixelStorei') return (k, v) => pixelStoreCalls.push([k, v]);
        if (name === 'texStorage2D') {
          return (_target, _levels, _internalformat, w, h) => {
            const px = new Uint8Array(w * h * 4);
            if (layerFill) {
              for (let i = 0; i < w * h; i++) {
                px[i * 4 + 0] = layerFill[0];
                px[i * 4 + 1] = layerFill[1];
                px[i * 4 + 2] = layerFill[2];
                px[i * 4 + 3] = layerFill[3];
              }
            }
            textures.set(boundTex, { w, h, px });
          };
        }
        if (name === 'texImage2D') {
          return (...args) => {
            // (target, level, internalformat, w, h, border, format, type, pixels)
            if (args.length === 9 && args[8]) {
              const [, , , w, h, , , , px] = args;
              texUploads.push({ tex: boundTex, w, h, px: new Uint8Array(px) });
              textures.set(boundTex, { w, h, px: new Uint8Array(px) });
              if (state.glErrorOnUpload) pendingError = state.glErrorOnUpload;
            }
          };
        }
        if (name === 'framebufferTexture2D') return (_, __, ___, t) => { fbTex = t; };
        if (name === 'checkFramebufferStatus') return () => target.FRAMEBUFFER_COMPLETE;
        if (name === 'readPixels') {
          return (_x, _y, _w, _h, _fmt, _type, dst) => {
            const entry = textures.get(fbTex);
            if (entry) dst.set(entry.px);
          };
        }
        return () => ({});
      },
    },
  );
  return { gl, state, pixelStoreCalls, texUploads, deletedTexs, textures };
}

function makeBrowser({
  maxTex = 4096,
  glErrorOnUpload = 0,
  contextLost = false,
  nullCreateTex = false,
  layerFill = null,
  secondaryGlOpts = null,
  onFrame = null,
} = {}) {
  const element = () => ({
    style: {},
    attributes: [],
    listeners: new Map(),
    setAttribute() {},
    appendChild() {},
    remove() {},
    addEventListener(name, f) {
      this.listeners.set(name, [...(this.listeners.get(name) || []), f]);
    },
    dispatch(name, e = {}) {
      for (const f of this.listeners.get(name) || []) f(e);
    },
  });
  const mainMock = makeMockGl({ maxTex, glErrorOnUpload, contextLost, nullCreateTex, layerFill });
  let secondaryMock = null;
  const popupRafs = [];
  let createImageBitmapCalls = 0;
  const outsideEvents = [];

  const canvas = {
    ...element(),
    width: 0,
    height: 0,
    getContext: () => mainMock.gl,
    getBoundingClientRect: () => ({ width: 200, height: 200, left: 0, top: 0 }),
  };
  const ime = element();
  const body = element();
  const head = element();
  const document = {
    title: 'main',
    body,
    head,
    documentElement: head,
    createElement: () => ({ ...element(), getContext: () => ({}) }),
    getElementById: (id) => (id === 'c' ? canvas : ime),
    addEventListener() {},
  };
  const win = {
    ...element(),
    document,
    navigator: { platform: 'MacIntel' },
    location: { search: '' },
    URLSearchParams,
    Intl,
    Blob,
    createImageBitmap: async () => {
      createImageBitmapCalls++;
      throw new Error('createImageBitmap must not be called for MBRAWPRE');
    },
    devicePixelRatio: 1,
    performance: { now: () => 0 },
    queueMicrotask,
    setTimeout,
    requestAnimationFrame() {},
    getComputedStyle: () => ({ backgroundColor: '' }),
    open() {
      secondaryMock = makeMockGl(secondaryGlOpts || { maxTex: 4 });
      const ccanvas = {
        ...element(),
        width: 0,
        height: 0,
        getContext: () => secondaryMock.gl,
        getBoundingClientRect: () => ({ width: 200, height: 200, left: 0, top: 0 }),
      };
      const cdoc = {
        title: '',
        body: element(),
        head: element(),
        documentElement: element(),
        createElement: (tag) => (tag === 'canvas' ? ccanvas : element()),
        addEventListener() {},
      };
      const child = {
        ...element(),
        closed: false,
        innerWidth: 200,
        innerHeight: 200,
        screenX: 0,
        screenY: 0,
        devicePixelRatio: 1,
        document: cdoc,
        focus() {},
        close() { this.closed = true; },
        requestAnimationFrame(fn) {
          popupRafs.push(fn);
          return popupRafs.length;
        },
        cancelAnimationFrame() {},
      };
      return child;
    },
  };
  win.window = win;
  vm.runInNewContext(source, win, { filename: 'host.js' });
  const host = win.__mbhost;
  host.setOutside((kind, id, state) => outsideEvents.push({ kind, id, state }));
  host.run(
    'test',
    onFrame || (() => {}),
    () => {},
    () => {},
    () => {},
    () => {},
  );
  return {
    host,
    canvas,
    gl: mainMock.gl,
    glState: mainMock.state,
    pixelStoreCalls: mainMock.pixelStoreCalls,
    texUploads: mainMock.texUploads,
    deletedTexs: mainMock.deletedTexs,
    outsideEvents,
    popupRafs,
    get secondary() {
      return secondaryMock;
    },
    get createImageBitmapCalls() {
      return createImageBitmapCalls;
    },
  };
}

const tick = () => Promise.resolve().then(() => new Promise((r) => setTimeout(r, 0)));

test('MBRAWPRE preserves async contract, uploads with UNPACK_PREMULTIPLY_ALPHA_WEBGL=false, and reads back exact premul pixels', async () => {
  const b = makeBrowser();
  const payload = makeRawPremul(4, 2, [100, 50, 25, 128]);
  const id = b.host.imageLoadBytes(payload, 64);
  const info = [0, 0, 0, 0];
  assert.equal(b.host.imageInfo(id, info), 0);
  await tick();
  assert.equal(b.host.imageInfo(id, info), 1);
  assert.deepEqual(info, [4, 2, 4, 2]);
  assert.equal(b.createImageBitmapCalls, 0);
  assert.ok(
    b.pixelStoreCalls.some(([k, v]) => k === b.gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL && v === false),
    'expected UNPACK_PREMULTIPLY_ALPHA_WEBGL = false',
  );
  assert.ok(
    b.pixelStoreCalls.some(([k, v]) => k === b.gl.UNPACK_ALIGNMENT && v === 4),
    'expected UNPACK_ALIGNMENT = 4',
  );
  const out = new Uint8Array(4 * 2 * 4);
  assert.equal(b.host.imagePixels(id, out), 1);
  assert.deepEqual(Array.from(out.slice(0, 4)), [100, 50, 25, 128]);
  assert.equal(b.outsideEvents.filter((e) => e.id === id).length, 1);
  assert.equal(b.outsideEvents.find((e) => e.id === id).state, 1);
});

test('MBRAWPRE downscales when exceeding maxPx or gl.MAX_TEXTURE_SIZE while keeping original dimensions', async () => {
  const b = makeBrowser({ maxTex: 4 });
  const payload = makeRawPremul(8, 4, [80, 40, 20, 128]);
  const id = b.host.imageLoadBytes(payload, 64);
  await tick();
  const info = [0, 0, 0, 0];
  assert.equal(b.host.imageInfo(id, info), 1);
  assert.deepEqual(info, [4, 2, 8, 4]);
  const out = new Uint8Array(4 * 2 * 4);
  assert.equal(b.host.imagePixels(id, out), 1);
  assert.deepEqual(Array.from(out.slice(0, 4)), [80, 40, 20, 128]);
  assert.equal(b.createImageBitmapCalls, 0);
});

test('MBRAWPRE 4x and 3.2x area-averaging downscale preserves 1px vertical white lines without aliasing', async () => {
  const b = makeBrowser();
  const payload = new Uint8Array(16 + 16 * 4 * 4);
  payload.set([0x4d, 0x42, 0x52, 0x41, 0x57, 0x50, 0x52, 0x45], 0);
  const dv = new DataView(payload.buffer);
  dv.setUint32(8, 16, true);
  dv.setUint32(12, 4, true);
  for (let y = 0; y < 4; y++) {
    for (let x = 0; x < 16; x += 4) {
      const o = 16 + (y * 16 + x) * 4;
      payload[o] = 255;
      payload[o + 1] = 255;
      payload[o + 2] = 255;
      payload[o + 3] = 255;
    }
  }
  const id = b.host.imageLoadBytes(payload, 4);
  await tick();
  const info = [0, 0, 0, 0];
  assert.equal(b.host.imageInfo(id, info), 1);
  assert.deepEqual(info, [4, 1, 16, 4]);
  const out = new Uint8Array(4 * 1 * 4);
  assert.equal(b.host.imagePixels(id, out), 1);
  for (let x = 0; x < 4; x++) {
    assert.deepEqual(Array.from(out.slice(x * 4, x * 4 + 4)), [64, 64, 64, 64]);
  }
  // Non-integer 3.2x downscale (16x4 -> 5x1) conserves total white line energy and maintains rgb <= a
  const id5 = b.host.imageLoadBytes(payload, 5);
  await tick();
  assert.equal(b.host.imageInfo(id5, info), 1);
  assert.deepEqual(info, [5, 1, 16, 4]);
  const out5 = new Uint8Array(5 * 1 * 4);
  assert.equal(b.host.imagePixels(id5, out5), 1);
  let sumR = 0;
  for (let x = 0; x < 5; x++) {
    assert.ok(out5[x * 4 + 0] <= out5[x * 4 + 3]);
    assert.ok(out5[x * 4 + 1] <= out5[x * 4 + 3]);
    assert.ok(out5[x * 4 + 2] <= out5[x * 4 + 3]);
    sumR += out5[x * 4 + 0];
  }
  assert.ok(sumR >= 314 && sumR <= 324, `expected sumR in [314, 324], got ${sumR}`);
});

test('MBRAWPRE downscale yields via macrotask and matches exact integer half-up rounding (98x1->1x1 and 18x1->11x1)', async () => {
  const b = makeBrowser();
  const info = [0, 0, 0, 0];

  // 98x1 -> 1x1: 49 pixels of r=1 + 49 pixels of r=2 => exact 1.5 => r=2, a=255
  const p98 = new Uint8Array(16 + 98 * 4);
  p98.set([0x4d, 0x42, 0x52, 0x41, 0x57, 0x50, 0x52, 0x45], 0);
  const dv98 = new DataView(p98.buffer);
  dv98.setUint32(8, 98, true);
  dv98.setUint32(12, 1, true);
  for (let x = 0; x < 98; x++) {
    p98[16 + x * 4 + 0] = x < 49 ? 1 : 2;
    p98[16 + x * 4 + 3] = 255;
  }
  const id98 = b.host.imageLoadBytes(p98, 1);
  // Downscale path must yield out of the microtask turn before running downscaleRawPremul
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(b.host.imageInfo(id98, info), 0);
  await tick();
  assert.equal(b.host.imageInfo(id98, info), 1);
  assert.deepEqual(info, [1, 1, 98, 1]);
  const out98 = new Uint8Array(4);
  assert.equal(b.host.imagePixels(id98, out98), 1);
  assert.deepEqual(Array.from(out98), [2, 0, 0, 255]);

  // 18x1 -> 11x1: only x=8 has r=1 => dst x=5 overlap 9/18=0.5 => r=1, all others r=0
  const p18 = new Uint8Array(16 + 18 * 4);
  p18.set([0x4d, 0x42, 0x52, 0x41, 0x57, 0x50, 0x52, 0x45], 0);
  const dv18 = new DataView(p18.buffer);
  dv18.setUint32(8, 18, true);
  dv18.setUint32(12, 1, true);
  for (let x = 0; x < 18; x++) {
    p18[16 + x * 4 + 0] = x === 8 ? 1 : 0;
    p18[16 + x * 4 + 3] = 255;
  }
  const id18 = b.host.imageLoadBytes(p18, 11);
  await tick();
  assert.equal(b.host.imageInfo(id18, info), 1);
  assert.deepEqual(info, [11, 1, 18, 1]);
  const out11 = new Uint8Array(11 * 4);
  assert.equal(b.host.imagePixels(id18, out11), 1);
  for (let x = 0; x < 11; x++) {
    assert.deepEqual(Array.from(out11.slice(x * 4, x * 4 + 4)), [x === 5 ? 1 : 0, 0, 0, 255]);
  }
});

test('MBRAWPRE large downscale yields across multiple row chunks and produces exact pixels', async () => {
  const b = makeBrowser();
  const info = [0, 0, 0, 0];
  // 1024x512 -> 512x256: rowsPerChunk = floor(131072 / 1024) = 128 < dh=256 => 2 chunks
  const payload = makeRawPremul(1024, 512, [90, 45, 15, 180]);
  const id = b.host.imageLoadBytes(payload, 512);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(b.host.imageInfo(id, info), 0);
  // After first macrotask only chunk 1 (rows 0..127) has run; upload must not have finished yet
  await tick();
  assert.equal(b.host.imageInfo(id, info), 0);
  // Complete remaining chunk(s)
  for (let i = 0; i < 8 && b.host.imageInfo(id, info) === 0; i++) {
    await tick();
  }
  assert.equal(b.host.imageInfo(id, info), 1);
  assert.deepEqual(info, [512, 256, 1024, 512]);
  const out = new Uint8Array(512 * 256 * 4);
  assert.equal(b.host.imagePixels(id, out), 1);
  assert.deepEqual(Array.from(out.slice(0, 4)), [90, 45, 15, 180]);
  const last = (512 * 256 - 1) * 4;
  assert.deepEqual(Array.from(out.slice(last, last + 4)), [90, 45, 15, 180]);

  // Tall/narrow image: 512x4096 -> 8x64 (srcRowsPerDstRow=64, srcPxPerDstRow=32768, rowsPerChunk=8 < dh=64 => 8 chunks)
  const tallPayload = makeRawPremul(512, 4096, [60, 120, 30, 200]);
  const tallId = b.host.imageLoadBytes(tallPayload, 64);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(b.host.imageInfo(tallId, info), 0);
  await tick();
  assert.equal(b.host.imageInfo(tallId, info), 0);
  for (let i = 0; i < 16 && b.host.imageInfo(tallId, info) === 0; i++) {
    await tick();
  }
  assert.equal(b.host.imageInfo(tallId, info), 1);
  assert.deepEqual(info, [8, 64, 512, 4096]);
  const tallOut = new Uint8Array(8 * 64 * 4);
  assert.equal(b.host.imagePixels(tallId, tallOut), 1);
  assert.deepEqual(Array.from(tallOut.slice(0, 4)), [60, 120, 30, 200]);
  const tallLast = (8 * 64 - 1) * 4;
  assert.deepEqual(Array.from(tallOut.slice(tallLast, tallLast + 4)), [60, 120, 30, 200]);

  // Extreme vertical compression: 1024x1024 -> 1x1 (srcPxPerDstRow=1,048,576 > 262,144 => 4 source-row strips)
  const extPayload = new Uint8Array(16 + 1024 * 1024 * 4);
  extPayload.set([0x4d, 0x42, 0x52, 0x41, 0x57, 0x50, 0x52, 0x45], 0);
  const extDv = new DataView(extPayload.buffer);
  extDv.setUint32(8, 1024, true);
  extDv.setUint32(12, 1024, true);
  // Top half rows: (100, 40, 20, 200); bottom half rows: (101, 40, 20, 200) => exact avg R=100.5 => half-up 101
  for (let y = 0; y < 1024; y++) {
    const rVal = y < 512 ? 100 : 101;
    const rowOff = 16 + y * 1024 * 4;
    for (let x = 0; x < 1024; x++) {
      const o = rowOff + x * 4;
      extPayload[o] = rVal;
      extPayload[o + 1] = 40;
      extPayload[o + 2] = 20;
      extPayload[o + 3] = 200;
    }
  }
  const extId = b.host.imageLoadBytes(extPayload, 1);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(b.host.imageInfo(extId, info), 0);
  // After first macrotask only strip 1 of 4 ran; must still be pending (0)
  await tick();
  assert.equal(b.host.imageInfo(extId, info), 0);
  for (let i = 0; i < 16 && b.host.imageInfo(extId, info) === 0; i++) {
    await tick();
  }
  assert.equal(b.host.imageInfo(extId, info), 1);
  assert.deepEqual(info, [1, 1, 1024, 1024]);
  const extOut = new Uint8Array(4);
  assert.equal(b.host.imagePixels(extId, extOut), 1);
  assert.deepEqual(Array.from(extOut), [101, 40, 20, 200]);
});

test('MBRAWPRE async downscale recovers cleanly after WebGL context loss and restore', async () => {
  const b = makeBrowser();
  const info = [0, 0, 0, 0];
  const payload = makeRawPremul(8, 8, [120, 60, 30, 180]);
  const id = b.host.imageLoadBytes(payload, 4);
  // Let initial microtask schedule the downscale timer, then lose WebGL context while downscale is pending
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(b.host.imageInfo(id, info), 0);
  b.glState.contextLost = true;
  b.canvas.dispatch('webglcontextlost', { preventDefault() {} });
  // Pending downscale wakes up, observes lost context, and stays pending (0) with no -1 event emitted
  await tick();
  assert.equal(b.host.imageInfo(id, info), 0);
  assert.deepEqual(b.outsideEvents.filter((e) => e.id === id), []);
  // Restore WebGL context: image self-heals via contextRestored and emits exactly [1] with exact pixels
  b.glState.contextLost = false;
  b.canvas.dispatch('webglcontextrestored', {});
  for (let i = 0; i < 8 && b.host.imageInfo(id, info) !== 1; i++) {
    await tick();
  }
  assert.equal(b.host.imageInfo(id, info), 1);
  assert.deepEqual(b.outsideEvents.filter((e) => e.id === id).map((e) => e.state), [1]);
  assert.deepEqual(info, [4, 4, 8, 8]);
  const out = new Uint8Array(4 * 4 * 4);
  assert.equal(b.host.imagePixels(id, out), 1);
  assert.deepEqual(Array.from(out.slice(0, 4)), [120, 60, 30, 180]);

  // Request issued while context is ALREADY lost emits [-1], then recovers to [1] on webglcontextrestored
  b.glState.contextLost = true;
  const payload2 = makeRawPremul(4, 4, [70, 50, 30, 140]);
  const id2 = b.host.imageLoadBytes(payload2, 64);
  await tick();
  assert.equal(b.host.imageInfo(id2, info), -1);
  assert.deepEqual(b.outsideEvents.filter((e) => e.id === id2).map((e) => e.state), [-1]);
  b.glState.contextLost = false;
  b.canvas.dispatch('webglcontextrestored', {});
  for (let i = 0; i < 8 && b.host.imageInfo(id2, info) !== 1; i++) {
    await tick();
  }
  assert.equal(b.host.imageInfo(id2, info), 1);
  assert.deepEqual(b.outsideEvents.filter((e) => e.id === id2).map((e) => e.state), [-1, 1]);
  assert.deepEqual(info, [4, 4, 4, 4]);
  const out2 = new Uint8Array(4 * 4 * 4);
  assert.equal(b.host.imagePixels(id2, out2), 1);
  assert.deepEqual(Array.from(out2.slice(0, 4)), [70, 50, 30, 140]);
});

test('malformed MBRAWPRE vectors fail with state=-1 and never call createImageBitmap', async () => {
  const b = makeBrowser();
  const malformed = [
    new Uint8Array([0x4d, 0x42, 0x52, 0x41, 0x57, 0x50, 0x52, 0x45, 1, 0, 0]),
    makeRawPremul(2, 2, [0, 0, 0, 0], 12),
    makeRawPremul(0, 2, [0, 0, 0, 0], 0),
    makeRawPremul(2, 0, [0, 0, 0, 0], 0),
    makeRawPremul(16385, 1, [0, 0, 0, 0], 0),
    makeRawPremul(65536, 65536, [0, 0, 0, 0], 0),
    makeRawPremul(1, 1, [0, 0, 0, 0], 5),
  ];
  const ids = malformed.map((m) => b.host.imageLoadBytes(m, 64));
  await tick();
  const info = [0, 0, 0, 0];
  for (const id of ids) {
    assert.equal(b.host.imageInfo(id, info), -1);
    const evs = b.outsideEvents.filter((e) => e.id === id);
    assert.equal(evs.length, 1);
    assert.equal(evs[0].state, -1);
  }
  assert.equal(b.createImageBitmapCalls, 0);
});

test('WebGL upload error on MBRAWPRE reports failure state=-1', async () => {
  const b = makeBrowser({ glErrorOnUpload: 0x0505 }); // GL_OUT_OF_MEMORY
  const id = b.host.imageLoadBytes(makeRawPremul(2, 2), 64);
  await tick();
  const info = [0, 0, 0, 0];
  assert.equal(b.host.imageInfo(id, info), -1);
  assert.equal(b.outsideEvents.find((e) => e.id === id).state, -1);
});

test('MBRAWPRE fails with state=-1 and imagePixels=0 when context is lost or createTexture returns null', async () => {
  const b = makeBrowser();
  // Simulate WebGL context lost + createTexture() === null with getError() === 0
  b.glState.contextLost = true;
  b.glState.nullCreateTex = true;
  const id1 = b.host.imageLoadBytes(makeRawPremul(2, 2, [60, 40, 20, 128]), 64);
  await tick();
  const info = [0, 0, 0, 0];
  assert.equal(b.host.imageInfo(id1, info), -1);
  assert.equal(b.host.imagePixels(id1, new Uint8Array(16)), 0);
  assert.equal(b.outsideEvents.find((e) => e.id === id1).state, -1);
  assert.equal(b.deletedTexs.length, 0);

  // Also check createTexture() === null while context is not marked lost
  b.glState.contextLost = false;
  b.glState.nullCreateTex = true;
  const id2 = b.host.imageLoadBytes(makeRawPremul(2, 2, [60, 40, 20, 128]), 64);
  await tick();
  assert.equal(b.host.imageInfo(id2, info), -1);
  assert.equal(b.host.imagePixels(id2, new Uint8Array(16)), 0);
  assert.equal(b.deletedTexs.length, 0);
});

test('slotTex downscales to secondary gl.MAX_TEXTURE_SIZE and retries instead of caching failed GL uploads', async () => {
  let imageId = 0;
  let hostRef = null;
  const drawImageFrame = () => {
    if (!imageId || !hostRef) return;
    // INST = 60 floats: kind=2 (image) at offset 5, tex=imageId at offset 20, clip [-1e9, -1e9, 1e9, 1e9] at 16..19
    const inst = new Float32Array(60);
    inst[0] = 0; inst[1] = 0; inst[2] = 64; inst[3] = 32;
    inst[5] = 2;
    inst[8] = 1; inst[9] = 1; inst[10] = 1; inst[11] = 1;
    inst[12] = 0; inst[13] = 0; inst[14] = 1; inst[15] = 1;
    inst[16] = -1e9; inst[17] = -1e9; inst[18] = 1e9; inst[19] = 1e9;
    inst[20] = imageId;
    hostRef.present(inst, 1, 0, 0, 0);
  };
  const b = makeBrowser({
    maxTex: 64,
    secondaryGlOpts: { maxTex: 4, glErrorOnUpload: 0x0505 },
    onFrame: drawImageFrame,
  });
  hostRef = b.host;

  // 1. Load an 8x4 MBRAWPRE on the main window (maxTex=64 -> stays 8x4)
  imageId = b.host.imageLoadBytes(makeRawPremul(8, 4, [90, 45, 15, 128]), 64);
  await tick();
  const info = [0, 0, 0, 0];
  assert.equal(b.host.imageInfo(imageId, info), 1);
  assert.deepEqual(info, [8, 4, 8, 4]);

  // 2. Open secondary window whose GL has maxTex=4 and initially returns GL_OUT_OF_MEMORY on upload
  const wid = b.host.windows(1, 0, JSON.stringify({ w: 200, h: 200 }));
  assert.equal(wid, 1);
  assert.ok(b.secondary, 'secondary window GL created');
  assert.ok(b.popupRafs.length > 0, 'secondary window frame scheduled');

  // First draw: secondary downscale runs asynchronously (zero synchronous upload inside draw callback),
  // then fails with GL_OUT_OF_MEMORY -> texture deleted and placeholder removed so next draw can retry
  b.popupRafs.shift()();
  assert.equal(b.secondary.texUploads.length, 0);
  await tick();
  assert.equal(b.secondary.texUploads.length, 1);
  assert.equal(b.secondary.texUploads[0].w, 4);
  assert.equal(b.secondary.texUploads[0].h, 2);
  assert.equal(b.secondary.deletedTexs.length, 1);

  // 3. Clear GL error and draw again -> because failed upload was not cached, slotTex retries asynchronously,
  // succeeds, and schedules a secondary redraw
  b.secondary.state.glErrorOnUpload = 0;
  b.host.requestFrame();
  assert.ok(b.popupRafs.length > 0);
  b.popupRafs.shift()();
  assert.equal(b.secondary.texUploads.length, 1);
  await tick();
  assert.equal(b.secondary.texUploads.length, 2);
  assert.equal(b.secondary.texUploads[1].w, 4);
  assert.equal(b.secondary.texUploads[1].h, 2);
  assert.deepEqual(Array.from(b.secondary.texUploads[1].px.slice(0, 4)), [90, 45, 15, 128]);
  assert.equal(b.secondary.deletedTexs.length, 1);
  assert.ok(b.popupRafs.length > 0, 'async slotTex upload scheduled redraw');

  // 4. Third draw -> cached texture is reused (no 3rd texImage2D upload)
  b.popupRafs.shift()();
  assert.equal(b.secondary.texUploads.length, 2);
});

test('slotTex downscales oversized layer texture (tid > LAYER_TEX) asynchronously on secondary window', async () => {
  const LAYER_TEX = 12582912;
  let layerTid = 0;
  let hostRef = null;
  const drawLayerFrame = () => {
    if (!layerTid || !hostRef) return;
    const inst = new Float32Array(60);
    inst[0] = 0; inst[1] = 0; inst[2] = 64; inst[3] = 32;
    inst[5] = 4;
    inst[8] = 1; inst[9] = 1; inst[10] = 1; inst[11] = 1;
    inst[12] = 0; inst[13] = 0; inst[14] = 1; inst[15] = 1;
    inst[16] = -1e9; inst[17] = -1e9; inst[18] = 1e9; inst[19] = 1e9;
    inst[20] = layerTid;
    hostRef.present(inst, 1, 0, 0, 0);
  };
  const b = makeBrowser({
    maxTex: 64,
    layerFill: [110, 55, 22, 220],
    secondaryGlOpts: { maxTex: 4 },
    onFrame: drawLayerFrame,
  });
  hostRef = b.host;
  await tick();

  // 1. Render an 8x4 offscreen layer on the main window
  assert.equal(b.host.layerRender(1, 8, 4, 1, new Float64Array(0), 0), 1);
  layerTid = LAYER_TEX + 1;

  // 2. Open secondary window with maxTex=4 and schedule a frame
  const wid = b.host.windows(1, 0, JSON.stringify({ w: 200, h: 200 }));
  assert.equal(wid, 1);
  assert.ok(b.secondary, 'secondary window GL created');
  b.host.requestFrame();
  assert.ok(b.popupRafs.length > 0, 'secondary window frame scheduled');

  // First draw: zero synchronous upload inside draw callback; async downscale completes after tick()
  b.popupRafs.shift()();
  assert.equal(b.secondary.texUploads.length, 0);
  await tick();
  assert.equal(b.secondary.texUploads.length, 1);
  assert.equal(b.secondary.texUploads[0].w, 4);
  assert.equal(b.secondary.texUploads[0].h, 2);
  assert.deepEqual(Array.from(b.secondary.texUploads[0].px.slice(0, 4)), [110, 55, 22, 220]);
  assert.ok(b.popupRafs.length > 0, 'async layer slotTex upload scheduled redraw');

  // Next draw reuses cached secondary layer texture (no second upload)
  b.popupRafs.shift()();
  assert.equal(b.secondary.texUploads.length, 1);
});
