#!/usr/bin/env node
// Web accessibility mirror (web/pion_host_bridge.mjs applyA11yTree) against a minimal fake DOM:
// two-letter state flags, one action per click/Enter on nested controls, DOM order following the
// semantic kids order, and descendants placed again when a parent moves.
import assert from "node:assert/strict";
import { attachHostBridge } from "../web/pion_host_bridge.mjs";

class FakeEvent {
  constructor(type, init = {}) {
    this.type = type;
    Object.assign(this, init);
    this.defaultPrevented = false;
    this.stopped = false;
  }
  preventDefault() { this.defaultPrevented = true; }
  stopPropagation() { this.stopped = true; }
}

class FakeElement {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.parentNode = null;
    this.attrs = new Map();
    this.dataset = {};
    this.style = { cssText: "" };
    this.listeners = new Map();
    this.textContent = "";
    this.tabIndex = -1;
    this.disabled = false;
    this.id = "";
  }
  get isConnected() {
    let n = this;
    while (n.parentNode) n = n.parentNode;
    return n === doc.documentElement;
  }
  setAttribute(k, v) { this.attrs.set(k, String(v)); }
  getAttribute(k) { return this.attrs.has(k) ? this.attrs.get(k) : null; }
  removeAttribute(k) { this.attrs.delete(k); }
  addEventListener(type, fn) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(fn);
  }
  detach(child) {
    const i = this.children.indexOf(child);
    if (i >= 0) this.children.splice(i, 1);
    child.parentNode = null;
  }
  appendChild(child) { return this.insertBefore(child, null); }
  insertBefore(child, ref) {
    if (child.parentNode) child.parentNode.detach(child);
    const i = ref ? this.children.indexOf(ref) : -1;
    if (i >= 0) this.children.splice(i, 0, child);
    else this.children.push(child);
    child.parentNode = this;
    return child;
  }
  removeChild(child) { this.detach(child); return child; }
  contains(other) {
    for (let n = other; n; n = n.parentNode) if (n === this) return true;
    return false;
  }
  focus() { doc.activeElement = this; }
  // bubbling dispatch, honouring stopPropagation
  dispatch(type, init) {
    const ev = new FakeEvent(type, init);
    for (let n = this; n && !ev.stopped; n = n.parentNode) {
      for (const fn of n.listeners.get(type) || []) fn(ev);
    }
    return ev;
  }
}

const doc = {
  documentElement: new FakeElement("html"),
  body: null,
  activeElement: null,
  createElement: (tag) => new FakeElement(tag),
  getElementById(id) {
    const walk = (n) => {
      if (n.id === id) return n;
      for (const c of n.children) {
        const r = walk(c);
        if (r) return r;
      }
      return null;
    };
    return walk(doc.documentElement);
  },
};
doc.body = doc.documentElement.appendChild(new FakeElement("body"));
globalThis.document = doc;

const posted = [];
globalThis.__mbhost = { post: (ch, json) => posted.push([ch, JSON.parse(json)]) };

const rt = { getMemory: () => null, readUtf8: (s) => s, imports: { pion_rt: {} } };
const { bridge } = attachHostBridge(rt);
const send = (wire) => bridge.mb_web_a11y_tree(wire, wire.length);

const US = "\x1f";
// N id role flags x y w h label value desc low at high actions widget kids
const node = (id, role, { flags = "", x = 0, y = 0, w = 10, h = 10, label = "", actions = "", kids = [] } = {}) =>
  ["N", id, role, flags, x, y, w, h, label, "", "", "", "", "", actions, id, kids.join(",")].join(US);
const tree = (full, nodes) => [`T${US}${full ? 1 : 0}${US}0`, ...nodes].join("\n") + "\n";

const root = () => doc.getElementById("a11y");
const elOf = (id) => {
  const walk = (n) => {
    if (n.dataset && n.dataset.a11yId === String(id)) return n;
    for (const c of n.children) {
      const r = walk(c);
      if (r) return r;
    }
    return null;
  };
  return walk(root());
};
let failures = 0;
const check = (name, fn) => {
  try {
    fn();
    console.log(`[PASS] ${name}`);
  } catch (e) {
    failures++;
    console.error(`[FAIL] ${name}: ${e.message}`);
  }
};

check("two-letter state flags map to ARIA states", () => {
  send(tree(true, [
    node(0, "Region", { kids: [1, 2, 3, 4, 5, 6, 7, 8] }),
    node(1, "Checkbox", { flags: "c" }),
    node(2, "Checkbox", { flags: "cC" }),
    node(3, "Checkbox", { flags: "cM" }),
    node(4, "Button", { flags: "e" }),
    node(5, "Button", { flags: "eE" }),
    node(6, "Button", { flags: "p" }),
    node(7, "Button", { flags: "pP" }),
    node(8, "Button", { flags: "" }),
  ]));
  assert.equal(elOf(1).getAttribute("aria-checked"), "false");
  assert.equal(elOf(2).getAttribute("aria-checked"), "true");
  assert.equal(elOf(3).getAttribute("aria-checked"), "mixed");
  assert.equal(elOf(4).getAttribute("aria-expanded"), "false");
  assert.equal(elOf(5).getAttribute("aria-expanded"), "true");
  assert.equal(elOf(6).getAttribute("aria-pressed"), "false");
  assert.equal(elOf(7).getAttribute("aria-pressed"), "true");
  for (const a of ["aria-checked", "aria-expanded", "aria-pressed"]) assert.equal(elOf(8).getAttribute(a), null, a);
  // a state turned off clears its attribute
  send(tree(false, [node(7, "Button", { flags: "" })]));
  assert.equal(elOf(7).getAttribute("aria-pressed"), null);
});

check("nested Tab inside TabList: one click / Enter activates only the Tab", () => {
  send(tree(true, [
    node(0, "Region", { kids: [1] }),
    node(1, "TabList", { actions: "a", kids: [2] }),
    node(2, "Tab", { actions: "a" }),
  ]));
  const tab = elOf(2);
  assert.equal(tab.parentNode, elOf(1), "Tab mounted inside its TabList");
  posted.length = 0;
  tab.dispatch("click");
  assert.deepEqual(posted.map(([, p]) => [p.action, p.id]), [["activate", 2]]);
  posted.length = 0;
  tab.dispatch("keydown", { key: "Enter" });
  assert.deepEqual(posted.map(([, p]) => [p.action, p.id]), [["activate", 2]]);
  posted.length = 0;
  elOf(1).dispatch("click");
  assert.deepEqual(posted.map(([, p]) => [p.action, p.id]), [["activate", 1]], "the TabList itself still activates");
});

check("reordered kids reorder the DOM", () => {
  const ids = (el) => el.children.map((c) => Number(c.dataset.a11yId));
  send(tree(true, [
    node(0, "Region", { kids: [2, 3, 4, 5] }),
    node(2, "Button", { label: "two" }),
    node(3, "Button", { label: "three" }),
    node(4, "Button", { label: "four" }),
    node(5, "Button", { label: "five" }),
  ]));
  assert.deepEqual(ids(root()), [2, 3, 4, 5]);
  send(tree(false, [node(0, "Region", { kids: [3, 2, 4, 5] })]));
  assert.deepEqual(ids(root()), [3, 2, 4, 5]);
  send(tree(false, [node(0, "Region", { kids: [5, 4, 3, 2] })]));
  assert.deepEqual(ids(root()), [5, 4, 3, 2]);
});

check("a parent changing meaning and place at once moves its children with it", () => {
  const left = (el) => /left:([-\d.]+)px/.exec(el.style.cssText)[1];
  const top = (el) => /top:([-\d.]+)px/.exec(el.style.cssText)[1];
  send(tree(true, [
    node(0, "Region", { kids: [1] }),
    node(1, "Group", { label: "card", x: 10, y: 20, w: 100, h: 100, kids: [2] }),
    node(2, "Button", { label: "inner", x: 5, y: 6, w: 20, h: 20, kids: [3] }),
    node(3, "Text", { label: "deep", x: 1, y: 1, w: 5, h: 5 }),
  ]));
  assert.equal(left(elOf(2)), "15");
  assert.equal(top(elOf(2)), "26");
  // the parent alone is told (new label + new place): children keep their relative place
  send(tree(false, [node(1, "Group", { label: "card moved", x: 110, y: 220, w: 100, h: 100, kids: [2] })]));
  assert.equal(left(elOf(1)), "110");
  assert.equal(left(elOf(2)), "115", "child placed again at once");
  assert.equal(top(elOf(2)), "226");
  assert.equal(left(elOf(3)), "116", "grandchild placed again at once");
  assert.equal(top(elOf(3)), "227");
});

if (failures) {
  console.error(`${failures} a11y bridge test(s) failed`);
  process.exit(1);
}
console.log("=== ALL A11Y BRIDGE TESTS PASSED ===");
// (no pending geometry timer keeps the process alive)
process.exit(0);
