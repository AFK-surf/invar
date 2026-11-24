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

      // 4. FS(ONE_KIND=6) Liquid Glass over blue & red backdrop textures (proves real u_bd sampling)
      {
        const p = linkProg('FS(ONE_KIND=6)');
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
        drawChecks.push({ name: 'draw:FS(ONE_KIND=6)', ok, center: blueCenter, redCenter, glErrors: errs });
        gl.deleteProgram(p);
      }

      gl.deleteTexture(whiteTex);
      gl.deleteTexture(bdBlueTex);
      gl.deleteTexture(bdRedTex);
      gl.deleteBuffer(vbo);
      gl.deleteVertexArray(vao);
    }

    report = {
      ok: passed === programs.length && drawPassed === drawChecks.length && drawChecks.length > 0,
      passed,
      total: programs.length,
      drawPassed,
      drawTotal: drawChecks.length,
      version: gl.getParameter(gl.VERSION),
      shadingLanguageVersion: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      renderer: gl.getParameter(gl.RENDERER),
      results,
      drawChecks,
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

  const timeout = setTimeout(() => {
    rejectResult(new Error(`Timed out waiting for WebGL2 shader check result.\nBrowser stderr:\n${Buffer.concat(stderrChunks).toString('utf8').slice(-1000)}`));
  }, 30000);

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
      console.log(`WebGL2 Shader Gate (${report.passed}/${report.total} compile/link, ${report.drawPassed || 0}/${report.drawTotal || 0} draw/readPixels) — ${report.ok ? 'PASS' : 'FAIL'}`);
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
