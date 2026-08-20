import { spawn } from 'node:child_process';
import { constants } from 'node:fs';
import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const root = path.resolve(new URL('..', import.meta.url).pathname);
const dist = process.env.SONARE_DIST_DIR
  ? path.resolve(process.env.SONARE_DIST_DIR)
  : path.join(root, 'dist');
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
  throw new Error('No executable Chrome/Chromium found. Set CHROME_BIN to run this smoke test.');
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
  return 'application/octet-stream';
}

function browserHarness() {
  return `<!doctype html>
<meta charset="utf-8">
<script type="module">
import createSonare from '/dist/sonare.js';
import {
  BLOCK,
  configureDynamicEq,
  INVALID_DRAIN_COUNTS,
  INVERSE_OVERFLOW_SHAPES,
  rms,
  sine,
  SR,
  WASM_FLOAT_BUDGET,
} from '/fixtures.mjs';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(fn, label) {
  try {
    fn();
  } catch {
    return;
  }
  throw new Error(label + ' did not throw');
}

window.runBoundarySmoke = async () => {
  const wasmBinary = await fetch('/dist/sonare.wasm').then((response) => {
    if (!response.ok) throw new Error('failed to fetch sonare.wasm: ' + response.status);
    return response.arrayBuffer();
  });
  const module = await createSonare({
    locateFile: (file) => '/dist/' + file,
    wasmBinary,
  });
  const checks = [];
  const checked = (label, fn) => {
    fn();
    checks.push(label);
  };

  checked('inverse overflow rejected and recovered', () => {
    for (const [rows, frames] of INVERSE_OVERFLOW_SHAPES) {
      expectThrow(
        () => module.melToStft(new Float32Array(1), rows, frames, SR, 1024, 0, 0, false),
        'overflowing inverse shape ' + rows + 'x' + frames,
      );
    }
    const valid = module.melToStft(new Float32Array([1]), 1, 1, SR, 2, 0, 0, false);
    assert(valid.nFrames === 1 && valid.power.length > 0, 'inverse module recovery failed');
  });

  checked('allocation preflight rejected and recovered', () => {
    expectThrow(
      () => module.meteringPeakDb({ length: WASM_FLOAT_BUDGET + 1 }, SR),
      'oversized array-like input',
    );
    expectThrow(
      () =>
        module.masterAudioStereo(
          'pop',
          { length: WASM_FLOAT_BUDGET / 2 },
          { length: WASM_FLOAT_BUDGET / 2 + 1 },
          SR,
          null,
        ),
      'cumulative oversized array-like inputs',
    );
    const peakDb = module.meteringPeakDb(new Float32Array([0.5]), SR);
    assert(Math.abs(peakDb + 6.0206) < 0.001, 'metering module recovery failed');
  });

  checked('sidechain ownership and failed-update atomicity', () => {
    const actual = module.createEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    const expected = module.createEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    const dry = module.createEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    try {
      configureDynamicEq(actual);
      configureDynamicEq(expected);
      configureDynamicEq(dry);
      const key = sine(0.8);
      expected.setSidechainStereo(key, key);
      const expectedOutputs = [expected.processMono(sine(0.005)), expected.processMono(sine(0.005))];
      const dryOutput = dry.processMono(sine(0.005));
      assert(rms(expectedOutputs[0]) < rms(dryOutput) * 0.8, 'sidechain did not engage');

      actual.setSidechainStereo(key, key);
      expectThrow(
        () => actual.setSidechainStereo(key, new Float32Array(BLOCK / 2)),
        'mismatched sidechain update',
      );
      for (let i = 0; i < 64; i++) {
        // Five arguments: js_mfcc_to_mel takes a lifter after n_mels. No tsc
        // program covers this file, so the argument count is checked by nothing
        // but this comment and the sibling loop in boundary-regressions.test.ts.
        module.mfccToMel(new Float32Array([1]), 1, 1, 1, 0);
      }
      const actualOutputs = [actual.processMono(sine(0.005)), actual.processMono(sine(0.005))];
      assert(
        JSON.stringify(actualOutputs.map((output) => Array.from(output))) ===
          JSON.stringify(expectedOutputs.map((output) => Array.from(output))),
        'active sidechain changed after stack churn or failed update',
      );
    } finally {
      actual.delete();
      expected.delete();
      dry.delete();
    }
  });

  checked('mixer drain validation and recovery', () => {
    const scene = module.mixingScenePresetJson('vocalReverbSend');
    const mixer = module.createMixerFromSceneJson(scene, SR, BLOCK);
    try {
      for (const invalid of INVALID_DRAIN_COUNTS) {
        expectThrow(() => mixer.drainTailStereo(invalid), 'invalid drain frame count ' + invalid);
      }
      assert(mixer.drainTailStereo(BLOCK - 1).left.length === BLOCK - 1, 'short drain recovery failed');
      assert(mixer.drainTailStereo(BLOCK).left.length === BLOCK, 'full drain recovery failed');
    } finally {
      mixer.delete();
    }
  });

  const finalPeak = module.meteringPeakDb(new Float32Array([0.25]), SR);
  assert(Number.isFinite(finalPeak), 'final same-module recovery check failed');
  return {
    ok: true,
    crossOriginIsolated,
    checks,
    finalPeak,
    usedJSHeapSize: performance.memory?.usedJSHeapSize ?? null,
  };
};
</script>`;
}

function startServer() {
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');
      if (url.pathname === '/boundary.html') {
        res.writeHead(200, headers('text/html'));
        res.end(browserHarness());
        return;
      }
      if (url.pathname === '/fixtures.mjs') {
        const fixtures = await readFile(path.join(root, 'tests', '_boundary_fixtures.mjs'));
        res.writeHead(200, headers('text/javascript'));
        res.end(fixtures);
        return;
      }
      if (url.pathname.startsWith('/dist/')) {
        const file = path.join(dist, url.pathname.slice('/dist/'.length));
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
    const timeout = setTimeout(
      () => reject(new Error('timed out waiting for Chrome DevTools')),
      15000,
    );
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
          'Chrome exited before DevTools became available: code=' +
            code +
            ' signal=' +
            signal +
            '\n' +
            stderr,
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
    const promise = new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
    this.ws.send(JSON.stringify({ id, method, params }));
    return promise;
  }

  close() {
    this.ws.close();
  }
}

async function waitForFunction(page, expression, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const result = await page.send('Runtime.evaluate', { expression, returnByValue: true });
    if (result.result?.value) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('timed out waiting for ' + expression);
}

function withTimeout(promise, timeoutMs, label) {
  let timeout;
  const timeoutPromise = new Promise((_, reject) => {
    timeout = setTimeout(
      () => reject(new Error(label + ' timed out after ' + timeoutMs + 'ms')),
      timeoutMs,
    );
  });
  return Promise.race([promise, timeoutPromise]).finally(() => clearTimeout(timeout));
}

async function main() {
  const chromePath = await findChrome();
  const server = await startServer();
  const port = server.address().port;
  const userDataDir = await mkdtemp(path.join(os.tmpdir(), 'sonare-boundary-chrome-'));
  const macArgs =
    process.platform === 'darwin'
      ? [
          '--single-process',
          '--disable-features=AudioServiceOutOfProcess,UseChromeOSDirectVideoDecoder',
          '--disable-crash-reporter',
          '--disable-breakpad',
        ]
      : [];
  const chrome = spawn(chromePath, [
    '--headless=new',
    '--disable-gpu',
    '--no-sandbox',
    '--no-first-run',
    '--no-default-browser-check',
    ...macArgs,
    ...(process.env.CHROME_EXTRA_ARGS
      ? process.env.CHROME_EXTRA_ARGS.split(/\s+/).filter(Boolean)
      : []),
    '--remote-debugging-port=0',
    `--user-data-dir=${userDataDir}`,
    `http://127.0.0.1:${port}/boundary.html`,
  ]);

  try {
    const browserWs = await waitForDevTools(chrome);
    const browser = new Cdp(browserWs);
    await browser.open();
    const { targetId } = await browser.send('Target.createTarget', {
      url: `http://127.0.0.1:${port}/boundary.html`,
    });
    const targets = await fetch(`http://127.0.0.1:${new URL(browserWs).port}/json`).then((res) =>
      res.json(),
    );
    const target = targets.find((candidate) => candidate.id === targetId);
    if (!target?.webSocketDebuggerUrl) throw new Error('Chrome page target was not available');
    const page = new Cdp(target.webSocketDebuggerUrl);
    await page.open();
    await page.send('Runtime.enable');
    await waitForFunction(page, 'typeof window.runBoundarySmoke === "function"');
    const result = await withTimeout(
      page.send('Runtime.evaluate', {
        expression: 'window.runBoundarySmoke()',
        awaitPromise: true,
        returnByValue: true,
      }),
      30000,
      'boundary browser smoke',
    );
    if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
    const value = result.result.value;
    if (!value?.ok) throw new Error('boundary browser smoke failed: ' + JSON.stringify(value));
    if (!value.crossOriginIsolated) throw new Error('page is not cross-origin isolated');
    if (value.checks?.length !== 4)
      throw new Error('not all boundary checks ran: ' + JSON.stringify(value));
    console.log(JSON.stringify(value, null, 2));
    page.close();
    browser.close();
  } finally {
    chrome.kill('SIGTERM');
    server.close();
    await rm(userDataDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
