#!/usr/bin/env node
// Web host WebGL2 glass blur performance regression test:
// Ensures blurGlass (blitFramebuffer) and postPass (drawArrays count=3) run for
// both frosted (kind=5) and liquid (kind=6) glass without calling
// gl.invalidateFramebuffer on pooled blur FBOs (which triggers ~1 ms/call stalls
// in Safari / WebKit).
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import vm from "node:vm";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const hostSrc = readFileSync(join(root, "web/host.js"), "utf8");

class FakeTarget {
  constructor() { this.listeners = new Map(); }
  addEventListener(type, fn) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(fn);
  }
  removeEventListener(type, fn) {
    const l = this.listeners.get(type) || [];
    const i = l.indexOf(fn);
    if (i >= 0) l.splice(i, 1);
  }
}

const calls = [];
function fakeGL(canvas) {
  const consts = new Map();
  let nextId = 1;
  const answers = {
    getError: () => 0,
    getExtension: () => null,
    getShaderParameter: () => true,
    getProgramParameter: () => true,
    getShaderInfoLog: () => "",
    getProgramInfoLog: () => "",
    getParameter: () => 4096,
    getUniformLocation: () => ({}),
    getAttribLocation: () => 0,
    getUniformBlockIndex: () => 0,
    isContextLost: () => false,
    checkFramebufferStatus: () => gl.FRAMEBUFFER_COMPLETE,
    getSupportedExtensions: () => [],
    getContextAttributes: () => ({ alpha: true }),
    createTexture: () => ({ id: nextId++ }),
    createFramebuffer: () => ({ id: nextId++ }),
    createBuffer: () => ({ id: nextId++ }),
    createVertexArray: () => ({ id: nextId++ }),
    createProgram: () => ({ id: nextId++ }),
    createShader: () => ({ id: nextId++ }),
    blitFramebuffer: (...args) => { calls.push({ fn: "blitFramebuffer", args }); },
    drawArrays: (mode, first, count) => { calls.push({ fn: "drawArrays", mode, first, count }); },
    invalidateFramebuffer: (tgt, attachments) => { calls.push({ fn: "invalidateFramebuffer", tgt, attachments }); },
  };
  const gl = new Proxy({ canvas, drawingBufferWidth: 800, drawingBufferHeight: 600 }, {
    get(t, k) {
      if (k in t) return t[k];
      if (typeof k !== "string") return undefined;
      if (/^[A-Z0-9_]+$/.test(k)) {
        if (!consts.has(k)) consts.set(k, consts.size + 1);
        return consts.get(k);
      }
      if (answers[k]) return answers[k];
      return () => ({});
    },
  });
  return gl;
}

class FakeElement extends FakeTarget {
  constructor(tag, d) {
    super();
    this.ownerDocument = d;
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.parentNode = null;
    this.attrs = new Map();
    this.style = { cssText: "" };
    this.dataset = {};
    this.id = "";
    this.width = 800;
    this.height = 600;
    this.clientWidth = 400;
    this.clientHeight = 300;
  }
  setAttribute(k, v) { this.attrs.set(k, String(v)); }
  getAttribute(k) { return this.attrs.has(k) ? this.attrs.get(k) : null; }
  removeAttribute(k) { this.attrs.delete(k); }
  appendChild(c) { return this.insertBefore(c, null); }
  insertBefore(c, ref) {
    if (c.parentNode) c.parentNode.removeChild(c);
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i >= 0) this.children.splice(i, 0, c);
    else this.children.push(c);
    c.parentNode = this;
    return c;
  }
  get nextSibling() {
    if (!this.parentNode) return null;
    const s = this.parentNode.children;
    return s[s.indexOf(this) + 1] || null;
  }
  removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); c.parentNode = null; return c; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  getBoundingClientRect() { return { left: 0, top: 0, width: 400, height: 300, right: 400, bottom: 300 }; }
  getContext(kind) { return kind === "webgl2" ? fakeGL(this) : null; }
  querySelector() { return null; }
  querySelectorAll() { return []; }
}

const doc = new FakeTarget();
Object.assign(doc, {
  title: "",
  visibilityState: "visible",
  readyState: "complete",
  currentScript: null,
  createElement: (tag) => new FakeElement(tag, doc),
  createElementNS: (_ns, tag) => new FakeElement(tag, doc),
  getElementById(id) {
    const walk = (n) => {
      if (n.id === id) return n;
      for (const c of n.children) { const f = walk(c); if (f) return f; }
      return null;
    };
    return walk(doc.documentElement);
  },
  querySelector() { return null; },
  querySelectorAll() { return []; },
});
doc.documentElement = new FakeElement("html", doc);
doc.head = new FakeElement("head", doc);
doc.body = new FakeElement("body", doc);
doc.documentElement.appendChild(doc.head);
doc.documentElement.appendChild(doc.body);
doc.activeElement = doc.body;
const canvas = doc.createElement("canvas");
canvas.id = "c";
const ta = doc.createElement("textarea");
ta.id = "ime";
doc.body.appendChild(canvas);
doc.body.appendChild(ta);

const win = new FakeTarget();
const noop = () => {};
Object.assign(win, {
  document: doc,
  location: { href: "http://localhost/", search: "", hash: "", origin: "http://localhost", pathname: "/" },
  navigator: { userAgent: "node", platform: "MacIntel", maxTouchPoints: 0, language: "en", languages: ["en"] },
  devicePixelRatio: 2,
  innerWidth: 400,
  innerHeight: 300,
  performance: { now: () => 16, timeOrigin: 0 },
  requestAnimationFrame: () => 0,
  cancelAnimationFrame: noop,
  setTimeout: () => 0,
  clearTimeout: noop,
  setInterval: () => 0,
  clearInterval: noop,
  queueMicrotask,
  matchMedia: () => ({ matches: false, addEventListener: noop, removeEventListener: noop, addListener: noop }),
  getComputedStyle: () => ({ backgroundColor: "", getPropertyValue: () => "" }),
  URL, URLSearchParams, TextEncoder, TextDecoder, Promise, Map, Set, WeakMap, Math, Date, JSON, Array, Uint8Array,
  Float32Array, Float64Array, Int32Array, Uint32Array, Uint16Array, Int16Array, Uint8ClampedArray, ArrayBuffer,
  DataView, Error, TypeError, Object, String, Number, Boolean, Symbol, Proxy, Reflect, console,
  ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
  MutationObserver: class { observe() {} disconnect() {} },
  history: { replaceState: noop, pushState: noop, state: null },
  localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
  sessionStorage: { getItem: () => null, setItem: noop, removeItem: noop },
  fetch: () => new Promise(noop),
  addEventListener: FakeTarget.prototype.addEventListener,
});
win.window = win;
win.self = win;
win.globalThis = win;
win.visualViewport = null;

const ctx = vm.createContext(win);
vm.runInContext(hostSrc, ctx, { filename: "web/host.js" });
const H = win.__mbhost;
assert.ok(H && typeof H.run === "function" && typeof H.present === "function", "host.js exposes __mbhost.run and present");
H.run("web perf test", noop, noop, noop, noop, noop);

const INST = 60;
const buf = new Float64Array(INST * 3);
// 0: opaque background rect
buf[0] = 0; buf[1] = 0; buf[2] = 400; buf[3] = 300;
buf[8] = 0.9; buf[9] = 0.9; buf[10] = 0.92; buf[11] = 1.0;
buf[16] = -1e9; buf[17] = -1e9; buf[18] = 1e9; buf[19] = 1e9;
// 1: frosted glass (kind=5) with blur radius > 0
const fg = INST;
buf[fg] = 20; buf[fg + 1] = 20; buf[fg + 2] = 140; buf[fg + 3] = 80;
buf[fg + 4] = 16; buf[fg + 5] = 5; buf[fg + 6] = 1; buf[fg + 7] = 12;
buf[fg + 8] = 0.96; buf[fg + 9] = 0.96; buf[fg + 10] = 0.98; buf[fg + 11] = 0.4;
buf[fg + 15] = 1;
buf[fg + 16] = -1e9; buf[fg + 17] = -1e9; buf[fg + 18] = 1e9; buf[fg + 19] = 1e9;
buf[fg + 20] = -10;
// 2: liquid glass (kind=6) with dual-scale blur
const lg = 2 * INST;
buf[lg] = 180; buf[lg + 1] = 20; buf[lg + 2] = 160; buf[lg + 3] = 80;
buf[lg + 4] = 20; buf[lg + 5] = 6; buf[lg + 6] = 1; buf[lg + 7] = 6;
buf[lg + 8] = 0.99; buf[lg + 9] = 0.99; buf[lg + 10] = 0.99; buf[lg + 11] = 0.35;
buf[lg + 13] = 8; buf[lg + 14] = -1; buf[lg + 15] = 1;
buf[lg + 16] = -1e9; buf[lg + 17] = -1e9; buf[lg + 18] = 1e9; buf[lg + 19] = 1e9;
buf[lg + 20] = -11; buf[lg + 21] = 6;

calls.length = 0;
H.present(buf, 3, 0.9, 0.9, 0.92);

const blits = calls.filter((c) => c.fn === "blitFramebuffer");
const postDraws = calls.filter((c) => c.fn === "drawArrays" && c.count === 3);
const invals = calls.filter((c) => c.fn === "invalidateFramebuffer");

assert.ok(blits.length >= 2, `expected >= 2 glass capture blits, got ${blits.length}`);
assert.ok(postDraws.length >= 4, `expected >= 4 postPass draws, got ${postDraws.length}`);
assert.equal(invals.length, 0, `expected 0 invalidateFramebuffer calls, got ${invals.length}`);

console.log(`test-host-web-perf: OK (blits=${blits.length}, postDraws=${postDraws.length}, invalidateFramebuffer=${invals.length})`);
