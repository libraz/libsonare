/**
 * One input, one error class, whichever form the caller wrote.
 *
 * `Audio` and the standalone facade functions are two spellings of the same
 * operation, so `audio.hpss(x)` and `hpss(samples, sampleRate, x)` must refuse
 * `x` the same way. A method that reaches the addon directly instead runs none of
 * the facade's guards and answers a `RangeError` where the facade — and the WASM
 * and Python surfaces — answer `SonareError(InvalidParameter)`, so a `catch`
 * written against one form does not catch the other.
 *
 * The table below is the whole population, not a sample: every `Audio` method
 * that delegates is listed, and the roster check cross-reads `src/audio.ts` so a
 * method added or re-routed later cannot stay out of it. Methods with no
 * integer-typed argument carry the reason instead of a driver, so an argument
 * gained later reads as a missing driver rather than as coverage.
 */

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index.js';
import {
  Audio,
  analyze,
  analyzeImpulseResponse,
  chordFunctionalAnalysis,
  chroma,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  hpss,
  melSpectrogram,
  meteringSilenceRatio,
  meteringSpectrum,
  meteringSpectrumFrame,
  meteringTruePeakDbtp,
  mfcc,
  onsetStrength,
  type PitchClass,
  pitchPyin,
  pitchYin,
  resample,
  rmsEnergy,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  stftDb,
  zeroCrossingRate,
} from '../src/index.js';

const AUDIO_SOURCE = new URL('../src/audio.ts', import.meta.url).pathname;

const sampleRate = 22050;
const tone = new Float32Array(4096).map((_, i) => Math.sin((2 * Math.PI * 440 * i) / sampleRate));

/**
 * The two shapes an integer argument can be wrong in. `2 ** 32` is past the
 * signed 32-bit range and narrows to 0, which several guards downstream read as
 * "use the default", so the refusal has to come from the surface rather than
 * from the core.
 *
 * The fraction is derived from each argument's own valid value rather than
 * fixed, because a fixed one probes the wrong thing. `1.5` looked like a
 * non-integer probe and was not: it narrows to `1`, which the core rejects for
 * being odd or non-positive, so every `nFft` read as guarded while `2048.5`
 * sailed through as `2048`. Only a fraction whose truncation lands on a LEGAL
 * value can tell a surface that checks from one that does not.
 */
const WRONG_VALUES = [
  { kind: 'outOfRange', label: 'out of the signed 32-bit range' },
  { kind: 'nonInteger', label: 'a fraction that truncates onto its valid value' },
] as const;

/** An integer-typed argument both forms take at the same position. */
interface IntArg {
  index: number;
  name: string;
}

/** An `Audio` method that delegates, and how to drive both of its forms. */
interface Delegation {
  name: string;
  /** The Audio member, where its name differs from the free function's. */
  member?: string;
  /** The arguments both forms take after `samples` / `sampleRate`, all valid. */
  valid?: number[];
  /** Positions in `valid` that are narrowed into a C `int`. */
  intArgs?: IntArg[];
  /** Why the method contributes no integer argument, when it contributes none. */
  noIntArgs?: string;
  method?: (audio: Audio, args: number[]) => unknown;
  facade?: (samples: Float32Array, rate: number, args: number[]) => unknown;
}

const DELEGATIONS: Delegation[] = [
  // -- Analysis --
  {
    name: 'analyze',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.analyze({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => analyze(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'detectKey',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.detectKey({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => detectKey(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'detectKeyCandidates',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.detectKeyCandidates({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) =>
      detectKeyCandidates(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'detectOnsets',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.detectOnsets({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => detectOnsets(samples, rate, { nFft: a, hopLength: b }),
  },
  { name: 'analyzeBpm', noIntArgs: 'takes an options object only' },
  {
    name: 'analyzeImpulseResponse',
    valid: [6],
    intArgs: [{ index: 0, name: 'nOctaveBands' }],
    method: (audio, [a]) => audio.analyzeImpulseResponse({ nOctaveBands: a }),
    facade: (samples, rate, [a]) => analyzeImpulseResponse(samples, rate, { nOctaveBands: a }),
  },
  { name: 'detectAcoustic', noIntArgs: 'takes an options object only' },
  { name: 'analyzeRhythm', noIntArgs: 'takes an options object only' },
  { name: 'analyzeDynamics', noIntArgs: 'takes an options object only' },
  { name: 'analyzeTimbre', noIntArgs: 'takes an options object only' },
  { name: 'detectChords', noIntArgs: 'takes an options object only' },
  {
    name: 'chordFunctionalAnalysis',
    valid: [0],
    intArgs: [{ index: 0, name: 'keyRoot' }],
    method: (audio, [a]) => audio.chordFunctionalAnalysis({ keyRoot: a as PitchClass }),
    facade: (samples, rate, [a]) =>
      chordFunctionalAnalysis(samples, rate, { keyRoot: a as PitchClass }),
  },

  // -- Effects --
  {
    name: 'hpss',
    valid: [31, 31],
    intArgs: [
      { index: 0, name: 'kernelHarmonic' },
      { index: 1, name: 'kernelPercussive' },
    ],
    method: (audio, [a, b]) => audio.hpss({ kernelHarmonic: a, kernelPercussive: b }),
    facade: (samples, rate, [a, b]) =>
      hpss(samples, rate, { kernelHarmonic: a, kernelPercussive: b }),
  },
  { name: 'harmonic', noIntArgs: 'takes no argument' },
  { name: 'percussive', noIntArgs: 'takes no argument' },
  { name: 'timeStretch', noIntArgs: 'rate is a float' },
  { name: 'pitchShift', noIntArgs: 'semitones is a float' },
  { name: 'pitchCorrectToMidi', noIntArgs: 'both MIDI endpoints are floats' },
  { name: 'noteStretch', noIntArgs: 'takes an options object only' },
  { name: 'noteMove', noIntArgs: 'takes an options object only' },
  { name: 'voiceChange', noIntArgs: 'takes an options object only' },
  { name: 'normalize', noIntArgs: 'targetDb is a float' },
  { name: 'masteringNormalizeLoudness', noIntArgs: 'takes an options object only' },
  { name: 'masteringApplyProcessor', noIntArgs: 'takes a processor name and a params bag' },
  { name: 'masterAudio', noIntArgs: 'takes a preset name and an overrides object' },
  { name: 'trim', noIntArgs: 'thresholdDb is a float; the frame options are not exposed here' },

  // -- Features --
  {
    name: 'stft',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.stft({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => stft(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'stftDb',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.stftDb({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => stftDb(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'melSpectrogram',
    valid: [2048, 512, 128],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
      { index: 2, name: 'nMels' },
    ],
    method: (audio, [a, b, c]) => audio.melSpectrogram({ nFft: a, hopLength: b, nMels: c }),
    facade: (samples, rate, [a, b, c]) =>
      melSpectrogram(samples, rate, { nFft: a, hopLength: b, nMels: c }),
  },
  {
    name: 'mfcc',
    valid: [2048, 512, 128, 20],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
      { index: 2, name: 'nMels' },
      { index: 3, name: 'nMfcc' },
    ],
    method: (audio, [a, b, c, d]) => audio.mfcc({ nFft: a, hopLength: b, nMels: c, nMfcc: d }),
    facade: (samples, rate, [a, b, c, d]) =>
      mfcc(samples, rate, { nFft: a, hopLength: b, nMels: c, nMfcc: d }),
  },
  {
    name: 'chroma',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.chroma({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => chroma(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'spectralCentroid',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.spectralCentroid({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => spectralCentroid(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'spectralBandwidth',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.spectralBandwidth({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => spectralBandwidth(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'spectralRolloff',
    valid: [2048, 512, 0.85],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b, c]) => audio.spectralRolloff({ nFft: a, hopLength: b, rollPercent: c }),
    facade: (samples, rate, [a, b, c]) =>
      spectralRolloff(samples, rate, { nFft: a, hopLength: b, rollPercent: c }),
  },
  {
    name: 'spectralFlatness',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.spectralFlatness({ nFft: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => spectralFlatness(samples, rate, { nFft: a, hopLength: b }),
  },
  {
    name: 'zeroCrossingRate',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.zeroCrossingRate({ frameLength: a, hopLength: b }),
    facade: (samples, rate, [a, b]) =>
      zeroCrossingRate(samples, rate, { frameLength: a, hopLength: b }),
  },
  {
    name: 'rmsEnergy',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.rmsEnergy({ frameLength: a, hopLength: b }),
    facade: (samples, rate, [a, b]) => rmsEnergy(samples, rate, { frameLength: a, hopLength: b }),
  },
  {
    name: 'pitchYin',
    valid: [2048, 512, 65, 2093, 0.1],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b, c, d, e]) =>
      audio.pitchYin({ frameLength: a, hopLength: b, f0MinHz: c, f0MaxHz: d, threshold: e }),
    facade: (samples, rate, [a, b, c, d, e]) =>
      pitchYin(samples, rate, {
        frameLength: a,
        hopLength: b,
        f0MinHz: c,
        f0MaxHz: d,
        threshold: e,
      }),
  },
  {
    name: 'pitchPyin',
    valid: [2048, 512, 65, 2093, 0.1],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b, c, d, e]) =>
      audio.pitchPyin({ frameLength: a, hopLength: b, f0MinHz: c, f0MaxHz: d, threshold: e }),
    facade: (samples, rate, [a, b, c, d, e]) =>
      pitchPyin(samples, rate, {
        frameLength: a,
        hopLength: b,
        f0MinHz: c,
        f0MaxHz: d,
        threshold: e,
      }),
  },
  {
    name: 'resample',
    valid: [16000],
    intArgs: [{ index: 0, name: 'targetSr' }],
    method: (audio, [a]) => audio.resample(a),
    facade: (samples, rate, [a]) => resample(samples, rate, a),
  },
  {
    name: 'onsetStrength',
    valid: [2048, 512, 128],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
      { index: 2, name: 'nMels' },
    ],
    method: (audio, [a, b, c]) => audio.onsetStrength({ nFft: a, hopLength: b, nMels: c }),
    facade: (samples, rate, [a, b, c]) =>
      onsetStrength(samples, rate, { nFft: a, hopLength: b, nMels: c }),
  },
  { name: 'nnlsChroma', noIntArgs: 'takes no argument' },
  { name: 'lufs', noIntArgs: 'takes no integer argument' },
  { name: 'momentaryLufs', noIntArgs: 'takes no integer argument' },
  { name: 'shortTermLufs', noIntArgs: 'takes no integer argument' },
  { name: 'detectBpm', noIntArgs: 'the tempo bounds are floats' },
  { name: 'detectBeats', noIntArgs: 'the tempo bounds are floats' },
  { name: 'detectDownbeats', noIntArgs: 'takes no argument' },
  { name: 'meteringPeakDb', noIntArgs: 'takes no argument' },
  { name: 'meteringRmsDb', noIntArgs: 'takes no argument' },
  { name: 'meteringDcOffset', noIntArgs: 'takes no argument' },
  { name: 'meteringCrestFactorDb', noIntArgs: 'takes no argument' },
  { name: 'ebur128LoudnessRangeLu', noIntArgs: 'takes no argument' },
  { name: 'meteringDynamicRange', noIntArgs: 'its window and hop are seconds' },
  {
    name: 'meteringSilenceRatio',
    member: 'silenceRatio',
    valid: [1024, 256],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.silenceRatio(-45, a, b),
    facade: (samples, rate, [a, b]) =>
      meteringSilenceRatio(samples, rate, { thresholdDb: -45, frameLength: a, hopLength: b }),
  },
  {
    name: 'meteringTruePeakDbtp',
    member: 'truePeakDbtp',
    valid: [4],
    intArgs: [{ index: 0, name: 'oversampleFactor' }],
    method: (audio, [a]) => audio.truePeakDbtp(a),
    facade: (samples, rate, [a]) => meteringTruePeakDbtp(samples, rate, { oversampleFactor: a }),
  },
  {
    name: 'meteringDetectClipping',
    noIntArgs: 'minRegionSamples is a 64-bit count; a value past the buffer finds no region',
  },
  {
    name: 'meteringSpectrum',
    member: 'spectrum',
    valid: [2048],
    intArgs: [{ index: 0, name: 'nFft' }],
    method: (audio, [a]) => audio.spectrum({ nFft: a }),
    facade: (samples, rate, [a]) => meteringSpectrum(samples, rate, { nFft: a }),
  },
  {
    name: 'meteringSpectrumFrame',
    member: 'spectrumFrame',
    // startSample is a 64-bit offset (a start past the end reads a zero-padded frame).
    valid: [2048],
    intArgs: [{ index: 0, name: 'nFft' }],
    method: (audio, [a]) => audio.spectrumFrame(0, { nFft: a }),
    facade: (samples, rate, [a]) =>
      meteringSpectrumFrame(samples, rate, { startSample: 0, nFft: a }),
  },
];

/** What a call answered: a refusal identified by class and message, or nothing. */
type Outcome = { refused: false } | { refused: true; errorClass: string; message: string };

function outcomeOf(run: () => unknown): Outcome {
  try {
    run();
    return { refused: false };
  } catch (error) {
    return {
      refused: true,
      errorClass: (error as object).constructor.name,
      message: (error as Error).message,
    };
  }
}

function withArg(entry: Delegation, index: number, value: number): number[] {
  const args = [...(entry.valid as number[])];
  args[index] = value;
  return args;
}

describe('the delegation roster covers every Audio method that delegates', () => {
  const source = readFileSync(AUDIO_SOURCE, 'utf8');

  it('names exactly the methods src/audio.ts routes through a facade', () => {
    const routed = [...source.matchAll(/return (\w+)Fn\(/g)].map((match) => match[1] as string);
    expect(routed.length, 'the delegation scanner matched nothing').toBeGreaterThan(0);
    expect([...new Set(routed)].sort()).toEqual(DELEGATIONS.map((entry) => entry.name).sort());
  });

  it('leaves no method reaching the addon outside the static constructors', () => {
    const direct = [...source.matchAll(/addon\.(\w+)/g)]
      .map((match) => match[1] as string)
      .filter((name) => name !== 'Audio');
    expect(direct).toEqual([]);
  });

  it('routes every Audio method that has a free-function twin through that twin', () => {
    const exported = new Set(
      Object.entries(sonare)
        .filter(([, value]) => typeof value === 'function')
        .map(([name]) => name),
    );
    const classBody = source.slice(source.indexOf('export class Audio {'));
    const methods = [
      ...classBody.matchAll(/\n {2}(?:async )?(\w+)\([^)]*\)[^{\n]*\{\n(.*?)\n {2}\}\n/gs),
    ];
    const twinned: string[] = [];
    const bypassing: string[] = [];
    for (const [, method, body] of methods) {
      const name = method as string;
      const capitalised = `${name.charAt(0).toUpperCase()}${name.slice(1)}`;
      const twin = [name, `metering${capitalised}`].find((candidate) => exported.has(candidate));
      if (twin === undefined) {
        continue;
      }
      twinned.push(name);
      if (!(body as string).includes(`${twin}Fn(`)) {
        bypassing.push(`${name} does not call ${twin}`);
      }
    }
    expect(twinned.length, 'the method scanner matched no twinned method').toBeGreaterThan(40);
    expect(bypassing).toEqual([]);
  });

  it('reaches the native handle only for the buffer accessors and destroy', () => {
    const used = new Set([...source.matchAll(/this\.native\.(\w+)/g)].map((match) => match[1]));
    expect([...used].sort()).toEqual([
      'destroy',
      'getData',
      'getDuration',
      'getLength',
      'getSampleRate',
    ]);
  });

  it('gives every listed method either integer arguments to drive or a reason', () => {
    for (const entry of DELEGATIONS) {
      const drivable = entry.intArgs !== undefined && entry.intArgs.length > 0;
      expect(
        drivable || Boolean(entry.noIntArgs),
        `${entry.name} is neither driven nor excused`,
      ).toBe(true);
      if (drivable) {
        expect(entry.method, `${entry.name} has no method driver`).toBeTypeOf('function');
        expect(entry.facade, `${entry.name} has no facade driver`).toBeTypeOf('function');
      }
    }
  });
});

describe('both forms answer a wrong integer with one class and one message', () => {
  for (const entry of DELEGATIONS) {
    for (const arg of entry.intArgs ?? []) {
      for (const wrong of WRONG_VALUES) {
        it(`${entry.name}: ${arg.name} ${wrong.label}`, () => {
          const value =
            wrong.kind === 'outOfRange'
              ? 2 ** 32
              : ((entry.valid as number[])[arg.index] as number) + 0.5;
          const args = withArg(entry, arg.index, value);
          const audio = Audio.fromBuffer(tone, sampleRate);
          try {
            const viaMethod = outcomeOf(() =>
              (entry.method as NonNullable<Delegation['method']>)(audio, args),
            );
            const viaFacade = outcomeOf(() =>
              (entry.facade as NonNullable<Delegation['facade']>)(tone, sampleRate, args),
            );
            // Each form names the entry the caller invoked, so the text after the
            // prefix is compared: as one object, so a disagreement names the class
            // AND the text at once, rather than failing on whichever ran first.
            const split = (o: Outcome) => {
              if (!o.refused) {
                return { refused: false };
              }
              const m = /^([\w.]+): ([\s\S]*)$/.exec(o.message);
              return { refused: true, errorClass: o.errorClass, prefix: m?.[1], text: m?.[2] };
            };
            const method = split(viaMethod);
            const facade = split(viaFacade);
            expect({ ...method, prefix: undefined }).toEqual({ ...facade, prefix: undefined });
            expect('prefix' in facade ? facade.prefix : undefined).toBe(entry.name);
            expect('prefix' in method ? method.prefix : undefined).toBe(
              `Audio.${entry.member ?? entry.name}`,
            );
            // Without this a pair that silently stopped refusing would compare
            // two successes and read as covered.
            expect(viaMethod.refused, 'neither form refused the value').toBe(true);
          } finally {
            audio.destroy();
          }
        });
      }
    }
  }
});

describe('Audio is built only by its factories', () => {
  it('refuses a direct construction with a TypeError naming the factories', () => {
    const Construct = Audio as unknown as new (...args: unknown[]) => Audio;
    expect(outcomeOf(() => new Construct(tone, sampleRate))).toEqual({
      refused: true,
      errorClass: 'TypeError',
      message:
        'Audio: construct it with Audio.fromBuffer, Audio.fromFile, Audio.fromFileChannel or Audio.fromMemory',
    });
  });
});

/**
 * The metering methods answer an unknown key, a `validate` key and a zero `nFft`
 * exactly as their facades do; the WASM suite pins the same table.
 */
const METERING_REFUSALS: [string, (audio: Audio) => unknown, string, string][] = [
  [
    'spectrum unknown key',
    (a) => a.spectrum({ bogus: 1 } as never),
    'TypeError',
    "Audio.spectrum: unknown request key 'bogus'",
  ],
  [
    'spectrum validate',
    (a) => a.spectrum({ validate: false } as never),
    'TypeError',
    "Audio.spectrum: unknown request key 'validate'",
  ],
  [
    'spectrum nFft 0',
    (a) => a.spectrum({ nFft: 0 }),
    'RangeError',
    'Audio.spectrum: nFft must not be 0; omit it for the default',
  ],
  [
    'spectrumFrame unknown key',
    (a) => a.spectrumFrame(0, { bogus: 1 } as never),
    'TypeError',
    "Audio.spectrumFrame: unknown request key 'bogus'",
  ],
  [
    'spectrumFrame validate',
    (a) => a.spectrumFrame(0, { validate: false } as never),
    'TypeError',
    "Audio.spectrumFrame: unknown request key 'validate'",
  ],
  [
    'spectrumFrame nFft 0',
    (a) => a.spectrumFrame(0, { nFft: 0 }),
    'RangeError',
    'Audio.spectrumFrame: nFft must not be 0; omit it for the default',
  ],
  [
    'dynamicRange unknown key',
    (a) => a.dynamicRange({ bogus: 1 } as never),
    'TypeError',
    "Audio.dynamicRange: unknown request key 'bogus'",
  ],
  [
    'dynamicRange validate',
    (a) => a.dynamicRange({ validate: false } as never),
    'TypeError',
    "Audio.dynamicRange: unknown request key 'validate'",
  ],
  [
    'detectClipping unknown key',
    (a) => a.detectClipping({ bogus: 1 } as never),
    'TypeError',
    "Audio.detectClipping: unknown request key 'bogus'",
  ],
  [
    'detectClipping validate',
    (a) => a.detectClipping({ validate: false } as never),
    'TypeError',
    "Audio.detectClipping: unknown request key 'validate'",
  ],
];

describe('the metering methods refuse as their facades do', () => {
  it.each(METERING_REFUSALS)('%s', (_label, run, errorClass, message) => {
    const audio = Audio.fromBuffer(tone, sampleRate);
    try {
      expect(outcomeOf(() => run(audio))).toEqual({ refused: true, errorClass, message });
    } finally {
      audio.destroy();
    }
  });
});
