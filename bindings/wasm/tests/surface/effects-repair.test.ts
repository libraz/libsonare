import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import type {
  MasteringRepairDeclickResult,
  MasteringRepairDenoiseClassicalResult,
  MasteringRepairStage,
  MasteringRepairTrimSilenceResult,
  TrimRange,
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

  it.each(MONO)('%s declares validate as an option of the request', (name, options) => {
    const fn = api[name] as Fn;
    const samples = name.includes('Trim') ? padded() : left;
    expect(() => fn({ samples, sampleRate, ...options, validate: false })).not.toThrow();
    expect(() => fn(samples, sampleRate, { ...options, validate: true })).not.toThrow();
    const bad = new Float32Array(samples);
    bad[10] = Number.NaN;
    expectThrows(() => fn({ samples: bad, sampleRate, ...options }), RangeError);
  });

  it.each(PAIR_CALLS)('%s declares validate as an option of the request', (name) => {
    const fn = api[name] as Fn;
    const request = name.includes('Trim')
      ? { left: padded(), right: padded(), sampleRate }
      : pairRequest();
    expect(() => fn({ ...request, validate: false })).not.toThrow();
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
    const base = sonare.masteringRepairTrimSilence({ samples: pair, sampleRate });
    const explicit = sonare.masteringRepairTrimSilence({
      samples: pair,
      sampleRate,
      thresholdDb: -60,
    });
    expectClose(base, explicit);
    const strict = sonare.masteringRepairTrimSilence({
      samples: pair,
      sampleRate,
      thresholdDb: -1,
    });
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
    expectThrows(
      () =>
        api.masteringRepairApply?.({
          channels: [left],
          sampleRate,
          stages: [{ stage: 'declip', clipThreshold: 0.9 }],
        }),
      TypeError,
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
      sonare.masteringRepairAnalyze({ channels: [left], sampleRate, validate: false }),
    ).not.toThrow();
  });
});
