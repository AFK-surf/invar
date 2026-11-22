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
    report = {
      ok: passed === programs.length,
      passed,
      total: programs.length,
      version: gl.getParameter(gl.VERSION),
      shadingLanguageVersion: gl.getParameter(gl.SHADING_LANGUAGE_VERSION),
      renderer: gl.getParameter(gl.RENDERER),
      results,
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
      console.log(`WebGL2 Shader Gate (${report.passed}/${report.total}) — ${report.ok ? 'PASS' : 'FAIL'}`);
      if (report.renderer) console.log(`  renderer: ${report.renderer}`);
      if (report.version) console.log(`  version : ${report.version}`);
      for (const r of report.results || []) {
        const tag = r.ok ? 'PASS' : 'FAIL';
        console.log(`  [${tag}] ${r.name}`);
        if (!r.ok && r.error) {
          console.log('    ' + r.error.replace(/\n/g, '\n    '));
        }
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
