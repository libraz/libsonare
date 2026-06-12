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
import { init, SonareEngineCommandType, SonareRealtimeEngineNode } from '/dist/worklet.js';

window.runSonareSmoke = async () => {
  const progress = [];
  const mark = (stage) => progress.push(stage);
  const limit = (promise, stage, ms = 5000) =>
    Promise.race([
      promise,
      new Promise((_, reject) => setTimeout(() => reject(new Error(stage + ' timed out')), ms)),
    ]);
  const tick = () => new Promise((resolve) => setTimeout(resolve, 0));
  const midi1Word = (status, channel, data0, data1) =>
    (0x2 << 28) |
    ((status & 0xf) << 20) |
    ((channel & 0xf) << 16) |
    ((data0 & 0x7f) << 8) |
    (data1 & 0x7f);
  let engine;
  let mixerEngine;
  let instrumentEngine;
  let captureEngine;
  try {
    mark('context');
    const context = new OfflineAudioContext(1, 2048, 48000);
    const rtWasmBinary = await fetch('/dist/sonare-rt.wasm').then((response) => {
      if (!response.ok) throw new Error('failed to fetch sonare-rt.wasm: ' + response.status);
      return response.arrayBuffer();
    });
    const sonareWasmBinary = await fetch('/dist/sonare.wasm').then((response) => {
      if (!response.ok) throw new Error('failed to fetch sonare.wasm: ' + response.status);
      return response.arrayBuffer();
    });
    await init({ locateFile: (path) => '/dist/' + path, wasmBinary: sonareWasmBinary });
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

    mark('mixer-context');
    const mixerContext = new OfflineAudioContext(1, 4096, 48000);
    const clip = new Float32Array(4096).fill(1);
    mark('mixer-create-node');
    mixerEngine = await SonareRealtimeEngineNode.create(mixerContext, {
      runtimeTarget: 'embind',
      mode: 'sab',
      moduleUrl: '/sonare-embind-engine-worklet.js',
      wasmBinary: sonareWasmBinary.slice(0),
      initialSyncMessages: [
        {
          type: 'syncMixer',
          lanes: [{ trackId: 10 }],
          buses: [],
        },
        {
          type: 'syncClips',
          clips: [{ id: 1, trackId: 10, channels: [clip], startPpq: 0 }],
        },
      ],
      initialCommands: [
        {
          type: SonareEngineCommandType.SetParamSmoothed,
          targetId: 0x4d580001,
          sampleTime: -1,
          argFloat: -12,
        },
        {
          type: SonareEngineCommandType.TransportPlay,
          sampleTime: -1,
        },
      ],
      blockSize: 128,
      channelCount: 1,
      commandRingCapacity: 16,
      telemetryRingCapacity: 16,
      meterRingCapacity: 16,
    });
    await limit(mixerEngine.ready, 'mixerEngine.ready');
    mixerEngine.node.connect(mixerContext.destination);
    mark('mixer-render');
    const mixerRendered = await limit(mixerContext.startRendering(), 'mixer startRendering', 10000);
    mark('mixer-rendered');
    const mixerData = mixerRendered.getChannelData(0);
    const mixerTelemetry = mixerEngine.pollTelemetry();
    const mixerMeters = mixerEngine.pollMeters();
    mixerEngine.destroy();
    let mixerPeak = 0;
    for (const sample of mixerData) mixerPeak = Math.max(mixerPeak, Math.abs(sample));
    let mixerTailPeak = 0;
    for (const sample of mixerData.subarray(Math.max(0, mixerData.length - 256))) {
      mixerTailPeak = Math.max(mixerTailPeak, Math.abs(sample));
    }
    const mixerMeterTargets = Array.from(new Set(mixerMeters.map((meter) => meter.targetId))).sort(
      (a, b) => a - b,
    );

    mark('instrument-context');
    const instrumentContext = new OfflineAudioContext(2, 2048, 48000);
    mark('instrument-create-node');
    instrumentEngine = await SonareRealtimeEngineNode.create(instrumentContext, {
      runtimeTarget: 'embind',
      mode: 'sab',
      moduleUrl: '/sonare-embind-engine-worklet.js',
      wasmBinary: sonareWasmBinary.slice(0),
      initialSyncMessages: [
        {
          type: 'syncBuiltinInstrument',
          destinationId: 12,
          config: { gain: 0.5 },
        },
        {
          type: 'syncMidiClips',
          clips: [
            {
              id: 1,
              trackId: 12,
              destinationId: 12,
              lengthSamples: 2048,
              events: [
                { renderFrame: 0, word0: midi1Word(0x9, 0, 64, 100), wordCount: 1 },
                { renderFrame: 1536, word0: midi1Word(0x8, 0, 64, 0), wordCount: 1 },
              ],
            },
          ],
        },
      ],
      initialCommands: [
        {
          type: SonareEngineCommandType.TransportPlay,
          sampleTime: -1,
        },
      ],
      blockSize: 128,
      channelCount: 2,
      commandRingCapacity: 16,
      telemetryRingCapacity: 16,
      meterRingCapacity: 16,
    });
    await limit(instrumentEngine.ready, 'instrumentEngine.ready');
    instrumentEngine.node.connect(instrumentContext.destination);
    mark('instrument-render');
    const instrumentRendered = await limit(
      instrumentContext.startRendering(),
      'instrument startRendering',
      10000,
    );
    mark('instrument-rendered');
    const instrumentData = instrumentRendered.getChannelData(0);
    const instrumentTelemetry = instrumentEngine.pollTelemetry();
    instrumentEngine.destroy();
    let instrumentPeak = 0;
    for (const sample of instrumentData) {
      instrumentPeak = Math.max(instrumentPeak, Math.abs(sample));
    }

    mark('capture-context');
    const captureContext = new OfflineAudioContext(1, 1024, 48000);
    mark('capture-create-node');
    captureEngine = await SonareRealtimeEngineNode.create(captureContext, {
      runtimeTarget: 'embind',
      mode: 'sab',
      moduleUrl: '/sonare-embind-engine-worklet.js',
      wasmBinary: sonareWasmBinary.slice(0),
      initialSyncMessages: [
        {
          type: 'syncCapture',
          bufferFrames: 1024,
          channels: 1,
          source: 'input',
          recordOffsetSamples: 0,
          inputMonitor: { enabled: false, gain: 1 },
        },
      ],
      blockSize: 128,
      channelCount: 1,
      commandRingCapacity: 16,
      telemetryRingCapacity: 16,
    });
    await limit(captureEngine.ready, 'captureEngine.ready');
    const captureSource = new ConstantSourceNode(captureContext, { offset: 0.375 });
    captureSource.connect(captureEngine.node);
    captureEngine.node.connect(captureContext.destination);
    captureEngine.sendCommand({
      type: SonareEngineCommandType.ArmRecord,
      sampleTime: -1,
      argInt: 1,
    });
    await tick();
    captureSource.start();
    mark('capture-render');
    await limit(captureContext.startRendering(), 'capture startRendering', 10000);
    mark('capture-rendered');
    const captureStatus = await captureEngine.requestCaptureStatus();
    const capturedAudio = await captureEngine.requestCapturedAudio();
    captureEngine.destroy();
    const capturedPeak = Math.max(...Array.from(capturedAudio[0] ?? [], Math.abs));

    return {
      ok: true,
      progress,
      crossOriginIsolated,
      mode: engine.capabilities.mode,
      runtimeTarget: engine.capabilities.runtimeTarget,
      telemetryCount: telemetry.length,
      lastTimelineSample: telemetry.at(-1)?.timelineSample ?? 0,
      peak,
      mixerPeak,
      mixerTailPeak,
      mixerTelemetryCount: mixerTelemetry.length,
      mixerMeterTargets,
      instrumentPeak,
      instrumentTelemetryCount: instrumentTelemetry.length,
      captureFrames: captureStatus.capturedFrames,
      captureSource: captureStatus.source,
      capturedPeak,
    };
  } catch (error) {
    engine?.destroy?.();
    mixerEngine?.destroy?.();
    instrumentEngine?.destroy?.();
    captureEngine?.destroy?.();
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
      if (url.pathname === '/sonare-embind-engine-worklet.js') {
        res.writeHead(200, headers('text/javascript'));
        res.end(`import createSonare from '/dist/sonare.js';
import { registerSonareRealtimeEngineWorkletProcessor } from '/dist/worklet.js';
globalThis.SonareEmbindModuleFactory = createSonare;
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
  const defaultChromeArgs =
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
    '--autoplay-policy=no-user-gesture-required',
    ...defaultChromeArgs,
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
    if (!(value.mixerTailPeak > 0.2 && value.mixerTailPeak < 0.4)) {
      throw new Error(`mixer fader did not affect output: ${JSON.stringify(value)}`);
    }
    if (!value.mixerMeterTargets?.includes(0) || !value.mixerMeterTargets?.includes(1)) {
      throw new Error(`missing mixer meter targets: ${JSON.stringify(value)}`);
    }
    if (value.mixerTelemetryCount <= 0) {
      throw new Error(`missing mixer telemetry: ${JSON.stringify(value)}`);
    }
    if (!(value.instrumentPeak > 0.001)) {
      throw new Error(`instrument MIDI clip rendered silent: ${JSON.stringify(value)}`);
    }
    if (value.instrumentTelemetryCount <= 0) {
      throw new Error(`missing instrument telemetry: ${JSON.stringify(value)}`);
    }
    if (value.captureSource !== 'input' || value.captureFrames <= 0) {
      throw new Error(`capture status did not update: ${JSON.stringify(value)}`);
    }
    if (!(value.capturedPeak > 0.3 && value.capturedPeak < 0.45)) {
      throw new Error(`captured audio is missing input signal: ${JSON.stringify(value)}`);
    }
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
