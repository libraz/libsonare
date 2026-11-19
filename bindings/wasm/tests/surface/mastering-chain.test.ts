import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
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
  StreamingMasteringChainConfig,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import { init, SonareError } from '../../dist/index.js';
import { init as initSource } from '../../src/index';
import { getSonareModule } from '../../src/module_state';

type Fn = (...args: unknown[]) => unknown;
// Any exported function by name, for tables that sweep many entry points.
const api = sonare as unknown as Record<string, Fn>;

beforeAll(async () => {
  await init();
});

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

function expectInvalidFormat(fn: () => unknown, message: string): void {
  const error = errorOf(fn);
  expect(error).toBeInstanceOf(SonareError);
  expect((error as SonareError).codeName).toBe('InvalidFormat');
  expect((error as Error).message).toContain(message);
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
  ['masterAudio', {}],
  ['masteringNormalizeLoudness', {}],
  ['masteringApplyProcessor', { processor: 'dynamics.limiter' }],
  ['masteringAudioProfile', {}],
  ['masteringAssistantSuggest', {}],
  ['masteringAssistantSuggestChain', {}],
  ['masteringStreamingPreview', {}],
  ['streamingLoudnessGain', {}],
];

/** Stereo entry points: request only. */
const STEREO: ReadonlyArray<[string, Record<string, unknown>]> = [
  ['masterAudioStereo', {}],
  ['masteringApplyProcessorStereo', { processor: 'dynamics.limiter' }],
  ['masteringAnalyzeStereo', { analysis: 'stereo.monoCompatCheck' }],
  ['masteringAudioProfileStereo', {}],
  ['masteringAssistantSuggestStereo', {}],
  ['masteringAssistantSuggestChainStereo', {}],
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

  it('refuses non-finite audio', () => {
    const bad = new Float32Array(left);
    bad[10] = Number.NaN;
    expectThrows(() => sonare.masterAudio({ samples: bad, sampleRate, preset: 'pop' }), RangeError);
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
      'repair.denoise.mode takes a token, not a number',
    ],
    [
      'denoise.mode synonym',
      { repair: { denoise: { enabled: true, mode: 'log_mmse' } } },
      "repair.denoise.mode: unknown name 'log_mmse'",
    ],
    [
      'noiseEstimator number',
      { repair: { denoise: { noiseEstimator: 2 } } },
      'repair.denoise.noiseEstimator takes a token, not a number',
    ],
    [
      'decrackle.mode number',
      { repair: { decrackle: { mode: 1 } } },
      'repair.decrackle.mode takes a token, not a number',
    ],
    [
      'dehum.mode number',
      { repair: { dehum: { mode: 0 } } },
      'repair.dehum.mode takes a token, not a number',
    ],
    [
      'crossover.slope number',
      { dynamics: { multibandComp: { crossover: { slope: 1 } } } },
      'dynamics.multibandComp.crossover.slope takes a token, not a number',
    ],
    [
      'crossover.mode number',
      { dynamics: { multibandComp: { crossover: { mode: 2 } } } },
      'dynamics.multibandComp.crossover.mode takes a token, not a number',
    ],
    [
      'band detector old spelling',
      { dynamics: { multibandComp: { bands: [{ detector: 'log_rms' }] } } },
      "dynamics.multibandComp.bands.0.detector: unknown name 'log_rms'",
    ],
    [
      'band detector number',
      { dynamics: { multibandComp: { bands: [{ detector: 1 }] } } },
      'dynamics.multibandComp.bands.0.detector takes a token, not a number',
    ],
  ])('the core refuses %s as InvalidFormat, naming the path', (_name, overrides, message) => {
    expectInvalidFormat(
      () =>
        sonare.masterAudio({
          samples: left,
          sampleRate,
          overrides: overrides as MasteringChainConfig,
        }),
      message,
    );
  });

  it.each([
    [
      'boolean denoise shorthand',
      { repair: { denoise: true } },
      'masterAudio: unknown key params.repair.denoise (renamed to params.repair.denoise.enabled)',
    ],
    ['flat repair.nFft', { repair: { nFft: 1024 } }, '(renamed to params.repair.denoise.nFft)'],
    [
      'flat repair.hopLength',
      { repair: { hopLength: 256 } },
      '(renamed to params.repair.denoise.hopLength)',
    ],
    [
      'flat repair.ddAlpha',
      { repair: { ddAlpha: 0.9 } },
      '(renamed to params.repair.denoise.ddAlpha)',
    ],
    [
      'flat repair.reductionDb',
      { repair: { reductionDb: 12 } },
      '(renamed to params.repair.denoise.reductionDb)',
    ],
    [
      'flat repair.gainFloor',
      { repair: { gainFloor: 0.2 } },
      '(renamed to params.repair.denoise.reductionDb)',
    ],
    [
      'denoise.gainFloor',
      { repair: { denoise: { gainFloor: 0.2 } } },
      '(renamed to params.repair.denoise.reductionDb)',
    ],
    ['eq.tiltDb', { eq: { tiltDb: 1 } }, '(renamed to params.eq.tilt.tiltDb)'],
    ['eq.pivotHz', { eq: { pivotHz: 1000 } }, '(renamed to params.eq.tilt.pivotHz)'],
    [
      'declip.clipThreshold',
      { repair: { declip: { clipThreshold: 0.9 } } },
      '(renamed to params.repair.declip.clipThresholdDb)',
    ],
    [
      'band sidechainHpfHz',
      { dynamics: { multibandComp: { bands: [{ sidechainHpfHz: 100 }] } } },
      '(renamed to params.dynamics.multibandComp.bands.0.sidechainHighPassHz)',
    ],
    [
      'band sidechainHpfEnabled',
      { dynamics: { multibandComp: { bands: [{ sidechainHpfEnabled: true }] } } },
      '(renamed to params.dynamics.multibandComp.bands.0.sidechainHighPassHz)',
    ],
  ])(
    'the core refuses the removed spelling %s, naming its successor',
    (_name, overrides, message) => {
      expectInvalidFormat(
        () =>
          sonare.masterAudio({
            samples: left,
            sampleRate,
            overrides: overrides as MasteringChainConfig,
          }),
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
    expectInvalidFormat(
      () =>
        sonare.masterAudio({
          samples: left,
          sampleRate,
          overrides: {
            loudness: { targetLufs: -14 },
            'loudness.targetLufs': -14,
          } as MasteringChainConfig,
        }),
      'masterAudio: duplicate key params.loudness.targetLufs',
    );
  });

  it('refuses a string on a number leaf the same way offline and streaming', () => {
    const bad = { maximizer: { truePeakLimiter: { ceilingDbtp: '-1.0' } } } as never;
    const message = 'maximizer.truePeakLimiter.ceilingDbtp is a string, but takes no token';
    expectInvalidFormat(
      () => sonare.masterAudio({ samples: left, sampleRate, overrides: bad }),
      message,
    );
    expectInvalidFormat(
      () => new sonare.StreamingMasteringChain({ sampleRate, numChannels: 1, ...(bad as object) }),
      message,
    );
  });

  it('refuses a renamed chain key by its new name, the same way offline and streaming', () => {
    for (const [bad, oldKey, newKey] of [
      [{ loudness: { ceilingDb: -1 } }, 'loudness.ceilingDb', 'loudness.ceilingDbtp'],
      [
        { maximizer: { truePeakLimiter: { ceilingDb: -1 } } },
        'maximizer.truePeakLimiter.ceilingDb',
        'maximizer.truePeakLimiter.ceilingDbtp',
      ],
    ] as const) {
      const message = `unknown key params.${oldKey} (renamed to params.${newKey})`;
      expectInvalidFormat(
        () => sonare.masterAudio({ samples: left, sampleRate, overrides: bad as never }),
        message,
      );
      expectInvalidFormat(
        () =>
          new sonare.StreamingMasteringChain({ sampleRate, numChannels: 1, ...(bad as object) }),
        message,
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
    expectInvalidFormat(() => run(0.5), 'repair.declip.clipThresholdDb must be at or below 0 dBFS');
    expectInvalidFormat(
      () => run(Number.NaN),
      'masterAudio: params.repair.declip.clipThresholdDb must be a number, a boolean or a token',
    );
    expectInvalidFormat(
      () => run('-1'),
      'repair.declip.clipThresholdDb is a string, but takes no token',
    );
  });

  it('band sidechainHighPassHz is a positive frequency; absent is off', () => {
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
    expectInvalidFormat(() => run(0), 'sidechainHighPassHz');
    expectInvalidFormat(() => run(-5), 'sidechainHighPassHz');
    expectInvalidFormat(
      () => run('100'),
      'dynamics.multibandComp.bands.0.sidechainHighPassHz is a string, but takes no token',
    );
  });

  it('the assistant chain nests, with tokens and dBFS, and passes straight back', () => {
    const chain = sonare.masteringAssistantSuggestChain({ samples: left, sampleRate }) as Record<
      string,
      unknown
    >;
    expect(Object.keys(chain).some((key) => key.includes('.'))).toBe(false);
    expect(() =>
      sonare.masterAudio({ samples: left, sampleRate, overrides: chain as MasteringChainConfig }),
    ).not.toThrow();
  });

  it('types: the config is closed over tokens', () => {
    expectTypeOf<
      NonNullable<MasteringChainConfig['repair']>['denoise']
    >().not.toMatchTypeOf<boolean>();
    expectTypeOf<
      NonNullable<NonNullable<NonNullable<MasteringChainConfig['repair']>['denoise']>['mode']>
    >().toEqualTypeOf<'logMmse' | 'mmseStsa' | 'spectralSubtraction'>();
  });
});

describe('results: unit-named fields and objects', () => {
  it('chain results name the loudness range with its unit', () => {
    const mono = sonare.masterAudio({ samples: left, sampleRate, preset: 'pop' });
    const stereo = sonare.masterAudioStereo({ left, right, sampleRate, preset: 'pop' });
    for (const result of [mono, stereo] as unknown as Record<string, unknown>[]) {
      expect(typeof result.outputLoudnessRangeLu).toBe('number');
      expect('outputLra' in result).toBe(false);
      const report = result.report as Record<string, Record<string, unknown>>;
      expect(typeof report.before.loudnessRangeLu).toBe('number');
      expect(typeof report.after.loudnessRangeLu).toBe('number');
      expect('loudnessRange' in report.before).toBe(false);
    }
    expectTypeOf<MasteringChainResult['outputLoudnessRangeLu']>().toEqualTypeOf<number>();
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
    const result = sonare.masteringAssistantSuggest(left, sampleRate, {
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

  it('the audio profile is grouped and its defects are absent when detection is off', () => {
    const profile = sonare.masteringAudioProfile(left, sampleRate);
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
    const off = sonare.masteringAudioProfile({ samples: left, sampleRate, detectDefects: false });
    expect(off.defects).toBeUndefined();
    const stereo = sonare.masteringAudioProfileStereo({ left, right, sampleRate });
    expect(stereo.defects).toBeDefined();
    expectTypeOf(profile).toEqualTypeOf<MasteringAudioProfile>();
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

describe('StreamingMasteringChainConfig is one flat constructor input', () => {
  it('extends the chain config with the stream layout', () => {
    expectTypeOf<StreamingMasteringChainConfig>()
      .toHaveProperty('sampleRate')
      .toEqualTypeOf<number>();
    expectTypeOf<StreamingMasteringChainConfig>().toHaveProperty('maxBlockSize');
    expectTypeOf<StreamingMasteringChainConfig>().toHaveProperty('numChannels');
    expectTypeOf<StreamingMasteringChainConfig>().toHaveProperty('loudness');
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
  // The raw module is the source tree's, which dist's init does not load.
  beforeAll(async () => {
    await initSource();
  });

  const raw = () => getSonareModule() as unknown as Record<string, Fn>;
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
