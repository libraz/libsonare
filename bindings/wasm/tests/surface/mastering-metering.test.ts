import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import type {
  ClipDetection,
  ClippingReport,
  CompressorOptions,
  GateOptions,
  LufsSeriesResult,
  MeteringDetectClippingOptions,
  MeteringDynamicRangeOptions,
  MeteringDynamicRangeRequest,
  MeteringPhaseScopeRequest,
  MeteringSilenceRatioRequest,
  MeteringSpectrumRequest,
  MeteringVectorscopeRequest,
  SpectrumOptions,
  WaveformPeaksReport,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import {
  init,
  lufsMultichannel,
  lufsSeriesMultichannel,
  masteringRepairDeclip,
  masteringRepairDeclipStereo,
  masteringRepairDetectClipping,
  meteringDetectClipping,
  meteringDynamicRange,
  waveformPeakPyramid,
  waveformPeaks,
} from '../../dist/index.js';
import { init as initSource } from '../../src/index';
import { getSonareModule } from '../../src/module_state';

const sampleRate = 22050;
// Any exported function by name, for tables that sweep many entry points.
const api = sonare as unknown as Record<string, (...args: unknown[]) => unknown>;

function tone(seconds = 0.5, amplitude = 0.4, hz = 440): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = amplitude * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  return out;
}

/** A tone driven into a hard 0.5 ceiling, so every peak is a clipped run. */
function clippedTone(): Float32Array {
  return tone(0.5, 1).map((v) => Math.max(-0.5, Math.min(0.5, v)));
}

function thrown(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

const samples = tone();
const left = tone(0.5, 0.4, 440);
const right = tone(0.5, 0.3, 660);

describe('mastering metering surface', () => {
  beforeAll(async () => {
    await init();
    // The raw module is the source tree's, which dist's init does not load.
    await initSource();
  });

  describe('required sampleRate', () => {
    const monoCalls = [
      'meteringPeakDb',
      'meteringRmsDb',
      'meteringCrestFactorDb',
      'meteringDcOffset',
      'meteringTruePeakDbtp',
      'meteringSilenceRatio',
      'meteringDetectClipping',
      'meteringDynamicRange',
      'meteringSpectrum',
      'meteringSpectrumFrame',
      'ebur128LoudnessRangeLu',
      'masteringDynamicsCompressor',
      'masteringDynamicsGate',
      'masteringDynamicsTransientShaper',
    ];
    it.each(monoCalls)(
      '%s refuses a request without sampleRate and a positional call without one',
      (name) => {
        expect(thrown(() => api[name]({ samples }))).toBeInstanceOf(TypeError);
        expect(thrown(() => api[name](samples))).toBeInstanceOf(TypeError);
      },
    );

    const stereoCalls = [
      'meteringCrestFactorDbStereo',
      'meteringStereoCorrelation',
      'meteringStereoWidth',
      'meteringVectorscope',
      'meteringPhaseScope',
    ];
    it.each(stereoCalls)('%s refuses a request without sampleRate', (name) => {
      expect(thrown(() => api[name]({ left, right }))).toBeInstanceOf(TypeError);
    });

    it.each(['lufsMultichannel', 'lufsSeriesMultichannel'])(
      '%s refuses a request without sampleRate',
      (name) => {
        expect(thrown(() => api[name]({ channels: [left, right] }))).toBeInstanceOf(TypeError);
      },
    );
  });

  describe('request and positional forms agree', () => {
    const monoPairs: Array<[string, Record<string, unknown>]> = [
      ['meteringPeakDb', {}],
      ['meteringRmsDb', {}],
      ['meteringCrestFactorDb', {}],
      ['meteringDcOffset', {}],
      ['meteringTruePeakDbtp', { oversampleFactor: 2 }],
      ['meteringSilenceRatio', { thresholdDb: -30, frameLength: 512, hopLength: 128 }],
      ['ebur128LoudnessRangeLu', {}],
    ];
    it.each(monoPairs)('%s', (name, options) => {
      const viaRequest = api[name]({ samples, sampleRate, ...options }) as number;
      const viaPositional = api[name](samples, sampleRate, options) as number;
      expect(Math.abs(viaRequest - viaPositional)).toBeLessThan(1e-6);
    });

    it('meteringSpectrumFrame takes startSample in the options', () => {
      const viaRequest = api.meteringSpectrumFrame({ samples, sampleRate, startSample: 512 }) as {
        magnitude: Float32Array;
      };
      const viaPositional = api.meteringSpectrumFrame(samples, sampleRate, {
        startSample: 512,
      }) as { magnitude: Float32Array };
      const first = api.meteringSpectrumFrame({ samples, sampleRate }) as {
        magnitude: Float32Array;
      };
      expect(Array.from(viaRequest.magnitude)).toEqual(Array.from(viaPositional.magnitude));
      expect(Array.from(viaRequest.magnitude)).not.toEqual(Array.from(first.magnitude));
    });

    it('refuses a request plus an extra positional argument', () => {
      expect(thrown(() => api.meteringPeakDb({ samples, sampleRate }, 22050))).toBeInstanceOf(
        TypeError,
      );
    });

    it('refuses an unknown request key before anything else', () => {
      expect(
        thrown(() => api.meteringPeakDb({ samples, sampleRate, frameOffset: 1 })),
      ).toBeInstanceOf(TypeError);
      expect(
        thrown(() => api.meteringSpectrumFrame({ samples, sampleRate, frameOffset: 1 })),
      ).toBeInstanceOf(TypeError);
    });
  });

  describe('removed overloads and names', () => {
    it.each([
      'lufsInterleaved',
      'lufsSeriesInterleaved',
      'meteringVectorscopeDecimated',
      'meteringPhaseScopeDecimated',
    ])('%s is gone', (name) => {
      expect(api[name]).toBeUndefined();
    });

    it.each([
      'meteringStereoCorrelation',
      'meteringStereoWidth',
      'meteringCrestFactorDbStereo',
      'meteringVectorscope',
      'meteringPhaseScope',
      'waveformPeaks',
      'waveformPeakPyramid',
    ])('%s is request-only', (name) => {
      expect(thrown(() => api[name](left, right, sampleRate))).toBeInstanceOf(TypeError);
    });
  });

  describe('scope calls fold the decimated forms in', () => {
    it.each(['meteringVectorscope', 'meteringPhaseScope'])(
      '%s decimates through maxPoints and refuses 0 as a point budget',
      (name) => {
        const all = api[name]({ left, right, sampleRate }) as { mid: Float32Array };
        const some = api[name]({ left, right, sampleRate, maxPoints: 64 }) as { mid: Float32Array };
        expect(all.mid.length).toBe(left.length);
        expect(thrown(() => api[name]({ left, right, sampleRate, maxPoints: 0 }))).toBeInstanceOf(
          RangeError,
        );
        expect(some.mid.length).toBeLessThanOrEqual(64);
        expect(thrown(() => api[name]({ left, right, sampleRate, maxPoints: -1 }))).toBeInstanceOf(
          RangeError,
        );
      },
    );

    it('types maxPoints on both requests', () => {
      expectTypeOf<MeteringVectorscopeRequest['maxPoints']>().toEqualTypeOf<number | undefined>();
      expectTypeOf<MeteringPhaseScopeRequest['maxPoints']>().toEqualTypeOf<number | undefined>();
    });
  });

  describe('explicit 0 on a zero-means-default field', () => {
    const cases: Array<[string, Record<string, unknown>]> = [
      ['meteringTruePeakDbtp', { oversampleFactor: 0 }],
      ['meteringDetectClipping', { minRegionSamples: 0 }],
      ['meteringDynamicRange', { windowSec: 0 }],
      ['meteringDynamicRange', { hopSec: 0 }],
      ['meteringSpectrum', { nFft: 0 }],
      ['meteringSpectrum', { octaveFraction: 0 }],
      ['meteringSpectrum', { dbRef: 0 }],
      ['meteringSpectrum', { dbAmin: 0 }],
      ['meteringSpectrumFrame', { nFft: 0 }],
      ['meteringSpectrumFrame', { dbRef: 0 }],
    ];
    it.each(cases)('%s refuses %j as a RangeError', (name, field) => {
      expect(thrown(() => api[name]({ samples, sampleRate, ...field }))).toBeInstanceOf(RangeError);
      expect(thrown(() => api[name](samples, sampleRate, field))).toBeInstanceOf(RangeError);
    });

    it.each(cases)('%s takes the default when the field is absent or null', (name, field) => {
      const nulled = Object.fromEntries(Object.keys(field).map((key) => [key, null]));
      expect(() => api[name]({ samples, sampleRate })).not.toThrow();
      expect(() => api[name]({ samples, sampleRate, ...nulled })).not.toThrow();
    });
  });

  describe('detect clipping in dBFS', () => {
    it('reports clipped runs through clippedSamples and clippedRatio', () => {
      const report = meteringDetectClipping(clippedTone(), sampleRate, { thresholdDb: -6.1 });
      expectTypeOf(report).toEqualTypeOf<ClippingReport>();
      expect(report.clippedSamples).toBeGreaterThan(0);
      expect(report.clippedRatio).toBeCloseTo(report.clippedSamples / clippedTone().length, 6);
      const keys = Object.keys(report);
      expect(keys).not.toContain('clippingRatio');
      expect(keys).toContain('clippedRatio');
    });

    it('reads thresholdDb as dBFS, not linear', () => {
      // 0.5 is about -6.02 dBFS: a -6.1 dBFS level catches the ceiling, a -5.9 level does not.
      expect(
        meteringDetectClipping(clippedTone(), sampleRate, { thresholdDb: -6.1 }).clippedSamples,
      ).toBeGreaterThan(0);
      expect(
        meteringDetectClipping(clippedTone(), sampleRate, { thresholdDb: -5.9 }).clippedSamples,
      ).toBe(0);
    });

    it('accepts 0 dBFS and refuses a level above it', () => {
      expect(() => meteringDetectClipping(samples, sampleRate, { thresholdDb: 0 })).not.toThrow();
      expect(
        thrown(() => meteringDetectClipping(samples, sampleRate, { thresholdDb: 1 })),
      ).toBeInstanceOf(RangeError);
      expect(
        thrown(() => api.meteringDetectClipping({ samples, sampleRate, thresholdDb: 'loud' })),
      ).toBeInstanceOf(TypeError);
    });

    it('refuses the removed linear name', () => {
      expect(
        thrown(() => api.meteringDetectClipping({ samples, sampleRate, threshold: 0.9 })),
      ).toBeInstanceOf(TypeError);
    });

    it('types the options', () => {
      expectTypeOf<MeteringDetectClippingOptions>().toHaveProperty('thresholdDb');
      expectTypeOf<MeteringDetectClippingOptions>().not.toHaveProperty('threshold');
    });
  });

  describe('dynamic range percentiles', () => {
    const range = tone(1);
    it.each([
      ['lowPercentile', -0.1],
      ['lowPercentile', 1.5],
      ['highPercentile', -0.1],
      ['highPercentile', 1.5],
    ])('refuses %s = %d outside [0, 1]', (field, value) => {
      expect(
        thrown(() => meteringDynamicRange(range, sampleRate, { [field]: value })),
      ).toBeInstanceOf(RangeError);
    });

    it('refuses a low percentile above the high one, naming the field', () => {
      const error = thrown(() =>
        meteringDynamicRange(range, sampleRate, { lowPercentile: 0.9, highPercentile: 0.2 }),
      );
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toContain('lowPercentile');
    });

    it('accepts equal bounds and the closed ends', () => {
      expect(() =>
        meteringDynamicRange(range, sampleRate, { lowPercentile: 0, highPercentile: 1 }),
      ).not.toThrow();
      expect(() =>
        meteringDynamicRange(range, sampleRate, { lowPercentile: 0.5, highPercentile: 0.5 }),
      ).not.toThrow();
    });

    it('types the options', () => {
      expectTypeOf<MeteringDynamicRangeOptions>().toHaveProperty('highPercentile');
    });
  });

  describe('planar channels', () => {
    it('measures loudness from planar channels like the interleaved native call', () => {
      const result = lufsMultichannel({ channels: [left, right], sampleRate });
      const native = getSonareModule().lufsInterleaved(
        Float32Array.from({ length: left.length * 2 }, (_, i) =>
          i % 2 === 0 ? (left[i / 2] as number) : (right[(i - 1) / 2] as number),
        ),
        2,
        sampleRate,
      );
      expect(result.integratedLufs).toBeCloseTo(native.integratedLufs, 5);
      expect(result.loudnessRangeLu).toBeCloseTo(native.loudnessRange, 5);
    });

    it('returns the series under momentaryLufs / shortTermLufs', () => {
      const series = lufsSeriesMultichannel({
        channels: [tone(1), tone(1)],
        sampleRate,
      });
      expectTypeOf(series).toEqualTypeOf<LufsSeriesResult>();
      expect(series.momentaryLufs).toBeInstanceOf(Float32Array);
      expect(series.shortTermLufs).toBeInstanceOf(Float32Array);
      expect(series.momentaryLufs.length).toBeGreaterThan(0);
      expect(Object.keys(series)).not.toContain('momentary');
      expect(Object.keys(series)).not.toContain('shortTerm');
    });

    it.each([
      ['lufsMultichannel', 'a plane count of zero', { channels: [], sampleRate }, RangeError],
      ['lufsMultichannel', 'a non-array', { channels: left, sampleRate }, TypeError],
      [
        'lufsMultichannel',
        'planes of unequal length',
        { channels: [left, right.subarray(0, 100)], sampleRate },
        RangeError,
      ],
      [
        'lufsSeriesMultichannel',
        'a plane that is not a Float32Array',
        { channels: [[1, 2]], sampleRate },
        TypeError,
      ],
      ['waveformPeaks', 'a non-array', { channels: left }, TypeError],
      ['waveformPeaks', 'no planes', { channels: [] }, RangeError],
      [
        'waveformPeaks',
        'planes of unequal length',
        { channels: [left, right.subarray(0, 100)] },
        RangeError,
      ],
      ['waveformPeakPyramid', 'a non-array', { channels: left }, TypeError],
    ] as const)('%s refuses %s', (name, _label, request, errorClass) => {
      expect(thrown(() => api[name](request))).toBeInstanceOf(errorClass);
    });

    it('splits waveform buckets into one array per channel', () => {
      const report = waveformPeaks({ channels: [left, right], samplesPerBucket: 1000 });
      expectTypeOf(report).toEqualTypeOf<WaveformPeaksReport>();
      expect(report.min).toHaveLength(2);
      expect(report.max).toHaveLength(2);
      expect(report.min[0]).toBeInstanceOf(Float32Array);
      expect(report.min[0]?.length).toBe(report.bucketCount);
      expect(Object.keys(report)).not.toContain('channels');
      // Channel 1 is quieter than channel 0 in the first bucket.
      expect(report.max[0]?.[0] as number).toBeGreaterThan(report.max[1]?.[0] as number);
    });

    it('returns a planar report per pyramid level and takes no sampleRate', () => {
      const levels = waveformPeakPyramid({
        channels: [left],
        samplesPerBucketLevels: [256, 1024],
      });
      expect(levels).toHaveLength(2);
      expect(levels[0]?.min).toHaveLength(1);
      expect(levels[1]?.samplesPerBucket).toBe(1024);
      expect(thrown(() => api.waveformPeaks({ channels: [left], sampleRate }))).toBeInstanceOf(
        TypeError,
      );
    });

    it('refuses the removed interleaved request shape', () => {
      expect(thrown(() => api.waveformPeaks({ samples: left, channels: 1 }))).toBeInstanceOf(
        TypeError,
      );
      expect(thrown(() => api.lufsMultichannel({ samples: left, sampleRate }))).toBeInstanceOf(
        TypeError,
      );
    });
  });

  describe('dynamics options', () => {
    it.each([['peak'], ['rms'], ['logRms']])(
      'the compressor accepts the %s detector token',
      (detector) => {
        const result = api.masteringDynamicsCompressor({ samples, sampleRate, detector }) as {
          samples: Float32Array;
        };
        expect(result.samples.length).toBe(samples.length);
      },
    );

    it.each([
      ['log_rms', RangeError],
      ['LOGRMS', RangeError],
      ['Peak', RangeError],
      [0, TypeError],
      [1, TypeError],
      [2, TypeError],
    ] as const)('the compressor refuses detector %j', (detector, errorClass) => {
      const error = thrown(() =>
        api.masteringDynamicsCompressor({ samples, sampleRate, detector }),
      );
      expect(error).toBeInstanceOf(errorClass);
    });

    it('maps sidechainHighPassHz onto the native enable flag and corner', () => {
      const plain = api.masteringDynamicsCompressor({
        samples,
        sampleRate,
        thresholdDb: -30,
        ratio: 8,
      }) as { samples: Float32Array };
      const filtered = api.masteringDynamicsCompressor({
        samples,
        sampleRate,
        thresholdDb: -30,
        ratio: 8,
        sidechainHighPassHz: 2000,
      }) as { samples: Float32Array };
      const sameAsPlain = Array.from(plain.samples).every((v, i) => v === filtered.samples[i]);
      expect(sameAsPlain).toBe(false);
    });

    it.each([0, -50])('refuses sidechainHighPassHz = %d', (value) => {
      expect(
        thrown(() =>
          api.masteringDynamicsCompressor({ samples, sampleRate, sidechainHighPassHz: value }),
        ),
      ).toBeInstanceOf(RangeError);
    });

    it('refuses the removed sidechain pair', () => {
      for (const key of ['sidechainHpfEnabled', 'sidechainHpfHz']) {
        expect(
          thrown(() => api.masteringDynamicsCompressor({ samples, sampleRate, [key]: 1 })),
        ).toBeInstanceOf(TypeError);
      }
    });

    it('the gate accepts keyHighPassHz including 0 and refuses a negative corner', () => {
      expect(() =>
        api.masteringDynamicsGate({ samples, sampleRate, keyHighPassHz: 1000 }),
      ).not.toThrow();
      expect(() =>
        api.masteringDynamicsGate({ samples, sampleRate, keyHighPassHz: 0 }),
      ).not.toThrow();
      expect(
        thrown(() => api.masteringDynamicsGate({ samples, sampleRate, keyHighPassHz: -1 })),
      ).toBeInstanceOf(RangeError);
      expect(
        thrown(() => api.masteringDynamicsGate({ samples, sampleRate, keyHpfHz: 1000 })),
      ).toBeInstanceOf(TypeError);
    });

    it('types the options', () => {
      expectTypeOf<CompressorOptions['detector']>().toEqualTypeOf<
        'peak' | 'rms' | 'logRms' | undefined
      >();
      expectTypeOf<CompressorOptions>().toHaveProperty('sidechainHighPassHz');
      expectTypeOf<GateOptions>().toHaveProperty('keyHighPassHz');
      expectTypeOf<GateOptions>().not.toHaveProperty('keyHpfHz');
    });
  });

  describe('clip detection in the repair family', () => {
    it('names the counts clippedSamples and clippedRatio', () => {
      const detected = masteringRepairDetectClipping({
        samples: clippedTone(),
        sampleRate,
        clipThresholdDb: -6.1,
      });
      expectTypeOf(detected).toEqualTypeOf<ClipDetection>();
      expect(detected.clippedSamples).toBeGreaterThan(0);
      const keys = Object.keys(detected);
      expect(keys).not.toContain('sampleCount');
      expect(keys).not.toContain('sampleFraction');
    });

    it('carries the same names inside the declip reports', () => {
      const mono = masteringRepairDeclip({
        samples: clippedTone(),
        sampleRate,
        clipThresholdDb: -6.1,
      });
      expect(mono.report.detected.clippedSamples).toBeGreaterThan(0);
      const stereo = masteringRepairDeclipStereo({
        left: clippedTone(),
        right: clippedTone(),
        sampleRate,
        clipThresholdDb: -6.1,
      });
      expect(stereo.reports[1]?.detected.clippedRatio).toBeGreaterThan(0);
      expect(Object.keys(stereo.reports[0]?.detected ?? {})).not.toContain('sampleCount');
    });
  });

  describe('spectrum options', () => {
    it('SpectrumOptions carries validate', () => {
      expectTypeOf<SpectrumOptions>().toHaveProperty('validate');
    });

    it('skips the finiteness pre-scan on validate: false', () => {
      const bad = Float32Array.from(samples);
      bad[10] = Number.NaN;
      expect(thrown(() => api.meteringSpectrum(bad, sampleRate))).toBeInstanceOf(RangeError);
      expect(
        thrown(() => api.meteringSpectrum(bad, sampleRate, { validate: false })),
      ).not.toBeInstanceOf(TypeError);
    });
  });

  describe('native still uses the old spelling', () => {
    const module = () => getSonareModule();
    const wasmSource = (path: string) =>
      readFileSync(new URL(`../../../../src/wasm/bindings/${path}`, import.meta.url), 'utf8');

    const rawCases: Array<[string, () => Record<string, unknown>, string, string | null]> = [
      [
        'the clipping report counts under clippingRatio',
        () => module().meteringDetectClipping(clippedTone(), sampleRate, 0.4, 1) as never,
        'clippingRatio',
        'clippedRatio',
      ],
      [
        'the loudness result names the range loudnessRange',
        () =>
          module().lufsInterleaved(Float32Array.from([...left, ...left]), 2, sampleRate) as never,
        'loudnessRange',
        'loudnessRangeLu',
      ],
      [
        'the loudness series is momentary / shortTerm',
        () =>
          module().lufsSeriesInterleaved(
            Float32Array.from([...tone(1), ...tone(1)]),
            2,
            sampleRate,
          ) as never,
        'momentary',
        'momentaryLufs',
      ],
      [
        'the declip analysis counts under sampleCount',
        () =>
          module().masteringRepairDetectClipping(clippedTone(), sampleRate, {
            clipThresholdDb: -8,
          }) as never,
        'sampleCount',
        'clippedSamples',
      ],
      [
        'the waveform buckets carry a channel count beside flat arrays',
        () => module().waveformPeaks(Float32Array.from([...left, ...left]), 2, 1000) as never,
        'channels',
        null,
      ],
    ];
    it.each(rawCases)('%s', (_label, call, oldKey, newKey) => {
      const raw = call();
      expect(Object.keys(raw)).toContain(oldKey);
      if (newKey !== null) {
        expect(Object.keys(raw)).not.toContain(newKey);
      }
    });

    it('the clip threshold is read as a linear amplitude', () => {
      // 0.5 ceiling: a linear 0.4 catches it, a linear 0.6 does not.
      const catches = module().meteringDetectClipping(clippedTone(), sampleRate, 0.4, 1);
      const misses = module().meteringDetectClipping(clippedTone(), sampleRate, 0.6, 1);
      expect(catches.clippedSamples).toBeGreaterThan(0);
      expect(misses.clippedSamples).toBe(0);
    });

    it('the spectrum frame start is the positional frame_offset', () => {
      const burst = tone(0.2);
      const silentThenTone = new Float32Array(4096 + burst.length);
      silentThenTone.set(burst, 4096);
      const early = module().meteringSpectrumFrame(silentThenTone, sampleRate, 0, {});
      const late = module().meteringSpectrumFrame(silentThenTone, sampleRate, 4096, {});
      const energy = (report: { magnitude: Float32Array }) =>
        report.magnitude.reduce((sum, v) => sum + v, 0);
      expect(energy(early)).toBe(0);
      expect(energy(late)).toBeGreaterThan(0);
    });

    it.each([
      [
        'the compressor sidechain high-pass',
        'effects/offline_dynamics_editing.cpp',
        'sidechainHpfHz',
      ],
      [
        'the compressor sidechain high-pass enable',
        'effects/offline_dynamics_editing.cpp',
        'sidechainHpfEnabled',
      ],
      ['the gate key high-pass', 'effects/offline_dynamics_editing.cpp', 'keyHpfHz'],
      ['the vectorscope decimation entry', 'metering/metering.cpp', 'meteringVectorscopeDecimated'],
    ])('%s is still spelled the old way in the embind source', (_label, path, spelling) => {
      expect(wasmSource(path)).toContain(spelling);
    });

    it('the meter ring header keeps its channel-count slot', async () => {
      const rings = await import('../../src/worklet/meter-rings');
      expect(rings.SONARE_METER_RING_CHANNEL_COUNT_OFFSET).toBe(16);
    });
  });
});

// One case per renamed, removed or tightened name, so each is exercised by what changed about it.
describe('renamed, removed and tightened names', () => {
  beforeAll(async () => {
    await init();
  });

  const mono = () => ({ samples: tone(), sampleRate });
  const stereo = () => ({ left: tone(), right: tone(0.5, 0.3, 660), sampleRate });
  const bodies: Record<string, () => Record<string, unknown>> = {
    lufsMultichannel: () => ({ channels: [tone(), tone()], sampleRate }),
    lufsSeriesMultichannel: () => ({ channels: [tone(), tone()], sampleRate }),
    waveformPeaks: () => ({ channels: [tone()] }),
    waveformPeakPyramid: () => ({ channels: [tone()] }),
    meteringCrestFactorDbStereo: stereo,
    meteringPhaseScope: stereo,
    meteringStereoCorrelation: stereo,
    meteringStereoWidth: stereo,
    meteringVectorscope: stereo,
  };
  const bodyOf = (fn: string): Record<string, unknown> => (bodies[fn] ?? mono)();

  const producers: Record<string, () => Record<string, unknown>> = {
    clipDetection: () =>
      masteringRepairDetectClipping({ samples: clippedTone(), sampleRate }) as unknown as Record<
        string,
        unknown
      >,
    clippingReport: () =>
      meteringDetectClipping({ samples: clippedTone(), sampleRate }) as unknown as Record<
        string,
        unknown
      >,
    lufsSeries: () =>
      lufsSeriesMultichannel({ channels: [tone(1), tone(1)], sampleRate }) as unknown as Record<
        string,
        unknown
      >,
  };

  type Row = [name: string, kind: string, ...args: unknown[]];
  const rows: Row[] = [
    ['ClipDetection.sampleCount', 'resultKey', 'clipDetection', 'sampleCount', 'clippedSamples'],
    [
      'ClipDetection.sampleFraction',
      'resultKey',
      'clipDetection',
      'sampleFraction',
      'clippedRatio',
    ],
    [
      'ClippingReport.clippingRatio',
      'resultKey',
      'clippingReport',
      'clippingRatio',
      'clippedRatio',
    ],
    ['CompressorDetector', 'detector', 'masteringDynamicsCompressor'],
    ['CompressorOptions.detector', 'detector', 'masteringDynamicsCompressor'],
    [
      'CompressorOptions.sidechainHpfEnabled',
      'oldKey',
      'masteringDynamicsCompressor',
      'sidechainHpfEnabled',
    ],
    ['CompressorOptions.sidechainHpfHz', 'oldKey', 'masteringDynamicsCompressor', 'sidechainHpfHz'],
    ['ebur128LoudnessRangeLu', 'exported', 'ebur128LoudnessRangeLu'],
    ['Ebur128LoudnessRangeLuRequest.sampleRate', 'rate', 'ebur128LoudnessRangeLu'],
    ['GateOptions.keyHpfHz', 'oldKey', 'masteringDynamicsGate', 'keyHpfHz'],
    ['LufsInterleavedRequest.sampleRate', 'rate', 'lufsMultichannel'],
    ['LufsInterleavedRequest.samples', 'oldKey', 'lufsMultichannel', 'samples'],
    ['LufsSeriesInterleavedRequest.sampleRate', 'rate', 'lufsSeriesMultichannel'],
    ['LufsSeriesInterleavedRequest.samples', 'oldKey', 'lufsSeriesMultichannel', 'samples'],
    ['LufsSeriesResult.momentary', 'resultKey', 'lufsSeries', 'momentary', 'momentaryLufs'],
    ['LufsSeriesResult.shortTerm', 'resultKey', 'lufsSeries', 'shortTerm', 'shortTermLufs'],
    ['masteringDynamicsCompressor', 'exported', 'masteringDynamicsCompressor'],
    ['MasteringDynamicsCompressorRequest.detector', 'detector', 'masteringDynamicsCompressor'],
    ['MasteringDynamicsCompressorRequest.sampleRate', 'rate', 'masteringDynamicsCompressor'],
    [
      'MasteringDynamicsCompressorRequest.sidechainHpfEnabled',
      'oldKey',
      'masteringDynamicsCompressor',
      'sidechainHpfEnabled',
    ],
    [
      'MasteringDynamicsCompressorRequest.sidechainHpfHz',
      'oldKey',
      'masteringDynamicsCompressor',
      'sidechainHpfHz',
    ],
    ['masteringDynamicsGate', 'exported', 'masteringDynamicsGate'],
    ['MasteringDynamicsGateRequest.keyHpfHz', 'oldKey', 'masteringDynamicsGate', 'keyHpfHz'],
    ['MasteringDynamicsGateRequest.sampleRate', 'rate', 'masteringDynamicsGate'],
    ['masteringDynamicsTransientShaper', 'exported', 'masteringDynamicsTransientShaper'],
    [
      'MasteringDynamicsTransientShaperRequest.sampleRate',
      'rate',
      'masteringDynamicsTransientShaper',
    ],
    ['meteringCrestFactorDb', 'exported', 'meteringCrestFactorDb'],
    ['meteringCrestFactorDbStereo', 'exported', 'meteringCrestFactorDbStereo'],
    ['meteringDcOffset', 'exported', 'meteringDcOffset'],
    ['meteringDetectClipping', 'exported', 'meteringDetectClipping'],
    [
      'MeteringDetectClippingOptions.minRegionSamples',
      'zero',
      'meteringDetectClipping',
      'minRegionSamples',
    ],
    ['MeteringDetectClippingOptions.threshold', 'oldKey', 'meteringDetectClipping', 'threshold'],
    ['MeteringDetectClippingRequest.sampleRate', 'rate', 'meteringDetectClipping'],
    ['MeteringDetectClippingRequest.threshold', 'oldKey', 'meteringDetectClipping', 'threshold'],
    ['meteringDynamicRange', 'exported', 'meteringDynamicRange'],
    ['MeteringDynamicRangeOptions.hopSec', 'zero', 'meteringDynamicRange', 'hopSec'],
    [
      'MeteringDynamicRangeOptions.lowPercentile',
      'outOfRange',
      'meteringDynamicRange',
      'lowPercentile',
      2,
    ],
    ['MeteringDynamicRangeOptions.windowSec', 'zero', 'meteringDynamicRange', 'windowSec'],
    ['MeteringDynamicRangeRequest.sampleRate', 'rate', 'meteringDynamicRange'],
    ['meteringPeakDb', 'exported', 'meteringPeakDb'],
    ['meteringPhaseScope', 'exported', 'meteringPhaseScope'],
    ['meteringRmsDb', 'exported', 'meteringRmsDb'],
    ['meteringSilenceRatio', 'exported', 'meteringSilenceRatio'],
    ['meteringSpectrum', 'exported', 'meteringSpectrum'],
    ['meteringSpectrumFrame', 'exported', 'meteringSpectrumFrame'],
    ['meteringSpectrumFrame(frameOffset)', 'positionalOffset'],
    ['MeteringSpectrumFrameRequest.frameOffset', 'oldKey', 'meteringSpectrumFrame', 'frameOffset'],
    ['MeteringSpectrumRequest.sampleRate', 'rate', 'meteringSpectrum'],
    ['meteringStereoCorrelation', 'exported', 'meteringStereoCorrelation'],
    ['meteringStereoWidth', 'exported', 'meteringStereoWidth'],
    ['meteringTruePeakDbtp', 'exported', 'meteringTruePeakDbtp'],
    [
      'MeteringTruePeakRequest.oversampleFactor',
      'zero',
      'meteringTruePeakDbtp',
      'oversampleFactor',
    ],
    ['MeteringTruePeakRequest.sampleRate', 'rate', 'meteringTruePeakDbtp'],
    ['meteringVectorscope', 'exported', 'meteringVectorscope'],
    ['WorkletMeterSnapshot.channelCount', 'ringSource'],
    ['SpectrumOptions.dbAmin', 'zero', 'meteringSpectrum', 'dbAmin'],
    ['SpectrumOptions.dbRef', 'zero', 'meteringSpectrum', 'dbRef'],
    ['SpectrumOptions.nFft', 'zero', 'meteringSpectrum', 'nFft'],
    ['SpectrumOptions.octaveFraction', 'zero', 'meteringSpectrum', 'octaveFraction'],
    ['waveformPeakPyramid', 'exported', 'waveformPeakPyramid'],
    ['WaveformPeakPyramidRequest.samples', 'oldKey', 'waveformPeakPyramid', 'samples'],
    ['waveformPeaks', 'exported', 'waveformPeaks'],
    ['WaveformPeaksReport.max', 'planarResult', 'max'],
    ['WaveformPeaksReport.min', 'planarResult', 'min'],
    ['WaveformPeaksRequest.samples', 'oldKey', 'waveformPeaks', 'samples'],
  ];

  it.each(rows)('%s', (_name, kind, ...args) => {
    switch (kind) {
      case 'exported':
        expect(typeof api[args[0] as string]).toBe('function');
        break;
      case 'rate': {
        const { sampleRate: _omitted, ...body } = bodyOf(args[0] as string);
        expect(thrown(() => api[args[0] as string](body))).toBeInstanceOf(TypeError);
        break;
      }
      case 'oldKey':
        expect(
          thrown(() =>
            api[args[0] as string]({ ...bodyOf(args[0] as string), [args[1] as string]: 1 }),
          ),
        ).toBeInstanceOf(TypeError);
        break;
      case 'zero':
        expect(
          thrown(() =>
            api[args[0] as string]({ ...bodyOf(args[0] as string), [args[1] as string]: 0 }),
          ),
        ).toBeInstanceOf(RangeError);
        break;
      case 'outOfRange':
        expect(
          thrown(() =>
            api[args[0] as string]({ ...bodyOf(args[0] as string), [args[1] as string]: args[2] }),
          ),
        ).toBeInstanceOf(RangeError);
        break;
      case 'detector':
        expect(
          thrown(() => api.masteringDynamicsCompressor({ ...mono(), detector: 'log_rms' })),
        ).toBeInstanceOf(RangeError);
        break;
      case 'resultKey': {
        const result = producers[args[0] as string]();
        expect(Object.keys(result)).not.toContain(args[1]);
        expect(Object.keys(result)).toContain(args[2]);
        break;
      }
      case 'planarResult': {
        const report = api.waveformPeaks({ channels: [tone(), tone()] }) as Record<string, unknown>;
        expect(Array.isArray(report[args[0] as string])).toBe(true);
        expect((report[args[0] as string] as unknown[]).length).toBe(2);
        break;
      }
      case 'positionalOffset':
        expect(thrown(() => api.meteringSpectrumFrame(tone(), sampleRate, 512))).toBeInstanceOf(
          TypeError,
        );
        break;
      case 'ringSource': {
        const source = readFileSync(
          new URL('../../src/worklet/meter-rings.ts', import.meta.url),
          'utf8',
        );
        expect(source).toContain('numChannels?: number');
        expect(source).not.toContain('channelCount?: number');
        break;
      }
      default:
        throw new Error(`unknown case kind ${kind}`);
    }
  });
});

describe('request types of the metering family', () => {
  it('MeteringSilenceRatioRequest carries the three options of the old positional form', () => {
    expectTypeOf<MeteringSilenceRatioRequest>().toHaveProperty('thresholdDb');
    expectTypeOf<MeteringSilenceRatioRequest>().toHaveProperty('frameLength');
    expectTypeOf<MeteringSilenceRatioRequest>().toHaveProperty('hopLength');
    expectTypeOf<MeteringSilenceRatioRequest['sampleRate']>().toEqualTypeOf<number>();
  });

  it('MeteringDynamicRangeRequest and MeteringSpectrumRequest require the sample rate', () => {
    expectTypeOf<MeteringDynamicRangeRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<MeteringSpectrumRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<MeteringSpectrumRequest>().not.toHaveProperty('startSample');
  });
});
