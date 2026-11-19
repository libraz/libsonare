import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import type {
  DeclickReport,
  DeclickResult,
  DeclipResult,
  DecrackleResult,
  DehumResult,
  DenoiseResult,
  DereverbResult,
  MasteringRepairDeclickResult,
  MasteringRepairDenoiseClassicalResult,
  MasteringRepairStage,
  MasteringRepairTrimSilenceResult,
  TrimRange,
  TrimReport,
  TrimSilenceResult,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import { init } from '../../dist/index.js';

type Fn = (...args: unknown[]) => unknown;
// Any exported function by name, for tables that sweep many entry points.
const api = sonare as unknown as Record<string, Fn>;

beforeAll(async () => {
  await init();
});

const sampleRate = 22050;

/** About 0.25 s of a tone with a few clicks and a flat-topped stretch, enough for every repair stage. */
function signal(hz = 440): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * 0.25));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.4 * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  out[1000] = 0.95;
  out[3000] = -0.95;
  for (let i = 4000; i < 4004; i++) {
    out[i] = 0.98;
  }
  return out;
}

/** A tone padded with silence on both sides, so a trim has something to remove. */
function padded(): Float32Array {
  const out = new Float32Array(sampleRate / 2);
  out.set(signal().subarray(0, 2000), 2000);
  return out;
}

function errorOf(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function expectThrows(fn: () => unknown, cls: ErrorConstructor): void {
  expect(errorOf(fn)).toBeInstanceOf(cls);
}

function expectClose(a: ArrayLike<number>, b: ArrayLike<number>, tol = 1e-5): void {
  expect(a.length).toBe(b.length);
  for (let i = 0; i < a.length; i++) {
    expect(Math.abs((a[i] as number) - (b[i] as number))).toBeLessThanOrEqual(tol);
  }
}

const left = signal(440);
const right = signal(330);

/** Mono calls with the options their tiny call needs. */
const MONO: ReadonlyArray<[string, Record<string, unknown>]> = [
  ['masteringRepairDeclick', {}],
  ['masteringRepairDeclip', {}],
  ['masteringRepairDecrackle', {}],
  ['masteringRepairDehum', {}],
  ['masteringRepairDenoiseClassical', {}],
  ['masteringRepairDereverbClassical', {}],
  ['masteringRepairTrimSilence', {}],
  ['masteringRepairDetectClicks', {}],
  ['masteringRepairDetectClipping', {}],
  ['masteringRepairDetectCrackle', {}],
  ['masteringRepairDetectHum', {}],
  ['masteringRepairDetectNoiseFloor', {}],
  ['masteringRepairDetectReverb', {}],
  ['masteringRepairDetectTrimRange', {}],
];

/** Stereo-pair calls, with how many reports each result carries. */
const PAIRED: ReadonlyArray<[string, number]> = [
  ['masteringRepairDeclickStereo', 2],
  ['masteringRepairDeclipStereo', 2],
  ['masteringRepairDecrackleStereo', 2],
  ['masteringRepairDehumStereo', 2],
  ['masteringRepairDenoiseClassicalStereo', 1],
  ['masteringRepairDereverbClassicalStereo', 1],
];

const LINKED = ['masteringRepairDenoiseClassicalLinked', 'masteringRepairDereverbClassicalLinked'];

/** Every call taking a stereo pair or a channel array. */
const PAIR_CALLS = [
  ...PAIRED.map(([name]) => name),
  'masteringRepairTrimSilenceStereo',
  'masteringRepairDetectTrimRangeStereo',
];

function pairRequest(): Record<string, unknown> {
  return { left, right, sampleRate };
}

describe('repair: sample rate and call shape', () => {
  it.each(MONO)('%s requires sampleRate in both forms', (name) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn(left), TypeError);
    expectThrows(() => fn(left, undefined), TypeError);
    expectThrows(() => fn({ samples: left }), TypeError);
    expectThrows(() => fn({ samples: left, sampleRate: '22050' }), TypeError);
    expectThrows(() => fn({ samples: left, sampleRate: 22050.5 }), RangeError);
  });

  it.each([...PAIR_CALLS, ...LINKED])('%s requires sampleRate', (name) => {
    const fn = api[name] as Fn;
    const request = LINKED.includes(name) ? { channels: [left, right, left] } : { left, right };
    expectThrows(() => fn(request), TypeError);
  });

  it.each(['masteringRepairAnalyze', 'masteringRepairApply'])('%s requires sampleRate', (name) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn({ channels: [left], stages: [] }), TypeError);
  });

  it.each(MONO)('%s takes one positional form and a request, equivalently', (name, options) => {
    const fn = api[name] as Fn;
    const samples = name.includes('Trim') ? padded() : left;
    const positional = fn(samples, sampleRate, options);
    const request = fn({ samples, sampleRate, ...options });
    expect(request).toEqual(positional);
  });

  it.each(MONO)('%s refuses a request plus an extra positional argument', (name, options) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn({ samples: left, sampleRate, ...options }, sampleRate), TypeError);
    expectThrows(() => fn(left, sampleRate, { sampleRate }), TypeError);
    expectThrows(() => fn(left, sampleRate, { samples: left }), TypeError);
  });

  it.each([...PAIR_CALLS, ...LINKED])('%s is request-only', (name) => {
    const fn = api[name] as Fn;
    // The former (left, right, sampleRate, config) and (channels, sampleRate, config) forms are gone.
    if (LINKED.includes(name)) {
      expectThrows(() => fn([left, right], sampleRate), TypeError);
    } else {
      expectThrows(() => fn(left, right, sampleRate), TypeError);
    }
    const request = LINKED.includes(name) ? { channels: [left, right], sampleRate } : pairRequest();
    expectThrows(() => fn(request, sampleRate), TypeError);
    expectThrows(() => fn({ ...request, bogus: 1 }), TypeError);
  });

  it.each(MONO)('%s refuses validate and scans for non-finite samples', (name, options) => {
    const fn = api[name] as Fn;
    const samples = name.includes('Trim') ? padded() : left;
    expectThrows(() => fn({ samples, sampleRate, ...options, validate: false }), TypeError);
    expectThrows(() => fn(samples, sampleRate, { ...options, validate: true }), TypeError);
    const bad = new Float32Array(samples);
    bad[10] = Number.NaN;
    expectThrows(() => fn({ samples: bad, sampleRate, ...options }), RangeError);
  });

  it.each(PAIR_CALLS)('%s refuses validate and scans for non-finite samples', (name) => {
    const fn = api[name] as Fn;
    const request = name.includes('Trim')
      ? { left: padded(), right: padded(), sampleRate }
      : pairRequest();
    expectThrows(() => fn({ ...request, validate: false }), TypeError);
    const bad = new Float32Array(request.left as Float32Array);
    bad[10] = Number.NaN;
    expectThrows(() => fn({ ...request, left: bad }), RangeError);
  });
});

describe('repair: results are {channels, reports}', () => {
  it.each(PAIRED)('%s returns {channels, reports}', (name, reportCount) => {
    const fn = api[name] as Fn;
    const result = fn(pairRequest()) as { channels: Float32Array[]; reports: unknown[] };
    expect(Object.keys(result).sort()).toEqual(['channels', 'reports']);
    expect(result.channels).toHaveLength(2);
    expect(result.channels[0]).toBeInstanceOf(Float32Array);
    expect(result.channels[0]?.length).toBe(left.length);
    expect(result.reports).toHaveLength(reportCount);
    expect(result).not.toHaveProperty('left');
    expect(result).not.toHaveProperty('report');
  });

  it.each(LINKED)('%s returns {channels, reports: [report]}', (name) => {
    const fn = api[name] as Fn;
    const result = fn({ channels: [left, right, left], sampleRate }) as {
      channels: Float32Array[];
      reports: unknown[];
    };
    expect(Object.keys(result).sort()).toEqual(['channels', 'reports']);
    expect(result.channels).toHaveLength(3);
    expect(result.reports).toHaveLength(1);
  });

  it('masteringRepairTrimSilenceStereo returns channels, one report and both ranges', () => {
    const pair = padded();
    const result = sonare.masteringRepairTrimSilenceStereo({ left: pair, right: pair, sampleRate });
    expect(Object.keys(result).sort()).toEqual(['channels', 'ranges', 'reports']);
    expect(result.channels).toHaveLength(2);
    expect(result.channels[0]?.length).toBeLessThan(pair.length);
    expect(result.channels[0]?.length).toBe(result.channels[1]?.length);
    expect(result.reports).toHaveLength(1);
    expect(result.ranges).toHaveLength(2);
    expectTypeOf(result.ranges).toEqualTypeOf<TrimRange[]>();
  });

  it('the dehum and denoise reports carry Float32Array detection arrays', () => {
    const hum = sonare.masteringRepairDetectHum({ samples: left, sampleRate });
    expect(hum.harmonicDbfs).toBeInstanceOf(Float32Array);
    const floor = sonare.masteringRepairDetectNoiseFloor({ samples: left, sampleRate });
    expect(floor.bandFloorDbfs).toBeInstanceOf(Float32Array);
    const dehum = sonare.masteringRepairDehumStereo(pairRequest() as never);
    expect(dehum.reports[0]?.detected.harmonicDbfs).toBeInstanceOf(Float32Array);
    const denoise = sonare.masteringRepairDenoiseClassicalStereo(pairRequest() as never);
    expect(denoise.reports[0]?.detected.bandFloorDbfs).toBeInstanceOf(Float32Array);
    expectTypeOf(hum.harmonicDbfs).toEqualTypeOf<Float32Array>();
    expectTypeOf(floor.bandFloorDbfs).toEqualTypeOf<Float32Array>();
  });

  it('removed result type names are gone', () => {
    // @ts-expect-error the Stereo/Linked result names were replaced by one result type per operation
    // biome-ignore lint/correctness/noUnusedVariables: the alias exists only to carry the expected error
    type Removed = import('../../dist/index.js').MasteringRepairDeclickStereoResult;
    expectTypeOf<MasteringRepairDeclickResult>().toHaveProperty('channels');
    expectTypeOf<MasteringRepairDenoiseClassicalResult>().toHaveProperty('reports');
    expectTypeOf<MasteringRepairTrimSilenceResult>().toHaveProperty('ranges');
  });
});

describe('repair: dBFS thresholds', () => {
  it('masteringRepairTrimSilence takes thresholdDb and refuses threshold', () => {
    const pair = padded();
    const base = sonare.masteringRepairTrimSilence({ samples: pair, sampleRate }).samples;
    const explicit = sonare.masteringRepairTrimSilence({
      samples: pair,
      sampleRate,
      thresholdDb: -60,
    }).samples;
    expectClose(base, explicit);
    const strict = sonare.masteringRepairTrimSilence({
      samples: pair,
      sampleRate,
      thresholdDb: -1,
    }).samples;
    expect(strict.length).toBeLessThanOrEqual(base.length);
    expectThrows(
      () => api.masteringRepairTrimSilence?.({ samples: pair, sampleRate, threshold: 0.01 }),
      TypeError,
    );
    expectThrows(
      () => sonare.masteringRepairTrimSilence({ samples: pair, sampleRate, thresholdDb: 3 }),
      RangeError,
    );
    expectThrows(
      () =>
        sonare.masteringRepairTrimSilence({
          samples: pair,
          sampleRate,
          thresholdDb: '-60' as unknown as number,
        }),
      TypeError,
    );
  });

  it.each([
    'masteringRepairDetectTrimRange',
    'masteringRepairTrimSilenceStereo',
    'masteringRepairDetectTrimRangeStereo',
  ])('%s takes thresholdDb', (name) => {
    const fn = api[name] as Fn;
    const pair = padded();
    const request =
      name === 'masteringRepairDetectTrimRange'
        ? { samples: pair, sampleRate }
        : { left: pair, right: pair, sampleRate };
    expect(fn({ ...request, thresholdDb: -40 })).toEqual(fn({ ...request, thresholdDb: -40 }));
    expectThrows(() => fn({ ...request, threshold: 0.01 }), TypeError);
    expectThrows(() => fn({ ...request, thresholdDb: 1 }), RangeError);
  });

  it.each(['masteringRepairDeclip', 'masteringRepairDetectClipping'])(
    '%s takes clipThresholdDb',
    (name) => {
      const fn = api[name] as Fn;
      const base = fn({ samples: left, sampleRate });
      const explicit = fn({ samples: left, sampleRate, clipThresholdDb: 20 * Math.log10(0.98) });
      if (base instanceof Float32Array) {
        expectClose(base, explicit as Float32Array, 1e-4);
      } else {
        expect(explicit).toEqual(base);
      }
      expectThrows(() => fn({ samples: left, sampleRate, clipThreshold: 0.98 }), TypeError);
      expectThrows(() => fn({ samples: left, sampleRate, clipThresholdDb: 0.5 }), RangeError);
      expectThrows(() => fn({ samples: left, sampleRate, clipThresholdDb: '-1' }), TypeError);
    },
  );

  it('masteringRepairDeclipStereo takes clipThresholdDb', () => {
    expect(() =>
      sonare.masteringRepairDeclipStereo({ left, right, sampleRate, clipThresholdDb: -3 }),
    ).not.toThrow();
    expectThrows(
      () => api.masteringRepairDeclipStereo?.({ left, right, sampleRate, clipThreshold: 0.5 }),
      TypeError,
    );
  });
});

describe('repair: request-only helpers', () => {
  it('masteringRepairNoiseBandBins takes a request with a required sampleRate', () => {
    const bins = sonare.masteringRepairNoiseBandBins({ sampleRate: 48000 });
    expect(bins).toBeInstanceOf(Int32Array);
    expect(bins).toHaveLength(33);
    expect(sonare.masteringRepairNoiseBandBins({ nFft: 2048, sampleRate: 48000 })[32]).toBe(1025);
    expectThrows(() => api.masteringRepairNoiseBandBins?.({ nFft: 1024 }), TypeError);
    expectThrows(() => api.masteringRepairNoiseBandBins?.(1024, 22050), TypeError);
    expectThrows(() => api.masteringRepairNoiseBandBins?.(), TypeError);
    expectThrows(() => sonare.masteringRepairNoiseBandBins({ sampleRate: 0 }), RangeError);
  });

  it('masteringRepairDereverbConfigForRoom takes a request and refuses the positional form', () => {
    expectThrows(
      () => api.masteringRepairDereverbConfigForRoom?.({ volume: 100, rt60Bands: [] }, {}),
      TypeError,
    );
    expectThrows(() => api.masteringRepairDereverbConfigForRoom?.({}), TypeError);
    expectThrows(() => api.masteringRepairDereverbConfigForRoom?.({ estimate: null }), TypeError);
  });
});

describe('repair: session', () => {
  it('masteringRepairApply stages are a discriminated union with clipThresholdDb', () => {
    const result = sonare.masteringRepairApply({
      channels: [left, right],
      sampleRate,
      stages: [{ stage: 'declip', clipThresholdDb: -0.5 }, { stage: 'declick' }],
    });
    expect(result.channels).toHaveLength(2);
    expect(result.reports.map((r) => r.stage)).toEqual(['declip', 'declick']);
    // The stage list is a document the core reads, so an earlier key is its InvalidFormat refusal.
    const earlier = () =>
      api.masteringRepairApply?.({
        channels: [left],
        sampleRate,
        stages: [{ stage: 'declip', clipThreshold: 0.9 }],
      });
    expect(earlier).toThrow(expect.objectContaining({ code: sonare.ErrorCode.InvalidFormat }));
    expect(earlier).toThrow(
      /unknown key \[0\]\.clipThreshold \(renamed to \[0\]\.clipThresholdDb\)/,
    );
    // @ts-expect-error a misspelt stage setting is a type error
    const _bad: MasteringRepairStage = { stage: 'declick', thresholdd: 1 };
    // @ts-expect-error the stage name is a closed set
    const _name: MasteringRepairStage = { stage: 'flatten' };
    expectTypeOf<MasteringRepairStage>().toMatchTypeOf<{ stage: string }>();
  });

  it('masteringRepairAnalyze recommends stages that masteringRepairApply takes as they stand', () => {
    const analysis = sonare.masteringRepairAnalyze({ channels: [left], sampleRate });
    for (const stage of analysis.recommended) {
      expect(stage).not.toHaveProperty('clipThreshold');
    }
    expect(() =>
      sonare.masteringRepairApply({ channels: [left], sampleRate, stages: analysis.recommended }),
    ).not.toThrow();
    expect(() =>
      sonare.masteringRepairAnalyze({ channels: [left], sampleRate, validate: false } as never),
    ).toThrow(TypeError);
  });
});

describe('repair: mono results are {samples, report}', () => {
  const rate = 22050;

  function tone(hz: number, frames: number, amp: number): Float32Array {
    const out = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
      out[i] = amp * Math.sin((2 * Math.PI * hz * i) / rate);
    }
    return out;
  }

  function noisy(frames: number): Float32Array {
    const out = tone(440, frames, 0.3);
    let state = 12345;
    for (let i = 0; i < frames; i++) {
      state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
      out[i] = (out[i] ?? 0) + ((state >>> 8) / (1 << 24) - 0.5) * 0.05;
    }
    return out;
  }

  it('the result types carry the audio and the report', () => {
    expectTypeOf<DeclickResult>().toEqualTypeOf<{ samples: Float32Array; report: DeclickReport }>();
    expectTypeOf<DeclipResult>().toHaveProperty('report');
    expectTypeOf<DecrackleResult>().toHaveProperty('report');
    expectTypeOf<DehumResult>().toHaveProperty('report');
    expectTypeOf<DenoiseResult>().toHaveProperty('report');
    expectTypeOf<DereverbResult>().toHaveProperty('report');
    expectTypeOf<TrimSilenceResult>().toEqualTypeOf<{
      samples: Float32Array;
      report: TrimReport;
    }>();
  });

  it('masteringRepairDeclick reports the clicks it repaired', () => {
    const bed = tone(440, 11025, 0.15);
    const clicked = bed.slice();
    for (const pos of [2053, 5462, 8100]) {
      clicked[pos] = (clicked[pos] ?? 0) + 0.6;
      clicked[pos + 1] = (clicked[pos + 1] ?? 0) + 0.6;
    }
    const options = { threshold: 0.35, neighborRatio: 2, maxClickSamples: 8, lpcOrder: 20 };
    const hit = sonare.masteringRepairDeclick({ samples: clicked, sampleRate: rate, ...options });
    expect(hit.samples).toBeInstanceOf(Float32Array);
    expect(hit.samples.length).toBe(clicked.length);
    expect(hit.report.detected.count).toBeGreaterThan(0);
    expect(hit.report.repairedRuns).toBeGreaterThan(0);
    expect(hit.report.repairedSamples).toBeGreaterThan(0);
    expect(hit.report.linkedRuns).toBe(0);
    const clean = sonare.masteringRepairDeclick({ samples: bed, sampleRate: rate, ...options });
    expect(clean.report.detected.count).toBe(0);
    expect(clean.report.repairedRuns).toBe(0);
    expect(clean.report.repairedSamples).toBe(0);
  });

  it('masteringRepairDeclip reports the clipped run it rebuilt', () => {
    const bed = tone(440, 11025, 0.3);
    const clipped = bed.slice();
    clipped.fill(0.95, 5000, 5020);
    const options = { clipThresholdDb: -0.9151 };
    const hit = sonare.masteringRepairDeclip({ samples: clipped, sampleRate: rate, ...options });
    expect(hit.samples.length).toBe(clipped.length);
    expect(hit.report.detected.clippedSamples).toBeGreaterThan(0);
    expect(hit.report.lpcReconstructedRuns + hit.report.interpolatedRuns).toBeGreaterThan(0);
    expect(hit.report.repairedSamples).toBeGreaterThan(0);
    const clean = sonare.masteringRepairDeclip({ samples: bed, sampleRate: rate, ...options });
    expect(clean.report.detected.clippedSamples).toBe(0);
    expect(clean.report.repairedSamples).toBe(0);
  });

  it('masteringRepairDecrackle reports the samples it replaced', () => {
    const bed = tone(440, 2000, 0.05);
    const spiked = bed.slice();
    for (const [i, pos] of [100, 300, 500, 700, 900].entries()) {
      spiked[pos] = i % 2 === 0 ? 0.5 : -0.5;
    }
    const hit = sonare.masteringRepairDecrackle({
      samples: spiked,
      sampleRate: rate,
      threshold: 0.3,
    });
    expect(hit.samples.length).toBe(spiked.length);
    expect(hit.report.detected.sampleCount).toBe(5);
    expect(hit.report.replacedSamples).toBe(5);
    const clean = sonare.masteringRepairDecrackle({
      samples: bed,
      sampleRate: rate,
      threshold: 0.3,
    });
    expect(clean.report.detected.sampleCount).toBe(0);
    expect(clean.report.replacedSamples).toBe(0);
  });

  it('masteringRepairDehum reports the hum it found', () => {
    const hummed = tone(50, rate, 0.3);
    const second = tone(100, rate, 0.15);
    const high = tone(3000, rate, 0.05);
    for (let i = 0; i < hummed.length; i++) {
      hummed[i] = (hummed[i] ?? 0) + (second[i] ?? 0) + (high[i] ?? 0);
    }
    const hit = sonare.masteringRepairDehum({ samples: hummed, sampleRate: rate });
    expect(hit.samples.length).toBe(hummed.length);
    expect(hit.report.detected.harmonics).toBeGreaterThanOrEqual(2);
    expect(hit.report.notchedHarmonics).toBeGreaterThan(0);
    const clean = sonare.masteringRepairDehum({ samples: high, sampleRate: rate });
    expect(clean.report.detected.harmonics).toBe(0);
  });

  it('masteringRepairDenoiseClassical returns a finite report and the input length', () => {
    const input = noisy(11025);
    const { samples, report } = sonare.masteringRepairDenoiseClassical({
      samples: input,
      sampleRate: rate,
    });
    expect(samples.length).toBe(input.length);
    for (const value of [
      report.meanReductionDb,
      report.maxReductionDb,
      report.floorLimitedFraction,
    ]) {
      expect(Number.isFinite(value)).toBe(true);
    }
  });

  it('masteringRepairDereverbClassical returns a finite report and the input length', () => {
    const input = noisy(11025);
    const { samples, report } = sonare.masteringRepairDereverbClassical({
      samples: input,
      sampleRate: rate,
    });
    expect(samples.length).toBe(input.length);
    for (const value of [
      report.meanReductionDb,
      report.suppressedFraction,
      report.wpePredictorNorm,
    ]) {
      expect(Number.isFinite(value)).toBe(true);
    }
  });

  it('masteringRepairTrimSilence reports the range it kept, and tolerates all silence', () => {
    const { samples, report } = sonare.masteringRepairTrimSilence({
      samples: padded(),
      sampleRate,
    });
    expect(samples.length).toBeGreaterThan(0);
    expect(report.range.lastExclusive - report.range.first).toBe(samples.length);
    expect(report.removedHeadSamples + report.removedTailSamples).toBe(
      padded().length - samples.length,
    );
    const silent = sonare.masteringRepairTrimSilence({
      samples: new Float32Array(sampleRate / 4),
      sampleRate,
    });
    expect(silent.samples.length).toBe(0);
  });
});
