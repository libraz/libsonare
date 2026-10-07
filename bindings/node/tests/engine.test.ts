import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { describe, expect, it } from 'vitest';
import type { EngineBounceOptions } from '../src/index.js';
import {
  Audio,
  ErrorCode,
  engineAbiVersion,
  isSonareError,
  MarkerKind,
  RealtimeEngine,
  voiceChangerAbiVersion,
} from '../src/index.js';
import { midi1Word, rms } from './_engine_signals.js';

describe('RealtimeEngine native binding', () => {
  /** Largest per-sample deviation between two equal-length renders. */
  const maxAbsDiff = (a: number[], b: number[]): number => {
    expect(a.length).toBe(b.length);
    let worst = 0;
    for (let i = 0; i < a.length; i++) {
      worst = Math.max(worst, Math.abs(a[i] - b[i]));
    }
    return worst;
  };

  it('exposes engine ABI version', () => {
    expect(engineAbiVersion()).toBeGreaterThan(0);
  });

  it('exposes voice changer ABI version', () => {
    const v = voiceChangerAbiVersion();
    expect(Number.isFinite(v)).toBe(true);
    expect(v).toBeGreaterThan(0);
  });

  it('rejects nonfinite and out-of-range engine sample rates', () => {
    for (const sampleRate of [Number.NaN, 7999, 384001]) {
      expect(() => new RealtimeEngine(sampleRate, 128)).toThrow();
    }
    const engine = new RealtimeEngine(48000, 128);
    try {
      for (const sampleRate of [Number.NaN, 7999, 384001]) {
        expect(() => engine.prepare(sampleRate, 128)).toThrow();
      }
    } finally {
      engine.destroy();
    }
  });

  it('accepts a declared channel capacity and rejects invalid capacities', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() => engine.prepare(48000, 128, 1024, 1024, 2)).not.toThrow();
      expect(() => engine.prepare(48000, 128, 1024, 1024, 0)).toThrow();
      expect(() => engine.prepare(48000, 128, 1024, 1024, 65)).toThrow();
    } finally {
      engine.destroy();
    }
    expect(() => new RealtimeEngine(48000, 128, 1024, 1024, 0)).toThrow();
  });

  it('puts the channel ceiling exactly where the engine constant puts it', () => {
    // The case above spells 65, which keeps passing if the ceiling moves down
    // and only ever asserts "something above 64 is refused". Derive the boundary
    // from kMaxAudioChannels instead, so lowering the constant fails here rather
    // than leaving a green test pinning a value the engine no longer uses.
    const header = readFileSync(
      new URL('../../../src/engine/realtime_engine.h', import.meta.url).pathname,
      'utf8',
    );
    const declared = header.match(/kMaxAudioChannels\s*=\s*(\d+);/)?.[1];
    // Self-check: the assertions below are vacuous if the constant moved or the
    // regex stopped matching.
    expect(declared, 'kMaxAudioChannels in src/engine/realtime_engine.h').toBeDefined();
    const maxChannels = Number(declared);
    expect(maxChannels).toBeGreaterThan(2);

    const engine = new RealtimeEngine(48000, 128);
    try {
      expect(() => engine.prepare(48000, 128, 1024, 1024, maxChannels)).not.toThrow();
      expect(() => engine.prepare(48000, 128, 1024, 1024, maxChannels + 1)).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('reports oversized process channels after prepare as max-channel telemetry', () => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.prepare(48000, 128, 1024, 1024, 2);
      engine.process([new Float32Array(128), new Float32Array(128), new Float32Array(128)]);

      expect(engine.drainTelemetry()).toContainEqual(
        expect.objectContaining({ error: 20, value: 3 }),
      );
    } finally {
      engine.destroy();
    }
  });

  it('keeps Audio factories usable after a worker loads and exits the addon', async () => {
    const addonPath = join(process.cwd(), 'build', 'Release', 'sonare-node.node');
    await new Promise<void>((resolve, reject) => {
      const worker = new Worker(`require(${JSON.stringify(addonPath)});`, { eval: true });
      worker.once('error', reject);
      worker.once('exit', (code) => {
        if (code === 0) {
          resolve();
        } else {
          reject(new Error(`worker exited with code ${code}`));
        }
      });
    });

    const audio = Audio.fromBuffer(new Float32Array([0.25, -0.25]), 48000);
    expect(audio.getLength()).toBe(2);
    audio.destroy();
  });

  it('installs tempo and time-signature segments and validates them', () => {
    const engine = new RealtimeEngine(48000, 128);
    // Valid ramp then a valid time-signature map.
    engine.setTempoSegments([
      { startPpq: 0, bpm: 120 },
      { startPpq: 1920, bpm: 120, endBpm: 140 },
    ]);
    engine.setTimeSignatureSegments([
      { startPpq: 0, numerator: 4, denominator: 4 },
      { startPpq: 1920, numerator: 3, denominator: 4 },
    ]);
    // Empty arrays clear the maps without error.
    engine.setTempoSegments([]);
    engine.setTimeSignatureSegments([]);
    // Invalid input is rejected (matching the C ABI and Python).
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 0 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: Number.NaN, bpm: 120 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 100000.1 }])).toThrow();
    expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: 100000.1 }])).toThrow();
    expect(() => engine.setTempo(100000)).not.toThrow();
    expect(() => engine.setTempo(100000.1)).toThrow();
    expect(() =>
      engine.setTimeSignatureSegments([{ startPpq: 0, numerator: 0, denominator: 4 }]),
    ).toThrow();
  });

  it('rejects hostile metronome click lengths', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setMetronome({ enabled: true, clickSamples: 2_000_000_000 })).toThrow();
    expect(() =>
      engine.setMetronome({ enabled: true, clickSamples: 0, clickSeconds: 2 }),
    ).toThrow();
    expect(() =>
      engine.setMetronome({ enabled: true, clickSamples: 0, clickSeconds: Number.NaN }),
    ).toThrow();
  });

  it('rejects out-of-range automation curve ordinals', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.addParameter({
      id: 9,
      name: 'gain',
      unit: 'dB',
      minValue: 0,
      maxValue: 1,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0,
    });
    // An out-of-range breakpoint curve is rejected, not clamped.
    // @ts-expect-error deliberately out-of-range curve ordinal; the addon must reject it.
    expect(() => engine.setAutomationLane(9, [{ ppq: 0, value: 0.5, curveToNext: 99 }])).toThrow();
    // An out-of-range default curve is rejected on registration.
    expect(() =>
      engine.addParameter({
        id: 10,
        name: 'pan',
        unit: '',
        minValue: -1,
        maxValue: 1,
        defaultValue: 0,
        rtSafe: true,
        // @ts-expect-error deliberately out-of-range curve ordinal; the addon must reject it.
        defaultCurve: 99,
      }),
    ).toThrow();
  });

  it('rejects a malformed parameter object without registering a bogus parameter', () => {
    const engine = new RealtimeEngine(48000, 128);
    const before = engine.parameterCount();
    // A non-number id must be rejected outright (previously it coerced to 0 and
    // still registered a zero-value parameter).
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: intentionally malformed input
      engine.addParameter({ name: 'bad', unit: '', defaultCurve: 0 } as any),
    ).toThrow();
    // A wrong-typed numeric field must also be rejected, not silently defaulted.
    expect(() =>
      engine.addParameter({
        id: 42,
        name: 'gain',
        unit: 'dB',
        // biome-ignore lint/suspicious/noExplicitAny: intentionally malformed input
        minValue: 'loud' as any,
        maxValue: 1,
        defaultValue: 0,
        rtSafe: true,
        defaultCurve: 0,
      }),
    ).toThrow();
    // Neither malformed call may have leaked a registration.
    expect(engine.parameterCount()).toBe(before);
  });

  it('processes a block and drains telemetry', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setTempo(60);
    engine.setTimeSignature(3, 4);
    engine.setMarkers([
      { id: 11, ppq: 1, name: 'intro' },
      { id: 12, ppq: 2, name: 'out' },
    ]);
    expect(engine.markerCount()).toBe(2);
    expect(engine.markerByIndex(0).name).toBe('intro');
    expect(engine.marker(12).ppq).toBe(2);
    engine.setLoopFromMarkers(11, 12);
    engine.setMetronome({ enabled: true, beatGain: 0.25, accentGain: 0.75, clickSamples: 16 });
    expect(engine.metronome().enabled).toBe(true);
    expect(engine.metronome().clickSamples).toBe(16);
    engine.setMetronome({ enabled: true });
    expect(engine.metronome().clickSamples).toBe(0);
    expect(engine.countInEndSample(0, 2)).toBe(288000);
    engine.setMetronome({ enabled: false });
    engine.addParameter({
      id: 7,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0, // canonical AutomationCurve::Linear
    });
    expect(engine.parameterCount()).toBe(1);
    expect(engine.parameterInfo(7).name).toBe('gain');
    expect(engine.parameterInfoByIndex(0).unit).toBe('dB');
    engine.setAutomationLane(7, [
      { ppq: 0, value: 0 },
      { ppq: 1, value: 6.0205999, curveToNext: 0 }, // Linear
    ]);
    expect(engine.automationLaneCount()).toBe(1);
    engine.setGraph({
      nodes: [
        { id: 'in', numPorts: 2 },
        { id: 'gain', type: 1, gainDb: 0, numPorts: 2 },
        { id: 'out', numPorts: 2 },
      ],
      connections: [
        { sourceNode: 'in', sourcePort: 0, destNode: 'gain', destPort: 0 },
        { sourceNode: 'in', sourcePort: 1, destNode: 'gain', destPort: 1 },
        { sourceNode: 'gain', sourcePort: 0, destNode: 'out', destPort: 0 },
        { sourceNode: 'gain', sourcePort: 1, destNode: 'out', destPort: 1 },
      ],
      inputNode: 'in',
      outputNode: 'out',
      numChannels: 2,
      parameterBindings: [{ paramId: 7, nodeId: 'gain' }],
    });
    expect(engine.graphNodeCount()).toBe(3);
    expect(engine.graphConnectionCount()).toBe(4);
    engine.setClips([
      {
        id: 101,
        channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.125)],
        startPpq: 1,
        lengthSamples: 128,
      },
    ]);
    expect(engine.clipCount()).toBe(1);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);
    engine.setCapturePunch(48000, 48128);
    engine.armCapture();
    engine.seekMarker(11);
    engine.play();

    const left = new Float32Array(128).fill(0.25);
    const right = new Float32Array(128).fill(-0.25);
    const processed = engine.process([left, right]);

    expect(processed).toHaveLength(2);
    expect(processed[0]).not.toBe(left);
    expect(processed[1]).not.toBe(right);
    expect(processed[0][0]).toBeCloseTo(0.75, 4);
    expect(processed[1][0]).toBeCloseTo(-0.75, 4);
    expect(left[0]).toBeCloseTo(0.25, 4);
    expect(right[0]).toBeCloseTo(-0.25, 4);
    const captureStatus = engine.captureStatus();
    expect(captureStatus.capturedFrames).toBe(128);
    expect(captureStatus.overflowCount).toBe(0);
    expect(captureStatus.armed).toBe(true);
    expect(captureStatus.source).toBe('output');
    expect(captureStatus.recordOffsetSamples).toBe(0);
    const captured = engine.capturedAudio();
    expect(captureLeft[0]).toBe(0);
    expect(captureRight[0]).toBe(0);
    expect(captured[0][0]).toBeCloseTo(0.75, 4);
    expect(captured[1][0]).toBeCloseTo(-0.75, 4);
    engine.resetCapture();
    expect(engine.captureStatus().capturedFrames).toBe(0);

    const telemetry = engine.drainTelemetry();
    expect(telemetry.length).toBeGreaterThan(0);
    expect(telemetry.at(-1)?.type).toBe(0);
    expect(telemetry.at(-1)?.error).toBe(0);
    expect(telemetry.at(-1)?.renderFrame).toBe(0);
    expect(telemetry.at(-1)?.timelineSample).toBe(48000 + 128);

    engine.setCaptureSource('input');
    engine.setRecordOffsetSamples(-37);
    engine.armCapture();
    engine.seekMarker(11);
    engine.process([left, right]);
    const inputCaptureStatus = engine.captureStatus();
    expect(inputCaptureStatus.source).toBe('input');
    expect(inputCaptureStatus.recordOffsetSamples).toBe(-37);
    expect(captureLeft[0]).toBe(0);
    expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.25, 4);
    expect(engine.drainMeterTelemetry().some((record) => record.targetId === 0xffff)).toBe(true);

    engine.setCaptureSource(0);
    expect(engine.captureStatus().source).toBe('output');
    expect(captureRight[0]).toBe(0);
    expect(engine.capturedAudio()[1][0]).toBeCloseTo(-0.25, 4);

    engine.setInputMonitor(false);
    engine.resetCapture();
    engine.armCapture();
    engine.seekMarker(11);
    let monitored = engine.process([
      new Float32Array(128).fill(0.25),
      new Float32Array(128).fill(-0.25),
    ]);
    expect(monitored[0][0]).toBeCloseTo(0.25, 4);
    expect(monitored[1][0]).toBeCloseTo(-0.25, 4);
    expect(captureLeft[0]).toBe(0);
    expect(engine.capturedAudio()[0][0]).toBeCloseTo(0.25, 4);

    engine.setInputMonitor(true, 0.5);
    engine.seekMarker(11);
    monitored = engine.process([
      new Float32Array(128).fill(0.25),
      new Float32Array(128).fill(-0.25),
    ]);
    expect(monitored[0][0]).toBeCloseTo(0.5, 4);
    expect(monitored[1][0]).toBeCloseTo(-0.5, 4);
    let badMonitorGainError: unknown;
    try {
      engine.setInputMonitor(true, Number.NaN);
    } catch (error) {
      badMonitorGainError = error;
    }
    expect(isSonareError(badMonitorGainError)).toBe(true);
    if (!isSonareError(badMonitorGainError)) {
      throw new Error('expected SonareError');
    }
    expect(badMonitorGainError.code).toBe(ErrorCode.InvalidParameter);

    engine.destroy();
  });

  it('configures and drains scope telemetry', () => {
    const sampleRate = 48000;
    const blockSize = 256;
    const engine = new RealtimeEngine(sampleRate, blockSize);
    expect(engine.configureScopeTelemetry(256, 32)).toBe(32);

    const toneFreq = 1000;
    const frames = blockSize * 16;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      const sample = 0.5 * Math.sin((2 * Math.PI * toneFreq * i) / sampleRate);
      left[i] = sample;
      right[i] = sample;
    }
    engine.setClips([
      {
        id: 1,
        trackId: 10,
        channels: [left, right],
        startPpq: 0,
        lengthSamples: frames,
      },
    ]);
    engine.setTrackLanes([10]);
    engine.play();

    let records: ReturnType<typeof engine.drainScopeTelemetry> = [];
    for (let block = 0; block < 12; block++) {
      engine.process([new Float32Array(blockSize), new Float32Array(blockSize)]);
      records = records.concat(engine.drainScopeTelemetry());
    }

    const master = records.find((record) => record.targetId === 0);
    expect(master).toBeDefined();
    if (!master) {
      throw new Error('expected a master scope record');
    }
    expect(master.bands).toHaveLength(32);
    const argmax = master.bands.reduce(
      (best, value, index) => (value > master.bands[best] ? index : best),
      0,
    );
    expect(argmax).toBeLessThanOrEqual(2);
    expect(master.points.length).toBeGreaterThan(0);

    engine.destroy();
  });

  it('round-trips marker kind and key signature', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setMarkers([
      { id: 1, ppq: 0, name: 'intro' },
      {
        id: 2,
        ppq: 4,
        name: 'G major',
        kind: MarkerKind.KeySignature,
        keyFifths: 1,
        keyMinor: false,
      },
      {
        id: 3,
        ppq: 8,
        name: 'A minor',
        kind: MarkerKind.KeySignature,
        keyFifths: 0,
        keyMinor: true,
      },
    ]);
    // A marker with no kind defaults to MarkerKind.Marker with neutral key fields.
    const intro = engine.markerByIndex(0);
    expect(intro.kind).toBe(MarkerKind.Marker);
    expect(intro.keyFifths).toBe(0);
    expect(intro.keyMinor).toBe(false);

    const gMajor = engine.marker(2);
    expect(gMajor.kind).toBe(MarkerKind.KeySignature);
    expect(gMajor.keyFifths).toBe(1);
    expect(gMajor.keyMinor).toBe(false);

    const aMinor = engine.markerByIndex(2);
    expect(aMinor.kind).toBe(MarkerKind.KeySignature);
    expect(aMinor.keyFifths).toBe(0);
    expect(aMinor.keyMinor).toBe(true);
    engine.destroy();
  });

  it('offline render matches repeated process output', () => {
    const frames = 256;
    const left = new Float32Array(frames);
    const right = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      left[i] = Math.sin(i * 0.01);
      right[i] = -left[i];
    }

    const realtime = new RealtimeEngine(48000, 128);
    realtime.play();
    const rtLeft: number[] = [];
    const rtRight: number[] = [];
    for (let offset = 0; offset < frames; offset += 128) {
      const block = realtime.process([
        left.slice(offset, offset + 128),
        right.slice(offset, offset + 128),
      ]);
      rtLeft.push(...block[0]);
      rtRight.push(...block[1]);
    }
    realtime.destroy();

    const offline = new RealtimeEngine(48000, 128);
    offline.play();
    const rendered = offline.renderOffline([left, right], 128);
    offline.destroy();

    expect(Array.from(rendered[0])).toEqual(rtLeft);
    expect(Array.from(rendered[1])).toEqual(rtRight);

    const bounce = new RealtimeEngine(48000, 128);
    bounce.play();
    const bounced = bounce.bounceOffline({
      totalFrames: 256,
      blockSize: 128,
      numChannels: 2,
      sourceSampleRate: 48000,
      targetSampleRate: 24000,
    });
    bounce.destroy();

    expect(bounced.frames).toBe(128);
    expect(bounced.numChannels).toBe(2);
    expect(bounced.sampleRate).toBe(24000);
    expect(bounced.interleaved.length).toBe(256);
    // Silent input integrates to -Infinity LUFS, which is the correct gating
    // result for digital silence; just assert a numeric value is reported.
    expect(typeof bounced.integratedLufs).toBe('number');

    const freeze = new RealtimeEngine(48000, 128);
    freeze.setClips([
      {
        id: 7,
        channels: [new Float32Array(128).fill(0.125), new Float32Array(128).fill(-0.25)],
        startPpq: 0,
        lengthSamples: 128,
      },
    ]);
    freeze.play();
    const frozen = freeze.freezeOffline({
      totalFrames: 128,
      blockSize: 128,
      numChannels: 2,
      clipId: 77,
    });
    expect(frozen.clipId).toBe(77);
    expect(frozen.frames).toBe(128);
    expect(frozen.numChannels).toBe(2);
    expect(freeze.clipCount()).toBe(1);
    freeze.seekSample(0);
    const frozenRendered = freeze.renderOffline(
      [new Float32Array(128), new Float32Array(128)],
      128,
    );
    freeze.destroy();
    expect(frozenRendered[0][0]).toBeCloseTo(0.125, 4);
    expect(frozenRendered[1][0]).toBeCloseTo(-0.25, 4);
  });

  it('bounces at the prepared rate by default and still refuses a different source rate', () => {
    const engine = new RealtimeEngine(44100, 128);
    try {
      const result = engine.bounceOffline({ totalFrames: 441, blockSize: 128 });
      expect(result.sampleRate).toBe(44100);
      expect(result.frames).toBe(441);

      const resampled = engine.bounceOffline({
        totalFrames: 44100,
        blockSize: 128,
        targetSampleRate: 48000,
      });
      expect(resampled.sampleRate).toBe(48000);
      expect(resampled.frames).toBe(48000);

      expect(() =>
        engine.bounceOffline({ totalFrames: 441, blockSize: 128, sourceSampleRate: 48000 }),
      ).toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('bounces clip content, hits the loudness target and applies dither', () => {
    const frames = 256;
    const amplitude = 0.5;
    const tone = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      tone[i] = amplitude * Math.sin((2 * Math.PI * 440 * i) / 48000);
    }

    const bounce = (
      options: Partial<EngineBounceOptions> = {},
    ): { samples: Float32Array; lufs: number } => {
      const engine = new RealtimeEngine(48000, 128);
      engine.setClips([
        { id: 1, channels: [tone, tone.slice()], startPpq: 0, lengthSamples: frames },
      ]);
      engine.play();
      const result = engine.bounceOffline({
        totalFrames: frames,
        blockSize: 128,
        numChannels: 2,
        sourceSampleRate: 48000,
        targetSampleRate: 48000,
        ...options,
      });
      engine.destroy();
      return { samples: result.interleaved, lufs: result.integratedLufs };
    };

    const peak = (samples: Float32Array): number =>
      samples.reduce((acc, sample) => Math.max(acc, Math.abs(sample)), 0);
    const maxAbsDiff = (a: Float32Array, b: Float32Array): number =>
      a.reduce((acc, sample, index) => Math.max(acc, Math.abs(sample - b[index])), 0);

    // The scheduled clip must actually reach the bounce: a bounce that renders
    // silence would satisfy every shape assertion while exporting nothing.
    const plain = bounce();
    expect(plain.samples.length).toBe(frames * 2);
    expect(peak(plain.samples)).toBeCloseTo(amplitude, 3);
    expect(plain.samples.some((sample) => sample !== 0)).toBe(true);
    expect(Number.isFinite(plain.lufs)).toBe(true);

    // Requested loudness is reached, and the documented 0 sentinel resolves to
    // the shared -14 LUFS default rather than to a literal 0 LUFS target.
    for (const targetLufs of [-20, -9]) {
      const normalized = bounce({ normalizeLufs: true, targetLufs });
      expect(normalized.lufs).toBeCloseTo(targetLufs, 1);
    }
    expect(bounce({ normalizeLufs: true, targetLufs: 0 }).lufs).toBeCloseTo(-14, 1);

    // Dither types are identified by what they do to the signal rather than by
    // the integer alone, so a remapped type cannot pass: RPDF and TPDF add
    // noise without quantizing (TPDF is the wider of the two, being the sum of
    // two uniform draws), and only the noise-shaped type quantizes onto the
    // target-depth grid.
    const bits = 8;
    const lsb = 1 / (1 << (bits - 1));
    const none = bounce({ dither: 0, ditherBits: bits, ditherSeed: 1 });
    const rpdf = bounce({ dither: 1, ditherBits: bits, ditherSeed: 1 });
    const tpdf = bounce({ dither: 2, ditherBits: bits, ditherSeed: 1 });
    const shaped = bounce({ dither: 3, ditherBits: bits, ditherSeed: 1 });
    const onGrid = (samples: Float32Array): number =>
      samples.reduce(
        (acc, sample) => acc + (Math.abs(sample / lsb - Math.round(sample / lsb)) < 1e-4 ? 1 : 0),
        0,
      );

    expect(maxAbsDiff(none.samples, plain.samples)).toBe(0);
    expect(maxAbsDiff(rpdf.samples, none.samples)).toBeGreaterThan(lsb / 4);
    expect(maxAbsDiff(tpdf.samples, none.samples)).toBeGreaterThan(
      maxAbsDiff(rpdf.samples, none.samples),
    );
    // Every non-None type lands the output on the target-depth grid -- that is
    // what the shared bit-depth field means -- so the type selects only the
    // noise, the triangular one being wider than the rectangular.
    expect(onGrid(rpdf.samples)).toBe(rpdf.samples.length);
    expect(onGrid(tpdf.samples)).toBe(tpdf.samples.length);
    expect(onGrid(shaped.samples)).toBe(shaped.samples.length);

    // A fixed seed is reproducible and a different seed is not, so the seed
    // reaches the generator instead of being dropped.
    const repeat = bounce({ dither: 2, ditherBits: bits, ditherSeed: 1 });
    expect(Array.from(repeat.samples)).toEqual(Array.from(tpdf.samples));
    const otherSeed = bounce({ dither: 2, ditherBits: bits, ditherSeed: 2 });
    expect(maxAbsDiff(otherSeed.samples, tpdf.samples)).toBeGreaterThan(0);
  });

  it('exposes transport state and live parameter injection', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setTempo(60);
    expect(engine.sampleAtPpq(1.5)).toBe(72000);
    let badPpqError: unknown;
    try {
      engine.sampleAtPpq(Number.NaN);
    } catch (error) {
      badPpqError = error;
    }
    expect(isSonareError(badPpqError)).toBe(true);
    if (!isSonareError(badPpqError)) {
      throw new Error('expected SonareError');
    }
    expect(badPpqError.code).toBe(ErrorCode.InvalidParameter);
    expect(() => engine.sampleAtPpq(1e300)).toThrow();
    expect(() => engine.seekPpq(1e300)).toThrow();
    expect(() => engine.setLoop(0, 1e300, true)).toThrow();
    engine.setTempo(90);
    engine.addParameter({
      id: 3,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0, // canonical AutomationCurve::Linear
    });
    // Live parameter injection must not throw.
    engine.setParameter(3, 3.0);
    engine.setParameterSmoothed(3, -3.0, -1);

    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);

    const transport = engine.getTransportState();
    expect(transport.playing).toBe(true);
    expect(transport.isPlaying).toBe(true);
    expect(transport.bpm).toBeCloseTo(90, 3);
    expect(transport.sampleRate).toBeCloseTo(48000, 1);
    expect(transport.samplePosition).toBeGreaterThanOrEqual(128);
    expect(Number.isFinite(transport.ppq)).toBe(true);
    // Musical beat readout: `beat` is one-based, `beatFraction` in [0, 1).
    expect(Number.isInteger(transport.beat)).toBe(true);
    expect(transport.beat).toBeGreaterThanOrEqual(1);
    expect(transport.beatFraction).toBeGreaterThanOrEqual(0);
    expect(transport.beatFraction).toBeLessThan(1);

    const meterRecords = engine.drainMeterTelemetry();
    expect(Array.isArray(meterRecords)).toBe(true);
    for (const record of meterRecords) {
      expect(typeof record.peakDbL).toBe('number');
      expect(typeof record.peakDbR).toBe('number');
      expect(typeof record.rmsDbL).toBe('number');
      expect(typeof record.rmsDbR).toBe('number');
      expect(Number.isFinite(record.integratedLufs)).toBe(true);
    }
    engine.destroy();
  });

  // A pad that outlives the render span: note-on at frame 0 and no note-off, so
  // whether it is still sounding at the end of a chunk is exactly the state a
  // non-finalizing render has to preserve.
  const padEngine = (): RealtimeEngine => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);
    engine.setMidiClips([
      {
        id: 1,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 1 << 20,
        events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 }],
      },
    ]);
    engine.play();
    return engine;
  };

  it('renderOffline accepts a request object identically to the positional form', () => {
    const frames = 2048;
    const positionalEngine = padEngine();
    const positional = positionalEngine.renderOffline(
      [new Float32Array(frames), new Float32Array(frames)],
      128,
    );
    positionalEngine.destroy();

    const requestEngine = padEngine();
    const request = requestEngine.renderOffline({
      channels: [new Float32Array(frames), new Float32Array(frames)],
      blockSize: 128,
    });
    requestEngine.destroy();

    // Non-vacuity: comparing two silent buffers would prove nothing.
    expect(rms(positional[0])).toBeGreaterThan(0);
    expect(maxAbsDiff(Array.from(request[0]), Array.from(positional[0]))).toBeLessThan(1e-6);
    expect(maxAbsDiff(Array.from(request[1]), Array.from(positional[1]))).toBeLessThan(1e-6);
  });

  it('renderOffline chunks concatenate to one continuous render', () => {
    const chunk = 4096;
    const chunks = 3;
    const total = chunk * chunks;

    const continuousEngine = padEngine();
    const [continuous] = continuousEngine.renderOffline([
      new Float32Array(total),
      new Float32Array(total),
    ]);
    continuousEngine.destroy();
    expect(rms(continuous)).toBeGreaterThan(0);

    const renderChunks = (finalize: boolean): number[] => {
      const engine = padEngine();
      const joined: number[] = [];
      for (let i = 0; i < chunks; i++) {
        const [left] = engine.renderOffline({
          channels: [new Float32Array(chunk), new Float32Array(chunk)],
          blockSize: 128,
          finalize,
        });
        joined.push(...left);
      }
      engine.finishOfflineRender();
      engine.destroy();
      return joined;
    };

    const reference = Array.from(continuous);
    expect(maxAbsDiff(renderChunks(false), reference)).toBeLessThan(1e-6);

    // Non-vacuity: finalizing every chunk is the defect the flag exists to fix.
    // The pad's note-off fires at the end of chunk 1 and no note-on is re-sent,
    // so the tail decays away instead of holding and the concatenation diverges.
    const finalizedPerChunk = renderChunks(true);
    const lastChunkRms = (samples: number[]): number => {
      const tail = samples.slice(total - chunk);
      return Math.sqrt(tail.reduce((sum, value) => sum + value * value, 0) / tail.length);
    };
    expect(maxAbsDiff(finalizedPerChunk, reference)).toBeGreaterThan(1e-3);
    expect(lastChunkRms(finalizedPerChunk)).toBeLessThan(0.5 * lastChunkRms(reference));
  });

  it('refuses an offline block size above the prepared block size', () => {
    const frames = 256;
    const planes = () => [new Float32Array(frames), new Float32Array(frames)];
    const engine = new RealtimeEngine(48000, 128);
    try {
      const refused = (run: () => unknown): void => {
        let caught: unknown;
        try {
          run();
        } catch (error) {
          caught = error;
        }
        expect(isSonareError(caught)).toBe(true);
        if (isSonareError(caught)) {
          expect(caught.code).toBe(ErrorCode.InvalidParameter);
        }
      };
      refused(() => engine.renderOffline(planes(), 129));
      refused(() => engine.renderOffline({ channels: planes(), blockSize: 129 }));
      refused(() => engine.primeOfflineParameters(2, 129));
      refused(() => engine.bounceOffline({ totalFrames: frames, blockSize: 129 }));
      refused(() => engine.freezeOffline({ totalFrames: frames, blockSize: 129 }));
      // The prepared block and anything smaller still render.
      expect(() => engine.renderOffline(planes(), 128)).not.toThrow();
      expect(() => engine.renderOffline(planes(), 64)).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('accepts a custom smoothed-parameter ramp time and rejects bad values', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setParamSmoothingMs(0)).not.toThrow();
    expect(() => engine.setParamSmoothingMs(75)).not.toThrow();
    expect(() => engine.setParamSmoothingMs(-1)).toThrow();
    expect(() => engine.setParamSmoothingMs(Number.NaN)).toThrow();
    engine.destroy();
  });

  it('settles insert parameters and applies due commands without throwing', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.settleInsertParameters()).not.toThrow();
    expect(() => engine.applyCommandsDueNowPreservingFuture()).not.toThrow();
    engine.destroy();
  });

  it('clearing an automation lane with no points removes it', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.addParameter({
      id: 7,
      name: 'gain',
      unit: 'dB',
      minValue: -60,
      maxValue: 12,
      defaultValue: 0,
      rtSafe: true,
      defaultCurve: 0,
    });
    engine.setAutomationLane(7, [
      { ppq: 0, value: 0 },
      { ppq: 1, value: 6 },
    ]);
    expect(engine.automationLaneCount()).toBe(1);
    engine.setAutomationLane(7, []);
    expect(engine.automationLaneCount()).toBe(0);
    engine.destroy();
  });
});
