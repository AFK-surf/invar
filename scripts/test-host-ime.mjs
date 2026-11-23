#!/usr/bin/env node
// Web host input method fields (web/host.js H.imeAction / H.setIme) against a minimal fake DOM and a
// no-op WebGL2 context: a secret's field types into a hidden <input type="password">, never into the
// textarea, and neither keeps a secret once its field lets go or another kind of field takes over.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import vm from "node:vm";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const hostSrc = readFileSync(join(root, "web/host.js"), "utf8");

class FakeEvent {
  constructor(type, init = {}) {
    this.type = type;
    this.timeStamp = 1;
    Object.assign(this, init);
    this.defaultPrevented = false;
  }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() {}
}

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
  dispatch(type, init) {
    const ev = new FakeEvent(type, { target: this, ...init });
    for (const fn of [...(this.listeners.get(type) || [])]) fn(ev);
    return ev;
  }
}

// A WebGL2 context that does nothing: constants are numbers, queries answer "fine", objects are tokens.
function fakeGL(canvas) {
  const consts = new Map();
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
    checkFramebufferStatus: (t) => gl.FRAMEBUFFER_COMPLETE,
    getSupportedExtensions: () => [],
    getContextAttributes: () => ({ alpha: true }),
  };
  const gl = new Proxy({ canvas, drawingBufferWidth: 1, drawingBufferHeight: 1 }, {
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
    this.type = tag === "textarea" ? "textarea" : "text";
    this.value = "";
    this.selectionStart = 0;
    this.selectionEnd = 0;
    this.tabIndex = 0;
    this.textContent = "";
    this.width = 300;
    this.height = 150;
    this.clientWidth = 400;
    this.clientHeight = 300;
  }
  get attributes() { return [...this.attrs].map(([name, value]) => ({ name, value })); }
  setAttribute(k, v) { this.attrs.set(k, String(v)); }
  getAttribute(k) { return this.attrs.has(k) ? this.attrs.get(k) : null; }
  removeAttribute(k) { this.attrs.delete(k); }
  hasAttribute(k) { return this.attrs.has(k); }
  appendChild(c) { return this.insertBefore(c, null); }
  insertBefore(c, ref) {
    if (c.parentNode) c.parentNode.removeChild(c);
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i >= 0) this.children.splice(i, 0, c);
    else this.children.push(c);
    c.parentNode = this;
    return c;
  }
  removeChild(c) {
    const i = this.children.indexOf(c);
    if (i >= 0) this.children.splice(i, 1);
    c.parentNode = null;
    return c;
  }
  get nextSibling() {
    if (!this.parentNode) return null;
    const s = this.parentNode.children;
    return s[s.indexOf(this) + 1] || null;
  }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; }
  focus() {
    const d = this.ownerDocument;
    if (d.activeElement === this) return;
    const was = d.activeElement;
    d.activeElement = this;
    if (was && was !== d.body) was.dispatch("blur");
    this.dispatch("focus");
  }
  blur() {
    const d = this.ownerDocument;
    if (d.activeElement !== this) return;
    d.activeElement = d.body;
    this.dispatch("blur");
  }
  setSelectionRange(s, e) { this.selectionStart = s; this.selectionEnd = e; }
  setPointerCapture() {}
  releasePointerCapture() {}
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
  devicePixelRatio: 1,
  innerWidth: 400,
  innerHeight: 300,
  performance: { now: () => 0, timeOrigin: 0 },
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
assert.ok(H && typeof H.run === "function" && typeof H.setIme === "function" && typeof H.imeAction === "function",
  "host.js exposes __mbhost with run, setIme, imeAction");

const told = [];
H.run("ime test", noop, noop, noop, noop, (kind, text, a, b) => told.push([kind, text, a, b]));

const pw = doc.body.children.find((el) => el.tagName === "INPUT");
assert.ok(pw, "a password input set up beside the textarea");
assert.equal(pw.type, "password", "the hidden input is a password input");
assert.equal(pw.value, "", "the password input starts empty");
assert.equal(doc.body.children.indexOf(pw), doc.body.children.indexOf(ta) + 1, "placed right after the textarea");
assert.equal(pw.getAttribute("aria-hidden"), "true", "hidden from assistive technology");
assert.equal(pw.getAttribute("autocomplete"), "off", "no autofill");
assert.equal(pw.tabIndex, -1, "out of the Tab order");

// 1. a secret's field: its words in the password input, focused; the textarea holds nothing
H.imeAction(4);
H.setIme(true, 10, 20, 100, 24, "s3cr3t", 6, 6);
assert.equal(doc.activeElement.tagName, "INPUT", "the password input has the focus");
assert.equal(doc.activeElement.type, "password", "focused input is a password input");
assert.equal(doc.activeElement, pw, "focused input is the window's password input");
assert.equal(pw.value, "s3cr3t", "the secret's words in the password input");
assert.equal(ta.value, "", "the textarea holds no secret");
assert.equal(pw.style.left, "10px", "placed at the caret");
assert.equal(pw.style.top, "20px", "placed at the caret");

// 2. typing into the password input tells the app what changed
told.length = 0;
pw.value = "s3cr3t!";
pw.selectionStart = 7;
pw.selectionEnd = 7;
pw.dispatch("input", {});
assert.deepEqual(told, [[0, "!", 0, 0]], `typed '!' told as an insert: ${JSON.stringify(told)}`);

// 3. back to a plain field: the password input emptied, the textarea focused with the words
H.imeAction(0);
H.setIme(true, 10, 50, 100, 24, "hello", 5, 5);
assert.equal(pw.value, "", "the password input emptied");
assert.equal(doc.activeElement, ta, "the textarea has the focus");
assert.equal(ta.value, "hello", "the plain field's words in the textarea");
told.length = 0;
ta.value = "hello!";
ta.selectionStart = 6;
ta.dispatch("input", {});
assert.deepEqual(told, [[0, "!", 0, 0]], `typed into the textarea told: ${JSON.stringify(told)}`);

// 4. a secret's field letting go: nothing kept, focus gone
H.imeAction(4);
H.setIme(true, 10, 20, 100, 24, "pw123", 5, 5);
assert.equal(doc.activeElement, pw, "password input focused again");
assert.equal(pw.value, "pw123", "the secret in the password input");
assert.equal(ta.value, "", "the textarea emptied on the way to a secret");
H.setIme(false, 0, 0, 0, 0, "", 0, 0);
assert.equal(pw.value, "", "the secret not kept once its field lets go");
assert.notEqual(doc.activeElement, pw, "the password input lets go of the focus");
assert.notEqual(doc.activeElement, ta, "nor the textarea holds it");

console.log("test-host-ime: OK (4 cases)");
