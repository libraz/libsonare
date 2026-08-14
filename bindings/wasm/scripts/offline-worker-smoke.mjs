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

function harness() {
  return `<!doctype html>
<meta charset="utf-8">
<output id="result" data-status="running">running</output>
<script type="module">
import { ErrorCode, OfflineWorkerClient, isSonareError } from '/dist/index.js';

const result = document.querySelector('#result');
const sampleRate = 22050;
const tone = (seconds) => {
  const samples = new Float32Array(seconds * sampleRate);
  for (let i = 0; i < samples.length; ++i) {
    samples[i] = 0.4 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
  }
  return samples;
};

try {
  const worker = new OfflineWorkerClient();
  let ticks = 0;
  const interval = setInterval(() => ++ticks, 5);
  const progress = [];
  try {
    const samples = tone(3);
    const analysis = await worker.analyze(
      { samples, sampleRate },
      { copy: true, onProgress: ({ stage }) => progress.push(stage) },
    );
    if (!(analysis.bpm > 0) || progress.length === 0 || ticks === 0) {
      throw new Error('worker analysis did not complete off the UI thread');
    }

    const transferred = tone(1);
    const bpm = await worker.detectBpm({ samples: transferred, sampleRate });
    if (!(bpm > 0) || transferred.byteLength !== 0) {
      throw new Error('worker input was not transferred');
    }

    let task;
    let cancelled = false;
    task = worker.analyze(
      { samples: tone(12), sampleRate },
      {
        copy: true,
        onProgress: () => {
          if (!cancelled) {
            cancelled = true;
            task.cancel();
          }
        },
      },
    );
    try {
      await task;
      throw new Error('worker cancellation unexpectedly completed');
    } catch (error) {
      if (!isSonareError(error) || error.code !== ErrorCode.Cancelled) throw error;
    }

    result.dataset.status = 'ok';
    result.textContent = JSON.stringify({
      crossOriginIsolated,
      bpm: analysis.bpm,
      progress: progress.length,
      ticks,
      cancelled,
    });
  } finally {
    clearInterval(interval);
    worker.dispose();
  }
} catch (error) {
  result.dataset.status = 'error';
  result.textContent = error instanceof Error ? error.stack || error.message : String(error);
}
</script>`;
}

async function startServer() {
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');
      if (url.pathname === '/smoke.html') {
        res.writeHead(200, headers('text/html'));
        res.end(harness());
        return;
      }
      if (url.pathname.startsWith('/dist/')) {
        const file = path.join(dist, url.pathname.slice('/dist/'.length));
        res.writeHead(200, headers(contentType(file)));
        res.end(await readFile(file));
        return;
      }
      res.writeHead(404, headers('text/plain'));
      res.end('not found');
    } catch (error) {
      res.writeHead(500, headers('text/plain'));
      res.end(error instanceof Error ? error.stack : String(error));
    }
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server)));
}

const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

function waitForDevToolsEndpoint(chrome, readStderr) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (callback, value) => {
      if (settled) return;
      settled = true;
      clearInterval(checkInterval);
      clearTimeout(timeout);
      chrome.off('error', onError);
      chrome.off('exit', onExit);
      callback(value);
    };
    const check = () => {
      const match = readStderr().match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) finish(resolve, match[1]);
    };
    const onError = (error) => finish(reject, error);
    const onExit = (code) => {
      finish(reject, new Error(`Chrome exited ${code} before exposing DevTools: ${readStderr()}`));
    };
    const checkInterval = setInterval(check, 25);
    const timeout = setTimeout(
      () => finish(reject, new Error(`Chrome did not expose DevTools: ${readStderr()}`)),
      10000,
    );
    chrome.once('error', onError);
    chrome.once('exit', onExit);
    check();
  });
}

function connectCdp(endpoint) {
  return new Promise((resolve, reject) => {
    const socket = new WebSocket(endpoint);
    const pending = new Map();
    let nextId = 1;
    let connected = false;

    const rejectPending = (error) => {
      for (const { reject: rejectRequest } of pending.values()) rejectRequest(error);
      pending.clear();
    };

    socket.addEventListener('open', () => {
      connected = true;
      resolve({
        send(method, params = {}, sessionId = undefined) {
          const id = nextId++;
          const message = { id, method, params, ...(sessionId ? { sessionId } : {}) };
          return new Promise((resolveRequest, rejectRequest) => {
            pending.set(id, { method, resolve: resolveRequest, reject: rejectRequest });
            socket.send(JSON.stringify(message));
          });
        },
        close() {
          socket.close();
        },
      });
    });
    socket.addEventListener('message', (event) => {
      const message = JSON.parse(String(event.data));
      const request = pending.get(message.id);
      if (!request) return;
      pending.delete(message.id);
      if (message.error) {
        request.reject(new Error(`CDP ${request.method}: ${message.error.message}`));
        return;
      }
      request.resolve(message.result);
    });
    socket.addEventListener('error', () => {
      const error = new Error('Chrome DevTools WebSocket error');
      rejectPending(error);
      if (!connected) reject(error);
    });
    socket.addEventListener('close', () => {
      rejectPending(new Error('Chrome DevTools WebSocket closed'));
    });
  });
}

async function stopChrome(chrome) {
  if (chrome.exitCode !== null) return;
  const exited = new Promise((resolve) => chrome.once('exit', resolve));
  chrome.kill('SIGTERM');
  await Promise.race([exited, sleep(5000)]);
}

async function readSmokeState(cdp, sessionId) {
  const response = await cdp.send(
    'Runtime.evaluate',
    {
      expression: `(() => {
        const result = document.querySelector('#result');
        return result ? { status: result.dataset.status, text: result.textContent } : null;
      })()`,
      returnByValue: true,
    },
    sessionId,
  );
  return response.result.value ?? null;
}

async function runBrowserSmoke(chromePath, url, userDataDir) {
  // `--dump-dom` with a virtual-time budget never reaches a deterministic
  // exit while this page owns a Worker.  Poll the real page through CDP so the
  // smoke waits for the protocol result and always tears Chrome down itself.
  const chrome = spawn(chromePath, [
    '--headless=new',
    '--disable-gpu',
    '--no-sandbox',
    '--no-first-run',
    '--no-default-browser-check',
    '--remote-debugging-port=0',
    `--user-data-dir=${userDataDir}`,
    'about:blank',
  ]);
  let stderr = '';
  chrome.stderr.setEncoding('utf8');
  chrome.stderr.on('data', (chunk) => (stderr += chunk));

  let cdp;
  try {
    cdp = await connectCdp(await waitForDevToolsEndpoint(chrome, () => stderr));
    const { targetId } = await cdp.send('Target.createTarget', { url });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const deadline = Date.now() + 90000;
    let state = null;
    while (Date.now() < deadline) {
      state = await readSmokeState(cdp, sessionId);
      if (state?.status && state.status !== 'running') return state;
      await sleep(25);
    }
    throw new Error(`offline worker smoke timed out: ${JSON.stringify(state)}\n${stderr}`);
  } finally {
    cdp?.close();
    await stopChrome(chrome);
  }
}

async function main() {
  const chromePath = await findChrome();
  const server = await startServer();
  const port = server.address().port;
  const userDataDir = await mkdtemp(path.join(os.tmpdir(), 'sonare-worker-chrome-'));
  try {
    const state = await runBrowserSmoke(
      chromePath,
      `http://127.0.0.1:${port}/smoke.html`,
      userDataDir,
    );
    if (state.status !== 'ok') throw new Error(`offline worker smoke failed: ${state.text}`);
    const result = JSON.parse(state.text);
    if (!result.crossOriginIsolated || !result.cancelled || result.ticks <= 0) {
      throw new Error(`offline worker smoke incomplete: ${state.text}`);
    }
    console.log(JSON.stringify(result, null, 2));
  } finally {
    server.close();
    await rm(userDataDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
