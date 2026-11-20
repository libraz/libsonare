import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import * as indexModule from '../../dist/index.js';
import {
  CHORD_QUALITIES,
  type ChordQuality,
  init,
  type Matrix2D,
  type ScaleKind,
  StreamAnalyzer,
  type StreamBarChord,
  type StreamChordChange,
  type StreamConfig,
  type StreamFrames,
  type StreamFramesI16,
  type StreamFramesU8,
  type StreamPatternScore,
  type StreamProgressiveEstimate,
  type StreamStats,
} from '../../dist/index.js';
import * as workletModule from '../../dist/worklet.js';
import {
  type SonareStreamAnalyzerNode,
  SonareStreamAnalyzerWorkletProcessor,
} from '../../dist/worklet.js';

const sampleRate = 22050;

/** A C major triad, `seconds` long. */
function triad(seconds: number): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (const f of [261.63, 329.63, 392.0]) {
    for (let i = 0; i < out.length; i++) {
      out[i] += 0.2 * Math.sin((2 * Math.PI * f * i) / sampleRate);
    }
  }
  return out;
}

function caught(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function analyzer(config: Partial<StreamConfig> = {}): StreamAnalyzer {
  return new StreamAnalyzer({ sampleRate, ...config });
}

describe('stream analyzer surface', () => {
  beforeAll(async () => {
    await init();
  });

  it('publishes no defaults function: an omitted field already means the default', () => {
    for (const name of ['streamAnalyzerConfigDefault', 'streamAnalyzerConfigDefaults']) {
      expect((indexModule as Record<string, unknown>)[name]).toBeUndefined();
    }
  });

  it('reads frameCount, sampleRate, availableFrames and currentTimeSec as getters', () => {
    const a = analyzer();
    try {
      expect(a.sampleRate).toBe(sampleRate);
      expect(a.frameCount).toBe(0);
      expect(a.availableFrames).toBe(0);
      expect(a.currentTimeSec).toBe(0);
      a.process(triad(1));
      expect(a.frameCount).toBeGreaterThan(0);
      expect(a.availableFrames).toBeGreaterThan(0);
      expect(a.currentTimeSec).toBeGreaterThan(0);
      expect((a as unknown as Record<string, unknown>).currentTime).toBeUndefined();
      expectTypeOf(a.frameCount).toEqualTypeOf<number>();
      expectTypeOf(a.sampleRate).toEqualTypeOf<number>();
      expectTypeOf(a.availableFrames).toEqualTypeOf<number>();
      expectTypeOf(a.currentTimeSec).toEqualTypeOf<number>();
    } finally {
      a.destroy();
    }
  });

  it('renames the tuning setter to setReferenceHz', () => {
    const a = analyzer();
    try {
      a.setReferenceHz(466.16);
      expect((a as unknown as Record<string, unknown>).setTuningRefHz).toBeUndefined();
      expect(caught(() => a.setReferenceHz(1000))).toBeInstanceOf(RangeError);
      expect(caught(() => a.setReferenceHz('440' as unknown as number))).toBeInstanceOf(TypeError);
    } finally {
      a.destroy();
    }
  });

  it('requires sampleRate on the config', () => {
    expect(caught(() => new StreamAnalyzer({} as StreamConfig))).toBeInstanceOf(TypeError);
    expect(caught(() => new (StreamAnalyzer as unknown as new () => object)())).toBeInstanceOf(
      TypeError,
    );
    // @ts-expect-error sampleRate is required
    const config: StreamConfig = {};
    expect(config).toEqual({});
  });

  it('refuses removed and ordinal config fields', () => {
    const bad: Array<[string, Record<string, unknown>, unknown]> = [
      ['computeMagnitude', { computeMagnitude: false }, TypeError],
      ['outputFormat', { outputFormat: 0 }, TypeError],
      ['fmin', { fmin: 0 }, TypeError],
      ['fmax', { fmax: 8000 }, TypeError],
      ['tuningRefHz', { tuningRefHz: 440 }, TypeError],
      ['window ordinal', { window: 0 }, TypeError],
      ['window synonym', { window: 'rect' }, RangeError],
      ['window unknown', { window: 'kaiser' }, RangeError],
      ['fmaxHz zero', { fmaxHz: 0 }, RangeError],
    ];
    for (const [, extra, errorClass] of bad) {
      const error = caught(() => analyzer(extra as Partial<StreamConfig>));
      expect(error).toBeInstanceOf(errorClass as ErrorConstructor);
    }
  });

  it('accepts the renamed config fields and string window tokens', () => {
    for (const window of ['hann', 'hamming', 'blackman', 'rectangular'] as const) {
      const a = analyzer({ window, fminHz: 40, fmaxHz: 8000, referenceHz: 442 });
      a.destroy();
    }
  });

  it('returns band-major Matrix2D mel and chroma that match the per-frame layout', () => {
    const audio = triad(1.5);
    const bulk = analyzer({ computeChroma: true });
    const single = analyzer({ computeChroma: true });
    try {
      bulk.process(audio);
      single.process(audio);
      const all = bulk.readFrames(bulk.availableFrames);
      const n = all.nFrames;
      expect(n).toBeGreaterThan(2);
      const mel = all.mel as Matrix2D;
      const chroma = all.chroma as Matrix2D;
      expect(mel.cols).toBe(n);
      expect(mel.data.length).toBe(mel.rows * n);
      expect(chroma.rows).toBe(12);
      expect(chroma.cols).toBe(n);
      for (let f = 0; f < n; f++) {
        const one = single.readFrames(1);
        const m1 = one.mel as Matrix2D;
        const c1 = one.chroma as Matrix2D;
        expect(m1.cols).toBe(1);
        for (let r = 0; r < mel.rows; r++) {
          expect(mel.data[r * n + f]).toBe(m1.data[r]);
        }
        for (let r = 0; r < chroma.rows; r++) {
          expect(chroma.data[r * n + f]).toBe(c1.data[r]);
        }
      }
      expect(Object.keys(all)).not.toContain('nMels');
      expect(Object.keys(all)).not.toContain('nChroma');
      expect(Object.keys(all)).not.toContain('featureFlags');
    } finally {
      bulk.destroy();
      single.destroy();
    }
  });

  it('names the per-frame time timestampsSec', () => {
    const a = analyzer();
    try {
      a.process(triad(1));
      const frames = a.readFrames(a.availableFrames);
      expect(frames.timestampsSec.length).toBe(frames.nFrames);
      expect((frames as unknown as Record<string, unknown>).timestamps).toBeUndefined();
      for (let i = 1; i < frames.timestampsSec.length; i++) {
        expect(frames.timestampsSec[i]).toBeGreaterThan(frames.timestampsSec[i - 1]);
      }
    } finally {
      a.destroy();
    }
  });

  it('leaves a feature array absent when the feature is not computed', () => {
    const a = analyzer({
      computeMel: false,
      computeChroma: false,
      computeOnset: false,
      computeSpectral: false,
    });
    try {
      a.process(triad(1));
      const frames = a.readFrames(a.availableFrames);
      for (const key of [
        'mel',
        'chroma',
        'onsetStrength',
        'spectralCentroid',
        'spectralFlatness',
        'chordRoot',
        'chordQuality',
        'chordConfidence',
      ]) {
        expect(frames).not.toHaveProperty(key);
      }
      expect(frames.timestampsSec.length).toBe(frames.nFrames);
    } finally {
      a.destroy();
    }
  });

  it('decodes frame chord ordinals through CHORD_QUALITIES', () => {
    const a = analyzer({ computeChroma: true });
    try {
      a.process(triad(3));
      const frames = a.readFrames(a.availableFrames);
      expect(frames.chordQuality).toBeInstanceOf(Int32Array);
      expect(frames.chordRoot).toBeInstanceOf(Int32Array);
      for (const ordinal of frames.chordQuality as Int32Array) {
        expect(CHORD_QUALITIES[ordinal]).toBeTypeOf('string');
      }
    } finally {
      a.destroy();
    }
  });

  it('has a token for every chord ordinal native can emit', () => {
    expect(CHORD_QUALITIES).toHaveLength(25);
    expect(new Set(CHORD_QUALITIES).size).toBe(25);
    expect(CHORD_QUALITIES[9]).toBe('unknown');
    for (let ordinal = 0; ordinal <= 24; ordinal++) {
      expect(CHORD_QUALITIES[ordinal]).toBeTypeOf('string');
    }
    expectTypeOf<ChordQuality>().toEqualTypeOf<(typeof CHORD_QUALITIES)[number]>();
  });

  it('returns band-major quantized frames that match the per-frame layout', () => {
    const audio = triad(1);
    const nMels = 16;
    const bulk = analyzer({ computeChroma: true, nMels });
    const single = analyzer({ computeChroma: true, nMels });
    try {
      bulk.process(audio);
      single.process(audio);
      const u8 = bulk.readFramesU8(3);
      const n = u8.nFrames;
      expect(n).toBeGreaterThan(1);
      expect(u8.mel).toBeInstanceOf(Uint8Array);
      expect(u8.mel?.length).toBe(nMels * n);
      expect(u8.timestampsSec.length).toBe(n);
      for (let f = 0; f < n; f++) {
        const one = single.readFramesU8(1);
        for (let r = 0; r < nMels; r++) {
          expect(u8.mel?.[r * n + f]).toBe(one.mel?.[r]);
        }
        for (let r = 0; r < 12; r++) {
          expect(u8.chroma?.[r * n + f]).toBe(one.chroma?.[r]);
        }
      }
      const i16 = bulk.readFramesI16(2);
      expect(i16.chroma).toBeInstanceOf(Int16Array);
      expect(i16.chroma?.length).toBe(12 * i16.nFrames);
      for (const frames of [u8, i16]) {
        for (const key of ['featureFlags', 'timestamps', 'nMels', 'nChroma']) {
          expect(frames).not.toHaveProperty(key);
        }
      }
    } finally {
      bulk.destroy();
      single.destroy();
    }
  });

  it('reports a fresh analyzer without sentinel values', () => {
    const a = analyzer();
    try {
      const stats = a.stats();
      expect(stats.durationSec).toBe(0);
      const estimate = stats.estimate;
      for (const key of [
        'bpm',
        'bpmConfidence',
        'key',
        'chord',
        'currentBar',
        'barDurationSec',
        'detectedPatternName',
        'detectedPatternScore',
      ]) {
        expect(estimate).not.toHaveProperty(key);
      }
      expect(estimate.accumulatedSec).toBe(0);
      for (const removed of [
        'accumulatedSeconds',
        'barDuration',
        'keyMinor',
        'keyConfidence',
        'keyName',
        'keyShortName',
        'chordRoot',
        'chordQuality',
        'chordName',
        'chordConfidence',
        'chordStartTime',
      ]) {
        expect(estimate).not.toHaveProperty(removed);
      }
      expect(stats).not.toHaveProperty('durationSeconds');
    } finally {
      a.destroy();
    }
  });

  it('carries the seconds suffix on every streamed time', () => {
    const a = analyzer({ keyUpdateIntervalSec: 0.5, bpmUpdateIntervalSec: 0.5 });
    try {
      a.process(triad(4));
      a.finalize();
      const stats = a.stats();
      expect(stats.durationSec).toBeGreaterThan(3);
      expect(stats.estimate.accumulatedSec).toBeGreaterThan(0);
      for (const entry of stats.estimate.chordProgression) {
        expect(entry.startSec).toBeTypeOf('number');
        expect(entry).not.toHaveProperty('startTime');
        expect(CHORD_QUALITIES).toContain(entry.quality);
      }
      for (const entry of stats.estimate.barChordProgression) {
        expect(entry.startSec).toBeTypeOf('number');
        expect(entry).not.toHaveProperty('startTime');
      }
      const key = stats.estimate.key;
      if (key !== undefined) {
        expect(Object.keys(key).sort()).toEqual([
          'confidence',
          'mode',
          'name',
          'root',
          'shortName',
        ]);
      }
      const chord = stats.estimate.chord;
      if (chord !== undefined) {
        expect(chord.startSec).toBeTypeOf('number');
        expect(CHORD_QUALITIES).toContain(chord.quality);
      }
    } finally {
      a.destroy();
    }
  });

  it('anchors processWithOffset and reset on startSample', () => {
    const a = analyzer();
    try {
      a.reset(sampleRate);
      a.processWithOffset(triad(1), sampleRate);
      const frames = a.readFrames(a.availableFrames);
      expect(frames.timestampsSec[0]).toBeGreaterThan(0.5);
      a.reset();
      a.process(triad(1));
      expect(a.readFrames(1).timestampsSec[0]).toBeLessThan(0.5);
      a.setExpectedDuration(10);
    } finally {
      a.destroy();
    }
  });

  it('declares the renamed record types', () => {
    expectTypeOf<StreamStats['durationSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamChordChange['startSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamBarChord['startSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamChordChange['quality']>().toEqualTypeOf<ChordQuality>();
    expectTypeOf<StreamPatternScore['score']>().toEqualTypeOf<number>();
    expectTypeOf<StreamProgressiveEstimate['accumulatedSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamProgressiveEstimate['barDurationSec']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamProgressiveEstimate['bpm']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamProgressiveEstimate['chord']>().toEqualTypeOf<
      StreamChordChange | undefined
    >();
    expectTypeOf<StreamFrames['timestampsSec']>().toEqualTypeOf<Float32Array>();
    expectTypeOf<StreamFrames['mel']>().toEqualTypeOf<Matrix2D | undefined>();
    expectTypeOf<StreamFrames['chroma']>().toEqualTypeOf<Matrix2D | undefined>();
    expectTypeOf<StreamFramesU8['timestampsSec']>().toEqualTypeOf<Float32Array>();
    expectTypeOf<StreamFramesI16['timestampsSec']>().toEqualTypeOf<Float32Array>();
    expectTypeOf<StreamConfig['window']>().toEqualTypeOf<
      'hann' | 'hamming' | 'blackman' | 'rectangular' | undefined
    >();
    expectTypeOf<StreamConfig['sampleRate']>().toEqualTypeOf<number>();
    // @ts-expect-error removed
    type _A = import('../../dist/index.js').AnalyzerStats;
    // @ts-expect-error removed
    type _B = import('../../dist/index.js').FrameBuffer;
    // @ts-expect-error removed
    type _C = import('../../dist/index.js').ProgressiveEstimate;
    // @ts-expect-error removed
    type _D = import('../../dist/index.js').StreamConfigDefaults;
    // @ts-expect-error removed
    type _E = import('../../dist/index.js').ChordChange;
    // @ts-expect-error removed
    type _F = import('../../dist/index.js').BarChord;
    // @ts-expect-error removed
    type _G = import('../../dist/index.js').PatternScore;
    expect(true).toBe(true);
  });
});

describe('voice scale tokens', () => {
  it('narrows ScaleKind to the lowerCamel token set', () => {
    expectTypeOf<ScaleKind>().toEqualTypeOf<'major' | 'naturalMinor' | 'chromatic'>();
  });
});

describe('stream analyzer worklet surface', () => {
  it('takes an untyped control message and ignores garbage', () => {
    const processor = new SonareStreamAnalyzerWorkletProcessor({ chunkFrames: 128 });
    expectTypeOf<Parameters<typeof processor.receiveMessage>[0]>().toEqualTypeOf<unknown>();
    for (const message of [null, undefined, 42, 'destroy', {}, { type: 'unknown' }]) {
      expect(() => processor.receiveMessage(message)).not.toThrow();
    }
    expect(processor.process([[new Float32Array(128)]], [])).toBe(true);
    processor.receiveMessage({ type: 'destroy' });
    expect(processor.process([[new Float32Array(128)]], [])).toBe(false);
  });

  it('no longer exports the wire-protocol message types', () => {
    // @ts-expect-error moved out of the public entry point
    type _A = import('../../dist/worklet.js').SonareStreamAnalyzerChunkMessage;
    // @ts-expect-error moved out of the public entry point
    type _B = import('../../dist/worklet.js').SonareStreamAnalyzerDestroyMessage;
    // @ts-expect-error moved out of the public entry point
    type _C = import('../../dist/worklet.js').SonareStreamAnalyzerMessage;
    // @ts-expect-error moved out of the public entry point
    type _D = import('../../dist/worklet.js').SonareStreamAnalyzerRecycleMessage;
    expect(Object.keys(workletModule)).toContain('SonareStreamAnalyzerNode');
  });

  it('delivers StreamFrames and StreamStats to node listeners', () => {
    type Node = SonareStreamAnalyzerNode;
    expectTypeOf<Parameters<Node['onFrames']>[0]>().toEqualTypeOf<(frames: StreamFrames) => void>();
    expectTypeOf<Parameters<Node['onStats']>[0]>().toEqualTypeOf<(stats: StreamStats) => void>();
  });
});
