import http from 'node:http';
import { access } from 'node:fs/promises';
import { constants } from 'node:fs';
import { readFile } from 'node:fs/promises';
import { mkdtemp, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

const root = path.resolve(new URL('..', import.meta.url).pathname);
const chromeCandidates = [
  process.env.CHROME_BIN,
  '/tmp/libsonare-ms-playwright/chromium_headless_shell-1223/chrome-headless-shell-mac-arm64/chrome-headless-shell',
  '/tmp/libsonare-ms-playwright/chromium-1223/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);

async function findChrome() {
  for (const candidate of chromeCandidates) {
    try {
      await access(candidate, constants.X_OK);
      return candidate;
    } catch {}
  }
  throw new Error(
    'No executable Chrome/Chromium found. Set CHROME_BIN or run with PLAYWRIGHT_BROWSERS_PATH=/tmp/libsonare-ms-playwright.',
  );
}

function headers(contentType) {
  return {
    'Content-Type': contentType,
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
  };
}

function contentType(file) {
  if (file.endsWith('.js')) return 'text/javascript';
  if (file.endsWith('.wasm')) return 'application/wasm';
  if (file.endsWith('.html')) return 'text/html';
  return 'application/octet-stream';
}

function startServer() {
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');
      if (url.pathname === '/smoke.html') {
        res.writeHead(200, headers('text/html'));
        res.end(`<!doctype html>
<meta charset="utf-8">
<script type="module">
import { SonareRealtimeEngineNode } from '/dist/worklet.js';

window.runSonareSmoke = async () => {
  const progress = [];
  const mark = (stage) => progress.push(stage);
  const limit = (promise, stage, ms = 5000) =>
    Promise.race([
      promise,
      new Promise((_, reject) => setTimeout(() => reject(new Error(stage + ' timed out')), ms)),
    ]);
  let engine;
  try {
    mark('context');
    const context = new OfflineAudioContext(1, 2048, 48000);
    const rtWasmBinary = await fetch('/dist/sonare-rt.wasm').then((response) => {
      if (!response.ok) throw new Error('failed to fetch sonare-rt.wasm: ' + response.status);
      return response.arrayBuffer();
    });
    mark('create-node');
    engine = await SonareRealtimeEngineNode.create(context, {
      runtimeTarget: 'sonare-rt',
      mode: 'sab',
      moduleUrl: '/sonare-engine-worklet.js',
      rtModuleUrl: new URL('/dist/sonare-rt-module.js', location.href).href,
      rtWasmBinary,
      blockSize: 128,
      channelCount: 1,
      commandRingCapacity: 16,
      telemetryRingCapacity: 16,
    });
    mark('wait-ready');
    await limit(engine.ready, 'engine.ready');
    mark('connect');
    const source = new ConstantSourceNode(context, { offset: 0.25 });
    source.connect(engine.node);
    engine.node.connect(context.destination);
    engine.play();
    source.start();
    mark('render');
    const rendered = await limit(context.startRendering(), 'startRendering', 10000);
    mark('rendered');
    const data = rendered.getChannelData(0);
    const telemetry = engine.pollTelemetry();
    engine.destroy();
    let peak = 0;
    for (const sample of data) peak = Math.max(peak, Math.abs(sample));
    return {
      ok: true,
      progress,
      crossOriginIsolated,
      mode: engine.capabilities.mode,
      runtimeTarget: engine.capabilities.runtimeTarget,
      telemetryCount: telemetry.length,
      lastTimelineSample: telemetry.at(-1)?.timelineSample ?? 0,
      peak,
    };
  } catch (error) {
    engine?.destroy?.();
    return {
      ok: false,
      progress,
      error: error instanceof Error ? error.message : String(error),
      stack: error instanceof Error ? error.stack : '',
    };
  }
};
</script>`);
        return;
      }
      if (url.pathname === '/sonare-engine-worklet.js') {
        res.writeHead(200, headers('text/javascript'));
        res.end(`import createSonareRt from '/dist/sonare-rt-module.js';
import { registerSonareRealtimeEngineWorkletProcessor } from '/dist/worklet.js';
globalThis.SonareRtModuleFactory = createSonareRt;
registerSonareRealtimeEngineWorkletProcessor();
`);
        return;
      }
      if (url.pathname.startsWith('/dist/')) {
        const file = path.join(root, url.pathname);
        const data = await readFile(file);
        res.writeHead(200, headers(contentType(file)));
        res.end(data);
        return;
      }
      res.writeHead(404, headers('text/plain'));
      res.end('not found');
    } catch (error) {
      res.writeHead(500, headers('text/plain'));
      res.end(error instanceof Error ? error.stack : String(error));
    }
  });
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

function waitForDevTools(chrome) {
  return new Promise((resolve, reject) => {
    let stderr = '';
    const timeout = setTimeout(() => reject(new Error('timed out waiting for Chrome DevTools')), 15000);
    chrome.stderr.setEncoding('utf8');
    chrome.stderr.on('data', (chunk) => {
      stderr += String(chunk);
      const match = String(chunk).match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) {
        clearTimeout(timeout);
        resolve(match[1]);
      }
    });
    chrome.once('exit', (code, signal) => {
      clearTimeout(timeout);
      reject(
        new Error(
          `Chrome exited before DevTools became available: code=${code} signal=${signal}\n${stderr}`,
        ),
      );
    });
  });
}

class Cdp {
  constructor(wsUrl) {
    this.nextId = 1;
    this.pending = new Map();
    this.ws = new WebSocket(wsUrl);
  }

  async open() {
    await new Promise((resolve, reject) => {
      this.ws.addEventListener('open', resolve, { once: true });
      this.ws.addEventListener('error', reject, { once: true });
    });
    this.ws.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      if (!message.id) return;
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(JSON.stringify(message.error)));
      else pending.resolve(message.result);
    });
  }

  send(method, params = {}) {
    const id = this.nextId++;
    const payload = JSON.stringify({ id, method, params });
    const promise = new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
    this.ws.send(payload);
    return promise;
  }

  close() {
    this.ws.close();
  }
}

async function waitForFunction(page, expression, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const result = await page.send('Runtime.evaluate', {
      expression,
      returnByValue: true,
    });
    if (result.result?.value) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`timed out waiting for ${expression}`);
}

function withTimeout(promise, timeoutMs, label) {
  let timeout;
  const timeoutPromise = new Promise((_, reject) => {
    timeout = setTimeout(() => reject(new Error(`${label} timed out after ${timeoutMs}ms`)), timeoutMs);
  });
  return Promise.race([promise, timeoutPromise]).finally(() => clearTimeout(timeout));
}

async function main() {
  const chromePath = await findChrome();
  const server = await startServer();
  const port = server.address().port;
  const userDataDir = await mkdtemp(path.join(os.tmpdir(), 'sonare-chrome-'));
  const chrome = spawn(chromePath, [
    '--headless=new',
    '--disable-gpu',
    '--no-sandbox',
    '--no-first-run',
    '--no-default-browser-check',
    '--autoplay-policy=no-user-gesture-required',
    ...(process.env.CHROME_EXTRA_ARGS ? process.env.CHROME_EXTRA_ARGS.split(/\s+/).filter(Boolean) : []),
    '--remote-debugging-port=0',
    `--user-data-dir=${userDataDir}`,
    `http://127.0.0.1:${port}/smoke.html`,
  ]);

  try {
    const browserWs = await waitForDevTools(chrome);
    const browser = new Cdp(browserWs);
    await browser.open();
    const { targetId } = await browser.send('Target.createTarget', {
      url: `http://127.0.0.1:${port}/smoke.html`,
    });
    const { webSocketDebuggerUrl } = await fetch(`http://127.0.0.1:${new URL(browserWs).port}/json`)
      .then((res) => res.json())
      .then((targets) => targets.find((target) => target.id === targetId));
    const page = new Cdp(webSocketDebuggerUrl);
    await page.open();
    await page.send('Runtime.enable');
    await waitForFunction(page, 'typeof window.runSonareSmoke === "function"');
    const result = await withTimeout(
      page.send('Runtime.evaluate', {
        expression: 'window.runSonareSmoke()',
        awaitPromise: true,
        returnByValue: true,
      }),
      20000,
      'AudioWorklet smoke',
    );
    if (result.exceptionDetails) {
      throw new Error(JSON.stringify(result.exceptionDetails));
    }
    const value = result.result.value;
    if (!value.ok) throw new Error(`browser smoke failed: ${JSON.stringify(value, null, 2)}`);
    if (!value.crossOriginIsolated) throw new Error('page is not cross-origin isolated');
    if (value.mode !== 'sab') throw new Error(`expected SAB mode, got ${value.mode}`);
    if (value.runtimeTarget !== 'sonare-rt') {
      throw new Error(`expected sonare-rt runtime, got ${value.runtimeTarget}`);
    }
    if (value.telemetryCount <= 0 || value.lastTimelineSample <= 0) {
      throw new Error(`missing process telemetry: ${JSON.stringify(value)}`);
    }
    if (!(value.peak > 0.01)) {
      throw new Error(`rendered output is silent: ${JSON.stringify(value)}`);
    }
    console.log(JSON.stringify(value, null, 2));
    page.close();
    browser.close();
  } finally {
    chrome.kill('SIGTERM');
    server.close();
    await rm(userDataDir, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
