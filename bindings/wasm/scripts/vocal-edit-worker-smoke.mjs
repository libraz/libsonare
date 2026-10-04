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

function headers(contentType, isolated) {
  return {
    'Content-Type': contentType,
    ...(isolated
      ? {
          'Cross-Origin-Opener-Policy': 'same-origin',
          'Cross-Origin-Embedder-Policy': 'require-corp',
        }
      : {}),
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
import {
  ErrorCode,
  VocalEditWorkerClient,
  VocalEditWorkerStaleResultError,
  isSonareError,
} from '/dist/index.js';

const result = document.querySelector('#result');
const sampleRate = 16000;
const sampleCount = 32000;
const sourceFrequency = 220;

const assert = (condition, message) => {
  if (!condition) throw new Error(message);
};

const makeTone = () => {
  const samples = new Float32Array(sampleCount);
  for (let i = 0; i < samples.length; ++i) {
    samples[i] = 0.4 * Math.sin((2 * Math.PI * sourceFrequency * i) / sampleRate);
  }
  return samples;
};

const makeAnalysis = () => {
  const frameCount = sampleCount / 160;
  const f0Hz = new Float32Array(frameCount);
  const voiced = new Uint8Array(frameCount);
  f0Hz.fill(sourceFrequency);
  voiced.fill(1);
  return {
    frameOriginSample: 0,
    samplesPerFrame: 160,
    frameLengthSamples: 320,
    f0Hz,
    voiced,
    algorithmId: 'host',
    algorithmVersion: 1,
    fminHz: 80,
    fmaxHz: 1000,
    yinThreshold: 0.1,
    voicedThreshold: 0.1,
    centered: false,
    segmentationThresholdCents: 50,
    minNoteMs: 30,
    referenceHz: 440,
  };
};

const maxAbsDelta = (left, right) => {
  assert(left.length === right.length, 'PCM lengths differ');
  let maximum = 0;
  for (let i = 0; i < left.length; ++i) {
    maximum = Math.max(maximum, Math.abs(left[i] - right[i]));
  }
  return maximum;
};

const rms = (samples) => {
  let sum = 0;
  for (const sample of samples) sum += sample * sample;
  return Math.sqrt(sum / samples.length);
};

const powerAt = (samples, frequency) => {
  const start = Math.floor(samples.length * 0.1);
  const end = Math.floor(samples.length * 0.9);
  const step = (2 * Math.PI * frequency) / sampleRate;
  const cosine = Math.cos(step);
  const sine = Math.sin(step);
  let phaseCosine = 1;
  let phaseSine = 0;
  let real = 0;
  let imaginary = 0;
  for (let i = start; i < end; ++i) {
    const value = samples[i];
    real += value * phaseCosine;
    imaginary -= value * phaseSine;
    const nextCosine = phaseCosine * cosine - phaseSine * sine;
    phaseSine = phaseSine * cosine + phaseCosine * sine;
    phaseCosine = nextCosine;
  }
  return real * real + imaginary * imaginary;
};

const dominantFrequency = (samples) => {
  let bestFrequency = 0;
  let bestPower = -Infinity;
  for (let frequency = 100; frequency <= 400; ++frequency) {
    const power = powerAt(samples, frequency);
    if (power > bestPower) {
      bestPower = power;
      bestFrequency = frequency;
    }
  }
  return { frequency: bestFrequency, power: bestPower };
};

const isStale = (error) =>
  error instanceof VocalEditWorkerStaleResultError ||
  (error instanceof Error && error.name === 'StaleResultError');

const errorShape = (error) => ({
  name: error instanceof Error ? error.name : typeof error,
  code: error?.code,
  reason: error?.reason,
  field: error?.field,
  expected: error?.expected,
  actual: error?.actual,
});

const runFlow = async () => {
  const mode = new URL(location.href).searchParams.get('mode') || 'isolated';
  const expectedIsolated = mode === 'isolated';
  const sabAvailable = typeof SharedArrayBuffer !== 'undefined';
  assert(
    crossOriginIsolated === expectedIsolated,
    'isolation mode mismatch: expected ' + expectedIsolated + ', got ' + crossOriginIsolated,
  );

  try {
    await import('/dist/vocal_edit_worker.js');
  } catch (error) {
    throw new Error(
      'vocal edit worker module cannot be imported by the page: ' +
        (error instanceof Error ? error.stack || error.message : String(error)),
    );
  }
  const nativeWorker = new Worker('/dist/vocal_edit_worker.js', { type: 'module' });
  const workerErrors = [];
  window.__workerErrors = workerErrors;
  nativeWorker.addEventListener('error', (event) => {
    workerErrors.push({
      message: event.message,
      filename: event.filename,
      lineno: event.lineno,
      colno: event.colno,
    });
  });
  const worker = new VocalEditWorkerClient({
    worker: nativeWorker,
    terminateWorkerOnDispose: true,
  });
  let session;
  let restored;
  try {
    const source = makeTone();
    const analysis = makeAnalysis();
    session = await worker.create(
      { samples: source, sampleRate, analysis, outputLengthSamples: sampleCount },
      { copy: true },
    );
    assert(
      session.created.notes.notes.length === 1,
      'host analysis did not produce one monophonic note: ' +
        JSON.stringify({
          noteCount: session.created.notes.notes.length,
          frameCount: session.created.analysis.f0Hz.length,
          voicedCount: session.created.analysis.voiced.length,
          algorithmId: session.created.analysis.algorithmId,
          samplesPerFrame: session.created.analysis.samplesPerFrame,
          frameLengthSamples: session.created.analysis.frameLengthSamples,
          referenceHz: session.created.analysis.referenceHz,
          minNoteMs: session.created.analysis.minNoteMs,
          segmentationThresholdCents: session.created.analysis.segmentationThresholdCents,
          firstF0Hz: session.created.analysis.f0Hz[0],
          voicedFrames: Array.from(session.created.analysis.voiced.slice(0, 4)),
          notes: session.created.notes.notes.map((value) => ({
            id: value.id,
            sourceStartSample: value.sourceStartSample,
            sourceEndSample: value.sourceEndSample,
            hasPitch: value.hasPitch,
            medianHz: value.medianHz,
          })),
        }),
    );
    const note = session.created.notes.notes[0];
    assert(note.hasPitch, 'host analysis produced an unpitched note');
    assert(note.sourceEndSample > note.sourceStartSample, 'note source span is empty');
    assert(Number.isFinite(note.centerMidi), 'note center MIDI is not finite');

    const baseline = await session.preview({ requestId: '1' });
    const baselineFrequency = dominantFrequency(baseline.samples);
    assert(Math.abs(baselineFrequency.frequency - sourceFrequency) <= 4, 'baseline pitch mismatch');
    assert(rms(baseline.samples) > 0.05, 'baseline render is silent');

    const targetMidi = note.centerMidi + 2;
    const targetFrequency = 440 * Math.pow(2, (targetMidi - 69) / 12);
    await session.beginEdit();
    const edit = {
      ...note.edit,
      pitch: {
        ...note.edit.pitch,
        target: { mode: 'center', midi: targetMidi },
        amount: 1,
        maxCorrectionSemitones: 12,
        speedMs: 0,
      },
    };
    await session.apply({
      expectedGeneration: session.token().generation,
      operations: [{ kind: 'setEdit', noteId: note.id, edit }],
    });
    const draftPreview = await session.preview({ requestId: '2' });
    const draftFrequency = dominantFrequency(draftPreview.samples);
    const draftDelta = maxAbsDelta(baseline.samples, draftPreview.samples);
    assert(draftDelta > 1e-3, 'pitch edit produced identity PCM');
    assert(
      Math.abs(draftFrequency.frequency - targetFrequency) <= 8,
      'pitch edit did not move the dominant frequency',
    );

    const committed = await session.commit();
    const committedPreview = await session.preview({ requestId: '3' });
    assert(
      maxAbsDelta(draftPreview.samples, committedPreview.samples) <= 1e-6,
      'commit changed the rendered PCM unexpectedly',
    );
    assert(committed.token.revision !== '0', 'commit did not advance the revision');

    let conflict;
    try {
      await session.beginEdit({ expectedRevision: '0' });
      throw new Error('revision conflict unexpectedly succeeded');
    } catch (error) {
      conflict = error;
    }
    assert(isSonareError(conflict), 'revision conflict was not a structured SonareError');
    assert(conflict.code === ErrorCode.InvalidState, 'revision conflict has the wrong error code');
    assert(Number.isInteger(conflict.reason) && conflict.reason > 0, 'missing conflict reason');
    assert(conflict.field === 'revision', 'missing conflict field');
    assert(conflict.expected === '0', 'conflict expected value was not serialized');
    assert(conflict.actual === session.token().revision, 'conflict actual value was not serialized');

    const staleTask = session.preview({ requestId: '10' });
    const freshTask = session.preview({ requestId: '11' });
    let staleError;
    try {
      await staleTask.result;
    } catch (error) {
      staleError = error;
    }
    const fresh = await freshTask.result;
    assert(isStale(staleError), 'superseded preview did not reject as stale');
    assert(fresh.samples.length === committedPreview.samples.length, 'fresh preview is incomplete');

    const cancelTask = session.preview({
      range: { startSample: 0, endSample: sampleCount },
      requestId: '12',
    });
    setTimeout(() => cancelTask.cancel(), 0);
    let cancelError;
    try {
      await cancelTask.result;
    } catch (error) {
      cancelError = error;
    }
    assert(cancelError instanceof Error && cancelError.name === 'AbortError', 'render cancellation failed');

    const exported = await session.exportState();
    assert(exported.data instanceof Uint8Array && exported.data.length > 32, 'state export is empty');
    restored = await worker.restore(
      { samples: source, sampleRate, state: exported.data.slice() },
      { copy: true },
    );
    assert(restored.created.notes.notes.length === 1, 'restore lost the monophonic note');
    const restoredPreview = await restored.preview({ requestId: '13' });
    assert(
      maxAbsDelta(committedPreview.samples, restoredPreview.samples) <= 1e-6,
      'restore did not recover committed PCM',
    );

    return {
      mode,
      crossOriginIsolated,
      sharedArrayBufferAvailable: sabAvailable,
      transport: sabAvailable ? 'shared-array-buffer' : 'post-message-fallback',
      noteCount: session.created.notes.notes.length,
      baselineFrequency: baselineFrequency.frequency,
      editedFrequency: draftFrequency.frequency,
      targetFrequency,
      maxPitchEditDelta: draftDelta,
      committedRevision: committed.token.revision,
      stateBytes: exported.data.length,
      staleRejected: true,
      cancellationRejected: true,
      conflict: errorShape(conflict),
    };
  } finally {
    if (restored) {
      try {
        await restored.dispose();
      } catch {}
    }
    if (session) {
      try {
        await session.dispose();
      } catch {}
    }
    worker.dispose();
  }
};

try {
  const measured = await runFlow();
  result.dataset.status = 'ok';
  result.textContent = JSON.stringify(measured);
} catch (error) {
  result.dataset.status = 'error';
  result.textContent = JSON.stringify({
    error: error instanceof Error ? error.stack || error.message : String(error),
    workerErrors: window.__workerErrors || [],
  });
}
</script>`;
}

async function startServer() {
  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url ?? '/', 'http://localhost');
      if (url.pathname === '/smoke.html') {
        res.writeHead(200, headers('text/html', url.searchParams.get('mode') === 'isolated'));
        res.end(harness());
        return;
      }
      if (url.pathname.startsWith('/dist/')) {
        const file = path.join(dist, url.pathname.slice('/dist/'.length));
        // Keep worker/module responses CORP-compatible for the isolated page;
        // these response headers do not opt a nonisolated document in.
        res.writeHead(200, headers(contentType(file), true));
        res.end(await readFile(file));
        return;
      }
      res.writeHead(404, headers('text/plain', false));
      res.end('not found');
    } catch (error) {
      res.writeHead(500, headers('text/plain', false));
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
      if (state?.status && state.status !== 'running') {
        return state;
      }
      await sleep(25);
    }
    throw new Error(`vocal edit worker smoke timed out: ${JSON.stringify(state)}\n${stderr}`);
  } finally {
    cdp?.close();
    await stopChrome(chrome);
  }
}

async function main() {
  const chromePath = await findChrome();
  const server = await startServer();
  const port = server.address().port;
  try {
    const results = [];
    for (const mode of ['isolated', 'nonisolated']) {
      const userDataDir = await mkdtemp(path.join(os.tmpdir(), 'sonare-vocal-worker-chrome-'));
      try {
        const state = await runBrowserSmoke(
          chromePath,
          `http://127.0.0.1:${port}/smoke.html?mode=${mode}`,
          userDataDir,
        );
        if (state.status !== 'ok') {
          throw new Error(
            `vocal edit worker smoke failed (${mode}): ${state.text}`,
          );
        }
        const result = JSON.parse(state.text);
        const expectedIsolation = mode === 'isolated';
        if (result.mode !== mode || result.crossOriginIsolated !== expectedIsolation) {
          throw new Error(`vocal edit worker isolation mismatch (${mode}): ${state.text}`);
        }
        if (!result.staleRejected || !result.cancellationRejected || result.noteCount !== 1) {
          throw new Error(`vocal edit worker smoke incomplete (${mode}): ${state.text}`);
        }
        results.push(result);
      } finally {
        await rm(userDataDir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
      }
    }
    console.log(JSON.stringify(results, null, 2));
  } finally {
    server.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
