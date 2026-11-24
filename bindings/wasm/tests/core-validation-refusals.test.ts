import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  ErrorCode,
  griffinLim,
  init,
  Mixer,
  masterAudio,
  masterAudioStereo,
  melToAudio,
  mfccToAudio,
  mfccToMel,
  Project,
  pitchShift,
  RealtimeEngine,
  resample,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  timeStretch,
  zeroCrossings,
} from '../src/index';
import { getSonareModule } from '../src/module_state';
import { expectRefusalOf, unknownKeyText } from './_helpers';

// The core function owns each refusal in this corpus. The embind wrappers call the
// core directly, so the refusal is asserted on the raw module as well as on the facade.

type Cause = 'argument' | 'missing_argument' | 'shape';
type MfccInput = { n_mfcc: number; n_frames: number; fill: number };
type MfccCase = { id: string; n_mels: number; cause?: Cause; message?: string };
type MfccRoute = { input: MfccInput; accepted: MfccCase[]; refused: MfccCase[] };
type MelInput = { n_mels: number; n_frames: number; fill: number };
type MelCase = { id: string; n_iter: number; cause?: Cause; message?: string };
type InverseHopCase = {
  id: string;
  n_fft: number;
  hop_length: number;
  derived_hop_length?: number;
  cause?: Cause;
  message?: string;
};
type InverseHops = { absent_hop: InverseHopCase[]; negative_hop: InverseHopCase[] };
type MelRoute = InverseHops & { input: MelInput; accepted: MelCase[]; refused: MelCase[] };
type GriffinLimRoute = InverseHops & {
  input: { sample_rate: number; n_frames: number; fill: number };
};
type MfccAudioInput = { n_mfcc: number; n_frames: number; n_mels: number; fill: number };
type MfccAudioRoute = InverseHops & {
  input: MfccAudioInput;
  accepted: MelCase[];
  refused: MelCase[];
};
type StretchInput = {
  sample_rate: number;
  length: number;
  fill: number;
  speed_ratio: number;
  pitch_semitones: number;
};
type StretchCase = {
  id: string;
  n_fft: number;
  hop_length: number;
  cause?: Cause;
  message?: string;
  /** The C ABI's text, where its hop rule refuses ahead of the core primitive. */
  c_message?: string;
};
type AbsentHopCase = {
  id: string;
  n_fft: number;
  hop_length: number;
  derived_hop_length: number;
  primitive_message: string;
};
type StretchRoute = {
  input: StretchInput;
  accepted: StretchCase[];
  refused: StretchCase[];
  absent_hop: AbsentHopCase[];
};
type RateBlockCase = {
  id: string;
  sample_rate: number;
  max_block_size: number;
  cause?: Cause;
  message?: string;
};
type RateBlockRoute = {
  input: { num_channels: 1 | 2 };
  accepted: RateBlockCase[];
  refused: RateBlockCase[];
};
type EqualizerRoute = { accepted: RateBlockCase[]; refused: RateBlockCase[] };
type RetuneCase = {
  id: string;
  sample_rate: number;
  max_block_size: number;
  cause?: Cause;
  message?: string;
};
type RetuneRoute = { accepted: RetuneCase[]; refused: RetuneCase[] };
type RetuneConfigField = 'semitones' | 'mix' | 'grain_size';
type RetuneConfigCase = {
  id: string;
  semitones: number | 'nan' | 'inf';
  mix: number | 'nan' | 'inf';
  grain_size: number;
  field?: RetuneConfigField;
  cause?: Cause;
  predicate?: string;
};
type RetuneConfigRoute = { accepted: RetuneConfigCase[]; refused: RetuneConfigCase[] };
type ResampleCase = {
  id: string;
  src_sr: number;
  target_sr: number;
  nan_at?: number;
  cause?: Cause;
  message?: string;
};
type ResampleRoute = {
  input: { length: number; fill: number };
  accepted: ResampleCase[];
  refused: ResampleCase[];
};
type MasterAudioCase = {
  id: string;
  overrides?: Record<string, never>;
  returns?: 'input';
  cause?: Cause;
  message?: string;
};
type MasterAudioRoute = {
  input: { sample_rate: number; length: number; frequency_hz: number; amplitude: number };
  accepted: MasterAudioCase[];
  refused: MasterAudioCase[];
};
type ZeroCrossingsCase = { id: string; nan_at?: number; cause?: Cause; message?: string };
type ZeroCrossingsRoute = {
  input: { length: number; fill: number };
  accepted: ZeroCrossingsCase[];
  refused: ZeroCrossingsCase[];
};

const corpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/core_validation_refusals.json', import.meta.url),
    'utf8',
  ),
) as {
  mfcc_to_mel: MfccRoute;
  mel_to_audio: MelRoute;
  mfcc_to_audio: MfccAudioRoute;
  griffin_lim: GriffinLimRoute;
  native_spectral_stretch: StretchRoute;
  streaming_mastering_chain_prepare: RateBlockRoute;
  equalizer_prepare: EqualizerRoute;
  streaming_retune_prepare: RetuneRoute;
  streaming_retune_config: RetuneConfigRoute;
  resample: ResampleRoute;
  zero_crossings: ZeroCrossingsRoute;
  master_audio: MasterAudioRoute;
};

const errorClassFor: Record<Cause, typeof RangeError> = {
  argument: RangeError,
  missing_argument: TypeError,
  shape: TypeError,
};
const literal = (text: string) => new RegExp(text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
/** A core text as the facade renders it: the leading snake_case key respelled in camelCase. */
const publicText = (text: string) =>
  literal(
    text.replace(/^[a-z0-9]+(?:_[a-z0-9]+)+/, (key) =>
      key.replace(/_([a-z0-9])/g, (_, c: string) => c.toUpperCase()),
    ),
  );

type RawInverse = {
  mfccToMel(
    data: Float32Array,
    nMfcc: number,
    nFrames: number,
    nMels: number,
    lifter: number,
  ): {
    nMels: number;
  };
  mfccToAudio(...args: unknown[]): Float32Array;
  resample(samples: Float32Array, srcSr: number, targetSr: number): Float32Array;
  melToAudio(...args: unknown[]): Float32Array;
  griffinLim(...args: unknown[]): Float32Array;
  timeStretch(...args: unknown[]): Float32Array;
  pitchShift(...args: unknown[]): Float32Array;
  createStreamingMasteringChain(configJson: string | null): {
    prepare(sampleRate: number, maxBlockSize: number, numChannels: number): void;
    delete(): void;
  };
  createEqualizer(config: Record<string, unknown>): { delete(): void };
  createStreamingRetune(config: Record<string, unknown>): {
    prepare(sampleRate: number, maxBlockSize: number): void;
    delete(): void;
  };
};

beforeAll(async () => {
  await init();
});

describe('mfcc_to_mel refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mfcc_to_mel;
  const data = new Float32Array(input.n_mfcc * input.n_frames).fill(input.fill);
  const coefficients = { data, rows: input.n_mfcc, cols: input.n_frames };
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ n_mels }) => {
    expect(mfccToMel({ coefficients, nMels: n_mels }).rows).toBe(n_mels);
    expect(raw().mfccToMel(data, input.n_mfcc, input.n_frames, n_mels, 0).nMels).toBe(n_mels);
  });

  it.each(refused)('refuses $id', ({ n_mels, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = publicText(message as string);
    const calls: Array<() => unknown> = [
      () => mfccToMel({ coefficients, nMels: n_mels }),
      () => raw().mfccToMel(data, input.n_mfcc, input.n_frames, n_mels, 0),
      () => mfccToAudio({ coefficients, nMels: n_mels, nIter: 1 }),
      () =>
        raw().mfccToAudio(
          data,
          input.n_mfcc,
          input.n_frames,
          n_mels,
          22050,
          2048,
          512,
          0,
          0,
          1,
          false,
          0,
        ),
    ];
    for (const call of calls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});

describe('mel_to_audio refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mel_to_audio;
  const data = new Float32Array(input.n_mels * input.n_frames).fill(input.fill);
  const power = { data, rows: input.n_mels, cols: input.n_frames };
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ n_iter }) => {
    expect(melToAudio({ power, nIter: n_iter }).length).toBeGreaterThan(0);
    const out = raw().melToAudio(
      data,
      input.n_mels,
      input.n_frames,
      22050,
      2048,
      512,
      0,
      0,
      n_iter,
      false,
    );
    expect(out.length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', ({ n_iter, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = publicText(message as string);
    const calls: Array<() => unknown> = [
      () => melToAudio({ power, nIter: n_iter }),
      () =>
        raw().melToAudio(data, input.n_mels, input.n_frames, 22050, 2048, 512, 0, 0, n_iter, false),
    ];
    for (const call of calls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});

describe('mfcc_to_audio refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mfcc_to_audio;
  const data = new Float32Array(input.n_mfcc * input.n_frames).fill(input.fill);
  const coefficients = { data, rows: input.n_mfcc, cols: input.n_frames };
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ n_iter }) => {
    const out = raw().mfccToAudio(
      data,
      input.n_mfcc,
      input.n_frames,
      input.n_mels,
      22050,
      2048,
      512,
      0,
      0,
      n_iter,
      false,
      0,
    );
    const viaFacade = mfccToAudio({ coefficients, nMels: input.n_mels, nIter: n_iter });
    expect(viaFacade.length).toBeGreaterThan(0);
    expect(out.length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', ({ n_iter, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = publicText(message as string);
    const calls: Array<() => unknown> = [
      () => mfccToAudio({ coefficients, nMels: input.n_mels, nIter: n_iter }),
      () =>
        raw().mfccToAudio(
          data,
          input.n_mfcc,
          input.n_frames,
          input.n_mels,
          22050,
          2048,
          512,
          0,
          0,
          n_iter,
          false,
          0,
        ),
    ];
    for (const call of calls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});

describe('an absent inverse-transform hop is derived from nFft by the core', () => {
  const mel = corpus.mel_to_audio;
  const melData = new Float32Array(mel.input.n_mels * mel.input.n_frames).fill(mel.input.fill);
  const power = { data: melData, rows: mel.input.n_mels, cols: mel.input.n_frames };
  const mfcc = corpus.mfcc_to_audio;
  const mfccData = new Float32Array(mfcc.input.n_mfcc * mfcc.input.n_frames).fill(mfcc.input.fill);
  const coefficients = { data: mfccData, rows: mfcc.input.n_mfcc, cols: mfcc.input.n_frames };
  const gl = corpus.griffin_lim;
  const magnitude = (nFft: number) => {
    const rows = nFft / 2 + 1;
    const data = new Float32Array(rows * gl.input.n_frames).fill(gl.input.fill);
    return { data, rows, cols: gl.input.n_frames };
  };
  const hop = (hopLength?: number) => (hopLength === undefined ? {} : { hopLength });
  const N_ITER = 2;
  const raw = () => getSonareModule() as unknown as RawInverse;

  type Route = {
    cases: InverseHops;
    facade: (nFft: number, hopLength?: number) => Float32Array;
    native: (nFft: number, hopLength: number) => Float32Array;
  };
  const routes: Array<[string, Route]> = [
    [
      'melToAudio',
      {
        cases: mel,
        facade: (nFft, hopLength) => melToAudio({ power, nFft, nIter: N_ITER, ...hop(hopLength) }),
        native: (nFft, hopLength) =>
          raw().melToAudio(
            melData,
            mel.input.n_mels,
            mel.input.n_frames,
            22050,
            nFft,
            hopLength,
            0,
            0,
            N_ITER,
            false,
          ),
      },
    ],
    [
      'mfccToAudio',
      {
        cases: mfcc,
        facade: (nFft, hopLength) =>
          mfccToAudio({
            coefficients,
            nMels: mfcc.input.n_mels,
            nFft,
            nIter: N_ITER,
            ...hop(hopLength),
          }),
        native: (nFft, hopLength) =>
          raw().mfccToAudio(
            mfccData,
            mfcc.input.n_mfcc,
            mfcc.input.n_frames,
            mfcc.input.n_mels,
            22050,
            nFft,
            hopLength,
            0,
            0,
            N_ITER,
            false,
            0,
          ),
      },
    ],
    [
      'griffinLim',
      {
        cases: gl,
        facade: (nFft, hopLength) =>
          griffinLim({
            magnitude: magnitude(nFft),
            sampleRate: gl.input.sample_rate,
            nFft,
            nIter: N_ITER,
            ...hop(hopLength),
          }),
        native: (nFft, hopLength) => {
          const m = magnitude(nFft);
          return raw().griffinLim(
            m.data,
            m.rows,
            m.cols,
            gl.input.sample_rate,
            nFft,
            hopLength,
            N_ITER,
            0.99,
          );
        },
      },
    ],
  ];

  for (const [name, route] of routes) {
    it.each(route.cases.absent_hop)(
      `${name} $id: an omitted hop runs as the derived one and the facade refuses an explicit 0`,
      ({ n_fft, hop_length, derived_hop_length }) => {
        const derived = derived_hop_length as number;
        expect(route.facade(n_fft)).toEqual(route.facade(n_fft, derived));
        // The native entry reads 0 as the derived hop; only the facade refuses it.
        expect(route.native(n_fft, hop_length)).toEqual(route.native(n_fft, derived));
        expect(expectRefusalOf(() => route.facade(n_fft, hop_length), RangeError).message).toMatch(
          /hopLength/,
        );
      },
    );

    it.each(route.cases.negative_hop)(
      `${name} $id is refused`,
      ({ n_fft, hop_length, cause, message }) => {
        const kind = errorClassFor[cause as Cause];
        expect(expectRefusalOf(() => route.facade(n_fft, hop_length), kind).message).toMatch(
          /hopLength/,
        );
        expect(expectRefusalOf(() => route.native(n_fft, hop_length), kind).message).toMatch(
          publicText(message as string),
        );
      },
    );
  }
});

describe('native spectral stretch geometry refusals come from the core', () => {
  const { input, accepted, refused } = corpus.native_spectral_stretch;
  const samples = new Float32Array(input.length).fill(input.fill);
  const rate = input.sample_rate;
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ n_fft, hop_length }) => {
    const options = { nFft: n_fft, hopLength: hop_length };
    const stretch = { samples, sampleRate: rate, speedRatio: input.speed_ratio, ...options };
    const shift = { samples, sampleRate: rate, pitchSemitones: input.pitch_semitones, ...options };
    expect(timeStretch(stretch).length).toBeGreaterThan(0);
    expect(pitchShift(shift).length).toBeGreaterThan(0);
    const viaStretch = raw().timeStretch(samples, rate, input.speed_ratio, n_fft, hop_length);
    const viaShift = raw().pitchShift(samples, rate, input.pitch_semitones, n_fft, hop_length);
    expect(viaStretch.length).toBeGreaterThan(0);
    expect(viaShift.length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', ({ n_fft, hop_length, cause, message, c_message }) => {
    const kind = errorClassFor[cause as Cause];
    // The module restates the C hop rule ahead of the core, so a hop it refuses carries the C text.
    const text = c_message === undefined ? new RegExp(message as string) : publicText(c_message);
    const options = { nFft: n_fft, hopLength: hop_length };
    // The facade's own geometry domain refuses first, so only the class is pinned there.
    const facadeCalls: Array<() => unknown> = [
      () => timeStretch({ samples, sampleRate: rate, speedRatio: input.speed_ratio, ...options }),
      () =>
        pitchShift({
          samples,
          sampleRate: rate,
          pitchSemitones: input.pitch_semitones,
          ...options,
        }),
    ];
    for (const call of facadeCalls) {
      expectRefusalOf(call, kind);
    }
    const rawCalls: Array<() => unknown> = [
      () => raw().timeStretch(samples, rate, input.speed_ratio, n_fft, hop_length),
      () => raw().pitchShift(samples, rate, input.pitch_semitones, n_fft, hop_length),
    ];
    for (const call of rawCalls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});

describe('an absent resynthesis hop is derived from nFft by the core', () => {
  const { input, absent_hop } = corpus.native_spectral_stretch;
  const samples = new Float32Array(input.length).fill(input.fill);
  const rate = input.sample_rate;
  const raw = () => getSonareModule() as unknown as RawInverse;

  // The core primitive's own refusal of a 0 hop is not reachable from a binding;
  // the C++ corpus reader pins primitive_message.
  it.each(absent_hop)(
    '$id: an omitted hop runs as the derived one',
    ({ n_fft, derived_hop_length }) => {
      const stretch = { samples, sampleRate: rate, speedRatio: input.speed_ratio, nFft: n_fft };
      const shift = {
        samples,
        sampleRate: rate,
        pitchSemitones: input.pitch_semitones,
        nFft: n_fft,
      };
      const derived = { hopLength: derived_hop_length };
      expect(timeStretch(stretch)).toEqual(timeStretch({ ...stretch, ...derived }));
      expect(pitchShift(shift)).toEqual(pitchShift({ ...shift, ...derived }));
      // The module reads 0, which the facades send for an omitted hop, as the derived one.
      expect(raw().timeStretch(samples, rate, input.speed_ratio, n_fft, 0)).toEqual(
        raw().timeStretch(samples, rate, input.speed_ratio, n_fft, derived_hop_length),
      );
      expect(raw().pitchShift(samples, rate, input.pitch_semitones, n_fft, 0)).toEqual(
        raw().pitchShift(samples, rate, input.pitch_semitones, n_fft, derived_hop_length),
      );
    },
  );

  it.each(absent_hop)(
    '$id: an explicit hop of 0 is refused by the facade and a negative one by the module',
    ({ n_fft, hop_length }) => {
      expect(
        expectRefusalOf(
          () => raw().timeStretch(samples, rate, input.speed_ratio, n_fft, -1),
          RangeError,
        ).message,
      ).toMatch(/hopLength must be an integer in \[1, 2147483647\]/);
      const options = { nFft: n_fft, hopLength: hop_length };
      const calls: Array<() => unknown> = [
        () => timeStretch({ samples, sampleRate: rate, speedRatio: input.speed_ratio, ...options }),
        () =>
          pitchShift({
            samples,
            sampleRate: rate,
            pitchSemitones: input.pitch_semitones,
            ...options,
          }),
      ];
      for (const call of calls) {
        expect(expectRefusalOf(call, RangeError).message).toMatch(/hopLength/);
      }
    },
  );
});

describe('streaming mastering chain prepare refusals come from the core', () => {
  const { input, accepted, refused } = corpus.streaming_mastering_chain_prepare;
  const channels = input.num_channels;
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const chain = new StreamingMasteringChain({
      sampleRate: sample_rate,
      numChannels: channels,
      maxBlockSize: max_block_size,
    });
    const handle = raw().createStreamingMasteringChain(null);
    try {
      expect(() => handle.prepare(sample_rate, max_block_size, channels)).not.toThrow();
    } finally {
      chain.destroy();
      handle.delete();
    }
  });

  it.each(refused)('refuses $id', ({ sample_rate, max_block_size, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = literal(message as string);
    const handle = raw().createStreamingMasteringChain(null);
    try {
      const calls: Array<() => unknown> = [
        () =>
          new StreamingMasteringChain({
            sampleRate: sample_rate,
            numChannels: channels,
            maxBlockSize: max_block_size,
          }),
        () => handle.prepare(sample_rate, max_block_size, channels),
      ];
      for (const call of calls) {
        expect(expectRefusalOf(call, kind).message).toMatch(text);
      }
    } finally {
      handle.delete();
    }
  });
});

describe('equalizer prepare refusals come from the core', () => {
  const { accepted, refused } = corpus.equalizer_prepare;
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const config = { sampleRate: sample_rate, maxBlockSize: max_block_size };
    const eq = new StreamingEqualizer(config);
    const handle = raw().createEqualizer(config);
    eq.destroy();
    handle.delete();
  });

  it.each(refused)('refuses $id', ({ sample_rate, max_block_size, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = literal(message as string);
    const config = { sampleRate: sample_rate, maxBlockSize: max_block_size };
    const calls: Array<() => unknown> = [
      () => new StreamingEqualizer(config),
      () => raw().createEqualizer(config),
    ];
    for (const call of calls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});

describe('streaming retune prepare refusals come from the core', () => {
  const { accepted, refused } = corpus.streaming_retune_prepare;
  const raw = () => getSonareModule() as unknown as RawInverse;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const retune = new StreamingRetune();
    const handle = raw().createStreamingRetune({});
    try {
      expect(() => retune.prepare(sample_rate, max_block_size)).not.toThrow();
      expect(() => handle.prepare(sample_rate, max_block_size)).not.toThrow();
    } finally {
      retune.destroy();
      handle.delete();
    }
  });

  it.each(refused)('refuses $id', ({ sample_rate, max_block_size, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = literal(message as string);
    const retune = new StreamingRetune();
    const handle = raw().createStreamingRetune({});
    try {
      const calls: Array<() => unknown> = [
        () => retune.prepare(sample_rate, max_block_size),
        () => handle.prepare(sample_rate, max_block_size),
      ];
      for (const call of calls) {
        expect(expectRefusalOf(call, kind).message).toMatch(text);
      }
    } finally {
      retune.destroy();
      handle.delete();
    }
  });
});

const retuneConfigKey: Record<RetuneConfigField, string> = {
  semitones: 'pitchSemitones',
  mix: 'mix',
  grain_size: 'grainSizeSamples',
};
const corpusNumber = (value: number | 'nan' | 'inf') =>
  value === 'nan' ? Number.NaN : value === 'inf' ? Number.POSITIVE_INFINITY : value;
// Grain 0 is the C spelling of an omitted grain.
const retuneControls = ({ semitones, mix, grain_size }: RetuneConfigCase) => ({
  pitchSemitones: corpusNumber(semitones),
  mix: corpusNumber(mix),
  ...(grain_size === 0 ? {} : { grainSizeSamples: grain_size }),
});

describe('streaming retune config refusals come from the core', () => {
  const { accepted, refused } = corpus.streaming_retune_config;

  it.each(accepted)('accepts $id', (item) => {
    const controls = retuneControls(item);
    const retune = new StreamingRetune(controls);
    try {
      expect(retune.config()).toEqual(controls);
      expect(() => retune.setConfig(controls)).not.toThrow();
    } finally {
      retune.destroy();
    }
  });

  it.each(refused)('refuses $id, and a refused update changes nothing', (item) => {
    const kind = errorClassFor[item.cause as Cause];
    const message = `${retuneConfigKey[item.field as RetuneConfigField]} ${item.predicate}`;
    const controls = retuneControls(item);
    expect(expectRefusalOf(() => new StreamingRetune(controls), kind).message).toMatch(
      literal(`StreamingRetune: ${message}`),
    );
    const retune = new StreamingRetune({ pitchSemitones: 3, mix: 0.5 });
    try {
      const before = retune.config();
      expect(expectRefusalOf(() => retune.setConfig(controls), kind).message).toMatch(
        literal(`StreamingRetune.setConfig: ${message}`),
      );
      expect(retune.config()).toEqual(before);
    } finally {
      retune.destroy();
    }
  });
});

describe('resample refusals come from the core', () => {
  const { input, accepted, refused } = corpus.resample;
  const raw = () => getSonareModule() as unknown as RawInverse;
  const samplesFor = ({ nan_at }: ResampleCase) => {
    const samples = new Float32Array(input.length).fill(input.fill);
    if (nan_at !== undefined) {
      samples[nan_at] = Number.NaN;
    }
    return samples;
  };

  it.each(accepted)('accepts $id', (item) => {
    const samples = samplesFor(item);
    expect(resample(samples, item.src_sr, item.target_sr).length).toBeGreaterThan(0);
    expect(raw().resample(samples, item.src_sr, item.target_sr).length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', (item) => {
    const kind = errorClassFor[item.cause as Cause];
    const text = publicText(item.message as string);
    const samples = samplesFor(item);
    const calls: Array<() => unknown> = [
      () => resample(samples, item.src_sr, item.target_sr),
      () => raw().resample(samples, item.src_sr, item.target_sr),
    ];
    for (const call of calls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
});

describe('resample takes no validate option', () => {
  it('refuses validate as an unknown request key', () => {
    const samples = new Float32Array(480).fill(0.25);
    const error = expectRefusalOf(
      () =>
        resample({ samples, sampleRate: 48000, targetSampleRate: 16000, validate: false } as never),
      TypeError,
    );
    expect(error.message).toMatch(/validate/);
  });
});

describe('zeroCrossings refuses a non-finite sample', () => {
  const { input, accepted, refused } = corpus.zero_crossings;
  const samplesFor = ({ nan_at }: ZeroCrossingsCase) => {
    const samples = new Float32Array(input.length).fill(input.fill);
    if (nan_at !== undefined) {
      samples[nan_at] = Number.NaN;
    }
    return samples;
  };

  it.each(accepted)('accepts $id', (item) => {
    const samples = samplesFor(item);
    expect(zeroCrossings(samples, 22050).length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id with the facade scan', (item) => {
    const kind = errorClassFor[item.cause as Cause];
    const samples = samplesFor(item);
    const error = expectRefusalOf(() => zeroCrossings(samples, 22050), kind);
    expect(error.message).toMatch(/NaN or Inf|non-finite/);
  });

  it('refuses validate as an unknown option', () => {
    const samples = samplesFor({ id: 'clean_signal' } as ZeroCrossingsCase);
    const error = expectRefusalOf(
      () => zeroCrossings(samples, 22050, { validate: false } as never),
      TypeError,
    );
    expect(error.message).toMatch(unknownKeyText('zeroCrossings: unknown key validate'));
  });
});

// The facade owns this refusal: a chain with neither a preset nor overrides would return its input.
describe('masterAudio refuses a call giving neither a preset nor overrides', () => {
  const { input, accepted, refused } = corpus.master_audio;
  const samples = Float32Array.from(
    { length: input.length },
    (_, i) =>
      input.amplitude * Math.sin((2 * Math.PI * input.frequency_hz * i) / input.sample_rate),
  );
  const sampleRate = input.sample_rate;

  it.each(accepted)('accepts $id', ({ overrides }) => {
    expect(masterAudio({ samples, sampleRate, overrides }).samples).toEqual(samples);
    expect(masterAudio(samples, sampleRate, { overrides }).samples).toEqual(samples);
    const stereo = masterAudioStereo({ left: samples, right: samples, sampleRate, overrides });
    expect(stereo.left).toEqual(samples);
    expect(stereo.right).toEqual(samples);
    using audio = Audio.fromBuffer(samples, sampleRate);
    expect(audio.masterAudio({ overrides }).samples).toEqual(samples);
  });

  const forms: Array<[string, () => unknown]> = [
    ['masterAudio', () => masterAudio({ samples, sampleRate })],
    ['masterAudio', () => masterAudio(samples, sampleRate)],
    [
      'masterAudio',
      () => masterAudio({ samples, sampleRate, preset: undefined, overrides: undefined }),
    ],
    ['masterAudio', () => masterAudio(samples, sampleRate, { onProgress: () => {} })],
    [
      'Audio.masterAudio',
      () => {
        using audio = Audio.fromBuffer(samples, sampleRate);
        return audio.masterAudio({});
      },
    ],
    ['masterAudioStereo', () => masterAudioStereo({ left: samples, right: samples, sampleRate })],
  ];

  it.each(refused)('refuses $id in every synchronous form', (item) => {
    const kind = errorClassFor[item.cause as Cause];
    for (const [name, call] of forms) {
      const text = new RegExp(`^${literal(`${name}: ${item.message as string}`).source}$`);
      const error = expectRefusalOf(call, kind);
      expect(error.message).toMatch(text);
    }
  });
});

type MixSceneLoader = 'mixer_from_json' | 'mixer_scene_option' | 'project_set_mixer_scene';
type MixSceneCase = {
  id: string;
  scene: Record<string, unknown>;
  cause?: 'format';
  code?: keyof typeof ErrorCode;
  message?: string;
};
const mixScene = (
  corpus as unknown as {
    mix_scene_document: {
      loaders: MixSceneLoader[];
      accepted: MixSceneCase[];
      refused: MixSceneCase[];
    };
  }
).mix_scene_document;
const MIX_OPTIONS = { sampleRate: 48000, maxBlockSize: 512 };
const loadMixScene: Record<MixSceneLoader, (scene: Record<string, unknown>) => void> = {
  mixer_from_json: (scene) => Mixer.fromJson(JSON.stringify(scene), MIX_OPTIONS).destroy(),
  mixer_scene_option: (scene) => new Mixer({ ...MIX_OPTIONS, scene: scene as never }).destroy(),
  project_set_mixer_scene: (scene) => {
    const project = new Project();
    try {
      project.setMixerScene(scene);
    } finally {
      project.destroy();
    }
  },
};

describe('mix-scene document refusals come from the core', () => {
  it.each(mixScene.loaders)('loads every accepted scene through %s', (loader) => {
    for (const { scene } of mixScene.accepted) {
      expect(() => loadMixScene[loader](scene)).not.toThrow();
    }
  });

  const refused = mixScene.loaders.flatMap((loader) =>
    mixScene.refused.map((entry) => ({ loader, ...entry })),
  );
  it.each(refused)('refuses $id through $loader', ({ loader, scene, cause, code, message, id }) => {
    expect(cause).toBe('format');
    if (loader === 'mixer_scene_option' || loader === 'project_set_mixer_scene') {
      const call = () => loadMixScene[loader](scene);
      const shape = id === 'unknown_key' || id === 'send_timing_ordinal';
      const entry = loader === 'mixer_scene_option' ? 'Mixer' : 'Project.setMixerScene';
      const text = (message as string).replace(/^(unknown key )?/, '$1scene.');
      expect(call).toThrow(shape ? TypeError : RangeError);
      expect(call).toThrow(
        expect.objectContaining({
          message: expect.stringContaining(`${entry}: ${text}`),
        }),
      );
      return;
    }
    expect(() => loadMixScene[loader](scene)).toThrow(
      expect.objectContaining({
        code: ErrorCode[code as keyof typeof ErrorCode],
        message: expect.stringContaining(message as string),
      }),
    );
  });
});

type CorpusCode = keyof typeof ErrorCode;
type RigCase = {
  id: string;
  inserts: unknown[];
  cause?: Cause | 'format';
  code?: CorpusCode;
  message?: string;
};
type FxCase = {
  id: string;
  config: Record<string, unknown>;
  cause?: Cause;
  code?: CorpusCode;
  message?: string;
};
const rigCorpus = (
  corpus as unknown as {
    part_rig_inserts: {
      setters: string[];
      accepting_setters: string[];
      accepted: RigCase[];
      refused: RigCase[];
      project_document: RigCase[];
    };
  }
).part_rig_inserts;
const fxCorpus = (
  corpus as unknown as {
    midi_fx_config: { setters: string[]; accepted: FxCase[]; refused: FxCase[] };
  }
).midi_fx_config;
const ENGINE_OPTIONS = { sampleRate: 48000, maxBlockSize: 128 };
const RIG_SETTERS: Record<string, (inserts: unknown[]) => void> = {
  project_set_part_rig: (inserts) => {
    const project = new Project();
    try {
      project.setPartRig({ destinationId: 1, part: 0, mode: 'chain', inserts: inserts as never });
    } finally {
      project.destroy();
    }
  },
  engine_set_part_rig: (inserts) => {
    const engine = new RealtimeEngine(ENGINE_OPTIONS);
    try {
      engine.setPartRig({ destinationId: 1, part: 0, mode: 'chain', inserts: inserts as never });
    } finally {
      engine.destroy();
    }
  },
};
const projectMidiFx =
  (call: 'bakeMidiFx' | 'previewMidiFxCount') => (config: Record<string, unknown>) => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      project[call]({ clipId, config: config as never });
    } finally {
      project.destroy();
    }
  };
const FX_SETTERS: Record<string, (config: Record<string, unknown>) => void> = {
  engine_set_midi_fx: (config) => {
    const engine = new RealtimeEngine(ENGINE_OPTIONS);
    try {
      engine.setMidiFx(5, config as never);
    } finally {
      engine.destroy();
    }
  },
  project_bake_midi_fx: projectMidiFx('bakeMidiFx'),
  project_preview_midi_fx_count: projectMidiFx('previewMidiFxCount'),
};
/** An argument refusal: the class follows the cause; the corpus code is the C ABI's. */
const expectArgumentRefusal = (
  call: () => void,
  cause: Cause,
  code: CorpusCode,
  message: string,
) => {
  expect(code).toBe('InvalidParameter');
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(errorClassFor[cause]);
  expect((caught as Error).message).toContain(message);
};

describe('part-rig inserts are refused as a call argument by the core', () => {
  it('drives every setter the corpus names', () => {
    expect(Object.keys(RIG_SETTERS).sort()).toEqual([...rigCorpus.setters].sort());
  });

  it.each(rigCorpus.accepted)('accepts $id', ({ inserts }) => {
    for (const setter of rigCorpus.accepting_setters) {
      expect(() => RIG_SETTERS[setter](inserts)).not.toThrow();
    }
  });

  const refused = rigCorpus.setters.flatMap((setter) =>
    rigCorpus.refused.map((entry) => ({ setter, ...entry })),
  );
  it.each(refused)('refuses $id through $setter', ({ setter, inserts, cause, code, message }) => {
    expectArgumentRefusal(
      () => RIG_SETTERS[setter](inserts),
      cause as Cause,
      code as CorpusCode,
      message as string,
    );
  });

  it.each(rigCorpus.project_document)(
    'refuses $id as document content inside a loaded project',
    ({ inserts, code, message }) => {
      const blank = new Project();
      const document = JSON.parse(blank.toJson()) as Record<string, unknown>;
      blank.destroy();
      document.partRigs = [{ destinationId: 1, part: 0, mode: 'chain', inserts }];
      expect(() => Project.fromJson(JSON.stringify(document))).toThrow(
        expect.objectContaining({
          code: ErrorCode[code as CorpusCode],
          message: expect.stringContaining(message as string),
        }),
      );
      let caught: unknown;
      try {
        Project.fromJson(JSON.stringify(document));
      } catch (error) {
        caught = error;
      }
      // A document's content is never re-labelled as an argument.
      expect(caught).not.toBeInstanceOf(TypeError);
      expect(caught).not.toBeInstanceOf(RangeError);
    },
  );
});

describe('MIDI-FX config is refused as a call argument by the core', () => {
  it('drives every setter the corpus names', () => {
    expect(Object.keys(FX_SETTERS).sort()).toEqual([...fxCorpus.setters].sort());
  });

  const accepted = fxCorpus.setters.flatMap((setter) =>
    fxCorpus.accepted.map((entry) => ({ setter, ...entry })),
  );
  it.each(accepted)('accepts $id through $setter', ({ setter, config }) => {
    expect(() => FX_SETTERS[setter](config)).not.toThrow();
  });

  const refused = fxCorpus.setters.flatMap((setter) =>
    fxCorpus.refused.map((entry) => ({ setter, ...entry })),
  );
  it.each(refused)('refuses $id through $setter', ({ setter, config, cause, code, message }) => {
    expectArgumentRefusal(
      () => FX_SETTERS[setter](config),
      cause as Cause,
      code as CorpusCode,
      message as string,
    );
  });
});
