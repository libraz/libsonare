import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  MasteringAssistantResult,
  MasteringAudioProfile,
  MasteringChainConfig,
  MasteringChainResult,
  MasteringNormalizeLoudnessResult,
  MasteringPlatform,
  MasteringResult,
  MasteringStreamingPreviewResult,
  StreamingLoudnessGainResult,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import { SonareError } from '../../dist/index.js';
import { addon } from '../../src/native.js';

type Fn = (...args: unknown[]) => unknown;
// Any exported function by name, for tables that sweep many entry points.
const api = sonare as unknown as Record<string, Fn>;

const sampleRate = 22050;

/** About 0.5 s of two tones, long enough for a loudness measurement. */
function tone(hz: number): Float32Array {
  const out = new Float32Array(sampleRate / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  return out;
}

const left = tone(440);
const right = tone(330);

/** Long enough (6 s) for the loudness range to be measured; shorter signals leave it absent. */
function longTone(hz: number): Float32Array {
  const out = new Float32Array(sampleRate * 6);
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  return out;
}

const longLeft = longTone(440);
const longRight = longTone(330);

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

/** A refused argument: plain TypeError (shape) or RangeError (domain), never a SonareError document refusal. */
function expectRefused(fn: () => unknown, cls: ErrorConstructor, message: string): void {
  const error = errorOf(fn);
  expect(error).toBeInstanceOf(cls);
  expect(error).not.toBeInstanceOf(SonareError);
  expect((error as Error).message).toContain(message);
  // An undeclared key never points at a successor; UPGRADING carries the old -> new map.
  expect((error as Error).message).not.toContain('(renamed to');
}

function expectClose(a: ArrayLike<number>, b: ArrayLike<number>, tol = 1e-4): void {
  expect(a.length).toBe(b.length);
  for (let i = 0; i < a.length; i++) {
    expect(Math.abs((a[i] as number) - (b[i] as number))).toBeLessThanOrEqual(tol);
  }
}

describe('mastering chain: one name per operation', () => {
  it.each([
    'masteringChain',
    'masteringChainStereo',
    'masteringChainWithProgress',
    'masteringChainStereoWithProgress',
    'masterAudioWithProgress',
    'masterAudioStereoWithProgress',
    'mastering',
    'masteringProcess',
    'masteringProcessStereo',
    'masteringPairProcess',
    'masteringPairProcessStereo',
    'masteringPairAnalyze',
    'masteringStereoAnalyze',
  ])('%s is gone', (name) => {
    expect(api[name]).toBeUndefined();
  });

  it.each([
    'masteringNormalizeLoudness',
    'masteringApplyProcessor',
    'masteringApplyProcessorStereo',
    'masteringApplyPairProcessor',
    'masteringApplyPairProcessorStereo',
    'masteringAnalyzePair',
    'masteringAnalyzeStereo',
  ])('%s is exported', (name) => {
    expect(typeof api[name]).toBe('function');
  });

  it('Audio carries the renamed methods only', () => {
    const audio = sonare.Audio.fromBuffer(left, sampleRate) as unknown as Record<string, Fn>;
    expect(typeof audio.masteringNormalizeLoudness).toBe('function');
    expect(typeof audio.masteringApplyProcessor).toBe('function');
    expect(typeof audio.masterAudio).toBe('function');
    expect(audio.mastering).toBeUndefined();
    expect(audio.masteringChain).toBeUndefined();
    expect(audio.masteringProcess).toBeUndefined();
  });

  it('Audio.masterAudio takes one labelled object', () => {
    const audio = sonare.Audio.fromBuffer(left, sampleRate);
    const viaAudio = audio.masterAudio({ preset: 'pop' });
    const direct = sonare.masterAudio({ samples: left, sampleRate, preset: 'pop' });
    expectClose(viaAudio.samples, direct.samples);
    const calls: number[] = [];
    audio.masterAudio({ preset: 'pop', onProgress: (progress) => calls.push(progress) });
    expect(calls.length).toBeGreaterThan(0);
    expectThrows(() => (audio.masterAudio as Fn)('pop'), TypeError);
  });
});

/** Mono entry points taking `(samples, sampleRate, options?)` or a request, with the options a call needs. */
const MONO: ReadonlyArray<[string, Record<string, unknown>]> = [
  ['masterAudio', { preset: 'pop' }],
  ['masteringNormalizeLoudness', {}],
  ['masteringApplyProcessor', { processor: 'dynamics.limiter' }],
  ['masteringAudioProfile', {}],
  ['masteringAssistantSuggest', {}],
  ['masteringStreamingPreview', {}],
  ['streamingLoudnessGain', {}],
];

/** Stereo entry points: request only. */
const STEREO: ReadonlyArray<[string, Record<string, unknown>]> = [
  ['masterAudioStereo', { preset: 'pop' }],
  ['masteringApplyProcessorStereo', { processor: 'dynamics.limiter' }],
  ['masteringAnalyzeStereo', { analysis: 'stereo.monoCompatCheck' }],
  ['masteringAudioProfileStereo', {}],
  ['masteringAssistantSuggestStereo', {}],
  ['masteringStreamingPreviewStereo', {}],
  ['streamingLoudnessGainStereo', {}],
];

/** Two-input entry points: request only. */
const PAIRED: ReadonlyArray<[string, Record<string, unknown>]> = [
  ['masteringApplyPairProcessor', { processor: 'match.abSwitch', source: left, reference: right }],
  ['masteringAnalyzePair', { analysis: 'match.referenceLoudness', source: left, reference: right }],
  ['masteringAbMatchLoudness', { source: left, reference: right }],
  [
    'masteringApplyPairProcessorStereo',
    {
      processor: 'match.abCrossfade',
      sourceLeft: left,
      sourceRight: right,
      referenceLeft: left,
      referenceRight: right,
    },
  ],
  [
    'masteringAbMatchLoudnessStereo',
    { sourceLeft: left, sourceRight: right, referenceLeft: left, referenceRight: right },
  ],
];

describe('mastering chain: sample rate and call shape', () => {
  it.each(MONO)('%s requires sampleRate in both forms', (name, options) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn(left, undefined, options), TypeError);
    expectThrows(() => fn({ samples: left, ...options }), TypeError);
    expectThrows(() => fn({ samples: left, sampleRate: '22050', ...options }), TypeError);
    expectThrows(() => fn({ samples: left, sampleRate: 22050.5, ...options }), RangeError);
  });

  it.each(STEREO)('%s requires sampleRate and refuses the positional spelling', (name, options) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn({ left, right, ...options }), TypeError);
    expectThrows(() => fn({ left, right, sampleRate: 22050.5, ...options }), RangeError);
    expectThrows(() => fn(left, right, sampleRate), TypeError);
  });

  it.each(PAIRED)('%s requires sampleRate', (name, options) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn({ ...options }), TypeError);
    expectThrows(() => fn({ ...options, sampleRate: 22050.5 }), RangeError);
  });

  it.each(MONO)('%s refuses a request plus a positional argument', (name, options) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn({ samples: left, sampleRate, ...options }, sampleRate), TypeError);
  });

  it.each(MONO)('%s refuses an unknown request key', (name, options) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn({ samples: left, sampleRate, ...options, notAField: 1 }), TypeError);
  });

  it.each([
    ['masteringApplyPairProcessor', 'masteringProcess'],
    ['masteringAnalyzePair', 'masteringPairAnalyze'],
  ])('%s is request-only', (name) => {
    const fn = api[name] as Fn;
    expectThrows(() => fn('match.referenceLoudness', left, right, sampleRate), TypeError);
  });

  it('masterAudio takes (samples, sampleRate, options) and refuses the removed overload', () => {
    const request = sonare.masterAudio({ samples: left, sampleRate, preset: 'pop' });
    const positional = sonare.masterAudio(left, sampleRate, { preset: 'pop' });
    expectClose(positional.samples, request.samples);
    expectThrows(() => (sonare.masterAudio as Fn)(left, sampleRate, 'pop'), TypeError);
    expectThrows(() => (sonare.masterAudio as Fn)(left, sampleRate, 'pop', {}), TypeError);
    expectThrows(() => sonare.masterAudio(left, sampleRate, { samples: left } as never), TypeError);
  });

  it('request and positional forms agree on result, defaults and refusal', () => {
    const request = sonare.masteringNormalizeLoudness({ samples: left, sampleRate });
    const positional = sonare.masteringNormalizeLoudness(left, sampleRate);
    expectClose(positional.samples, request.samples);
    expect(Math.abs(positional.appliedGainDb - request.appliedGainDb)).toBeLessThan(1e-6);
    expectThrows(
      () => sonare.masteringNormalizeLoudness(left, sampleRate, { x: 1 } as never),
      TypeError,
    );
    expectThrows(
      () => sonare.masteringNormalizeLoudness({ samples: left, sampleRate, x: 1 } as never),
      TypeError,
    );
  });
});

describe('masterAudio: preset, overrides, progress and cancel', () => {
  const overrides: MasteringChainConfig = { loudness: { enabled: true, targetLufs: -20 } };

  it('runs overrides alone when no preset is given', () => {
    const alone = sonare.masterAudio({ samples: left, sampleRate, overrides });
    const withPreset = sonare.masterAudio({ samples: left, sampleRate, preset: 'pop', overrides });
    expect(alone.stages.length).toBeLessThan(withPreset.stages.length);
  });

  it('masterAudioStereo runs overrides alone when no preset is given', () => {
    const alone = sonare.masterAudioStereo({ left, right, sampleRate, overrides });
    const withPreset = sonare.masterAudioStereo({
      left,
      right,
      sampleRate,
      preset: 'pop',
      overrides,
    });
    expect(alone.stages.length).toBeLessThan(withPreset.stages.length);
  });

  it.each(['masterAudio', 'masterAudioStereo'])(
    '%s reports progress and honours cancel',
    (name) => {
      const fn = api[name] as Fn;
      const audio = name === 'masterAudio' ? { samples: left } : { left, right };
      const stages: string[] = [];
      fn({
        ...audio,
        sampleRate,
        preset: 'pop',
        onProgress: (_p: number, stage: string) => stages.push(stage),
      });
      expect(stages.length).toBeGreaterThan(0);
      const cancelled = errorOf(() =>
        fn({ ...audio, sampleRate, preset: 'pop', cancel: () => true }),
      );
      expect(cancelled).toBeInstanceOf(SonareError);
      expect((cancelled as SonareError).codeName).toBe('Cancelled');
      expectThrows(() => fn({ ...audio, sampleRate, preset: 'pop', onProgress: 'x' }), TypeError);
      expectThrows(() => fn({ ...audio, sampleRate, preset: 'pop', cancel: 1 }), TypeError);
    },
  );

  it('refuses an unknown preset and a non-string preset', () => {
    expectThrows(
      () => sonare.masterAudio({ samples: left, sampleRate, preset: 'nope' as never }),
      Error,
    );
    expectThrows(
      () => sonare.masterAudio({ samples: left, sampleRate, preset: 3 as never }),
      TypeError,
    );
  });

  it('refuses non-finite audio and a validate key', () => {
    const bad = new Float32Array(left);
    bad[10] = Number.NaN;
    expectThrows(() => sonare.masterAudio({ samples: bad, sampleRate, preset: 'pop' }), RangeError);
    expectThrows(
      () =>
        sonare.masterAudio({ samples: left, sampleRate, preset: 'pop', validate: false } as never),
      TypeError,
    );
  });
});

describe('chain config: nested form only, tokens only', () => {
  it('masteringPresetParams returns the nested config and a result passes back as overrides', () => {
    const params = sonare.masteringPresetParams('pop') as Record<string, unknown>;
    expect(Object.keys(params).some((key) => key.includes('.'))).toBe(false);
    expect(typeof params.loudness).toBe('object');
    const viaOverrides = sonare.masterAudio({
      samples: left,
      sampleRate,
      preset: 'pop',
      overrides: params as MasteringChainConfig,
    });
    const preset = sonare.masterAudio({ samples: left, sampleRate, preset: 'pop' });
    expectClose(viaOverrides.samples, preset.samples);
  });

  it.each([
    ['repair.denoise.mode', { repair: { denoise: { enabled: true, mode: 'mmseStsa' } } }],
    [
      'repair.denoise.noiseEstimator',
      { repair: { denoise: { enabled: true, noiseEstimator: 'mcra' } } },
    ],
    ['repair.decrackle.mode', { repair: { decrackle: { enabled: true, mode: 'median' } } }],
    ['repair.dehum.mode', { repair: { dehum: { enabled: true, mode: 'notch' } } }],
    [
      'crossover.slope',
      { dynamics: { multibandComp: { enabled: true, crossover: { slope: 'lr4' } } } },
    ],
    [
      'crossover.mode',
      { dynamics: { multibandComp: { enabled: true, crossover: { mode: 'linkwitzRiley' } } } },
    ],
    [
      'bands.detector',
      {
        dynamics: {
          multibandComp: { enabled: true, bands: [{ detector: 'logRms' }, {}, {}] },
        },
      },
    ],
  ])('%s takes its token', (_name, overrides) => {
    expect(() =>
      sonare.masterAudio({
        samples: left,
        sampleRate,
        overrides: overrides as MasteringChainConfig,
      }),
    ).not.toThrow();
  });

  it.each([
    [
      'denoise.mode number',
      { repair: { denoise: { enabled: true, mode: 1 } } },
      'repair.denoise.mode must be one of logMmse, mmseStsa, spectralSubtraction',
    ],
    [
      'denoise.mode synonym',
      { repair: { denoise: { enabled: true, mode: 'log_mmse' } } },
      'repair.denoise.mode must be one of logMmse, mmseStsa, spectralSubtraction',
    ],
    [
      'noiseEstimator number',
      { repair: { denoise: { noiseEstimator: 2 } } },
      'repair.denoise.noiseEstimator must be one of quantile, mcra, imcra, spp',
    ],
    [
      'decrackle.mode number',
      { repair: { decrackle: { mode: 1 } } },
      'repair.decrackle.mode must be one of median, waveletShrinkage',
    ],
    [
      'dehum.mode number',
      { repair: { dehum: { mode: 0 } } },
      'repair.dehum.mode must be one of subtract, notch',
    ],
    [
      'crossover.slope number',
      { dynamics: { multibandComp: { crossover: { slope: 1 } } } },
      'dynamics.multibandComp.crossover.slope must be one of lr2, lr4, lr8',
    ],
    [
      'crossover.mode number',
      { dynamics: { multibandComp: { crossover: { mode: 2 } } } },
      'dynamics.multibandComp.crossover.mode must be one of linkwitzRiley, butterworth, bessel, firLinearPhase',
    ],
    [
      'band detector old spelling',
      { dynamics: { multibandComp: { bands: [{ detector: 'log_rms' }] } } },
      'dynamics.multibandComp.bands.0.detector must be one of peak, rms, logRms',
    ],
    [
      'band detector number',
      { dynamics: { multibandComp: { bands: [{ detector: 1 }] } } },
      'dynamics.multibandComp.bands.0.detector must be one of peak, rms, logRms',
    ],
  ])('the core refuses %s as an argument, naming the path', (name, overrides, message) => {
    expectRefused(
      () =>
        sonare.masterAudio({
          samples: left,
          sampleRate,
          overrides: overrides as MasteringChainConfig,
        }),
      name.endsWith('number') ? TypeError : RangeError,
      `masterAudio: overrides.${message}`,
    );
  });

  it.each([
    [
      'boolean denoise shorthand',
      { repair: { denoise: true } },
      'masterAudio: unknown key overrides.repair.denoise',
    ],
    [
      'flat repair.nFft',
      { repair: { nFft: 1024 } },
      'masterAudio: unknown key overrides.repair.nFft',
    ],
    [
      'flat repair.hopLength',
      { repair: { hopLength: 256 } },
      'masterAudio: unknown key overrides.repair.hopLength',
    ],
    [
      'flat repair.ddAlpha',
      { repair: { ddAlpha: 0.9 } },
      'masterAudio: unknown key overrides.repair.ddAlpha',
    ],
    [
      'flat repair.reductionDb',
      { repair: { reductionDb: 12 } },
      'masterAudio: unknown key overrides.repair.reductionDb',
    ],
    [
      'flat repair.gainFloor',
      { repair: { gainFloor: 0.2 } },
      'masterAudio: unknown key overrides.repair.gainFloor',
    ],
    [
      'denoise.gainFloor',
      { repair: { denoise: { gainFloor: 0.2 } } },
      'masterAudio: unknown key overrides.repair.denoise.gainFloor',
    ],
    ['eq.tiltDb', { eq: { tiltDb: 1 } }, 'masterAudio: unknown key overrides.eq.tiltDb'],
    ['eq.pivotHz', { eq: { pivotHz: 1000 } }, 'masterAudio: unknown key overrides.eq.pivotHz'],
    [
      'declip.clipThreshold',
      { repair: { declip: { clipThreshold: 0.9 } } },
      'masterAudio: unknown key overrides.repair.declip.clipThreshold',
    ],
    [
      'band sidechainHpfHz',
      { dynamics: { multibandComp: { bands: [{ sidechainHpfHz: 100 }] } } },
      'masterAudio: unknown key overrides.dynamics.multibandComp.bands.0.sidechainHpfHz',
    ],
    [
      'band sidechainHpfEnabled',
      { dynamics: { multibandComp: { bands: [{ sidechainHpfEnabled: true }] } } },
      'masterAudio: unknown key overrides.dynamics.multibandComp.bands.0.sidechainHpfEnabled',
    ],
  ])(
    'the core refuses the removed spelling %s as any other unknown key',
    (_name, overrides, message) => {
      expectRefused(
        () =>
          sonare.masterAudio({
            samples: left,
            sampleRate,
            overrides: overrides as MasteringChainConfig,
          }),
        TypeError,
        message,
      );
    },
  );

  it('takes a dotted path as the same chain as its nested spelling', () => {
    const nested = sonare.masterAudio({
      samples: left,
      sampleRate,
      overrides: { loudness: { targetLufs: -20 }, eq: { tilt: { tiltDb: 1 } } },
    });
    const dotted = sonare.masterAudio({
      samples: left,
      sampleRate,
      overrides: { 'loudness.targetLufs': -20, eq: { 'tilt.tiltDb': 1 } } as MasteringChainConfig,
    });
    expect(Array.from(dotted.samples)).toEqual(Array.from(nested.samples));
  });

  it('refuses one path given twice, once nested and once dotted', () => {
    expectRefused(
      () =>
        sonare.masterAudio({
          samples: left,
          sampleRate,
          overrides: {
            loudness: { targetLufs: -14 },
            'loudness.targetLufs': -14,
          } as MasteringChainConfig,
        }),
      TypeError,
      'masterAudio: overrides.loudness.targetLufs must not be given twice',
    );
  });

  describe('chain content is refused by the core under the caller argument it arrived in', () => {
    const mono = () => tone(440);
    const CALLS: ReadonlyArray<[string, string, (config: Record<string, unknown>) => unknown]> = [
      [
        'masterAudio',
        'masterAudio: overrides.',
        (c) =>
          sonare.masterAudio({ samples: mono(), sampleRate, overrides: c as MasteringChainConfig }),
      ],
      [
        'masterAudio preset',
        'masterAudio: overrides.',
        (c) =>
          sonare.masterAudio({
            samples: mono(),
            sampleRate,
            preset: 'pop',
            overrides: c as MasteringChainConfig,
          }),
      ],
      [
        'masterAudioStereo',
        'masterAudioStereo: overrides.',
        (c) =>
          sonare.masterAudioStereo({
            left: mono(),
            right: mono(),
            sampleRate,
            overrides: c as MasteringChainConfig,
          }),
      ],
      [
        'streamingLoudnessGain',
        'streamingLoudnessGain: config.',
        (c) => sonare.streamingLoudnessGain({ samples: mono(), sampleRate, config: c as never }),
      ],
      [
        'streamingLoudnessGainStereo',
        'streamingLoudnessGainStereo: config.',
        (c) =>
          sonare.streamingLoudnessGainStereo({
            left: mono(),
            right: mono(),
            sampleRate,
            config: c as never,
          }),
      ],
      [
        'StreamingMasteringChain',
        'StreamingMasteringChain: ',
        (c) => new sonare.StreamingMasteringChain({ sampleRate, numChannels: 1, ...c }),
      ],
    ];
    const REFUSALS: ReadonlyArray<[string, Record<string, unknown>, ErrorConstructor, string]> = [
      [
        'a path given twice',
        { eq: { tilt: { tiltDb: 1 } }, 'eq.tilt.tiltDb': 1 },
        TypeError,
        'eq.tilt.tiltDb must not be given twice',
      ],
      [
        'a null leaf',
        { loudness: { targetLufs: null } },
        TypeError,
        'loudness.targetLufs must be a number, a boolean or a token',
      ],
      [
        'a number for a token',
        { dynamics: { compressor: { detector: 1 } } },
        TypeError,
        'dynamics.compressor.detector must be one of peak, rms, logRms',
      ],
      [
        'a clip threshold above full scale',
        { repair: { declip: { clipThresholdDb: 1 } } },
        RangeError,
        'repair.declip.clipThresholdDb must be finite and in [-890, 0]',
      ],
    ];
    for (const [call, prefix, run] of CALLS) {
      it.each(REFUSALS)(`${call}: %s`, (_name, config, cls, text) => {
        const error = errorOf(() => run(config));
        expect(error).toBeInstanceOf(cls);
        expect(error).not.toBeInstanceOf(SonareError);
        expect((error as Error).message).toBe(`${prefix}${text}`);
      });
    }
  });

  it('refuses a string on a number leaf the same way offline and streaming', () => {
    const bad = { maximizer: { truePeakLimiter: { ceilingDbtp: '-1.0' } } } as never;
    const message = 'maximizer.truePeakLimiter.ceilingDbtp must be a number or a boolean';
    expectRefused(
      () => sonare.masterAudio({ samples: left, sampleRate, overrides: bad }),
      TypeError,
      `masterAudio: overrides.${message}`,
    );
    expectRefused(
      () => new sonare.StreamingMasteringChain({ sampleRate, numChannels: 1, ...(bad as object) }),
      TypeError,
      message,
    );
  });

  it('refuses a renamed chain key as any other unknown key, the same way offline and streaming', () => {
    for (const [bad, oldKey] of [
      [{ loudness: { ceilingDb: -1 } }, 'loudness.ceilingDb'],
      [
        { maximizer: { truePeakLimiter: { ceilingDb: -1 } } },
        'maximizer.truePeakLimiter.ceilingDb',
      ],
    ] as const) {
      expectRefused(
        () => sonare.masterAudio({ samples: left, sampleRate, overrides: bad as never }),
        TypeError,
        `masterAudio: unknown key overrides.${oldKey}`,
      );
      expectRefused(
        () =>
          new sonare.StreamingMasteringChain({ sampleRate, numChannels: 1, ...(bad as object) }),
        TypeError,
        `StreamingMasteringChain: unknown key ${oldKey}`,
      );
    }
  });

  it('declip.clipThresholdDb is dBFS at or below 0', () => {
    const run = (clipThresholdDb: unknown) =>
      sonare.masterAudio({
        samples: left,
        sampleRate,
        overrides: {
          repair: { declip: { enabled: true, clipThresholdDb } },
        } as MasteringChainConfig,
      });
    expect(() => run(-1)).not.toThrow();
    expect(() => run(0)).not.toThrow();
    expect(() => run(-890)).not.toThrow();
    expectRefused(
      () => run(0.5),
      RangeError,
      'masterAudio: overrides.repair.declip.clipThresholdDb must be finite and in [-890, 0]',
    );
    expectRefused(
      () => run(-891),
      RangeError,
      'masterAudio: overrides.repair.declip.clipThresholdDb must be finite and in [-890, 0]',
    );
    expectRefused(
      () => run(Number.NaN),
      TypeError,
      'masterAudio: overrides.repair.declip.clipThresholdDb must be a number, a boolean or a token',
    );
    expectRefused(
      () => run('-1'),
      TypeError,
      'repair.declip.clipThresholdDb must be a number or a boolean',
    );
  });

  it('band sidechainHighPassHz is a non-negative frequency; absent or 0 is off', () => {
    const run = (sidechainHighPassHz: unknown) =>
      sonare.masterAudio({
        samples: left,
        sampleRate,
        overrides: {
          dynamics: { multibandComp: { enabled: true, bands: [{ sidechainHighPassHz }, {}, {}] } },
        } as MasteringChainConfig,
      });
    expect(() => run(100)).not.toThrow();
    expect(() => run(undefined)).not.toThrow();
    expect(() => run(0)).not.toThrow();
    expectRefused(() => run(-5), RangeError, 'sidechainHighPassHz');
    expectRefused(
      () => run('100'),
      TypeError,
      'dynamics.multibandComp.bands.0.sidechainHighPassHz must be a number or a boolean',
    );
  });

  it('the assistant chain nests, with tokens and dBFS, and passes straight back', () => {
    const chain = sonare.masteringAssistantSuggest({ samples: left, sampleRate })
      .chainConfig as Record<string, unknown>;
    expect(Object.keys(chain).some((key) => key.includes('.'))).toBe(false);
    expect(() =>
      sonare.masterAudio({ samples: left, sampleRate, overrides: chain as MasteringChainConfig }),
    ).not.toThrow();
  });

  it('MasteringChainConfig uses a nested denoise object and closed token fields', () => {
    expectTypeOf<
      NonNullable<MasteringChainConfig['repair']>['denoise']
    >().not.toMatchTypeOf<boolean>();
    expectTypeOf<
      NonNullable<NonNullable<NonNullable<MasteringChainConfig['repair']>['denoise']>['mode']>
    >().toEqualTypeOf<'logMmse' | 'mmseStsa' | 'spectralSubtraction'>();
    const booleanDenoise = () => {
      // @ts-expect-error denoise takes its configuration object, not a boolean shorthand
      const config: MasteringChainConfig = { repair: { denoise: true } };
      return config;
    };
    expect(typeof booleanDenoise).toBe('function');
  });
});

describe('results: unit-named fields and objects', () => {
  it('chain results name the loudness range with its unit', () => {
    const mono = sonare.masterAudio({ samples: longLeft, sampleRate, preset: 'pop' });
    const stereo = sonare.masterAudioStereo({
      left: longLeft,
      right: longRight,
      sampleRate,
      preset: 'pop',
    });
    for (const result of [mono, stereo] as unknown as Record<string, unknown>[]) {
      expect(typeof result.outputLoudnessRangeLu).toBe('number');
      expect('outputLra' in result).toBe(false);
      const report = result.report as Record<string, Record<string, unknown>>;
      expect(typeof report.before.loudnessRangeLu).toBe('number');
      expect(typeof report.after.loudnessRangeLu).toBe('number');
      expect('loudnessRange' in report.before).toBe(false);
    }
    expectTypeOf<MasteringChainResult['outputLoudnessRangeLu']>().toEqualTypeOf<
      number | undefined
    >();
  });

  it('streamingLoudnessGain reports truePeakDbtp', () => {
    const gain = sonare.streamingLoudnessGain(left, sampleRate, { config: {} });
    expect(typeof gain.truePeakDbtp).toBe('number');
    expect('truePeakDb' in gain).toBe(false);
    const stereo = sonare.streamingLoudnessGainStereo({ left, right, sampleRate });
    expect(typeof stereo.truePeakDbtp).toBe('number');
    expectTypeOf<StreamingLoudnessGainResult['truePeakDbtp']>().toEqualTypeOf<number>();
  });

  it('the assistant returns objects, not JSON strings', () => {
    const result = sonare.masteringAssistantSuggest(longLeft, sampleRate, {
      targetPlatform: 'podcast',
    });
    expect(typeof result).toBe('object');
    expect(typeof result.chainConfig).toBe('object');
    expect(result.explanation.length).toBeGreaterThan(0);
    const profile = result.profile as unknown as Record<string, unknown>;
    expect(typeof profile.loudnessRangeLu).toBe('number');
    expect(typeof profile.truePeakDbtp).toBe('number');
    expect(typeof profile.attackPerSecond).toBe('number');
    for (const old of ['lraLu', 'truePeakDb', 'attackDensity']) {
      expect(old in profile).toBe(false);
    }
    expectTypeOf(result).toEqualTypeOf<MasteringAssistantResult>();
  });

  it('the audio profile is grouped and includes defects when requested', () => {
    const profile = sonare.masteringAudioProfile(longLeft, sampleRate, {
      detectDefects: true,
    });
    expect(profile.loudness).toEqual(
      expect.objectContaining({
        integratedLufs: expect.any(Number),
        loudnessRangeLu: expect.any(Number),
        truePeakDbtp: expect.any(Number),
        crestFactorDb: expect.any(Number),
      }),
    );
    expect(typeof profile.dynamics.attackPerSecond).toBe('number');
    expect('attackDensity' in profile.dynamics).toBe(false);
    expect(profile.defects).toBeDefined();
    const defects = profile.defects as unknown as Record<string, Record<string, unknown>>;
    for (const group of ['click', 'clip', 'crackle', 'noise', 'hum', 'reverb']) {
      expect(typeof defects[group]).toBe('object');
    }
    expect('measured' in (profile.defects as object)).toBe(false);
    expect(typeof defects.clip.clippedSamples).toBe('number');
    expect(typeof defects.clip.clippedRatio).toBe('number');
    expect(defects.noise.bandFloorDbfs).toBeInstanceOf(Float32Array);
    expect(defects.hum.harmonicDbfs).toBeInstanceOf(Float32Array);
    expect('latePredictability' in defects.reverb).toBe(false);
    const stereo = sonare.masteringAudioProfileStereo({
      left,
      right,
      sampleRate,
      detectDefects: true,
    });
    expect(stereo.defects).toBeDefined();
    expectTypeOf(profile).toEqualTypeOf<MasteringAudioProfile>();
  });

  it('omits profile defects by default in both request and positional calls', () => {
    const mono = { samples: left, sampleRate };
    const stereo = { left, right, sampleRate };
    const monoOff = sonare.masteringAudioProfile({ ...mono, detectDefects: false });
    const stereoOff = sonare.masteringAudioProfileStereo({ ...stereo, detectDefects: false });
    expect(monoOff).not.toHaveProperty('defects');
    expect(stereoOff).not.toHaveProperty('defects');

    const readings = ({ loudness, spectral, dynamics, ...summary }: MasteringAudioProfile) =>
      ({ ...summary, ...loudness, ...spectral, ...dynamics }) as Record<string, unknown>;
    const expectProfile = (actual: MasteringAudioProfile, expected: MasteringAudioProfile) => {
      expect(actual).not.toHaveProperty('defects');
      const got = readings(actual);
      const want = readings(expected);
      expect(Object.keys(got).sort()).toEqual(Object.keys(want).sort());
      for (const [field, value] of Object.entries(want)) {
        if (typeof value === 'number' && Number.isFinite(value)) {
          expect(typeof got[field], field).toBe('number');
          const tolerance = 1e-6 * Math.max(1, Math.abs(value));
          expect(Math.abs((got[field] as number) - value), field).toBeLessThanOrEqual(tolerance);
        } else {
          expect(got[field], field).toBe(value);
        }
      }
    };

    for (const result of [
      sonare.masteringAudioProfile(mono),
      sonare.masteringAudioProfile(left, sampleRate),
    ]) {
      expectProfile(result, monoOff);
    }
    expectProfile(sonare.masteringAudioProfileStereo(stereo), stereoOff);
  });

  it.each([
    [{ nFft: 0 }, 48000, 'nFft must be an even integer in [2, 524288]'],
    [{ nFft: 524288 }, 96000, 'nFft rescaled to sampleRate must fit the largest FFT size'],
    [
      { hopLength: 2 ** 30 },
      96000,
      'hopLength rescaled to sampleRate must fit the largest hop length',
    ],
  ])('the audio profile names its own field when refusing %o at %d Hz', (fields, rate, text) => {
    expect(() =>
      sonare.masteringAudioProfile({ samples: left, sampleRate: rate, ...fields }),
    ).toThrow(new RangeError(`masteringAudioProfile: ${text}`));
  });

  // Evenness is checked on the caller's value, so an odd size is refused at every rate.
  it.each([48000, 96000])('both audio profile entries refuse an odd nFft at %d Hz', (rate) => {
    const text = 'nFft must be an even integer in [2, 524288]';
    expect(() =>
      sonare.masteringAudioProfile({ samples: left, sampleRate: rate, nFft: 1023 }),
    ).toThrow(new RangeError(`masteringAudioProfile: ${text}`));
    expect(() =>
      sonare.masteringAudioProfileStereo({ left, right, sampleRate: rate, nFft: 1023 }),
    ).toThrow(new RangeError(`masteringAudioProfileStereo: ${text}`));
  });

  it.each([0, 1.5, -1])('the audio profile refuses nFft %s', (nFft) => {
    expectThrows(
      () => sonare.masteringAudioProfile({ samples: left, sampleRate, nFft }),
      RangeError,
    );
  });

  it('the streaming preview reports truePeakDbtp', () => {
    const preview = sonare.masteringStreamingPreview(left, sampleRate);
    const stereo = sonare.masteringStreamingPreviewStereo({ left, right, sampleRate });
    for (const result of [preview, stereo]) {
      expect(result.platforms.length).toBeGreaterThan(0);
      const platform = result.platforms[0] as unknown as Record<string, unknown>;
      expect(typeof platform.truePeakDbtp).toBe('number');
      expect('truePeakDb' in platform).toBe(false);
    }
    expectTypeOf(preview).toEqualTypeOf<MasteringStreamingPreviewResult>();
  });

  it.each([
    ['source', 'reference'],
    ['reference', 'source'],
  ] as const)('match EQ refuses a silent %s, naming it', (silentRole, otherRole) => {
    const takes = { [silentRole]: new Float32Array(sampleRate), [otherRole]: left } as {
      source: Float32Array;
      reference: Float32Array;
    };
    expectRefused(
      () => sonare.masteringAnalyzePair({ analysis: 'match.matchEqCurve', ...takes, sampleRate }),
      RangeError,
      `masteringAnalyzePair: ${silentRole} has no measurable level`,
    );
    expectRefused(
      () =>
        sonare.masteringApplyPairProcessor({
          processor: 'match.applyMatchEq',
          ...takes,
          sampleRate,
        }),
      RangeError,
      `masteringApplyPairProcessor: ${silentRole} has no measurable level`,
    );
  });

  it('pair and stereo analyses return objects, and the log-band tonal balance shares the result', () => {
    const loudness = sonare.masteringAnalyzePair({
      analysis: 'match.referenceLoudness',
      source: left,
      reference: right,
      sampleRate,
    });
    expect(typeof loudness).toBe('object');
    expect(typeof loudness.sourceLufs).toBe('number');
    const bands = sonare.masteringAnalyzePair({
      analysis: 'match.tonalBalanceLogBands',
      source: left,
      reference: right,
      sampleRate,
    });
    expect(Array.isArray(bands.bands)).toBe(true);
    const mono = sonare.masteringAnalyzeStereo({
      analysis: 'stereo.monoCompatCheck',
      left,
      right,
      sampleRate,
    });
    expect(typeof mono.correlation).toBe('number');
  });

  it('every processor result reports its latency, and the normalize step its target flag', () => {
    const applied = sonare.masteringApplyProcessor({
      processor: 'dynamics.limiter',
      samples: left,
      sampleRate,
    });
    expect(typeof applied.latencySamples).toBe('number');
    const pair = sonare.masteringApplyPairProcessor({
      processor: 'match.abSwitch',
      source: left,
      reference: right,
      sampleRate,
    });
    expect(typeof pair.latencySamples).toBe('number');
    const stereo = sonare.masteringApplyProcessorStereo({
      processor: 'dynamics.limiter',
      left,
      right,
      sampleRate,
    });
    expect(typeof stereo.latencySamples).toBe('number');
    const normalized = sonare.masteringNormalizeLoudness({ samples: left, sampleRate });
    expect(typeof normalized.loudnessTargetLimited).toBe('boolean');
    expectTypeOf<MasteringResult['latencySamples']>().toEqualTypeOf<number>();
    expectTypeOf<
      MasteringNormalizeLoudnessResult['loudnessTargetLimited']
    >().toEqualTypeOf<boolean>();
  });
});

describe('masterAudioAsync: a preset or overrides required, every failure a rejection', () => {
  it('resolves with the synchronous result', async () => {
    const sync = sonare.masterAudio({ samples: longLeft, sampleRate, preset: 'pop' });
    const request = await sonare.masterAudioAsync({ samples: longLeft, sampleRate, preset: 'pop' });
    const positional = await sonare.masterAudioAsync(longLeft, sampleRate, { preset: 'pop' });
    expectClose(request.samples, sync.samples);
    expectClose(positional.samples, sync.samples);
    expect(typeof request.outputLoudnessRangeLu).toBe('number');
    expect('outputLra' in (request as object)).toBe(false);
    const stereo = await sonare.masterAudioStereoAsync({
      left: longLeft,
      right: longRight,
      sampleRate,
      preset: 'pop',
    });
    expect(typeof stereo.outputLoudnessRangeLu).toBe('number');
  });

  it.each([
    [
      'no sampleRate',
      () => (sonare.masterAudioAsync as Fn)({ samples: left, preset: 'pop' }),
      TypeError,
    ],
    [
      'neither preset nor overrides',
      () => (sonare.masterAudioAsync as Fn)({ samples: left, sampleRate }),
      TypeError,
    ],
    [
      'unknown preset',
      () => (sonare.masterAudioAsync as Fn)({ samples: left, sampleRate, preset: 'nope' }),
      Error,
    ],
    [
      'removed positional preset',
      () => (sonare.masterAudioAsync as Fn)(left, sampleRate, 'pop'),
      TypeError,
    ],
    [
      'non-integer sampleRate',
      () => (sonare.masterAudioAsync as Fn)({ samples: left, sampleRate: 22050.5, preset: 'pop' }),
      RangeError,
    ],
    [
      'unknown key',
      () =>
        (sonare.masterAudioAsync as Fn)({
          samples: left,
          sampleRate,
          preset: 'pop',
          signal: undefined,
          x: 1,
        }),
      TypeError,
    ],
    [
      'stereo no sampleRate',
      () => (sonare.masterAudioStereoAsync as Fn)({ left, right, preset: 'pop' }),
      TypeError,
    ],
    [
      'stereo no preset',
      () => (sonare.masterAudioStereoAsync as Fn)({ left, right, sampleRate }),
      TypeError,
    ],
    [
      'stereo positional',
      () => (sonare.masterAudioStereoAsync as Fn)(left, right, sampleRate, 'pop'),
      TypeError,
    ],
  ])('rejects: %s', async (_name, call, cls) => {
    let thrown: unknown;
    let pending: Promise<unknown> | undefined;
    try {
      pending = call() as Promise<unknown>;
    } catch (error) {
      thrown = error;
    }
    expect(thrown).toBeUndefined();
    await expect(pending).rejects.toBeInstanceOf(cls as ErrorConstructor);
  });
});

describe('explicit 0 on a default-bearing field', () => {
  it.each([
    ['truePeakOversample', 0],
    ['releaseMs', 0],
  ])('masteringNormalizeLoudness refuses %s = %s', (name, value) => {
    expectThrows(
      () => sonare.masteringNormalizeLoudness({ samples: left, sampleRate, [name]: value }),
      RangeError,
    );
    expect(() => sonare.masteringNormalizeLoudness({ samples: left, sampleRate })).not.toThrow();
  });

  it('a given positive value is used', () => {
    expect(() =>
      sonare.masteringNormalizeLoudness({
        samples: left,
        sampleRate,
        truePeakOversample: 2,
        releaseMs: 80,
      }),
    ).not.toThrow();
  });
});

describe('closed sets and catalogs', () => {
  it('platform names are the typed set, in native order', () => {
    const expected: MasteringPlatform[] = [
      'streaming',
      'youtube',
      'broadcast',
      'podcast',
      'audiobook',
      'cinema',
      'club',
      'cd',
    ];
    expect(sonare.masteringPlatformNames()).toEqual(expected);
  });

  it.each([
    ['targetPlatform', 'tidal'],
    ['targetPlatform', 3],
    ['preset', 'nope'],
    ['preset', 2],
  ])('the assistant refuses %s = %s', (name, value) => {
    expectThrows(
      () => sonare.masteringAssistantSuggest({ samples: left, sampleRate, [name]: value }),
      typeof value === 'number' ? TypeError : RangeError,
    );
  });

  it('the assistant params are flat request fields', () => {
    expect(() =>
      sonare.masteringAssistantSuggest({
        samples: left,
        sampleRate,
        targetLufs: -16,
        ceilingDbtp: -1,
        enableRepair: false,
        preferStreamingSafe: true,
        speechMonoAmount: 0.5,
        targetPlatform: 'club',
        preset: 'edm',
      }),
    ).not.toThrow();
    expectThrows(
      () =>
        sonare.masteringAssistantSuggest({
          samples: left,
          sampleRate,
          params: { targetLufs: -16 },
        } as never),
      TypeError,
    );
  });

  it('masteringInsertParamInfo and masteringInsertTiming are request-only on `insert`', () => {
    const info = sonare.masteringInsertParamInfo({ insert: 'dynamics.compressor' });
    expect(info.length).toBeGreaterThan(0);
    const atRate = sonare.masteringInsertParamInfo({ insert: 'eq.parametric', sampleRate: 96000 });
    expect(atRate.length).toBeGreaterThan(0);
    expectThrows(() => (sonare.masteringInsertParamInfo as Fn)('dynamics.compressor'), TypeError);
    expectThrows(
      () => (sonare.masteringInsertParamInfo as Fn)({ name: 'dynamics.compressor' }),
      TypeError,
    );
    const timing = sonare.masteringInsertTiming({
      insert: 'dynamics.compressor',
      params: {},
      sampleRate,
    });
    expect(typeof timing.latencySamples).toBe('number');
    expectThrows(
      () => (sonare.masteringInsertTiming as Fn)('dynamics.compressor', {}, sampleRate),
      TypeError,
    );
    expectThrows(
      () => (sonare.masteringInsertTiming as Fn)({ insert: 'dynamics.compressor', params: {} }),
      TypeError,
    );
  });

  it('masteringInsertParamNames refuses an unknown insert', () => {
    expect(sonare.masteringInsertParamNames('dynamics.compressor').length).toBeGreaterThan(0);
    expectThrows(() => (sonare.masteringInsertParamNames as Fn)('not.an.insert'), RangeError);
    expectThrows(() => (sonare.masteringInsertParamNames as Fn)(3), TypeError);
  });

  it('masteringInsertNames is the typed insert set', () => {
    const names = sonare.masteringInsertNames();
    expect(names).toContain('dynamics.compressor');
    for (const insert of names) {
      expect(sonare.masteringInsertParamNames(insert)).toBeInstanceOf(Array);
    }
  });

  it('masteringApplyProcessor params are typed by processor', () => {
    expectTypeOf<sonare.MasteringApplyProcessorRequest<'dynamics.compressor'>>().toHaveProperty(
      'processor',
    );
    const result = sonare.masteringApplyProcessor({
      processor: 'dynamics.compressor',
      samples: left,
      sampleRate,
      params: { thresholdDb: -20, detector: 'rms' },
    });
    expect(result.samples.length).toBe(left.length);
  });
});

describe('native still uses the old spelling', () => {
  const raw = () => addon as Record<string, Fn>;
  const pop = () => raw().masteringPresetParams('pop') as string;

  it.each([
    [
      'streaming loudness gain names the true peak truePeakDb',
      () => {
        const result = raw().masteringStreamingLoudnessGain(left, sampleRate, null) as Record<
          string,
          unknown
        >;
        return ['truePeakDb' in result && !('truePeakDbtp' in result)];
      },
    ],
    [
      'a preset comes back as version 2 document text with dotted params',
      () => {
        const document = JSON.parse(pop()) as { version: number; params: object };
        const keys = Object.keys(document.params);
        return [document.version === 2, keys.every((key) => key.includes('.')), keys.length > 0];
      },
    ],
  ])('%s', (_name, probe) => {
    for (const holds of probe() as boolean[]) {
      expect(holds).toBe(true);
    }
  });
});
