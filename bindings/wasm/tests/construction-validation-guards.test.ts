/**
 * WASM guards that come from the core rather than from a per-surface copy.
 *
 * The WASM build does not link the C-ABI translation unit, so historically each
 * of these checks was hand-copied next to a "mirror the C-ABI oracle" comment
 * and drifted whenever only one side was updated. The rules now live where the
 * value is constructed — `validate_config` for the analysis / STFT / stem
 * configs, `RealtimeEngine::render_offline` for the prepared-channel
 * precondition, and the shared `wasmArrayLikeLength` guard for caller-supplied
 * JS lengths — so these assert that the WASM facade reports them, not that it
 * re-implements them.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  type EngineAutomationPoint,
  ErrorCode,
  fixFrames,
  init,
  isSonareError,
  RealtimeEngine,
  type SonareError,
  waveformPeakPyramid,
} from '../dist/index.js';

const SR = 22050;

type AnalyzeOptions = NonNullable<Parameters<typeof analyze>[2]>;

beforeAll(async () => {
  await init();
});

function makeSine(seconds: number, freq: number, sampleRate = SR): Float32Array {
  const n = Math.floor(seconds * sampleRate);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.25 * Math.sin((2 * Math.PI * freq * i) / sampleRate);
  }
  return out;
}

function expectInvalidParameter(fn: () => unknown): void {
  let caught: unknown;
  try {
    fn();
  } catch (e) {
    caught = e;
  }
  expect(caught, 'expected a SonareError, got no throw').toBeDefined();
  expect(isSonareError(caught)).toBe(true);
  expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
}

function midi1Word(status: number, channel: number, data1: number, data2: number): number {
  return (
    (0x2 << 28) |
    ((status & 0xf) << 20) |
    ((channel & 0xf) << 16) |
    ((data1 & 0x7f) << 8) |
    (data2 & 0x7f)
  );
}

describe('offline render honours the prepared channel count', () => {
  // A request wider than prepare() reserved used to silence every plane and
  // hand back a result object (frames / numChannels / integratedLufs computed
  // from that silence) while Node and Python errored on the same call.
  const preparedChannels = 2;
  const requestedChannels = 6;

  it('renderOffline rejects more planes than were prepared', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024, preparedChannels);
    const channels = Array.from({ length: requestedChannels }, () => new Float32Array(128));
    expectInvalidParameter(() => engine.renderOffline(channels, 128));
    engine.destroy();
  });

  it('bounceOffline rejects more planes than were prepared', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024, preparedChannels);
    expectInvalidParameter(() =>
      engine.bounceOffline({
        totalFrames: 256,
        blockSize: 128,
        numChannels: requestedChannels,
        sourceSampleRate: 48000,
        targetSampleRate: 48000,
      }),
    );
    engine.destroy();
  });

  it('freezeOffline rejects more planes than were prepared', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024, preparedChannels);
    expectInvalidParameter(() =>
      engine.freezeOffline({
        totalFrames: 256,
        blockSize: 128,
        numChannels: requestedChannels,
      }),
    );
    engine.destroy();
  });

  it('still renders at the prepared width', () => {
    const engine = new RealtimeEngine(48000, 128, 1024, 1024, preparedChannels);
    const rendered = engine.renderOffline([new Float32Array(128), new Float32Array(128)], 128);
    expect(rendered.length).toBe(preparedChannels);
    engine.destroy();
  });
});

describe('analyze rejects the same configurations as the C ABI', () => {
  const samples = makeSine(0.25, 440);
  const rejected: Array<[string, AnalyzeOptions]> = [
    ['bpmMin below zero', { bpmMin: 0 }],
    ['an inverted bpm range', { bpmMin: 120, bpmMax: 60 }],
    ['a negative chroma highpass', { chromaHighpassHz: -1 }],
    ['a non-positive chord HMM beam width', { chordHmmBeamWidth: 0 }],
  ];

  for (const [label, options] of rejected) {
    it(`rejects ${label}`, () => {
      expectInvalidParameter(() => analyze(samples, SR, options));
    });
  }

  it('rejects a non-finite option instead of narrowing it', () => {
    expectInvalidParameter(() => analyze(samples, SR, { bpmMin: Number.NaN }));
    expectInvalidParameter(() => analyze(samples, SR, { nFft: Number.POSITIVE_INFINITY }));
  });

  it('rejects an odd nFft, which has no n_fft/2 + 1 bin layout', () => {
    expectInvalidParameter(() => analyze(samples, SR, { nFft: 2047 }));
  });
});

describe('drainExternalMidi reports a budget it can never make progress on', () => {
  // One queue record lowers to at most 3 MIDI 1.0 messages, so a smaller
  // budget consumed nothing and returned an empty array forever while the
  // queue grew and started dropping events.
  function engineWithPendingExternalMidi(): RealtimeEngine {
    const engine = new RealtimeEngine(48000, 128);
    engine.setTempo(120);
    engine.setBuiltinInstrument({ gain: 0.5 }, 5);
    engine.setMidiDestinationExternal(5, true);
    engine.setMidiClips([
      {
        id: 1,
        trackId: 5,
        destinationId: 5,
        lengthSamples: 8192,
        events: [
          { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
          { renderFrame: 64, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
        ],
      },
    ]);
    engine.play();
    engine.process([new Float32Array(128), new Float32Array(128)]);
    expect(engine.externalMidiPendingCount()).toBeGreaterThan(0);
    return engine;
  }

  for (const maxRecords of [1, 2]) {
    it(`rejects maxRecords = ${maxRecords}`, () => {
      const engine = engineWithPendingExternalMidi();
      expectInvalidParameter(() => engine.drainExternalMidi(maxRecords));
      // The queue is untouched, so a caller with a workable budget still drains.
      expect(engine.drainExternalMidi(3).length).toBeGreaterThan(0);
      engine.destroy();
    });
  }
});

describe('caller-supplied JS lengths cannot drive an allocation', () => {
  const paramId = 0x4d580001;

  const rejectedPoints: Array<[string, unknown]> = [
    ['undefined', undefined],
    ['an over-budget length', { length: 2e9 }],
    ['a negative length', { length: -1 }],
    ['a fractional length', { length: 1.5 }],
  ];

  for (const [label, points] of rejectedPoints) {
    it(`setAutomationLane rejects ${label}`, () => {
      const engine = new RealtimeEngine(48000, 128);
      expectInvalidParameter(() =>
        engine.setAutomationLane(paramId, points as EngineAutomationPoint[]),
      );
      engine.destroy();
    });
  }

  it('fixFrames rejects a fabricated typed-array length', () => {
    // A Proxy passes `instanceof Int32Array`, so the TypeScript facade forwards
    // it and the native reader is the only thing standing between a lied-about
    // `.length` and a 4 GB vector resize.
    const backing = new Int32Array([0, 1, 2]);
    const lying = new Proxy(backing, {
      get(target, property, receiver) {
        if (property === 'length') {
          return 1e9;
        }
        return Reflect.get(target, property, receiver);
      },
    });
    expectInvalidParameter(() => fixFrames(lying, 0, -1, true));
  });

  it('waveformPeakPyramid rejects a level count fabricated after the facade checks', () => {
    // The facade reads `.length` twice (empty check, then `some`); the native
    // reader is the third read, so this exercises the guard the audit named
    // without needing the facade to forward an invalid value.
    let reads = 0;
    const levels = new Proxy([512, 1024], {
      get(target, property, receiver) {
        if (property === 'length') {
          reads += 1;
          if (reads > 2) {
            return 2e9;
          }
        }
        return Reflect.get(target, property, receiver);
      },
    });
    const samples = new Float32Array(2048);
    expectInvalidParameter(() =>
      waveformPeakPyramid(samples, 1, { samplesPerBucketLevels: levels }),
    );
  });
});
