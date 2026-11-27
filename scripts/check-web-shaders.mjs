#!/usr/bin/env node
/**
 * Compile and link all WebGL2 shader variants from web/host.js in a real
 * Headless Chromium WebGL2 context (zero external npm dependencies).
 *
 * Usage:
 *   node scripts/check-web-shaders.mjs [--json]
 *   CHROME_BIN=/path/to/chrome-headless-shell node scripts/check-web-shaders.mjs
 */

import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const REPO_ROOT = path.resolve(__dirname, '..');
const HOST_JS_PATH = path.join(REPO_ROOT, 'web', 'host.js');

const ONE_KINDS = [-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13];
const JSON_MODE = process.argv.includes('--json');

function isExecutable(p) {
  if (!p) return false;
  try {
    fs.accessSync(p, fs.constants.X_OK);
    return fs.statSync(p).isFile();
  } catch {
    return false;
  }
}

function findInPlaywrightCache(cacheDir) {
  if (!fs.existsSync(cacheDir)) return null;
  let entries;
  try {
    entries = fs.readdirSync(cacheDir);
  } catch {
    return null;
  }
  const sortDesc = (a, b) => b.localeCompare(a, undefined, { numeric: true });
  const shellDirs = entries.filter((e) => e.startsWith('chromium_headless_shell-')).sort(sortDesc);
  const fullDirs = entries.filter((e) => /^chromium-\d+/.test(e)).sort(sortDesc);

  const candidates = [];
  for (const dir of shellDirs) {
    const base = path.join(cacheDir, dir);
    candidates.push(
      path.join(base, 'chrome-headless-shell-mac-arm64', 'chrome-headless-shell'),
      path.join(base, 'chrome-headless-shell-mac-x64', 'chrome-headless-shell'),
      path.join(base, 'chrome-headless-shell-linux64', 'chrome-headless-shell'),
      path.join(base, 'chrome-linux', 'headless_shell'),
    );
  }
  for (const dir of fullDirs) {
    const base = path.join(cacheDir, dir);
    candidates.push(
      path.join(base, 'chrome-mac-arm64', 'Google Chrome for Testing.app', 'Contents', 'MacOS', 'Google Chrome for Testing'),
      path.join(base, 'chrome-mac', 'Chromium.app', 'Contents', 'MacOS', 'Chromium'),
      path.join(base, 'chrome-linux', 'chrome'),
      path.join(base, 'chrome-linux64', 'chrome'),
    );
  }
  for (const c of candidates) {
    if (isExecutable(c)) return c;
  }
  return null;
}

function findChromeBinary() {
  if (process.env.CHROME_BIN) {
    if (isExecutable(process.env.CHROME_BIN)) return process.env.CHROME_BIN;
    throw new Error(`CHROME_BIN is not executable: ${process.env.CHROME_BIN}`);
  }
  const home = os.homedir();
  const cacheDirs = [
    path.join(home, 'Library', 'Caches', 'ms-playwright'),
    path.join(home, '.cache', 'ms-playwright'),
  ];
  for (const dir of cacheDirs) {
    const found = findInPlaywrightCache(dir);
    if (found) return found;
  }
  const sysCandidates = [
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
  ];
  for (const c of sysCandidates) {
    if (isExecutable(c)) return c;
  }
  for (const name of ['google-chrome', 'chromium', 'chromium-browser', 'chrome-headless-shell']) {
    const res = spawnSync('which', [name], { encoding: 'utf8' });
    if (res.status === 0) {
      const p = res.stdout.trim();
      if (isExecutable(p)) return p;
    }
  }
  throw new Error('No Chromium/Chrome binary found. Set CHROME_BIN to a valid executable.');
}

function extractShaders(hostJsPath) {
  const src = fs.readFileSync(hostJsPath, 'utf8');
  const atlasMatch = src.match(/const\s+ATLAS\s*=\s*\d+\s*;/);
  if (!atlasMatch) throw new Error('Could not locate ATLAS constant in web/host.js');

  const startIdx = src.indexOf('const VS = `#version 300 es');
  if (startIdx < 0) throw new Error('Could not locate VS shader in web/host.js');

  const endIdx = src.indexOf('function compileAsync(', startIdx);
  if (endIdx < 0) throw new Error('Could not locate end of shader declarations in web/host.js');

  const snippet = `${atlasMatch[0]}\n${src.slice(startIdx, endIdx)}\nreturn { VS, CORNERS, CONTAINED_GLSL, FS, FS_FLAT, FS_RECT, PVS, PFS, AD_FS };`;
  const shaders = new Function(snippet)();
  for (const key of ['VS', 'CORNERS', 'CONTAINED_GLSL', 'FS', 'FS_FLAT', 'FS_RECT', 'PVS', 'PFS', 'AD_FS']) {
    if (typeof shaders[key] !== 'string' || !shaders[key].length) {
      throw new Error(`Extracted shader ${key} is empty or invalid`);
    }
  }
  return shaders;
}

function buildTestPrograms(shaders) {
  const fsHeader = '#version 300 es\n';
  if (!shaders.FS.startsWith(fsHeader)) {
    throw new Error('FS does not start with #version 300 es');
  }
  const fsTail = shaders.FS.slice(fsHeader.length);
  const programs = [];
  for (const kind of ONE_KINDS) {
    programs.push({
      name: `FS(ONE_KIND=${kind})`,
      vs: shaders.VS,
      fs: `${fsHeader}#define ONE_KIND ${kind}\n${fsTail}`,
    });
  }
  programs.push(
    { name: 'FS_FLAT', vs: shaders.VS, fs: shaders.FS_FLAT },
    { name: 'FS_RECT', vs: shaders.VS, fs: shaders.FS_RECT },
    { name: 'PFS', vs: shaders.PVS, fs: shaders.PFS },
    { name: 'AD_FS', vs: shaders.PVS, fs: shaders.AD_FS },
  );
  return programs;
}

function buildHtmlPage(programs) {
  const payload = JSON.stringify(programs).replace(/<\//g, '<\\/');
  return `<!doctype html>
<html>
<head><meta charset="utf-8"><title>WebGL2 Shader Check</title></head>
<body>
<canvas id="c" width="16" height="16"></canvas>
<script>
(async () => {
  const programs = ${payload};
  const canvas = document.getElementById('c');
  const gl = canvas.getContext('webgl2', { premultipliedAlpha: true, antialias: false, alpha: true });
  let report;
  if (!gl) {
    report = { ok: false, error: 'WebGL2 context unavailable', passed: 0, total: programs.length, results: [] };
  } else {
    const results = [];
    let passed = 0;
    for (const item of programs) {
      const vs = gl.createShader(gl.VERTEX_SHADER);
      gl.shaderSource(vs, item.vs);
      gl.compileShader(vs);
      const vsOk = !!gl.getShaderParameter(vs, gl.COMPILE_STATUS);
      const vsLog = vsOk ? '' : (gl.getShaderInfoLog(vs) || 'VS compile failed');

      const fs = gl.createShader(gl.FRAGMENT_SHADER);
      gl.shaderSource(fs, item.fs);
      gl.compileShader(fs);
      const fsOk = !!gl.getShaderParameter(fs, gl.COMPILE_STATUS);
      const fsLog = fsOk ? '' : (gl.getShaderInfoLog(fs) || 'FS compile failed');

      const prog = gl.createProgram();
      gl.attachShader(prog, vs);
      gl.attachShader(prog, fs);
      gl.linkProgram(prog);
      const linkOk = !!gl.getProgramParameter(prog, gl.LINK_STATUS);
      const linkLog = linkOk ? '' : (gl.getProgramInfoLog(prog) || 'Program link failed');

      const ok = vsOk && fsOk && linkOk;
      if (ok) passed++;
      results.push({
        name: item.name,
        ok,
        vsOk,
        fsOk,
        linkOk,
        error: [vsLog && ('VS: ' + vsLog), fsLog && ('FS: ' + fsLog), linkLog && ('LINK: ' + linkLog)].filter(Boolean).join('\\n').trim(),
      });
      gl.deleteProgram(prog);
      gl.deleteShader(vs);
      gl.deleteShader(fs);
    }

    // Real WebGL2 draw & gl.readPixels smoke tests (same 16x16 WebGL2 context)
    const drawChecks = [];
    let drawPassed = 0;
    if (passed === programs.length) {
      const INST = 60;
      const W = 16, H = 16;
      const byName = new Map(programs.map((p) => [p.name, p]));
      const linkProg = (name) => {
        const item = byName.get(name);
        const vs = gl.createShader(gl.VERTEX_SHADER);
        gl.shaderSource(vs, item.vs);
        gl.compileShader(vs);
        const fs = gl.createShader(gl.FRAGMENT_SHADER);
        gl.shaderSource(fs, item.fs);
        gl.compileShader(fs);
        const p = gl.createProgram();
        gl.attachShader(p, vs);
        gl.attachShader(p, fs);
        gl.linkProgram(p);
        gl.deleteShader(vs);
        gl.deleteShader(fs);
        return p;
      };
      const makeTex1x1 = (rgba) => {
        const t = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, t);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(rgba));
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        return t;
      };
      const vao = gl.createVertexArray();
      gl.bindVertexArray(vao);
      const vbo = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
      gl.bufferData(gl.ARRAY_BUFFER, INST * 4, gl.STREAM_DRAW);
      for (let i = 0; i < INST / 4; i++) {
        gl.enableVertexAttribArray(i);
        gl.vertexAttribPointer(i, 4, gl.FLOAT, false, INST * 4, i * 16);
        gl.vertexAttribDivisor(i, 1);
      }
      const whiteTex = makeTex1x1([255, 255, 255, 255]);
      const bdBlueTex = makeTex1x1([64, 160, 224, 255]);
      const bdRedTex = makeTex1x1([224, 64, 64, 255]);
      gl.viewport(0, 0, W, H);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

      const readPx = (x, yTop) => {
        const px = new Uint8Array(4);
        gl.readPixels(x, H - 1 - yTop, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        return [px[0], px[1], px[2], px[3]];
      };
      const near = (a, b, tol = 2) =>
        Math.abs(a[0] - b[0]) <= tol &&
        Math.abs(a[1] - b[1]) <= tol &&
        Math.abs(a[2] - b[2]) <= tol &&
        Math.abs(a[3] - b[3]) <= tol;

      const drainErr = () => {
        const errs = [];
        let e;
        while ((e = gl.getError()) !== gl.NO_ERROR) errs.push(e);
        return errs;
      };
      drainErr();

      // 1. FS_FLAT solid rect (1.0, 0.25, 0.5, 1.0) -> [255, 64, 128, 255]
      {
        const p = linkProg('FS_FLAT');
        gl.useProgram(p);
        gl.uniform2f(gl.getUniformLocation(p, 'u_vp'), W, H);
        gl.uniform2f(gl.getUniformLocation(p, 'u_origin'), 0, 0);
        const inst = new Float32Array(INST);
        inst.set([0, 0, W, H], 0);
        inst.set([0, 0, 0, 0], 4);
        inst.set([1.0, 0.25, 0.5, 1.0], 8);
        inst.set([0, 0, 0, 1], 12);
        inst.set([0, 0, W, H], 16);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, inst);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, 1);
        const center = readPx(8, 8);
        const errs = drainErr();
        const expected = [255, 64, 128, 255];
        const ok = errs.length === 0 && near(center, expected, 2);
        if (ok) drawPassed++;
        drawChecks.push({ name: 'draw:FS_FLAT', ok, center, expected, glErrors: errs });
        gl.deleteProgram(p);
      }

      // 2. FS_RECT inset rect (0.2, 0.8, 0.3, 1.0) -> center matches, outside corner transparent
      {
        const p = linkProg('FS_RECT');
        gl.useProgram(p);
        gl.uniform2f(gl.getUniformLocation(p, 'u_vp'), W, H);
        gl.uniform2f(gl.getUniformLocation(p, 'u_origin'), 0, 0);
        gl.uniform1f(gl.getUniformLocation(p, 'u_scale'), 1.0);
        gl.uniform1f(gl.getUniformLocation(p, 'u_top'), H);
        const inst = new Float32Array(INST);
        inst.set([2, 2, 12, 12], 0);
        inst.set([0, 0, 0, 0], 4);
        inst.set([0.2, 0.8, 0.3, 1.0], 8);
        inst.set([0, 0, 0, 1], 12);
        inst.set([0, 0, W, H], 16);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, inst);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, 1);
        const center = readPx(8, 8);
        const corner = readPx(0, 0);
        const errs = drainErr();
        const expected = [51, 204, 77, 255];
        const ok = errs.length === 0 && near(center, expected, 2) && near(corner, [0, 0, 0, 0], 0);
        if (ok) drawPassed++;
        drawChecks.push({ name: 'draw:FS_RECT', ok, center, corner, expected, glErrors: errs });
        gl.deleteProgram(p);
      }

      // 3. FS(ONE_KIND=0) rounded solid rect (0.2, 0.5, 0.9, 1.0, radius=6) -> center matches, rounded corner transparent
      {
        const p = linkProg('FS(ONE_KIND=0)');
        gl.useProgram(p);
        gl.uniform2f(gl.getUniformLocation(p, 'u_vp'), W, H);
        gl.uniform2f(gl.getUniformLocation(p, 'u_origin'), 0, 0);
        gl.uniform1f(gl.getUniformLocation(p, 'u_scale'), 1.0);
        gl.uniform1f(gl.getUniformLocation(p, 'u_top'), H);
        const inst = new Float32Array(INST);
        inst.set([0, 0, W, H], 0);
        inst.set([6.0, 0.0, 0.0, 0.0], 4);
        inst.set([0.2, 0.5, 0.9, 1.0], 8);
        inst.set([0, 0, 0, 1], 12);
        inst.set([0, 0, W, H], 16);
        inst.set([6.0, 6.0, 6.0, 6.0], 36);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, inst);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, 1);
        const center = readPx(8, 8);
        const corner = readPx(0, 0);
        const errs = drainErr();
        const expected = [51, 128, 230, 255];
        const ok = errs.length === 0 && near(center, expected, 2) && near(corner, [0, 0, 0, 0], 0);
        if (ok) drawPassed++;
        drawChecks.push({ name: 'draw:FS(ONE_KIND=0)', ok, center, corner, expected, glErrors: errs });
        gl.deleteProgram(p);
      }

      // 4. FS(ONE_KIND=13) Liquid Glass over blue & red backdrop textures (proves real u_bd sampling). An unmerged
      // liquid (merge radius <= 0, u_n = 0) is drawn by the single liquid's program, as the host's draw loop does;
      // FS(ONE_KIND=6) is the merged group's only (no droplet, no single-shape branch)
      {
        const p = linkProg('FS(ONE_KIND=13)');
        gl.useProgram(p);
        gl.uniform2f(gl.getUniformLocation(p, 'u_vp'), W, H);
        gl.uniform2f(gl.getUniformLocation(p, 'u_origin'), 0, 0);
        gl.uniform1f(gl.getUniformLocation(p, 'u_scale'), 1.0);
        gl.uniform1f(gl.getUniformLocation(p, 'u_top'), H);
        const uAtlas = gl.getUniformLocation(p, 'u_atlas');
        const uBd = gl.getUniformLocation(p, 'u_bd');
        const uAd = gl.getUniformLocation(p, 'u_ad');
        if (uAtlas) gl.uniform1i(uAtlas, 0);
        if (uBd) gl.uniform1i(uBd, 2);
        if (uAd) gl.uniform1i(uAd, 4);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, whiteTex);
        gl.activeTexture(gl.TEXTURE4);
        gl.bindTexture(gl.TEXTURE_2D, whiteTex);
        const uMap = gl.getUniformLocation(p, 'u_map');
        const uUvc = gl.getUniformLocation(p, 'u_uvc');
        const uGb = gl.getUniformLocation(p, 'u_gb');
        const uN = gl.getUniformLocation(p, 'u_n');
        const uGlight = gl.getUniformLocation(p, 'u_glight');
        const uAdN = gl.getUniformLocation(p, 'u_ad_n');
        if (uMap) gl.uniform4f(uMap, 1.0 / W, -1.0 / H, 0.0, 1.0);
        if (uUvc) gl.uniform4f(uUvc, 0.0, 0.0, 1.0, 1.0);
        if (uGb) gl.uniform4f(uGb, 0.0, 0.0, W, H);
        if (uN) gl.uniform1i(uN, 0);
        if (uGlight) gl.uniform2f(uGlight, 0.7071, 0.7071);
        if (uAdN) gl.uniform1i(uAdN, 0);
        const inst = new Float32Array(INST);
        inst.set([2, 2, 12, 12], 0);
        inst.set([4.0, 6.0, 1.3, 2.0], 4);
        inst.set([1.0, 1.0, 1.0, 0.30], 8);
        inst.set([0.55, 4.0, 0.0, 1.0], 12);
        inst.set([0, 0, W, H], 16);
        inst.set([-1.0, 4.0, 0.15, 0.0], 20);
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, inst);

        // Pass A: blue backdrop [64, 160, 224, 255]
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, bdBlueTex);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, 1);
        const blueCenter = readPx(8, 8);

        // Pass B: red backdrop [224, 64, 64, 255]
        gl.activeTexture(gl.TEXTURE2);
        gl.bindTexture(gl.TEXTURE_2D, bdRedTex);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, 1);
        const redCenter = readPx(8, 8);

        const errs = drainErr();
        const blueOk = blueCenter[3] > 0 && blueCenter[2] > blueCenter[0] + 40;
        const redOk = redCenter[3] > 0 && redCenter[0] > redCenter[2] + 40;
        const ok = errs.length === 0 && blueOk && redOk;
        if (ok) drawPassed++;
        drawChecks.push({ name: 'draw:FS(ONE_KIND=13)', ok, center: blueCenter, redCenter, glErrors: errs });
        gl.deleteProgram(p);
      }

      gl.deleteTexture(whiteTex);
      gl.deleteTexture(bdBlueTex);
      gl.deleteTexture(bdRedTex);
      gl.bindVertexArray(null);
      gl.deleteBuffer(vbo);
      gl.deleteVertexArray(vao);
    }

    // Real WebGL2 production-shader readback smoke (V1..V9 2D ROI) using FS(ONE_KIND=13), the unmerged liquid's program
    const vectorResults = [];
    let vectorsPassed = 0;
    const glassItem = programs.find((p) => p.name === 'FS(ONE_KIND=13)');
    if (glassItem && passed === programs.length) {
      const W = 320, H = 240, SCALE = 2.0;
      canvas.width = W;
      canvas.height = H;
      gl.viewport(0, 0, W, H);

      const vs = gl.createShader(gl.VERTEX_SHADER);
      gl.shaderSource(vs, glassItem.vs);
      gl.compileShader(vs);
      const fs = gl.createShader(gl.FRAGMENT_SHADER);
      gl.shaderSource(fs, glassItem.fs);
      gl.compileShader(fs);
      const prog = gl.createProgram();
      gl.attachShader(prog, vs);
      gl.attachShader(prog, fs);
      gl.linkProgram(prog);
      gl.useProgram(prog);

      const bdTex = gl.createTexture();
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, bdTex);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

      const u = (n) => gl.getUniformLocation(prog, n);
      gl.uniform2f(u('u_vp'), W / SCALE, H / SCALE);
      gl.uniform2f(u('u_origin'), 0.0, 0.0);
      gl.uniform4f(u('u_gb'), 0.0, 0.0, 0.0, 0.0);
      gl.uniform1f(u('u_scale'), SCALE);
      gl.uniform1f(u('u_top'), H);
      gl.uniform1i(u('u_bd'), 0);
      gl.uniform4f(u('u_map'), SCALE / W, SCALE / H, 0.0, 0.0);
      gl.uniform4f(u('u_uvc'), 0.0, 0.0, 1.0, 1.0);
      gl.uniform1i(u('u_n'), 0);
      gl.uniform2f(u('u_glight'), 0.70710678, 0.70710678);
      gl.uniform1i(u('u_ad_n'), 0);

      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);

      const sampleGlass = (bg, tint, blur, rim, bezel, refract, chroma, adaptive) => {
        const bgByte = new Uint8Array([
          Math.round(bg[0] * 255),
          Math.round(bg[1] * 255),
          Math.round(bg[2] * 255),
          255,
        ]);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, bdTex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, bgByte);

        gl.clearColor(bg[0], bg[1], bg[2], 1.0);
        gl.clear(gl.COLOR_BUFFER_BIT);

        gl.vertexAttrib4f(0, 20.0, 20.0, 120.0, 80.0); // a_rect
        gl.vertexAttrib4f(1, 20.0, 6.0, 1.0, blur); // a_p
        gl.vertexAttrib4f(2, tint[0], tint[1], tint[2], tint[3]); // a_color
        gl.vertexAttrib4f(3, rim, bezel, adaptive ? -1.0 : 0.0, 1.0); // a_c2
        gl.vertexAttrib4f(4, 0.0, 0.0, 1e9, 1e9); // a_clip
        gl.vertexAttrib4f(5, -1.0, refract, chroma, 0.0); // a_pad
        gl.vertexAttrib4f(6, 0.0, 0.0, 0.0, 0.0); // a_rc
        gl.vertexAttrib4f(7, 0.0, 0.0, 0.0, 0.0); // a_rs
        gl.vertexAttrib4f(8, 0.0, 0.0, 0.0, 0.0); // a_rot
        gl.vertexAttrib4f(9, 20.0, 20.0, 20.0, 20.0); // a_rr
        gl.vertexAttrib4f(10, 0.0, 0.0, 1e9, 1e9); // a_oclip
        gl.vertexAttrib4f(11, 0.0, 0.0, 0.0, 0.0); // a_orc
        gl.vertexAttrib4f(12, 0.0, 0.0, 0.0, 0.0); // a_ors
        gl.vertexAttrib4f(13, 0.0, 0.0, 0.0, 0.0); // a_fade
        gl.vertexAttrib4f(14, 0.0, 0.0, 0.0, 0.0); // a_fadex

        gl.drawArrays(gl.TRIANGLES, 0, 6);
        const px = new Uint8Array(4);
        gl.readPixels(160, 120, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        return [px[0], px[1], px[2]];
      };

      const darkTint = [32 / 255, 32 / 255, 32 / 255, 103 / 255];
      const lightTint = [253 / 255, 252 / 255, 253 / 255, 103 / 255];
      const vectors = [
        {
          name: 'V1 dark regular on #000000',
          got: sampleGlass([0, 0, 0], darkTint, 1.2, 0.6, 14.0, 20.0, 0.06, false),
          check: ([r, g, b]) => Math.abs(r - 34) <= 3 && Math.abs(g - 34) <= 3 && Math.abs(b - 34) <= 3,
          expected: '(34,34,34) +/-3',
        },
        {
          name: 'V2 dark regular on #1B1C1F',
          got: sampleGlass([27 / 255, 28 / 255, 31 / 255], darkTint, 1.2, 0.6, 14.0, 20.0, 0.06, false),
          check: ([r, g, b]) => Math.abs(r - 58) <= 3 && Math.abs(g - 59) <= 3 && Math.abs(b - 62) <= 3,
          expected: '(58,59,62) +/-3',
        },
        {
          name: 'V3 dark adaptive regular on #FFFFFF',
          got: sampleGlass([1, 1, 1], darkTint, 1.2, 0.6, 14.0, 20.0, 0.06, true),
          check: ([r, g, b]) => Math.abs(r - 174) <= 3 && Math.abs(g - 174) <= 3 && Math.abs(b - 174) <= 3,
          expected: '(174,174,174) +/-3',
        },
        {
          name: 'V4 light regular on #0088FF',
          got: sampleGlass([0, 136 / 255, 1], lightTint, 1.2, 0.6, 14.0, 20.0, 0.06, false),
          check: ([r, g, b]) => {
            const mx = Math.max(r, g, b), mn = Math.min(r, g, b);
            const sat = mx > 0 ? (mx - mn) / mx : 0;
            return Math.abs(r - 89) <= 4 && Math.abs(g - 206) <= 4 && Math.abs(b - 255) <= 4 && sat >= 0.62;
          },
          expected: '(89,206,255) +/-4 & HSV S>=0.62',
        },
        {
          name: 'V5 pure clear on (0,255,64)',
          got: sampleGlass([0, 1, 64 / 255], [1, 1, 1, 0], 0.0, 0.0, 8.0, 0.0, 0.0, false),
          check: ([r, g, b]) => Math.abs(r - 0) <= 2 && Math.abs(g - 255) <= 2 && Math.abs(b - 64) <= 2,
          expected: '(0,255,64) +/-2',
        },
        {
          name: 'V6 high-density light 0xD9 on (0,0.4,1)',
          got: sampleGlass([0, 0.4, 1], [253 / 255, 252 / 255, 253 / 255, 217 / 255], 2.0, 0.6, 14.0, 20.0, 0.06, false),
          check: ([r]) => r >= 184,
          expected: 'R >= 184',
        },
        {
          name: 'V7 opaque light 0xFF on (0,0.4,1)',
          got: sampleGlass([0, 0.4, 1], [253 / 255, 252 / 255, 253 / 255, 1.0], 2.0, 0.6, 14.0, 20.0, 0.06, false),
          check: ([r, g, b]) => Math.abs(r - 253) <= 2 && Math.abs(g - 252) <= 2 && Math.abs(b - 253) <= 2,
          expected: '(253,252,253) +/-2',
        },
      ];

      for (const v of vectors) {
        const ok = v.check(v.got);
        if (ok) vectorsPassed++;
        vectorResults.push({
          name: v.name,
          ok,
          got: v.got,
          expected: v.expected,
        });
      }

      // Background blit program so run2D renders the actual 2D backdrop (e.g. green track) before blending glass
      const bgVs = gl.createShader(gl.VERTEX_SHADER);
      gl.shaderSource(bgVs, '#version 300 es\\nout vec2 v_uv;\\nvoid main(){\\n  vec2 p = vec2((gl_VertexID == 1 || gl_VertexID == 4 || gl_VertexID == 5) ? 1.0 : -1.0,\\n                (gl_VertexID == 2 || gl_VertexID == 3 || gl_VertexID == 5) ? -1.0 : 1.0);\\n  v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);\\n  gl_Position = vec4(p, 0.0, 1.0);\\n}');
      gl.compileShader(bgVs);
      const bgFs = gl.createShader(gl.FRAGMENT_SHADER);
      gl.shaderSource(bgFs, '#version 300 es\\nprecision highp float;\\nuniform sampler2D u_tex;\\nin vec2 v_uv;\\nout vec4 o;\\nvoid main(){ o = texture(u_tex, v_uv); }');
      gl.compileShader(bgFs);
      const bgProg = gl.createProgram();
      gl.attachShader(bgProg, bgVs);
      gl.attachShader(bgProg, bgFs);
      gl.linkProgram(bgProg);
      const bgTexLoc = gl.getUniformLocation(bgProg, 'u_tex');

      // Real 2D multi-pixel ROI gates V8 & V9 (using full 2D backdrop textures at 3x & 2x!)
      const run2D = (wPt, hPt, sc, fillBgFn, glass) => {
        const W2 = Math.round(wPt * sc), H2 = Math.round(hPt * sc);
        if (canvas.width !== W2 || canvas.height !== H2) {
          canvas.width = W2;
          canvas.height = H2;
        }
        gl.viewport(0, 0, W2, H2);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, bdTex);
        if (fillBgFn) {
          const bgTexBuf = new Uint8Array(W2 * H2 * 4);
          for (let py = 0; py < H2; py++) {
            const ptY = (py + 0.5) / sc;
            for (let px = 0; px < W2; px++) {
              const ptX = (px + 0.5) / sc;
              const c = fillBgFn(ptX, ptY);
              const i = (py * W2 + px) * 4;
              bgTexBuf[i] = c[0]; bgTexBuf[i + 1] = c[1]; bgTexBuf[i + 2] = c[2]; bgTexBuf[i + 3] = 255;
            }
          }
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, W2, H2, 0, gl.RGBA, gl.UNSIGNED_BYTE, bgTexBuf);
        }

        // Blit 2D backdrop texture onto framebuffer before blending glass (matches Metal/GLES3/D3D11)
        gl.useProgram(bgProg);
        gl.disable(gl.BLEND);
        gl.uniform1i(bgTexLoc, 0);
        gl.drawArrays(gl.TRIANGLES, 0, 6);

        gl.useProgram(prog);
        gl.enable(gl.BLEND);
        gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        gl.uniform2f(u('u_vp'), wPt, hPt);
        gl.uniform1f(u('u_scale'), sc);
        gl.uniform1f(u('u_top'), H2);
        gl.uniform4f(u('u_map'), 1.0 / wPt, 1.0 / hPt, 0.0, 0.0);
        gl.uniform4f(u('u_uvc'), 0.5 / W2, 0.5 / H2, 1.0 - 0.5 / W2, 1.0 - 0.5 / H2);

        gl.vertexAttrib4f(0, glass.x, glass.y, glass.w, glass.h);
        gl.vertexAttrib4f(1, glass.r, 6.0, 1.0, glass.blurSlot);
        const col = glass.col || [1.0, 1.0, 1.0, 0.0];
        gl.vertexAttrib4f(2, col[0], col[1], col[2], col[3]);
        gl.vertexAttrib4f(3, glass.rim, glass.bezel, 0.0, 1.0);
        gl.vertexAttrib4f(4, 0.0, 0.0, 1e9, 1e9);
        gl.vertexAttrib4f(5, -1.0, glass.refract, glass.chroma, 0.0);
        gl.vertexAttrib4f(9, glass.r, glass.r, glass.r, glass.r);
        gl.drawArrays(gl.TRIANGLES, 0, 6);

        const out = new Uint8Array(W2 * H2 * 4);
        gl.readPixels(0, 0, W2, H2, gl.RGBA, gl.UNSIGNED_BYTE, out);
        // Flip Y so (x, y) is top-down matching Metal/GLES3/D3D11
        const at = (x, y) => {
          const fy = H2 - 1 - y;
          const idx = (fy * W2 + x) * 4;
          return [out[idx], out[idx + 1], out[idx + 2]];
        };
        const rgbaAt = (x, y) => {
          const fy = H2 - 1 - y;
          const idx = (fy * W2 + x) * 4;
          return [out[idx], out[idx + 1], out[idx + 2], out[idx + 3]];
        };
        const lumaAt = (x, y) => {
          const [r, g, b] = at(x, y);
          return Math.floor((r * 2126 + g * 7152 + b * 722) / 10000);
        };
        return { at, rgbaAt, lumaAt, W2, H2 };
      };

      const pushVec = (name, ok, got, expected) => {
        if (ok) vectorsPassed++;
        vectorResults.push({ name, ok, got, expected });
      };

      // V8: Slider 2D ROI (Dark + Monotonicity + Light + dropRole=0 magnifier)
      {
        const s = run2D(160, 100, 3, (x, y) => {
          if (y >= 47 && y <= 53 && x >= 10 && x <= 150) {
            return x < 80 ? [0, 136, 255] : [104, 104, 110];
          }
          return [28, 28, 30];
        }, { x: 51, y: 31, w: 58, h: 38, r: 19, blurSlot: -16, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        let bestB = 0, blueDx = -1, bestG = 0, greyDx = -1;
        for (let i = 0; i <= 18; i++) {
          const dxPt = (i + 0.5) / 3;
          let cb = 0, cg = 0;
          for (let y = 93; y < 207; y++) {
            const [rb, , bb] = s.at(153 + i, y);
            if (bb - rb > 100) cb++;
            if (s.lumaAt(326 - i, y) > 60) cg++;
          }
          if (cb / 3 > bestB) { bestB = cb / 3; blueDx = dxPt; }
          if (cg / 3 > bestG) { bestG = cg / 3; greyDx = dxPt; }
        }
        let c55B = 0, c55G = 0;
        for (let y = 93; y < 207; y++) {
          const [r, , b] = s.at(169, y);
          if (b - r > 100) c55B++;
          if (s.lumaAt(310, y) > 72) c55G++;
        }
        const h55B = c55B / 3, h55G = c55G / 3;
        const [rl, , bl] = s.at(237, 150), [rr, , br] = s.at(243, 150);
        const [rt, , bt] = s.at(156, 105), [rbo, , bbo] = s.at(156, 195);
        const yHair = s.lumaAt(240, 93), yRim = s.lumaAt(240, 95), ySh = s.lumaAt(240, 99), yDome = s.lumaAt(240, 120), yLow = s.lumaAt(240, 165);

        const sm = run2D(160, 100, 3, (_x, y) => {
          const t = Math.max(0, Math.min(1, (y - 31) / 38));
          return [0, Math.round(t * 255), 0];
        }, { x: 51, y: 31, w: 58, h: 38, r: 19, blurSlot: -16, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        let monoOk = true, prevG = -1;
        for (let y = 138; y <= 162; y++) {
          const g = sm.at(159, y)[1];
          if (prevG >= 0 && g < prevG - 2) monoOk = false;
          prevG = g;
        }

        const sl = run2D(160, 100, 3, (x, y) => {
          if (y >= 47 && y <= 53 && x >= 10 && x <= 150) {
            return x < 80 ? [0, 136, 255] : [115, 115, 122];
          }
          return [255, 255, 255];
        }, { x: 51, y: 31, w: 58, h: 38, r: 19, blurSlot: -16, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        let lBestB = 0, lDxB = -1, lBestG = 0, lDxG = -1;
        for (let i = 0; i <= 18; i++) {
          const dxPt = (i + 0.5) / 3;
          let cb = 0, cg = 0;
          for (let y = 93; y < 207; y++) {
            const [rb, , bb] = sl.at(153 + i, y);
            if (bb - rb > 100) cb++;
            const lg = sl.lumaAt(326 - i, y);
            if (lg > 40 && lg < 175) cg++;
          }
          if (cb / 3 > lBestB) { lBestB = cb / 3; lDxB = dxPt; }
          if (cg / 3 > lBestG) { lBestG = cg / 3; lDxG = dxPt; }
        }
        let lc55B = 0, lc55G = 0;
        for (let y = 93; y < 207; y++) {
          const [r, , b] = sl.at(169, y);
          if (b - r > 100) lc55B++;
          const lg55 = sl.lumaAt(310, y);
          if (lg55 > 40 && lg55 < 175) lc55G++;
        }
        const lh55B = lc55B / 3, lh55G = lc55G / 3;
        const [lrl, , lbl] = sl.at(237, 150), [lrr, , lbr] = sl.at(243, 150);
        const [lrt, , lbt] = sl.at(156, 105), [lrbo, , lbbo] = sl.at(156, 195);
        const lHair = sl.lumaAt(240, 93), lRim = sl.lumaAt(240, 95), lSh = sl.lumaAt(240, 99), lDome = sl.lumaAt(240, 120), lLow = sl.lumaAt(240, 165);
        const lightOk = lBestB >= 9.5 && lBestB <= 12.5 && lDxB >= 0.33 && lDxB <= 1.50 &&
          lBestG >= 9.5 && lBestG <= 12.5 && lDxG >= 0.33 && lDxG <= 1.50 &&
          lh55B >= 5.6 && lh55B <= 6.4 && lh55G >= 5.6 && lh55G <= 6.4 &&
          (lbl - lrl > 100 && lbr - lrr < 25) && (lbt - lrt < 25 && lbbo - lrbo < 25) &&
          lHair < lLow - 15 && lRim > lHair && lSh < lLow && lDome < lLow;

        const smag = run2D(160, 100, 3, (x, y) => (y >= 47 && y <= 53 && x >= 10 && x <= 150) ? [0, 136, 255] : [28, 28, 30],
          { x: 18, y: 26, w: 124, h: 48, r: 24, blurSlot: 0, rim: 1, bezel: 8, refract: 3, chroma: 0 });
        let cmag = 0;
        for (let y = 78; y < 222; y++) { const [r, , b] = smag.at(57, y); if (b - r > 100) cmag++; }
        const hMag = cmag / 3;

        const szoom = run2D(160, 100, 3, (x, y) => (y >= 31.8 && y <= 32.8 && x >= 40 && x <= 120) ? [48, 209, 88] : [28, 28, 30],
          { x: 40, y: 30, w: 80, h: 40, r: 20, blurSlot: -0.18, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const zEdgeGR = szoom.at(240, 90)[1] - szoom.at(240, 90)[0];
        const zMidGR = szoom.at(240, 150)[1] - szoom.at(240, 150)[0];

        const v8Flags = {
          peaks: bestB >= 9.5 && bestB <= 12.5 && blueDx >= 0.33 && blueDx <= 1.50 && bestG >= 9.5 && bestG <= 12.5 && greyDx >= 0.33 && greyDx <= 1.50,
          h55: h55B >= 5.6 && h55B <= 6.4 && h55G >= 5.6 && h55G <= 6.4,
          split: bl - rl > 100 && br - rr < 25,
          zeroBlue: bt - rt < 25 && bbo - rbo < 25,
          dark3D: yDome < yLow && yRim > yDome && yRim > ySh && yHair < yRim,
          mono: monoOk,
          lightFull: lightOk,
          lightDiag: { lBestB: Number(lBestB.toFixed(2)), lDxB: Number(lDxB.toFixed(3)), lBestG: Number(lBestG.toFixed(2)), lDxG: Number(lDxG.toFixed(3)), lh55B: Number(lh55B.toFixed(2)), lh55G: Number(lh55G.toFixed(2)), l5: [lHair, lRim, lSh, lDome, lLow] },
          role0: hMag < 7.0 && zEdgeGR > 40 && zMidGR < 15,
        };
        const v8Ok = v8Flags.peaks && v8Flags.h55 && v8Flags.split && v8Flags.zeroBlue && v8Flags.dark3D && v8Flags.mono && v8Flags.lightFull && v8Flags.role0;
        pushVec('V8 slider 2D ROI (mirror/light-track/5pt/role0)', v8Ok,
          [Number(bestB.toFixed(2)), Number(blueDx.toFixed(3)), Number(bestG.toFixed(2)), Number(greyDx.toFixed(3)), Number(h55B.toFixed(2)), Number(h55G.toFixed(2)), Number(hMag.toFixed(2))],
          v8Ok ? 'peaks [10,12]@dx[0.67..1.33], h55 [5.6,6.4], lightFull, hMag<7' : JSON.stringify(v8Flags));
      }

      // V9: Switch ON 3x 3-row TIR + 2x phase-shift + Switch OFF right cap + Light Segmented Prism + half-press p=0.5/0.6/1.0
      {
        const sw = run2D(160, 100, 3, (x, y) => {
          const dx = Math.max(0, Math.abs(x - 80) - 17.5);
          const dy = Math.abs(y - 49.2);
          return (dx * dx + dy * dy <= 14 * 14) ? [48, 209, 88] : [28, 28, 30];
        }, { x: 61.4, y: 30.0, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const gr0 = sw.at(273, 90)[1] - sw.at(273, 90)[0];
        const gr1 = sw.at(273, 91)[1] - sw.at(273, 91)[0];
        const gr2 = sw.at(273, 92)[1] - sw.at(273, 92)[0];
        let cTop = -1, cBot = -1;
        for (let y = 96; y < 199; y++) {
          if (sw.at(273, y)[1] - sw.at(273, y)[0] > 60) { if (cTop < 0) cTop = y; cBot = y; }
        }
        const coreH = (cBot - cTop + 1) / 3, moat = (cTop - 92) / 3;
        const grTip = sw.at(360, 148)[1] - sw.at(360, 148)[0];

        const swHalfH = (p) => {
          const swh = run2D(160, 100, 3, null,
            { x: 61.4, y: 30.0, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: p, bezel: 9 * p, refract: 9 * p, chroma: 0 });
          let t = -1, b = -1;
          for (let y = 96; y < 199; y++) {
            if (swh.at(273, y)[1] - swh.at(273, y)[0] > 60) { if (t < 0) t = y; b = y; }
          }
          return (b - t + 1) / 3;
        };
        const h50 = swHalfH(0.5), h60 = swHalfH(0.6), h100 = coreH;
        const halfOk = h50 < 27.0 && h100 < h60 && h60 <= h50;

        let p2Ok = true;
        for (const yOff of [30.0, 30.25]) {
          const sw2 = run2D(160, 100, 2, (x, y) => {
            const dx = Math.max(0, Math.abs(x - 80) - 17.5);
            const dy = Math.abs(y - (yOff + 19.2));
            return (dx * dx + dy * dy <= 14 * 14) ? [48, 209, 88] : [28, 28, 30];
          }, { x: 61.4, y: yOff, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0 });
          let mass = 0;
          for (let y = 60; y <= 64; y++) { const [r, g] = sw2.at(182, y); if (g > r) mass += g - r; }
          if (mass <= 40) p2Ok = false;
        }

        const swOff = run2D(160, 100, 3, (x, y) => {
          const dx = Math.max(0, Math.abs(x - 80) - 17.5);
          const dy = Math.abs(y - 49.2);
          return (dx * dx + dy * dy <= 14 * 14) ? [90, 90, 96] : [28, 28, 30];
        }, { x: 39.4, y: 30.0, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const yCard = swOff.lumaAt(10, 10);
        const offOk = swOff.lumaAt(207, 148) > yCard + 15 && swOff.lumaAt(207, 92) > swOff.lumaAt(207, 102) &&
          swOff.lumaAt(291, 148) > yCard + 10 && swOff.lumaAt(288, 148) > yCard + 10 && swOff.lumaAt(285, 148) > yCard + 10;

        const seg = run2D(160, 100, 3, () => [255, 255, 255],
          { x: 25, y: 27, w: 110, h: 45, r: 22.5, blurSlot: -48, rim: 1, bezel: 9, refract: 9, chroma: 0.04 });
        const cyan = seg.at(240, 213)[2] - seg.at(240, 213)[0];
        const cyanTop = seg.at(240, 84)[2] - seg.at(240, 84)[0];
        const amber = seg.at(78, 148)[0] - seg.at(78, 148)[2];

        const segTxtBg = (x, y) => (x >= 72 && x <= 88 && y >= 45 && y <= 54) ? [0, 0, 0] : [255, 255, 255];
        const segTxtC1 = run2D(160, 100, 3, segTxtBg,
          { x: 25, y: 27, w: 110, h: 45, r: 22.5, blurSlot: -48, rim: 1, bezel: 9, refract: 9, chroma: 0.04 });
        const segTxtC0 = run2D(160, 100, 3, null,
          { x: 25, y: 27, w: 110, h: 45, r: 22.5, blurSlot: -48, rim: 1, bezel: 9, refract: 9, chroma: 0.0 });
        let maxTxtDiff = 0;
        for (let ty = 132; ty <= 165; ty++) {
          for (let tx = 210; tx <= 270; tx++) {
            const c1 = segTxtC1.at(tx, ty), c0 = segTxtC0.at(tx, ty);
            const d = Math.max(Math.abs(c1[0] - c0[0]), Math.abs(c1[1] - c0[1]), Math.abs(c1[2] - c0[2]));
            if (d > maxTxtDiff) maxTxtDiff = d;
          }
        }

        const tabBgEmpty = (x, y) => {
          const dx = Math.max(0, Math.abs(x - 80) - 48);
          const dy = Math.abs(y - 50);
          return (dx * dx + dy * dy <= 22 * 22) ? [42, 42, 48] : [16, 16, 20];
        };
        const tabBgIcon = (x, y) => {
          if (x >= 109.0 && x <= 111.0 && y >= 28.0 && y <= 72.0) return [255, 149, 0];
          return tabBgEmpty(x, y);
        };
        const tabBgLip = (x, y) => {
          if (x >= 115.2 && x <= 116.2 && y >= 45.0 && y <= 55.0) return [255, 149, 0];
          return tabBgEmpty(x, y);
        };
        const tabEmpty = run2D(160, 100, 3, tabBgEmpty,
          { x: 30, y: 25, w: 88, h: 50, r: 25, blurSlot: -64, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const tabIcon = run2D(160, 100, 3, tabBgIcon,
          { x: 30, y: 25, w: 88, h: 50, r: 25, blurSlot: -64, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const tabLip = run2D(160, 100, 3, tabBgLip,
          { x: 30, y: 25, w: 88, h: 50, r: 25, blurSlot: -64, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const c1 = tabIcon.at(349, 150), c0 = tabEmpty.at(349, 150), c2 = tabLip.at(349, 150);
        const ghostDelta = Math.abs(c1[0] - c0[0]) + Math.abs(c1[1] - c0[1]) + Math.abs(c1[2] - c0[2]);
        const lipDelta = Math.abs(c2[0] - c0[0]) + Math.abs(c2[1] - c0[1]) + Math.abs(c2[2] - c0[2]);
        const g1x = tabIcon.at(349, 87), g0x = tabEmpty.at(349, 87);
        const gate0SameX = Math.abs(g1x[0] - g0x[0]) + Math.abs(g1x[1] - g0x[1]) + Math.abs(g1x[2] - g0x[2]);
        const g1c = tabIcon.at(311, 87), g0c = tabEmpty.at(311, 87);
        const gate0InCap = Math.abs(g1c[0] - g0c[0]) + Math.abs(g1c[1] - g0c[1]) + Math.abs(g1c[2] - g0c[2]);

        // Role 4 strictly monotonic: empty vs bar A/B luma diff across the outer edge (d=0..4pt, px_x=96..108)
        const tabNgGlass = { x: 32, y: 18, w: 95, h: 63.5, r: 31.75, blurSlot: -64, rim: 1, bezel: 9, refract: 9, chroma: 0 };
        const tabNg0 = run2D(160, 100, 3, () => [246, 244, 240], tabNgGlass);
        const ngEmptyL = [];
        for (let gx = 96; gx <= 108; gx++) ngEmptyL.push(tabNg0.lumaAt(gx, 149));
        const tabNg = run2D(160, 100, 3, (x, y) => (x >= 37.0 && x <= 38.5 && y >= 18.0 && y <= 81.5) ? [90, 90, 90] : [246, 244, 240], tabNgGlass);
        const ngBarL = tabNg.lumaAt(114, 149);
        let ngGhost = 0, ngCleanL = 255;
        for (let gx = 96; gx <= 108; gx++) {
          const lb = tabNg.lumaAt(gx, 149);
          ngGhost = Math.max(ngGhost, Math.abs(lb - ngEmptyL[gx - 96]));
          if (gx >= 99) ngCleanL = Math.min(ngCleanL, lb);
        }
        const ngOk = ngBarL < 130 && ngGhost <= 2 && ngCleanL > 220;

        // Role 4 dark clarity: 62pt droplet over a dark 50pt bar (6pt overhang stays clear, no dead-black side ring)
        const tabDk = run2D(160, 100, 3, (x, y) => {
          const dx = Math.max(0, Math.abs(x - 80) - 45);
          const dy = Math.abs(y - 50);
          return (dx * dx + dy * dy <= 25 * 25) ? [55, 57, 60] : [25, 27, 30];
        }, { x: 35, y: 19, w: 90, h: 62, r: 31, blurSlot: -64, rim: 1, bezel: 9, refract: 9, chroma: 0 });
        const dkTopL = tabDk.lumaAt(240, 66), dkBotL = tabDk.lumaAt(240, 234);
        const dkLeftL = tabDk.lumaAt(106, 150), dkRightL = tabDk.lumaAt(373, 150);
        const dkOk = dkTopL < 55 && dkBotL < 55 && dkLeftL >= 42 && dkRightL >= 42;

        const segScaleBg = (x, y) => {
          if (x >= 78 && x <= 82 && y >= 43.5 && y <= 55.5) return [0, 0, 0];
          const dx = Math.max(0, Math.abs(x - 80) - 48);
          const dy = Math.abs(y - 49.5);
          return (dx * dx + dy * dy <= 17 * 17) ? [195, 195, 198] : [255, 255, 255];
        };
        const segScale = run2D(160, 100, 3, segScaleBg,
          { x: 25, y: 27, w: 110, h: 45, r: 22.5, blurSlot: -48, rim: 1, bezel: 9, refract: 9, chroma: 0.04 });
        let segStrokeSpan = 0;
        for (let sy = 110; sy <= 190; sy++) {
          if (segScale.lumaAt(240, sy) < 120) segStrokeSpan++;
        }
        const segRingL = segScale.lumaAt(240, 105);
        const segTrackL = segScale.lumaAt(240, 116);
        const segScaleOk = segStrokeSpan >= 35 && segStrokeSpan <= 37 && segRingL >= 225 && segTrackL <= 210 && (segRingL - segTrackL) >= 20;

        const v9Ok = gr0 < 15 && gr1 > 60 && gr2 < 15 && coreH >= 20 && coreH <= 22.5 && moat >= 6.5 &&
          grTip < 15 && p2Ok && offOk && cyan >= 10 && cyanTop >= 10 && amber >= 20 && maxTxtDiff === 0 && ghostDelta === 0 && lipDelta >= 80 && gate0SameX === 0 && gate0InCap === 0 && ngOk && dkOk && halfOk && segScaleOk;
        pushVec('V9 switch ON/OFF/half, light segmented & role4 tab 2D ROI', v9Ok,
          [gr0, gr1, gr2, Number(coreH.toFixed(2)), cyan, cyanTop, amber, maxTxtDiff, ghostDelta, lipDelta, gate0SameX, gate0InCap, ngBarL, Number(ngGhost.toFixed(1)), Number(ngCleanL.toFixed(1)), Number(dkTopL.toFixed(1)), Number(dkBotL.toFixed(1)), Number(dkLeftL.toFixed(1)), Number(dkRightL.toFixed(1)), Number(h50.toFixed(2)), segStrokeSpan, segRingL, segTrackL],
          'rows<15/>60/<15, core [20,22.5], moat>=6.5, cyan/top>=10, amber>=20, txtDiff=0, ghostDelta=0, lipDelta>=80, gate0=0, ngBar<130, ngGhost<=2, ngClean>220, dkTop/Bot<55, dkSide>=42, segSpan[35..37], ring>=225/track<=210');
      }

      // V10: Regular Glass Light AA & Dark Rim Gate (@2x, R=24pt)
      {
        gl.uniform2f(u('u_glight'), 1.0, 0.0);
        const lumaF = (res, x, y) => {
          const [r, g, b] = res.at(x, y);
          return 0.2126 * r + 0.7152 * g + 0.0722 * b;
        };
        const regL = run2D(160, 120, 2, () => [242, 240, 236], {
          x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 2.1,
          col: [253 / 255, 252 / 255, 253 / 255, 103 / 255],
          rim: 0.6, bezel: 14, refract: 18, chroma: 0.06,
        });
        const masses = [];
        const bgL = 0.2126 * 242 + 0.7152 * 240 + 0.0722 * 236;
        for (let py = 44; py <= 84; py++) {
          let mass = 0;
          for (let px = 200; px <= 284; px++) {
            const l = lumaF(regL, px, py);
            if (l < bgL) mass += bgL - l;
          }
          masses.push(mass / 2.0);
        }
        let maxD2 = 0;
        for (let k = 1; k < masses.length - 1; k++) {
          const d2 = Math.abs(masses[k - 1] - 2 * masses[k] + masses[k + 1]);
          if (d2 > maxD2) maxD2 = d2;
        }
        let sideMin = 1e9;
        for (let sx = 270; sx <= 284; sx++) {
          const l = lumaF(regL, sx, 120);
          if (l < sideMin) sideMin = l;
        }
        const lightOuterTop = lumaF(regL, 160, 39);
        const lightTop = lumaF(regL, 160, 40);
        const lightMid = lumaF(regL, 160, 120);
        const regD = run2D(160, 120, 2, () => [28, 28, 30], {
          x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 2.1,
          col: [32 / 255, 32 / 255, 32 / 255, 103 / 255],
          rim: 0.6, bezel: 14, refract: 18, chroma: 0.06,
        });
        const ydMid = lumaF(regD, 160, 120), ydTop = lumaF(regD, 160, 40);
        let h1 = 2166136261 >>> 0, h2 = 5381 >>> 0;
        for (let ry = 38; ry <= 122; ry++) {
          for (let rx = 160; rx <= 283; rx++) {
            const px4 = regD.rgbaAt(rx, ry);
            for (let ch = 0; ch < 4; ch++) {
              h1 = Math.imul(h1 ^ px4[ch], 16777619) >>> 0;
              h2 = (Math.imul(h2, 33) ^ px4[ch]) >>> 0;
            }
          }
        }
        const v10DarkHashOk = (h1 === 2429946954 && h2 === 3477172240);
        const v10Ok = maxD2 <= 14.0 && sideMin >= 150.0 && sideMin <= 176.0 &&
          lightOuterTop <= lightMid - 15.0 && lightTop >= lightMid && ydTop > ydMid + 5.0 && v10DarkHashOk;
        pushVec('V10 regular glass Light AA & Dark ROI RGBA8 exact (@2x, R=24)', v10Ok,
          [Number(maxD2.toFixed(2)), Number(sideMin.toFixed(1)), Number(lightOuterTop.toFixed(1)), Number(lightTop.toFixed(1)), Number(lightMid.toFixed(1)), Number(ydTop.toFixed(1)), Number(ydMid.toFixed(1)), h1, h2],
          'lightD2<=14, sideMin [150,176], outerTop<=mid-15, top>=mid, darkTop>darkMid+5, darkROI=(2429946954,3477172240) max_abs_diff==0');
      }

      // V11: 3D Slim Bevel Wall & Curved-Cap AA Gate (V11-AA, V11-Wall, V11-Top)
      {
        const lumaF = (res, x, y) => {
          const [r, g, b] = res.at(x, y);
          return 0.2126 * r + 0.7152 * g + 0.0722 * b;
        };
        const arcD2 = (res, x0, x1, y0, y1) => {
          const mins = [];
          for (let py = y0; py <= y1; py++) {
            let rm = 1e9;
            for (let px = x0; px <= x1; px++) {
              const l = lumaF(res, px, py);
              if (l < rm) rm = l;
            }
            mins.push(rm);
          }
          let md = 0;
          for (let k = 1; k < mins.length - 1; k++) {
            const d2 = Math.abs(mins[k - 1] - 2 * mins[k] + mins[k + 1]);
            if (d2 > md) md = d2;
          }
          return md;
        };
        let v11Ok = true;
        const v11Summary = {};
        for (const dark of [true, false]) {
          const bg = dark ? [28, 28, 30] : [255, 255, 255];
          const tr = dark ? [48, 209, 88] : [52, 199, 89];
          const swBg = (x, y) => {
            const dx = Math.max(0, Math.abs(x - 50) - 17.5);
            const dy = Math.abs(y - 32);
            return (dx * dx + dy * dy <= 14 * 14) ? tr : bg;
          };
          const sw3 = run2D(120, 65, 3, swBg, {
            x: 31.58, y: 12.8, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0,
          });
          // Verify run2D blitted the real green track outside the left cap (not flat bg0)
          const [outR, outG] = sw3.at(88, 96);
          const bgBlitOk = (outG - outR) > 100;
          const d2L3 = arcD2(sw3, 89, 109, 66, 125);
          const d2R3 = arcD2(sw3, 259, 281, 66, 125);
          const lim3 = dark ? 10.0 : 12.0;
          let rDeepX = 272, rDeepL = 1e9;
          for (let sx = 266; sx <= 274; sx++) {
            const l = lumaF(sw3, sx, 96);
            if (l < rDeepL) { rDeepL = l; rDeepX = sx; }
          }
          const [rrD, rgD] = sw3.at(rDeepX, 96);
          const l1 = lumaF(sw3, rDeepX - 1, 96), l2 = lumaF(sw3, rDeepX - 2, 96), l3 = lumaF(sw3, rDeepX - 3, 96), l4 = lumaF(sw3, rDeepX - 4, 96);
          const wallOk = dark
            ? (rDeepL <= 14.0 && (rgD - rrD) <= 12 && l1 >= rDeepL && l2 >= l1 && l3 >= l2)
            : (rDeepL >= 170.0 && rDeepL <= 205.0 && l1 >= rDeepL && l2 >= l1 && l3 >= l2 && l4 >= l3 && l4 >= 230.0);
          let emeraldOk = false;
          for (let lx = 93; lx <= 99; lx++) {
            const [lr, lg] = sw3.at(lx, 96);
            if (lg - lr >= 90 && lg >= 125 && lg <= 165) emeraldOk = true;
          }
          let hornOk = true;
          let prevHorn = 0.0;
          for (const dx of [6, 8, 10, 12, 14, 16, 18, 20]) {
            const pc = Math.round((31.58 + dx) * 3.0);
            const rem = Math.max(0.0, 19.2 - dx);
            const dyc = Math.sqrt(19.2 * 19.2 - rem * rem);
            const y0 = Math.round((32.0 - dyc + 1.0) * 3.0);
            let best = 0, cur = 0;
            for (let py = y0; py < 96; py++) {
              const [hr, hg] = sw3.at(pc, py);
              if (hg - hr < 26) { cur++; if (cur > best) best = cur; } else cur = 0;
            }
            const moat = best / 3.0;
            if (dx === 6 || dx === 8) { if (moat !== 0.0) hornOk = false; }
            else if (dx === 10) { if (moat < 1.5 || moat > 4.5) hornOk = false; }
            else { if (moat < prevHorn - 0.35 || moat > prevHorn + 3.5) hornOk = false; if (dx === 20 && moat < 7.0) hornOk = false; }
            prevHorn = moat;
          }
          let centerOk = true;
          if (dark) {
            const swG = run2D(120, 65, 3, () => [48, 209, 88], {
              x: 31.58, y: 12.8, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0,
            });
            const [cr, cg, cb] = swG.at(184, 96);
            centerOk = Math.abs(cr - 57) <= 2 && Math.abs(cg - 218) <= 2 && Math.abs(cb - 97) <= 2;
            v11Summary.centerGreen = [cr, cg, cb];
          }
          const sw2 = run2D(120, 65, 2, swBg, {
            x: 31.58, y: 12.8, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0,
          });
          const d2L2 = arcD2(sw2, 58, 74, 44, 83);
          const d2R2 = arcD2(sw2, 170, 188, 44, 83);
          const lim2 = dark ? 12.0 : 14.0;
          const tag = dark ? 'dark' : 'light';
          v11Summary[tag] = {
            d2_3x: [Number(d2L3.toFixed(2)), Number(d2R3.toFixed(2))],
            d2_2x: [Number(d2L2.toFixed(2)), Number(d2R2.toFixed(2))],
            rDeepL: Number(rDeepL.toFixed(1)),
            bgBlitOk, wallOk, emeraldOk, hornOk, centerOk,
          };
          if (!(bgBlitOk && d2L3 <= lim3 && d2R3 <= lim3 && d2L2 <= lim2 && d2R2 <= lim2 && wallOk && emeraldOk && hornOk && centerOk)) v11Ok = false;

          for (const off of [5.00, 5.20]) {
            const th = 38.4 - 2.0 * off;
            const trR = th * 0.5;
            const topBg = (x, y) => {
              const dx = Math.max(0, Math.abs(x - 50) - (31.5 - trR));
              const dy = Math.abs(y - (21.0 + 19.2));
              return (dx * dx + dy * dy <= trR * trR) ? tr : bg;
            };
            const t3 = run2D(120, 70, 3, topBg, {
              x: 31.4, y: 21.0, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0,
            });
            const gr0 = t3.at(183, 63)[1] - t3.at(183, 63)[0];
            const gr1 = t3.at(183, 64)[1] - t3.at(183, 64)[0];
            const gr2 = t3.at(183, 65)[1] - t3.at(183, 65)[0];
            const l0 = lumaF(t3, 183, 63), l2t = lumaF(t3, 183, 65);
            const top3Ok = dark
              ? (gr0 < 15 && l0 <= 35 && gr1 >= 100 && gr2 < 15 && l2t >= 75)
              : (gr0 < 15 && l0 >= 180 && l0 <= 215 && gr1 >= 95 && gr2 < 15 && l2t >= 245);
            const t2 = run2D(120, 70, 2, topBg, {
              x: 31.4, y: 21.0, w: 59.2, h: 38.4, r: 19.2, blurSlot: -32, rim: 1, bezel: 9, refract: 9, chroma: 0,
            });
            const g21 = t2.at(122, 43)[1] - t2.at(122, 43)[0];
            const g22 = t2.at(122, 44)[1] - t2.at(122, 44)[0];
            const top2Ok = g21 >= 70 && g22 < 15;
            if (!(top3Ok && top2Ok)) {
              v11Ok = false;
              v11Summary[tag + '_top_' + off] = { gr0, l0: Number(l0.toFixed(1)), gr1, gr2, l2t: Number(l2t.toFixed(1)), g21, g22 };
            }
          }
        }
        // V11-Frost-Continuity: Switch ON continuous frost sweep (0.0 -> 0.0001 -> 0.00011 -> 0.01 -> 0.05 -> 0.10 -> 0.13 -> 0.30)
        {
          const swDarkBg = (x, y) => {
            const dx = Math.max(0, Math.abs(x - 50) - 17.5);
            const dy = Math.abs(y - 32);
            return (dx * dx + dy * dy <= 14 * 14) ? [48, 209, 88] : [28, 28, 30];
          };
          const drawFrost = (f, initBg) => {
            const active = f > 0.0001;
            const aFrac = active ? 0.94 * f : 0.0;
            const blurSlot = aFrac <= 0.12 ? -32.0 : Math.max(0.01, 14.0 * ((aFrac - 0.12) / 0.88) * 0.5);
            const refr = active ? 9.0 * (1.0 - 0.55 * f) : 9.0;
            const trgb = active ? 1.0 + (240.0 / 255.0 - 1.0) * (0.5 * f) : 1.0;
            const ta = Math.round(aFrac * 255.0) / 255.0;
            return run2D(120, 65, 3, initBg ? swDarkBg : null, {
              x: 31.58, y: 12.8, w: 59.2, h: 38.4, r: 19.2, blurSlot,
              col: [trgb, trgb, trgb, ta], rim: 1, bezel: 9, refract: refr, chroma: 0,
            });
          };
          const roiDiff = (a, b) => {
            let md = 0;
            for (let py = 38; py <= 154; py++) {
              for (let px = 95; px <= 272; px++) {
                const c1 = a.at(px, py), c0 = b.at(px, py);
                md = Math.max(md, Math.abs(c1[0] - c0[0]), Math.abs(c1[1] - c0[1]), Math.abs(c1[2] - c0[2]));
              }
            }
            return md;
          };
          const meanL = (res, x0, x1, y0, y1) => {
            let s = 0, n = 0;
            for (let py = y0; py <= y1; py++)
              for (let px = x0; px <= x1; px++) { s += lumaF(res, px, py); n++; }
            return s / n;
          };
          const b0 = drawFrost(0.0, true);
          drawFrost(0.0001, false);
          const b00011 = drawFrost(0.00011, false);
          drawFrost(0.01, false);
          drawFrost(0.05, false);
          const b10 = drawFrost(0.10, false);
          const b127 = drawFrost(0.127, false);
          const b128 = drawFrost(0.128, false);
          const b13 = drawFrost(0.13, false);
          const b30 = drawFrost(0.30, false);
          const dEps = roiDiff(b0, b00011);
          const dBlur = roiDiff(b127, b128);
          const dCross = roiDiff(b10, b13);
          const ml10 = meanL(b10, 95, 272, 38, 154), ml13 = meanL(b13, 95, 272, 38, 154);
          const ml0Roi = meanL(b0, 95, 272, 38, 154), ml30Roi = meanL(b30, 95, 272, 38, 154);
          const frostOk = dEps <= 2 && dBlur <= 8 && dCross <= 90 && Math.abs(ml13 - ml10) < 15.0 && ml30Roi > ml0Roi + 15.0;
          v11Summary.frost = { dEps, dBlur, dCross, dMeanL: Number(Math.abs(ml13 - ml10).toFixed(1)), ml0Roi: Number(ml0Roi.toFixed(1)), ml30Roi: Number(ml30Roi.toFixed(1)), frostOk };
          if (!frostOk) v11Ok = false;
        }
        pushVec('V11 3D slim bevel wall, curved-cap AA & frost continuity (V11-AA/Wall/Top/Frost)', v11Ok,
          [
            v11Summary.dark.d2_3x[0], v11Summary.dark.d2_3x[1],
            v11Summary.light.d2_3x[0], v11Summary.light.d2_3x[1],
            v11Summary.dark.d2_2x[0], v11Summary.dark.d2_2x[1],
            v11Summary.light.d2_2x[0], v11Summary.light.d2_2x[1],
            v11Summary.dark.rDeepL, v11Summary.light.rDeepL,
            v11Summary.frost.dEps, v11Summary.frost.dBlur, v11Summary.frost.dCross,
          ],
          v11Ok ? '3x<=10/12, 2x<=12/14, wall+emerald, top 5.00/5.20pt, frost dEps<=2 dBlur<=8 dCross<=90' : JSON.stringify(v11Summary));
      }

      // V12-Unified-Overlay: Light 0xFDFCFD80 bright-bg chroma + top rim, 0xFF uniformity, gray monotonicity, ctrl 0-diff
      {
        const lumaF = (res, x, y) => {
          const [r, g, b] = res.at(x, y);
          return 0.2126 * r + 0.7152 * g + 0.0722 * b;
        };
        const bubbleBg = () => [211, 232, 253];
        const bf = run2D(160, 120, 2, bubbleBg, {
          x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 14.0,
          col: [253 / 255, 252 / 255, 253 / 255, 128 / 255], rim: 0.6, bezel: 20, refract: 16, chroma: 0.0,
        });
        const [cr, , cb] = bf.at(160, 88);
        const centerChroma = cb - cr;
        const topL = lumaF(bf, 160, 40);
        const centerL = lumaF(bf, 160, 88);
        const ff = run2D(160, 120, 2, null, {
          x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 14.0,
          col: [253 / 255, 252 / 255, 253 / 255, 1.0], rim: 0.6, bezel: 20, refract: 16, chroma: 0.0,
        });
        const dLff = Math.abs(lumaF(ff, 160, 40) - lumaF(ff, 160, 88));
        const g60 = run2D(160, 120, 2, () => [153, 153, 153], {
          x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 14.0,
          col: [253 / 255, 252 / 255, 253 / 255, 128 / 255], rim: 0.6, bezel: 20, refract: 16, chroma: 0.0,
        });
        const g72 = run2D(160, 120, 2, () => [184, 184, 184], {
          x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 14.0,
          col: [253 / 255, 252 / 255, 253 / 255, 128 / 255], rim: 0.6, bezel: 20, refract: 16, chroma: 0.0,
        });
        const lG60 = lumaF(g60, 160, 88);
        const lG72 = lumaF(g72, 160, 88);
        const satBlue = () => [60, 120, 210];
        const ctrlMaxDiff = (col, bgFn) => {
          const r6 = run2D(160, 120, 2, bgFn, { x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 1.2, col, rim: 0.6, bezel: 14, refract: 20, chroma: 0.06 });
          const r0 = run2D(160, 120, 2, null, { x: 20, y: 20, w: 120, h: 80, r: 24, blurSlot: 1.2, col, rim: 0.6, bezel: 14, refract: 20, chroma: 0.0 });
          let md = 0;
          for (let py = 40; py <= 200; py++) {
            for (let px = 40; px <= 280; px++) {
              const p6 = r6.at(px, py), p0 = r0.at(px, py);
              for (let c = 0; c < 3; c++) md = Math.max(md, Math.abs(p6[c] - p0[c]));
            }
          }
          return md;
        };
        const dL85 = ctrlMaxDiff([253 / 255, 252 / 255, 253 / 255, 133 / 255], bubbleBg);
        const dLAB = ctrlMaxDiff([253 / 255, 252 / 255, 253 / 255, 171 / 255], bubbleBg);
        const dDAB = ctrlMaxDiff([32 / 255, 32 / 255, 32 / 255, 171 / 255], satBlue);
        const v12Ok = centerChroma >= 12 && (topL - centerL) >= 4.0 && dLff <= 3.0 && lG72 >= lG60 + 2.0 && dL85 === 0 && dLAB === 0 && dDAB === 0;
        pushVec('V12-Unified-Overlay (0x80 chroma/rim/mono, 0xFF, ctrl 0-diff)', v12Ok,
          [centerChroma, Number((topL - centerL).toFixed(1)), Number(dLff.toFixed(2)), Number(lG60.toFixed(1)), Number(lG72.toFixed(1)), dL85, dLAB, dDAB],
          '0x80 B-R>=12 dRim>=4 L72>=L60+2, 0xFF |dL|<=3, ctrlDiff=(0,0,0)');
      }

      // V13: Light adaptive glass opacity (AD_FS + FS(ONE_KIND=13) + FS(ONE_KIND=10)):
      // half-dark wide capsule at 0x67 inverts left ink bright; raising tint_a to 0.522/0.752/0.913 on the same slot
      // keeps left ink dark on bright frosted glass; returning to 0x67 restores bright inversion;
      // full-dark at 0.752 and 0x67->0.913 snap stays light frosted glass + dark ink.
      {
        const adItem = programs.find((p) => p.name === 'AD_FS');
        const inkItem = programs.find((p) => p.name === 'FS(ONE_KIND=10)');
        const mkProg = (item) => {
          const vsS = gl.createShader(gl.VERTEX_SHADER);
          gl.shaderSource(vsS, item.vs);
          gl.compileShader(vsS);
          const fsS = gl.createShader(gl.FRAGMENT_SHADER);
          gl.shaderSource(fsS, item.fs);
          gl.compileShader(fsS);
          const p = gl.createProgram();
          gl.attachShader(p, vsS);
          gl.attachShader(p, fsS);
          gl.linkProgram(p);
          gl.deleteShader(vsS);
          gl.deleteShader(fsS);
          return p;
        };
        const adP = mkProg(adItem);
        const inkP = mkProg(inkItem);
        const adT = [gl.createTexture(), gl.createTexture()];
        const adF = [gl.createFramebuffer(), gl.createFramebuffer()];
        for (let i = 0; i < 2; i++) {
          gl.bindTexture(gl.TEXTURE_2D, adT[i]);
          gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 16, 1);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
          gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
          gl.bindFramebuffer(gl.FRAMEBUFFER, adF[i]);
          gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, adT[i], 0);
          gl.clearColor(0, 0, 0, 0);
          gl.clear(gl.COLOR_BUFFER_BIT);
        }
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        let adC = 0;

        const W3 = 800, H3 = 600, S3 = 2.0, wPt = 400.0, hPt = 300.0;
        canvas.width = W3;
        canvas.height = H3;

        const runAdCase = (darkW, tintA, isNew) => {
          // Upload backdrop texture (top-down matching u_map = (1/wPt, 1/hPt, 0, 0))
          const bgBuf = new Uint8Array(W3 * H3 * 4);
          const darkPx = Math.round(darkW * S3);
          for (let py = 0; py < H3; py++) {
            for (let px = 0; px < W3; px++) {
              const i = (py * W3 + px) * 4;
              if (px < darkPx) { bgBuf[i] = 20; bgBuf[i + 1] = 20; bgBuf[i + 2] = 24; bgBuf[i + 3] = 255; }
              else { bgBuf[i] = 255; bgBuf[i + 1] = 255; bgBuf[i + 2] = 255; bgBuf[i + 3] = 255; }
            }
          }
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, bdTex);
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, W3, H3, 0, gl.RGBA, gl.UNSIGNED_BYTE, bgBuf);

          // Step 1: Run AD_FS into adF[nxt] slot 0 for capsule center=(200,150) half=(160,22)
          const cur = adC, nxt = 1 - cur;
          gl.disable(gl.BLEND);
          gl.bindFramebuffer(gl.FRAMEBUFFER, adF[nxt]);
          gl.viewport(0, 0, 16, 1);
          gl.useProgram(adP);
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, bdTex);
          gl.uniform1i(gl.getUniformLocation(adP, 'u_bd'), 0);
          gl.activeTexture(gl.TEXTURE1);
          gl.bindTexture(gl.TEXTURE_2D, adT[cur]);
          gl.uniform1i(gl.getUniformLocation(adP, 'u_prev'), 1);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_map'), 1.0 / wPt, 1.0 / hPt, 0.0, 0.0);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_uvc'), 0.5 / W3, 0.5 / H3, 1.0 - 0.5 / W3, 1.0 - 0.5 / H3);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_box'), 200.0, 150.0, 160.0, 22.0);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_param'), -tintA, isNew ? 1.0 : 0.0, 1.0 / 60.0, 0.0);
          gl.drawArrays(gl.TRIANGLES, 0, 3);
          const adPx = new Uint8Array(4);
          gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, adPx);
          const adTVal = (adPx[0] * 256 + adPx[1]) / 65535.0;
          adC = nxt;

          // Step 2: Blit backdrop to default framebuffer
          gl.bindFramebuffer(gl.FRAMEBUFFER, null);
          gl.viewport(0, 0, W3, H3);
          gl.useProgram(bgProg);
          gl.disable(gl.BLEND);
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, bdTex);
          gl.uniform1i(bgTexLoc, 0);
          gl.drawArrays(gl.TRIANGLES, 0, 6);

          // Step 3: Draw light adaptive glass capsule (kind 6)
          gl.useProgram(prog);
          gl.enable(gl.BLEND);
          gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, bdTex);
          gl.activeTexture(gl.TEXTURE4);
          gl.bindTexture(gl.TEXTURE_2D, adT[adC]);
          gl.uniform1i(u('u_bd'), 0);
          gl.uniform1i(u('u_ad'), 4);
          gl.uniform1i(u('u_ad_n'), 1);
          gl.uniform4f(u('u_ad_box[0]'), 200.0, 150.0, 160.0, 22.0);
          gl.uniform2f(u('u_vp'), wPt, hPt);
          gl.uniform2f(u('u_origin'), 0.0, 0.0);
          gl.uniform1f(u('u_scale'), S3);
          gl.uniform1f(u('u_top'), H3);
          gl.uniform4f(u('u_map'), 1.0 / wPt, 1.0 / hPt, 0.0, 0.0);
          gl.uniform4f(u('u_uvc'), 0.5 / W3, 0.5 / H3, 1.0 - 0.5 / W3, 1.0 - 0.5 / H3);
          gl.vertexAttrib4f(0, 40.0, 128.0, 320.0, 44.0);
          gl.vertexAttrib4f(1, 22.0, 6.0, 1.0, 2.0);
          gl.vertexAttrib4f(2, 253 / 255, 252 / 255, 253 / 255, tintA);
          gl.vertexAttrib4f(3, 0.0, 14.0, -1.0, 1.0);
          gl.vertexAttrib4f(4, 0.0, 0.0, 1e9, 1e9);
          gl.vertexAttrib4f(5, -1.0, 0.0, 0.0, 0.0);
          gl.vertexAttrib4f(9, 22.0, 22.0, 22.0, 22.0);
          gl.drawArrays(gl.TRIANGLES, 0, 6);

          // Step 4: Draw left dark adaptive stroke (kind 10) at (60, 140, 24, 20) -> center (72, 150)
          gl.useProgram(inkP);
          const ui = (n) => gl.getUniformLocation(inkP, n);
          gl.uniform1i(ui('u_bd'), 0);
          gl.uniform1i(ui('u_ad'), 4);
          gl.uniform1i(ui('u_ad_n'), 1);
          gl.uniform4f(ui('u_ad_box[0]'), 200.0, 150.0, 160.0, 22.0);
          gl.uniform2f(ui('u_vp'), wPt, hPt);
          gl.uniform2f(ui('u_origin'), 0.0, 0.0);
          gl.uniform1f(ui('u_scale'), S3);
          gl.uniform1f(ui('u_top'), H3);
          gl.uniform4f(ui('u_map'), 1.0 / wPt, 1.0 / hPt, 0.0, 0.0);
          gl.uniform4f(ui('u_uvc'), 0.5 / W3, 0.5 / H3, 1.0 - 0.5 / W3, 1.0 - 0.5 / H3);
          gl.vertexAttrib4f(0, 60.0, 140.0, 24.0, 20.0);
          gl.vertexAttrib4f(1, 6.0, 10.0, 0.0, 0.0);
          gl.vertexAttrib4f(2, 0.14, 0.15, 0.17, 1.0);
          gl.vertexAttrib4f(3, 0.1, 0.5, 0.9, 0.5);
          gl.vertexAttrib4f(4, 0.0, 0.0, 1e9, 1e9);
          gl.vertexAttrib4f(5, 0.0, 200.0, 150.0, 44.0);
          gl.vertexAttrib4f(9, 6.0, 6.0, 6.0, 6.0);
          gl.drawArrays(gl.TRIANGLES, 0, 6);

          const out = new Uint8Array(W3 * H3 * 4);
          gl.readPixels(0, 0, W3, H3, gl.RGBA, gl.UNSIGNED_BYTE, out);
          const lumAt = (xPt, yPt) => {
            const fy = H3 - 1 - Math.round(yPt * S3);
            const idx = (fy * W3 + Math.round(xPt * S3)) * 4;
            return (0.2126 * out[idx] + 0.7152 * out[idx + 1] + 0.0722 * out[idx + 2]) / 255.0;
          };
          return { ink: lumAt(72, 150), glassL: lumAt(140, 150), glassR: lumAt(310, 150), adTVal };
        };

        const r0 = runAdCase(184, 103 / 255, true);
        const frostRes = [0.522, 0.752, 0.913].map((a) => ({ a, ...runAdCase(184, a, false) }));
        const rBack = runAdCase(184, 103 / 255, false);
        const r65 = runAdCase(400, 0.752, true);
        const rDark0 = runAdCase(400, 103 / 255, true);
        const r95 = runAdCase(400, 0.913, false);

        const v13Ok = r0.ink < 0.25 && rBack.ink < 0.25 &&
          frostRes.every((r) => r.ink < 0.25 && r.glassL > 0.56) &&
          r65.ink < 0.25 && r65.glassR > 0.72 &&
          rDark0.ink > 0.75 && rDark0.glassR < 0.35 &&
          r95.adTVal <= 0.02 && r95.ink < 0.25 && r95.glassR > 0.85;
        pushVec('V13 light adaptive glass opacity (half-dark uniform + full-dark snap)', v13Ok,
          [Number(r0.ink.toFixed(3)), Number(frostRes[1].ink.toFixed(3)), Number(rBack.ink.toFixed(3)), Number(r65.ink.toFixed(3)), Number(r65.glassR.toFixed(3)), Number(r95.ink.toFixed(3)), Number(r95.glassR.toFixed(3)), Number(r95.adTVal.toFixed(3))],
          'l0<0.25, frost ink<0.25 glass>0.56, lBack<0.25, full65 ink<0.25 glass>0.72, full95 snap t<=0.02 ink<0.25 glass>0.85');

        // V14: adaptive glass over kind 9 native-view holes (AD_FS through bit in u_param.y, FS u_through fallback):
        // a fully transparent hole (a == 0) composites over white before its luma, so Light glass keeps dark ink;
        // an opaque black stage (a == 1) and a 70% black scrim over the hole (premultiplied, Porter-Duff over) still
        // flip to dark glass + white ink; without the through bit the hole reads as black as before.
        const runHoleCase = (rgba, through) => {
          const bgBuf = new Uint8Array(W3 * H3 * 4);
          for (let i = 0; i < W3 * H3 * 4; i += 4) { bgBuf[i] = rgba[0]; bgBuf[i + 1] = rgba[1]; bgBuf[i + 2] = rgba[2]; bgBuf[i + 3] = rgba[3]; }
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, bdTex);
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, W3, H3, 0, gl.RGBA, gl.UNSIGNED_BYTE, bgBuf);
          const cur = adC, nxt = 1 - cur;
          gl.disable(gl.BLEND);
          gl.bindFramebuffer(gl.FRAMEBUFFER, adF[nxt]);
          gl.viewport(0, 0, 16, 1);
          gl.useProgram(adP);
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, bdTex);
          gl.uniform1i(gl.getUniformLocation(adP, 'u_bd'), 0);
          gl.activeTexture(gl.TEXTURE1);
          gl.bindTexture(gl.TEXTURE_2D, adT[cur]);
          gl.uniform1i(gl.getUniformLocation(adP, 'u_prev'), 1);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_map'), 1.0 / wPt, 1.0 / hPt, 0.0, 0.0);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_uvc'), 0.5 / W3, 0.5 / H3, 1.0 - 0.5 / W3, 1.0 - 0.5 / H3);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_box'), 200.0, 150.0, 160.0, 22.0);
          gl.uniform4f(gl.getUniformLocation(adP, 'u_param'), -103 / 255, 1.0 + (through ? 2.0 : 0.0), 1.0 / 60.0, 0.0);
          gl.drawArrays(gl.TRIANGLES, 0, 3);
          const adPx = new Uint8Array(4);
          gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, adPx);
          const adTVal = (adPx[0] * 256 + adPx[1]) / 65535.0;
          adC = nxt;
          const inkLuma = (adN) => {
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.viewport(0, 0, W3, H3);
            gl.disable(gl.BLEND);
            gl.clearColor(0, 0, 0, 0);
            gl.clear(gl.COLOR_BUFFER_BIT);
            gl.useProgram(inkP);
            const ui = (n) => gl.getUniformLocation(inkP, n);
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, bdTex);
            gl.activeTexture(gl.TEXTURE4);
            gl.bindTexture(gl.TEXTURE_2D, adT[adC]);
            gl.uniform1i(ui('u_bd'), 0);
            gl.uniform1i(ui('u_ad'), 4);
            gl.uniform1i(ui('u_ad_n'), adN);
            gl.uniform1f(ui('u_through'), through ? 1.0 : 0.0);
            gl.uniform4f(ui('u_ad_box[0]'), 200.0, 150.0, 160.0, 22.0);
            gl.uniform2f(ui('u_vp'), wPt, hPt);
            gl.uniform2f(ui('u_origin'), 0.0, 0.0);
            gl.uniform1f(ui('u_scale'), S3);
            gl.uniform1f(ui('u_top'), H3);
            gl.uniform4f(ui('u_map'), 1.0 / wPt, 1.0 / hPt, 0.0, 0.0);
            gl.uniform4f(ui('u_uvc'), 0.5 / W3, 0.5 / H3, 1.0 - 0.5 / W3, 1.0 - 0.5 / H3);
            gl.vertexAttrib4f(0, 60.0, 140.0, 24.0, 20.0);
            gl.vertexAttrib4f(1, 6.0, 10.0, 0.0, 0.0);
            gl.vertexAttrib4f(2, 24 / 255, 24 / 255, 27 / 255, 1.0);
            gl.vertexAttrib4f(3, 0.1, 0.5, 0.9, 0.5);
            gl.vertexAttrib4f(4, 0.0, 0.0, 1e9, 1e9);
            gl.vertexAttrib4f(5, 0.0, 200.0, 150.0, 44.0);
            gl.vertexAttrib4f(9, 6.0, 6.0, 6.0, 6.0);
            gl.drawArrays(gl.TRIANGLES, 0, 6);
            const o = new Uint8Array(4);
            gl.readPixels(Math.round(72 * S3), H3 - 1 - Math.round(150 * S3), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, o);
            gl.uniform1f(ui('u_through'), 0.0);
            return (0.2126 * o[0] + 0.7152 * o[1] + 0.0722 * o[2]) / 255.0;
          };
          return { adT: adTVal, inkSlot: inkLuma(1), inkFallback: inkLuma(0) };
        };
        const hHole = runHoleCase([0, 0, 0, 0], true);
        const hLegacy = runHoleCase([0, 0, 0, 0], false);
        const hStage = runHoleCase([0, 0, 0, 255], true);
        const hScrim = runHoleCase([0, 0, 0, 179], true);
        const v14Ok = hHole.adT <= 0.05 && hHole.inkSlot < 0.25 && hHole.inkFallback < 0.25 &&
          hLegacy.adT >= 0.95 &&
          hStage.adT >= 0.95 && hStage.inkSlot > 0.75 && hStage.inkFallback > 0.75 &&
          hScrim.adT >= 0.95 && hScrim.inkSlot > 0.75 && hScrim.inkFallback > 0.75;
        const r3 = (v) => Number(v.toFixed(3));
        pushVec('V14 adaptive glass over kind 9 hole (through: dark ink; opaque stage & 70% scrim: white ink)', v14Ok,
          [r3(hHole.adT), r3(hHole.inkSlot), r3(hHole.inkFallback), r3(hLegacy.adT), r3(hStage.adT), r3(hStage.inkSlot), r3(hStage.inkFallback), r3(hScrim.adT), r3(hScrim.inkSlot), r3(hScrim.inkFallback)],
          'hole t<=0.05 ink<0.25 (slot & fallback), no-through hole t>=0.95, stage & scrim70 t>=0.95 ink>0.75 (slot & fallback)');

        gl.useProgram(prog);
        gl.uniform1i(u('u_ad_n'), 0);
        for (let i = 0; i < 2; i++) { gl.deleteFramebuffer(adF[i]); gl.deleteTexture(adT[i]); }
        gl.deleteProgram(adP);
        gl.deleteProgram(inkP);
      }

      gl.deleteTexture(bdTex);
      gl.deleteProgram(bgProg);
      gl.deleteShader(bgVs);
      gl.deleteShader(bgFs);
      gl.deleteProgram(prog);
      gl.deleteShader(vs);
      gl.deleteShader(fs);
    }

    report = {
      ok: passed === programs.length && drawPassed === drawChecks.length && drawChecks.length > 0 && vectorsPassed === vectorResults.length && vectorResults.length === 14,
      passed,
      total: programs.length,
      drawPassed,
      drawTotal: drawChecks.length,
      vectorsPassed,
      vectorsTotal: vectorResults.length,
      version: gl.getParameter(gl.VERSION),
      shadingLanguageVersion: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      renderer: gl.getParameter(gl.RENDERER),
      results,
      drawChecks,
      vectors: vectorResults,
    };
  }
  await fetch('/result', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(report),
  });
})();
</script>
</body>
</html>`;
}

async function runCheck() {
  const chromeBin = findChromeBinary();
  const shaders = extractShaders(HOST_JS_PATH);
  const programs = buildTestPrograms(shaders);
  const html = buildHtmlPage(programs);

  let resolveResult;
  let rejectResult;
  const resultPromise = new Promise((res, rej) => {
    resolveResult = res;
    rejectResult = rej;
  });

  const server = http.createServer((req, res) => {
    if (req.method === 'GET' && (req.url === '/' || req.url === '/index.html')) {
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
      res.end(html);
      return;
    }
    if (req.method === 'POST' && req.url === '/result') {
      const chunks = [];
      req.on('data', (c) => chunks.push(c));
      req.on('end', () => {
        res.writeHead(200, { 'content-type': 'text/plain' });
        res.end('ok');
        try {
          resolveResult(JSON.parse(Buffer.concat(chunks).toString('utf8')));
        } catch (err) {
          rejectResult(err);
        }
      });
      return;
    }
    res.writeHead(404);
    res.end();
  });

  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();
  const url = `http://127.0.0.1:${port}/`;

  const userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'invar-shader-check-'));
  const isHeadlessShell = path.basename(chromeBin).includes('headless');
  const args = [
    ...(isHeadlessShell ? [] : ['--headless=new']),
    '--use-gl=angle',
    ...(process.platform === 'darwin'
      ? ['--use-angle=metal']
      : ['--use-angle=swiftshader', '--enable-unsafe-swiftshader']),
    '--enable-webgl',
    '--ignore-gpu-blocklist',
    '--no-first-run',
    '--no-default-browser-check',
    `--user-data-dir=${userDataDir}`,
    url,
  ];

  const stderrChunks = [];
  const child = spawn(chromeBin, args, { stdio: ['ignore', 'ignore', 'pipe'] });
  child.stderr.on('data', (d) => stderrChunks.push(d));
  child.on('error', (err) => rejectResult(err));
  child.on('exit', (code, signal) => {
    if (code !== null && code !== 0) {
      rejectResult(new Error(`Browser exited early (code=${code}, signal=${signal}): ${Buffer.concat(stderrChunks).toString('utf8').slice(-1000)}`));
    }
  });

  const timeoutMs = Number(process.env.WEB_SHADER_TIMEOUT_MS || 60000);
  const timeout = setTimeout(() => {
    rejectResult(new Error(`Timed out waiting for WebGL2 shader check result.\nBrowser stderr:\n${Buffer.concat(stderrChunks).toString('utf8').slice(-1000)}`));
  }, timeoutMs);

  try {
    const report = await resultPromise;
    report.chromeBin = chromeBin;
    return report;
  } finally {
    clearTimeout(timeout);
    try { child.kill('SIGKILL'); } catch {}
    server.close();
    try { fs.rmSync(userDataDir, { recursive: true, force: true }); } catch {}
  }
}

runCheck()
  .then((report) => {
    if (JSON_MODE) {
      process.stdout.write(JSON.stringify(report, null, 2) + '\n');
    } else {
      console.log(`WebGL2 Shader Gate (${report.passed}/${report.total} compile/link, ${report.drawPassed || 0}/${report.drawTotal || 0} draw/readPixels, ${report.vectorsPassed || 0}/${report.vectorsTotal || 0} V1..V14) — ${report.ok ? 'PASS' : 'FAIL'}`);
      if (report.renderer) console.log(`  renderer: ${report.renderer}`);
      if (report.version) console.log(`  version : ${report.version}`);
      for (const r of report.results || []) {
        const tag = r.ok ? 'PASS' : 'FAIL';
        console.log(`  [${tag}] ${r.name}`);
        if (!r.ok && r.error) {
          console.log('    ' + r.error.replace(/\n/g, '\n    '));
        }
      }
      for (const d of report.drawChecks || []) {
        const tag = d.ok ? 'PASS' : 'FAIL';
        const extra = d.corner
          ? ` center=${JSON.stringify(d.center)} corner=${JSON.stringify(d.corner)}`
          : d.redCenter
            ? ` blue=${JSON.stringify(d.center)} red=${JSON.stringify(d.redCenter)}`
            : ` center=${JSON.stringify(d.center)}`;
        const errTxt = d.glErrors && d.glErrors.length ? ` glErrors=${JSON.stringify(d.glErrors)}` : '';
        console.log(`  [${tag}] ${d.name}${extra}${errTxt}`);
      }
      for (const v of report.vectors || []) {
        const tag = v.ok ? 'PASS' : 'FAIL';
        console.log(`  [${tag}] ${v.name}: got=(${v.got.join(',')}) expected=${v.expected}`);
      }
      if (report.error) console.error(`  ERROR: ${report.error}`);
    }
    process.exit(report.ok ? 0 : 1);
  })
  .catch((err) => {
    if (JSON_MODE) {
      process.stdout.write(JSON.stringify({ ok: false, error: String(err.message || err) }, null, 2) + '\n');
    } else {
      console.error(`WebGL2 Shader Gate ERROR: ${err.message || err}`);
    }
    process.exit(1);
  });
