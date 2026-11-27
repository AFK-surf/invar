#!/usr/bin/env node
// Tests for the Web host's WebGL2 programs: the first ones compiled and linked in one batch with no status read until
// the first frame, the rest prewarmed in the background (KHR_parallel_shader_compile) once a frame is presented and
// adopted without linking again, the prewarm cancelled by a lost context and let go of by a closed window, link
// failures told with their logs, and the merged liquid's program built without the droplet's GLSL.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../web/host.js', import.meta.url), 'utf8');

const VERTEX_SHADER = 0x8b31;
const FRAGMENT_SHADER = 0x8b30;

function makeMockGl({ parallel, failLink = null }) {
  let nextId = 1;
  let lost = false;
  const ops = [];
  const shaderSrc = new Map();
  const progShaders = new Map();
  const createdProgs = new Set();
  const createdShaders = new Set();
  const createdVs = new Set();
  const deletedProgs = new Set();
  const deletedShaders = new Set();
  const gl = new Proxy(
    {
      VERTEX_SHADER,
      FRAGMENT_SHADER,
      COMPILE_STATUS: 0x8b81,
      LINK_STATUS: 0x8b82,
      NO_ERROR: 0,
      MAX_TEXTURE_SIZE: 0x0d33,
      FRAMEBUFFER_COMPLETE: 0x8cd5,
      FRAMEBUFFER_BINDING: 0x8ca6,
    },
    {
      get(target, name) {
        if (name in target) return target[name];
        if (name === 'getExtension') {
          return (ext) => {
            if (ext === 'KHR_parallel_shader_compile') return parallel ? { COMPLETION_STATUS_KHR: 0x91b1 } : null;
            if (ext === 'WEBGL_lose_context') return { loseContext() {}, restoreContext() {} };
            return null;
          };
        }
        if (name === 'isContextLost') return () => lost;
        if (name === 'getError') return () => 0;
        if (name === 'getParameter') return (p) => (p === 0x0d33 ? 4096 : null);
        if (name === 'checkFramebufferStatus') return () => 0x8cd5;
        if (name === 'createShader') {
          return (type) => {
            const s = { id: nextId++, type };
            createdShaders.add(s.id);
            if (type === VERTEX_SHADER) createdVs.add(s.id);
            ops.push(['createShader', s.id, type]);
            return s;
          };
        }
        if (name === 'shaderSource') return (s, src) => { shaderSrc.set(s.id, src); ops.push(['shaderSource', s.id, src]); };
        if (name === 'compileShader') return (s) => { ops.push(['compileShader', s.id]); };
        if (name === 'getShaderParameter') {
          return (s, pname) => {
            ops.push(['getShaderParameter', s.id, pname]);
            return !(failLink && s.type === FRAGMENT_SHADER && failLink(shaderSrc.get(s.id) || ''));
          };
        }
        if (name === 'getShaderInfoLog') return (s) => `shader-log-${s.id}`;
        if (name === 'deleteShader') return (s) => { if (s) deletedShaders.add(s.id); ops.push(['deleteShader', s?.id]); };
        if (name === 'createProgram') {
          return () => {
            const p = { id: nextId++ };
            createdProgs.add(p.id);
            progShaders.set(p.id, []);
            ops.push(['createProgram', p.id]);
            return p;
          };
        }
        if (name === 'attachShader') return (p, s) => { progShaders.get(p.id).push(s); ops.push(['attachShader', p.id, s.id]); };
        if (name === 'linkProgram') return (p) => { ops.push(['linkProgram', p.id]); };
        if (name === 'getProgramParameter') {
          return (p, pname) => {
            ops.push(['getProgramParameter', p.id, pname]);
            if (lost) return false;
            if (failLink && (progShaders.get(p.id) || []).some((s) => failLink(shaderSrc.get(s.id) || ''))) return false;
            return true;
          };
        }
        if (name === 'getProgramInfoLog') return (p) => `prog-log-${p.id}`;
        if (name === 'deleteProgram') return (p) => { if (p) deletedProgs.add(p.id); ops.push(['deleteProgram', p?.id]); };
        if (name === 'getUniformLocation') return (p, u) => ({ p: p.id, u });
        if (name === 'useProgram') return (p) => { ops.push(['useProgram', p ? p.id : null]); };
        return () => ({});
      },
    },
  );
  const count = (k) => ops.filter((op) => op[0] === k).length;
  // the ONE_KIND of each kind program's fragment shader, in the order they were given their sources
  const oneKinds = () => ops
    .filter((op) => op[0] === 'shaderSource' && /^#version 300 es\n#define ONE_KIND -?\d+\n/.test(op[2]))
    .map((op) => Number(op[2].match(/#define ONE_KIND (-?\d+)/)[1]));
  return {
    gl, ops, shaderSrc, progShaders, createdProgs, createdShaders, createdVs, deletedProgs, deletedShaders, count, oneKinds,
    setLost(v) { lost = v; },
  };
}

function makeBrowser({ parallel = true, webdriver = false, search = '', failLink = null, onFrame = null } = {}) {
  const element = () => ({
    style: {},
    attributes: [],
    listeners: new Map(),
    setAttribute() {},
    appendChild() {},
    remove() {},
    addEventListener(name, f) { this.listeners.set(name, [...(this.listeners.get(name) || []), f]); },
    dispatch(name, e = {}) { for (const f of this.listeners.get(name) || []) f(e); },
  });
  const main = makeMockGl({ parallel, failLink });
  const canvas = {
    ...element(),
    width: 0,
    height: 0,
    getContext: () => main.gl,
    getBoundingClientRect: () => ({ width: 200, height: 150, left: 0, top: 0 }),
  };
  const ime = element();
  const document = {
    title: 'main',
    body: element(),
    head: element(),
    documentElement: element(),
    createElement: () => ({ ...element(), getContext: () => ({ measureText: () => ({ width: 8 }) }) }),
    getElementById: (id) => (id === 'c' ? canvas : ime),
    addEventListener() {},
  };
  const popups = [];
  const win = {
    ...element(),
    document,
    navigator: { platform: 'MacIntel', webdriver },
    location: { search },
    URLSearchParams,
    Intl,
    Blob,
    devicePixelRatio: 2,
    performance: { now: () => 0 },
    queueMicrotask,
    requestAnimationFrame() { return 1; },
    cancelAnimationFrame() {},
    getComputedStyle: () => ({ backgroundColor: '' }),
    open() {
      const pgl = makeMockGl({ parallel, failLink });
      const pcanvas = {
        ...element(),
        width: 0,
        height: 0,
        getContext: () => pgl.gl,
        getBoundingClientRect: () => ({ width: 200, height: 200, left: 0, top: 0 }),
      };
      const pdoc = {
        title: '',
        body: element(),
        head: element(),
        documentElement: element(),
        createElement: (tag) => (tag === 'canvas' ? pcanvas : element()),
        addEventListener() {},
      };
      const rafs = [];
      const pwin = {
        ...element(),
        closed: false,
        innerWidth: 200,
        innerHeight: 200,
        screenX: 0,
        screenY: 0,
        devicePixelRatio: 1,
        document: pdoc,
        focus() {},
        close() { this.closed = true; },
        requestAnimationFrame(fn) { rafs.push(fn); return rafs.length; },
        cancelAnimationFrame() {},
      };
      popups.push({ win: pwin, canvas: pcanvas, gl: pgl, rafs });
      return pwin;
    },
  };
  win.window = win;
  vm.runInNewContext(source, win, { filename: 'host.js' });
  const H = win.__mbhost;
  H.run('test', () => { if (onFrame) onFrame(H); }, () => {}, () => {}, () => {}, () => {});
  return { H, win, canvas, gl: main, popups };
}

const INST = 60;
function makeInst(kind, padW = 0, tex = 0) {
  const buf = new Float32Array(INST);
  buf[0] = 10; buf[1] = 10; buf[2] = 60; buf[3] = 40;
  buf[4] = 8;
  buf[5] = kind;
  buf[8] = 0.5; buf[9] = 0.5; buf[10] = 0.5; buf[11] = 0.8;
  buf[16] = -1e9; buf[17] = -1e9; buf[18] = 1e9; buf[19] = 1e9;
  buf[20] = tex;
  buf[23] = padW;
  return buf;
}
function frameOf(...insts) {
  const buf = new Float32Array(insts.length * INST);
  insts.forEach((it, i) => buf.set(it, i * INST));
  return buf;
}

const flushMicrotasks = () => new Promise((r) => setImmediate(r));

// The kinds with a program of their own (host.js hasOwnProg) and the one of any kind (-1): every one is either
// compiled at start (BOOT_KINDS) or prewarmed, heaviest glass first.
const BOOT = [0, 1, 2, 7, 11, 13];
const PREWARM = [6, 5, 8, -1, 10, 4, 3];

// The fragment shader as the GLSL preprocessor leaves it for one ONE_KIND: #if / #ifdef / #ifndef / #elif / #else /
// #endif and #define of plain values taken, the expressions of ONE_KIND, MB_HAS_DROPLET and defined(...) worked out.
function preprocess(src, oneKind) {
  const defs = new Map([['ONE_KIND', String(oneKind)]]);
  const evalExpr = (e) => {
    const js = e
      .replace(/defined\s*\(\s*(\w+)\s*\)/g, (_, n) => (defs.has(n) ? '1' : '0'))
      .replace(/\b[A-Za-z_]\w*\b/g, (n) => (defs.has(n) ? `(${defs.get(n)})` : '0'));
    if (!/^[\s\d()<>=!&|+-]*$/.test(js)) throw new Error(`unexpected #if expression: ${e}`);
    return !!Function(`return (${js});`)();
  };
  const out = [];
  const stack = []; // { live, taken }
  const live = () => stack.every((f) => f.live);
  for (const line of src.split('\n')) {
    const t = line.trim();
    let m;
    if ((m = t.match(/^#if\s+(.*)$/))) {
      const outer = live();
      const v = outer && evalExpr(m[1]);
      stack.push({ live: v, taken: v, outer });
    } else if ((m = t.match(/^#ifdef\s+(\w+)/))) {
      const v = live() && defs.has(m[1]);
      stack.push({ live: v, taken: v, outer: live() });
    } else if ((m = t.match(/^#ifndef\s+(\w+)/))) {
      const outer = live();
      const v = outer && !defs.has(m[1]);
      stack.push({ live: v, taken: v, outer });
    } else if ((m = t.match(/^#elif\s+(.*)$/))) {
      const f = stack.at(-1);
      const v = f.outer && !f.taken && evalExpr(m[1]);
      f.live = v;
      f.taken = f.taken || v;
    } else if (/^#else\b/.test(t)) {
      const f = stack.at(-1);
      f.live = f.outer && !f.taken;
      f.taken = true;
    } else if (/^#endif\b/.test(t)) {
      stack.pop();
    } else if ((m = t.match(/^#define\s+(\w+)(?:\s+(.*))?$/))) {
      if (live()) {
        if (!/\(/.test(m[1])) defs.set(m[1], (m[2] || '1').trim());
        out.push(line);
      }
    } else if (live()) {
      out.push(line);
    }
  }
  assert.equal(stack.length, 0, 'balanced #if / #endif');
  return out.join('\n');
}

test('setupGL compiles and links every first program in one batch, reading no status until the first frame', () => {
  const b = makeBrowser({ parallel: true });
  const { ops, count } = b.gl;
  assert.equal(count('getShaderParameter'), 0, 'setupGL never stalls on COMPILE_STATUS');
  assert.equal(count('getProgramParameter'), 0, 'setupGL never stalls on LINK_STATUS: all of it read by the first frame');
  assert.equal(count('linkProgram'), 4 + BOOT.length, 'flat, rect, post and adaptive with the 6 first kinds: one batch');
  assert.deepEqual(b.gl.oneKinds(), [...BOOT], 'the first kinds, no other, at start');
  assert.equal(b.gl.createdVs.size, 2, 'two vertex shaders: the instances\' and the passes\'');

  b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  const links = ops.map((op, i) => (op[0] === 'linkProgram' ? i : -1)).filter((i) => i >= 0);
  const checks = ops.map((op, i) => (op[0] === 'getProgramParameter' ? i : -1)).filter((i) => i >= 0);
  assert.equal(checks.length, 4 + BOOT.length, 'the first frame reads each first program\'s LINK_STATUS once');
  assert.ok(checks[0] > links[4 + BOOT.length - 1], 'every first link is submitted before the first status is read');
  const [instVs] = [...b.gl.createdVs];
  assert.ok(!b.gl.deletedShaders.has(instVs), 'the instances\' vertex shader is kept for programs linked later');
});

test('the first frame prewarms the 7 other programs heaviest first, and a merged liquid frame adopts them unlinked', async () => {
  const b = makeBrowser({ parallel: true });
  const { count } = b.gl;
  const links0 = count('linkProgram');
  b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  assert.equal(count('linkProgram'), links0, 'nothing prewarmed within the frame itself');
  const statusReads = count('getProgramParameter') + count('getShaderParameter');
  await flushMicrotasks();
  assert.deepEqual(b.gl.oneKinds(), [...BOOT, ...PREWARM], 'heavy glass first: merged liquid, frosted, lens, any kind, ...');
  assert.equal(count('linkProgram'), links0 + PREWARM.length, '7 programs started in the background');
  assert.equal(count('getProgramParameter') + count('getShaderParameter'), statusReads, 'prewarm never waits on a status');
  const own = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13];
  assert.deepEqual([...BOOT, ...PREWARM].sort((x, y) => x - y), [-1, ...own], 'every program is made at start or prewarmed');

  // a frame of merged liquid glass (capture -1, merge radius 12) and frosted glass draws from the prewarmed programs
  const linked = count('linkProgram');
  b.H.present(frameOf(makeInst(0), makeInst(6, 12, -1), makeInst(5, 0, -2)), 3, 1, 1, 1);
  assert.equal(count('linkProgram'), linked, 'the glass frame adopts prewarmed programs, linking none again');
  assert.equal(b.gl.ops.filter((op) => op[0] === 'useProgram' && op[1] === null).length, 0, 'never useProgram(null)');
  b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  await flushMicrotasks();
  assert.equal(count('linkProgram'), linked, 'prewarm runs once per context');
});

test('the merged liquid program carries no droplet GLSL; the single liquid and any-kind programs keep all of it', () => {
  const fs = new Function(`${source.match(/const\s+ATLAS\s*=\s*\d+\s*;/)[0]}\n` +
    `${source.slice(source.indexOf('const VS = `#version 300 es'), source.indexOf('function compileAsync('))}\nreturn FS;`)();
  const body = fs.slice('#version 300 es\n'.length);
  const merged = preprocess(body, 6);
  const single = preprocess(body, 13);
  const any = preprocess(body, -1);
  // [what, its pattern, whether the single liquid's program has it (it has the one shape, never the choice)]
  for (const [name, re, inSingle] of [
    ['the droplet Snell profile', /float mb_snell_intersect|vec2 mb_droplet_snell/, true],
    ['the droplet roles', /bool isModernDrop|bool isRoleTab|float dropHairline|bool is_drop\b/, true],
    ['the droplet look', /if \(isModernDrop\) \{\s+float backdropLuma/, true],
    ['the choice of the single shape', /if \(k <= 0\.0\) \{\s+d = sd_rr\(p, v_rect, s_rr\);/, false],
    ['the single shape\'s corners', /vec4 s_rr = /, true],
  ]) {
    assert.doesNotMatch(merged, re, `merged liquid (ONE_KIND 6) has no ${name}`);
    if (inSingle) assert.match(single, re, `single liquid (ONE_KIND 13) keeps ${name}`);
    assert.match(any, re, `the program of any kind (ONE_KIND -1) keeps ${name}`);
  }
  assert.match(merged, /sd_group\(p \+ vec2\(e, 0\.0\), k\)/, 'merged liquid keeps the group\'s shape');
  assert.ok(merged.split('\n').length < any.split('\n').length - 300, 'merged liquid sheds over 300 lines of GLSL');
  for (const k of [0, 1, 2, 3, 4, 5, 7, 8, 10, 11]) {
    assert.doesNotMatch(preprocess(body, k), /mb_droplet_snell/, `ONE_KIND ${k} has no droplet`);
  }
  // an unmerged liquid (merge radius <= 0) is drawn with the single liquid's program, a liquid outside any capture
  // with the program of any kind: never with the merged one
  assert.match(source, /const gkind = kind === 6 \? \(mergedLiquid \? 6 : MB_KIND_LIQUID_SINGLE\)/);
  assert.match(source, /const own = tid >= 0 && !h && kind !== 6 && hasOwnProg\(kind\);/);
});

test('no prewarm without KHR_parallel_shader_compile, on a page a test drives, or in a run to a declared moment', async () => {
  for (const opts of [
    { parallel: false },
    { parallel: true, webdriver: true },
    { parallel: true, search: '?shot_at=100' },
  ]) {
    const b = makeBrowser(opts);
    b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
    const links = b.gl.count('linkProgram');
    await flushMicrotasks();
    assert.equal(b.gl.count('linkProgram'), links, `no background prewarm for ${JSON.stringify(opts)}`);
    assert.deepEqual(b.gl.oneKinds(), BOOT, `only the first kinds for ${JSON.stringify(opts)}`);
  }
});

test('a lost context cancels the queued prewarm, and the context given back prewarms afresh, once', async () => {
  const b = makeBrowser({ parallel: true });
  const { count } = b.gl;
  b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  const links0 = count('linkProgram');
  b.gl.setLost(true);
  b.canvas.dispatch('webglcontextlost', { preventDefault() {} });
  await flushMicrotasks();
  assert.equal(count('linkProgram'), links0, 'nothing linked into a lost context');
  // a frame while lost: every link fails, said by nothing, drawn by nothing
  assert.doesNotThrow(() => b.H.present(frameOf(makeInst(0), makeInst(6, 12, -1)), 2, 1, 1, 1));

  b.gl.setLost(false);
  b.canvas.dispatch('webglcontextrestored', {});
  b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  // queued for this context, which is lost and given back before it runs: the generation alone stops it
  b.canvas.dispatch('webglcontextlost', { preventDefault() {} });
  b.canvas.dispatch('webglcontextrestored', {});
  const linksAfterFastRestore = count('linkProgram');
  await flushMicrotasks();
  assert.equal(count('linkProgram'), linksAfterFastRestore, 'a prewarm queued for an older context never links');

  b.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  await flushMicrotasks();
  assert.equal(count('linkProgram'), linksAfterFastRestore + PREWARM.length, 'the context given back prewarms the 7');
});

test('a failed link says its program\'s and shader\'s logs and lets go of both; a closed window lets go of all it made', async () => {
  const fail = makeBrowser({ parallel: false, failLink: (src) => src.startsWith('#version 300 es\n#define ONE_KIND 5\n') });
  fail.H.present(frameOf(makeInst(0)), 1, 1, 1, 1);
  assert.throws(
    () => fail.H.present(frameOf(makeInst(0), makeInst(5, 0, -1)), 2, 1, 1, 1),
    /FS: shader-log-\d+\nprog-log-\d+/,
    'the frosted program\'s failure carries its fragment shader\'s and program\'s logs',
  );
  const failedFs = [...fail.gl.shaderSrc].find(([, src]) => src.startsWith('#version 300 es\n#define ONE_KIND 5\n'))[0];
  assert.ok(fail.gl.deletedShaders.has(failedFs), 'the failed fragment shader is deleted');
  const failedProg = [...fail.gl.progShaders].find(([, ss]) => ss.some((s) => s.id === failedFs))[0];
  assert.ok(fail.gl.deletedProgs.has(failedProg), 'the failed program is deleted');
  const [instVs] = [...fail.gl.createdVs];
  assert.ok(!fail.gl.deletedShaders.has(instVs), 'a failed link keeps the shared vertex shader');

  // a window beside the page: its first frame prewarms in its own context, its closing deletes every program and
  // shader it made, prewarmed ones not yet adopted among them
  const b = makeBrowser({
    parallel: true,
    onFrame(H) { if (H.windows(4, 0, '') === 1) H.present(frameOf(makeInst(0)), 1, 1, 1, 1); },
  });
  await flushMicrotasks(); // (the host told what it decodes: frames asked for from here)
  const wid = b.H.windows(1, 0, JSON.stringify({ title: 'aux', w: 200, h: 200 }));
  assert.equal(wid, 1);
  const popup = b.popups[0];
  assert.equal(popup.gl.createdProgs.size, 4 + BOOT.length, 'the window\'s context starts with the first programs');
  b.H.requestFrame();
  assert.ok(popup.rafs.length > 0, 'the window\'s frame asked for');
  while (popup.rafs.length) popup.rafs.shift()(16);
  await flushMicrotasks();
  assert.equal(popup.gl.createdProgs.size, 4 + BOOT.length + PREWARM.length, 'the window prewarmed its own 7');
  assert.equal(b.gl.oneKinds().length, BOOT.length, 'the page\'s context prewarmed nothing for the window');
  b.H.windows(3, wid, '');
  await flushMicrotasks();
  for (const id of popup.gl.createdProgs) assert.ok(popup.gl.deletedProgs.has(id), `program ${id} deleted on close`);
  for (const id of popup.gl.createdShaders) assert.ok(popup.gl.deletedShaders.has(id), `shader ${id} deleted on close`);

  // one closed before its first frame took its first programs: those, never linked into it, deleted all the same
  const wid2 = b.H.windows(1, 0, JSON.stringify({ title: 'brief', w: 200, h: 200 }));
  assert.ok(wid2 > 0);
  const brief = b.popups[1];
  assert.equal(brief.gl.count('getProgramParameter'), 0, 'its first programs not yet taken');
  b.H.windows(3, wid2, '');
  await flushMicrotasks();
  for (const id of brief.gl.createdProgs) assert.ok(brief.gl.deletedProgs.has(id), `program ${id} deleted on early close`);
  for (const id of brief.gl.createdShaders) assert.ok(brief.gl.deletedShaders.has(id), `shader ${id} deleted on early close`);
});
