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
import {
  Audio,
  analyzeImpulseResponse,
  chordFunctionalAnalysis,
  chroma,
  hpss,
  melSpectrogram,
  mfcc,
  onsetEnvelope,
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
  { name: 'analyzeBpm', noIntArgs: 'takes an options object only' },
  {
    name: 'analyzeImpulseResponse',
    valid: [6],
    intArgs: [{ index: 0, name: 'nOctaveBands' }],
    method: (audio, [a]) => audio.analyzeImpulseResponse(a),
    facade: (samples, rate, [a]) => analyzeImpulseResponse(samples, rate, a),
  },
  { name: 'detectAcoustic', noIntArgs: 'takes an options object only' },
  { name: 'analyzeRhythm', noIntArgs: 'takes an options object only' },
  { name: 'analyzeDynamics', noIntArgs: 'takes an options object only' },
  { name: 'analyzeTimbre', noIntArgs: 'takes an options object only' },
  { name: 'detectChords', noIntArgs: 'takes an options object only' },
  {
    name: 'chordFunctionalAnalysis',
    valid: [0, 0],
    intArgs: [
      { index: 0, name: 'keyRoot' },
      { index: 1, name: 'keyMode' },
    ],
    method: (audio, [a, b]) => audio.chordFunctionalAnalysis(a, b),
    // The only entry whose two forms disagree on argument order: the facade
    // takes the key before the sample rate.
    facade: (samples, rate, [a, b]) => chordFunctionalAnalysis(samples, a, b, rate),
  },

  // -- Effects --
  {
    name: 'hpss',
    valid: [31, 31],
    intArgs: [
      { index: 0, name: 'kernelHarmonic' },
      { index: 1, name: 'kernelPercussive' },
    ],
    method: (audio, [a, b]) => audio.hpss(a, b),
    facade: (samples, rate, [a, b]) => hpss(samples, rate, a, b),
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
  { name: 'mastering', noIntArgs: 'takes an options object only' },
  { name: 'masteringProcess', noIntArgs: 'takes a processor name and a params bag' },
  { name: 'masteringChain', noIntArgs: 'takes a config object only' },
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
    method: (audio, [a, b]) => audio.stft(a, b),
    facade: (samples, rate, [a, b]) => stft(samples, rate, a, b),
  },
  {
    name: 'stftDb',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.stftDb(a, b),
    facade: (samples, rate, [a, b]) => stftDb(samples, rate, a, b),
  },
  {
    name: 'melSpectrogram',
    valid: [2048, 512, 128, 0, 0],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
      { index: 2, name: 'nMels' },
    ],
    method: (audio, [a, b, c]) => audio.melSpectrogram(a, b, c),
    facade: (samples, rate, [a, b, c]) => melSpectrogram(samples, rate, a, b, c),
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
    method: (audio, [a, b, c, d]) => audio.mfcc(a, b, c, d),
    facade: (samples, rate, [a, b, c, d]) => mfcc(samples, rate, a, b, c, d),
  },
  {
    name: 'chroma',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.chroma(a, b),
    facade: (samples, rate, [a, b]) => chroma(samples, rate, a, b),
  },
  {
    name: 'spectralCentroid',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.spectralCentroid(a, b),
    facade: (samples, rate, [a, b]) => spectralCentroid(samples, rate, a, b),
  },
  {
    name: 'spectralBandwidth',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.spectralBandwidth(a, b),
    facade: (samples, rate, [a, b]) => spectralBandwidth(samples, rate, a, b),
  },
  {
    name: 'spectralRolloff',
    valid: [2048, 512, 0.85],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b, c]) => audio.spectralRolloff(a, b, c),
    facade: (samples, rate, [a, b, c]) => spectralRolloff(samples, rate, a, b, c),
  },
  {
    name: 'spectralFlatness',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.spectralFlatness(a, b),
    facade: (samples, rate, [a, b]) => spectralFlatness(samples, rate, a, b),
  },
  {
    name: 'zeroCrossingRate',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.zeroCrossingRate(a, b),
    facade: (samples, rate, [a, b]) => zeroCrossingRate(samples, rate, a, b),
  },
  {
    name: 'rmsEnergy',
    valid: [2048, 512],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b]) => audio.rmsEnergy(a, b),
    facade: (samples, rate, [a, b]) => rmsEnergy(samples, rate, a, b),
  },
  {
    name: 'pitchYin',
    valid: [2048, 512, 65, 2093, 0.1],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b, c, d, e]) => audio.pitchYin(a, b, c, d, e),
    facade: (samples, rate, [a, b, c, d, e]) => pitchYin(samples, rate, a, b, c, d, e),
  },
  {
    name: 'pitchPyin',
    valid: [2048, 512, 65, 2093, 0.1],
    intArgs: [
      { index: 0, name: 'frameLength' },
      { index: 1, name: 'hopLength' },
    ],
    method: (audio, [a, b, c, d, e]) => audio.pitchPyin(a, b, c, d, e),
    facade: (samples, rate, [a, b, c, d, e]) => pitchPyin(samples, rate, a, b, c, d, e),
  },
  {
    name: 'resample',
    valid: [16000],
    intArgs: [{ index: 0, name: 'targetSr' }],
    method: (audio, [a]) => audio.resample(a),
    facade: (samples, rate, [a]) => resample(samples, rate, a),
  },
  {
    name: 'onsetEnvelope',
    valid: [2048, 512, 128],
    intArgs: [
      { index: 0, name: 'nFft' },
      { index: 1, name: 'hopLength' },
      { index: 2, name: 'nMels' },
    ],
    method: (audio, [a, b, c]) => audio.onsetEnvelope(a, b, c),
    facade: (samples, rate, [a, b, c]) => onsetEnvelope(samples, rate, a, b, c),
  },
  { name: 'nnlsChroma', noIntArgs: 'takes no argument' },
  { name: 'lufs', noIntArgs: 'takes a validate option only' },
  { name: 'momentaryLufs', noIntArgs: 'takes a validate option only' },
  { name: 'shortTermLufs', noIntArgs: 'takes a validate option only' },
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
            // Compared as one object so a disagreement names the class AND the
            // message at once, rather than failing on whichever ran first.
            expect(viaMethod).toEqual(viaFacade);
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
