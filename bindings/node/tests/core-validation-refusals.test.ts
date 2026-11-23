import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  ErrorCode,
  griffinLim,
  Mixer,
  masterAudio,
  masterAudioAsync,
  masterAudioStereo,
  masterAudioStereoAsync,
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
} from '../src/index.js';
import { addon } from '../src/native.js';
import { expectRefusal } from './_helpers.js';

// The core function owns each refusal in this corpus. The addon calls the core
// directly, so the refusal is asserted on the raw addon as well as on the facade.

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
type ZeroCrossingsCase = {
  id: string;
  nan_at?: number;
  cause?: Cause;
  message?: string;
  c_message?: string;
};
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

const errorClassFor: Record<Cause, ErrorConstructor> = {
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

describe('mfcc_to_mel refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mfcc_to_mel;
  const data = new Float32Array(input.n_mfcc * input.n_frames).fill(input.fill);
  const coefficients = { data, rows: input.n_mfcc, cols: input.n_frames };

  it.each(accepted)('accepts $id', ({ n_mels }) => {
    expect(mfccToMel({ coefficients, nMels: n_mels }).rows).toBe(n_mels);
    expect(addon.mfccToMel(data, input.n_mfcc, input.n_frames, n_mels, 0).nMels).toBe(n_mels);
  });

  it.each(refused)('refuses $id', ({ n_mels, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = publicText(message as string);
    expectRefusal(() => mfccToMel({ coefficients, nMels: n_mels }), kind, text);
    expectRefusal(() => addon.mfccToMel(data, input.n_mfcc, input.n_frames, n_mels, 0), kind, text);
    expectRefusal(() => mfccToAudio({ coefficients, nMels: n_mels, nIter: 1 }), kind, text);
    expectRefusal(
      () =>
        addon.mfccToAudio(
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
      kind,
      text,
    );
  });
});

describe('mel_to_audio refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mel_to_audio;
  const data = new Float32Array(input.n_mels * input.n_frames).fill(input.fill);
  const power = { data, rows: input.n_mels, cols: input.n_frames };

  it.each(accepted)('accepts $id', ({ n_iter }) => {
    expect(melToAudio({ power, nIter: n_iter }).length).toBeGreaterThan(0);
    const out = addon.melToAudio(
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
    expectRefusal(() => melToAudio({ power, nIter: n_iter }), kind, text);
    expectRefusal(
      () =>
        addon.melToAudio(data, input.n_mels, input.n_frames, 22050, 2048, 512, 0, 0, n_iter, false),
      kind,
      text,
    );
  });
});

describe('mfcc_to_audio refusals come from the core', () => {
  const { input, accepted, refused } = corpus.mfcc_to_audio;
  const data = new Float32Array(input.n_mfcc * input.n_frames).fill(input.fill);
  const coefficients = { data, rows: input.n_mfcc, cols: input.n_frames };

  it.each(accepted)('accepts $id', ({ n_iter }) => {
    const out = addon.mfccToAudio(
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
    expectRefusal(
      () => mfccToAudio({ coefficients, nMels: input.n_mels, nIter: n_iter }),
      kind,
      text,
    );
    expectRefusal(
      () =>
        addon.mfccToAudio(
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
      kind,
      text,
    );
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

  type Route = {
    cases: InverseHops;
    facade: (nFft: number, hopLength?: number) => Float32Array;
    native: (nFft: number, hopLength?: number) => Float32Array;
  };
  const routes: Array<[string, Route]> = [
    [
      'melToAudio',
      {
        cases: mel,
        facade: (nFft, hopLength) => melToAudio({ power, nFft, nIter: N_ITER, ...hop(hopLength) }),
        native: (nFft, hopLength) =>
          addon.melToAudio(
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
          addon.mfccToAudio(
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
          return addon.griffinLim(
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
        // The native entry reads an omitted hop and 0 as the derived one; only the facade refuses 0.
        expect(route.native(n_fft)).toEqual(route.native(n_fft, derived));
        expect(route.native(n_fft, hop_length)).toEqual(route.native(n_fft, derived));
        expectRefusal(() => route.facade(n_fft, hop_length), RangeError, /hopLength/);
      },
    );

    it.each(route.cases.negative_hop)(
      `${name} $id is refused`,
      ({ n_fft, hop_length, cause, message }) => {
        const kind = errorClassFor[cause as Cause];
        expectRefusal(() => route.facade(n_fft, hop_length), kind, /hopLength/);
        expectRefusal(() => route.native(n_fft, hop_length), kind, publicText(message as string));
      },
    );
  }
});

describe('native spectral stretch geometry refusals come from the core', () => {
  const { input, accepted, refused } = corpus.native_spectral_stretch;
  const samples = new Float32Array(input.length).fill(input.fill);
  const rate = input.sample_rate;

  it.each(accepted)('accepts $id', ({ n_fft, hop_length }) => {
    const options = { nFft: n_fft, hopLength: hop_length };
    const stretch = { samples, sampleRate: rate, speedRatio: input.speed_ratio, ...options };
    const shift = { samples, sampleRate: rate, pitchSemitones: input.pitch_semitones, ...options };
    expect(timeStretch(stretch).length).toBeGreaterThan(0);
    expect(pitchShift(shift).length).toBeGreaterThan(0);
    const viaStretch = addon.timeStretch(samples, rate, input.speed_ratio, n_fft, hop_length);
    const viaShift = addon.pitchShift(samples, rate, input.pitch_semitones, n_fft, hop_length);
    expect(viaStretch.length).toBeGreaterThan(0);
    expect(viaShift.length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', ({ n_fft, hop_length, cause, message, c_message }) => {
    const kind = errorClassFor[cause as Cause];
    // The addon restates the C hop rule ahead of the core, so a hop it refuses carries the C text.
    const text = c_message === undefined ? new RegExp(message as string) : publicText(c_message);
    const options = { nFft: n_fft, hopLength: hop_length };
    // The facade's own geometry domain refuses first, so only the class is pinned there.
    expectRefusal(
      () => timeStretch({ samples, sampleRate: rate, speedRatio: input.speed_ratio, ...options }),
      kind,
    );
    expectRefusal(
      () =>
        pitchShift({
          samples,
          sampleRate: rate,
          pitchSemitones: input.pitch_semitones,
          ...options,
        }),
      kind,
    );
    expectRefusal(
      () => addon.timeStretch(samples, rate, input.speed_ratio, n_fft, hop_length),
      kind,
      text,
    );
    expectRefusal(
      () => addon.pitchShift(samples, rate, input.pitch_semitones, n_fft, hop_length),
      kind,
      text,
    );
  });
});

describe('an absent resynthesis hop is derived from nFft by the core', () => {
  const { input, absent_hop } = corpus.native_spectral_stretch;
  const samples = new Float32Array(input.length).fill(input.fill);
  const rate = input.sample_rate;

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
      expect(addon.timeStretch(samples, rate, input.speed_ratio, n_fft)).toEqual(
        addon.timeStretch(samples, rate, input.speed_ratio, n_fft, derived_hop_length),
      );
      expect(addon.pitchShift(samples, rate, input.pitch_semitones, n_fft)).toEqual(
        addon.pitchShift(samples, rate, input.pitch_semitones, n_fft, derived_hop_length),
      );
      // 0, which the facades send for an omitted hop.
      expect(addon.timeStretch(samples, rate, input.speed_ratio, n_fft, 0)).toEqual(
        addon.timeStretch(samples, rate, input.speed_ratio, n_fft, derived_hop_length),
      );
    },
  );

  it.each(absent_hop)(
    '$id: an explicit hop of 0 is refused by the facade and a negative one by the addon',
    ({ n_fft, hop_length }) => {
      expectRefusal(
        () => addon.timeStretch(samples, rate, input.speed_ratio, n_fft, -1),
        RangeError,
        /hopLength must be an integer in \[1, 2147483647\]/,
      );
      const options = { nFft: n_fft, hopLength: hop_length };
      expectRefusal(
        () => timeStretch({ samples, sampleRate: rate, speedRatio: input.speed_ratio, ...options }),
        RangeError,
        /hopLength/,
      );
      expectRefusal(
        () =>
          pitchShift({
            samples,
            sampleRate: rate,
            pitchSemitones: input.pitch_semitones,
            ...options,
          }),
        RangeError,
        /hopLength/,
      );
    },
  );
});

describe('streaming mastering chain prepare refusals come from the core', () => {
  const { input, accepted, refused } = corpus.streaming_mastering_chain_prepare;
  const channels = input.num_channels;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const chain = new StreamingMasteringChain({
      sampleRate: sample_rate,
      numChannels: channels,
      maxBlockSize: max_block_size,
    });
    const raw = new addon.StreamingMasteringChain(null);
    try {
      expect(() => raw.prepare(sample_rate, max_block_size, channels)).not.toThrow();
    } finally {
      chain.destroy();
      raw.destroy();
    }
  });

  it.each(refused)('refuses $id', ({ sample_rate, max_block_size, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = literal(message as string);
    expectRefusal(
      () =>
        new StreamingMasteringChain({
          sampleRate: sample_rate,
          numChannels: channels,
          maxBlockSize: max_block_size,
        }),
      kind,
      text,
    );
    const raw = new addon.StreamingMasteringChain(null);
    try {
      expectRefusal(() => raw.prepare(sample_rate, max_block_size, channels), kind, text);
    } finally {
      raw.destroy();
    }
  });
});

describe('equalizer prepare refusals come from the core', () => {
  const { accepted, refused } = corpus.equalizer_prepare;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const eq = new StreamingEqualizer({ sampleRate: sample_rate, maxBlockSize: max_block_size });
    const raw = new addon.StreamingEqualizer({
      sampleRate: sample_rate,
      maxBlockSize: max_block_size,
    });
    eq.destroy();
    raw.destroy();
  });

  it.each(refused)('refuses $id', ({ sample_rate, max_block_size, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = literal(message as string);
    expectRefusal(
      () => new StreamingEqualizer({ sampleRate: sample_rate, maxBlockSize: max_block_size }),
      kind,
      text,
    );
    expectRefusal(
      () => new addon.StreamingEqualizer({ sampleRate: sample_rate, maxBlockSize: max_block_size }),
      kind,
      text,
    );
  });
});

describe('streaming retune prepare refusals come from the core', () => {
  const { accepted, refused } = corpus.streaming_retune_prepare;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const retune = new StreamingRetune();
    const raw = new addon.StreamingRetune({});
    try {
      expect(() => retune.prepare(sample_rate, max_block_size)).not.toThrow();
      expect(() => raw.prepare(sample_rate, max_block_size)).not.toThrow();
    } finally {
      retune.destroy();
      raw.destroy();
    }
  });

  it.each(refused)('refuses $id', ({ sample_rate, max_block_size, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = literal(message as string);
    const retune = new StreamingRetune();
    const raw = new addon.StreamingRetune({});
    try {
      expectRefusal(() => retune.prepare(sample_rate, max_block_size), kind, text);
      expectRefusal(() => raw.prepare(sample_rate, max_block_size), kind, text);
    } finally {
      retune.destroy();
      raw.destroy();
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
    expectRefusal(
      () => new StreamingRetune(controls),
      kind,
      literal(`StreamingRetune: ${message}`),
    );
    const retune = new StreamingRetune({ pitchSemitones: 3, mix: 0.5 });
    try {
      const before = retune.config();
      expectRefusal(
        () => retune.setConfig(controls),
        kind,
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
    expect(addon.resample(samples, item.src_sr, item.target_sr).length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', (item) => {
    const kind = errorClassFor[item.cause as Cause];
    const text = publicText(item.message as string);
    const samples = samplesFor(item);
    expectRefusal(() => resample(samples, item.src_sr, item.target_sr), kind, text);
    expectRefusal(() => addon.resample(samples, item.src_sr, item.target_sr), kind, text);
  });
});

describe('resample takes no validate option', () => {
  it('refuses validate as an unknown request key', () => {
    const samples = new Float32Array(480).fill(0.25);
    expectRefusal(
      () =>
        resample({ samples, sampleRate: 48000, targetSampleRate: 16000, validate: false } as never),
      TypeError,
      /validate/,
    );
  });
});

describe('zeroCrossings refuses a non-finite sample in the core', () => {
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

  it.each(refused)('refuses $id in the core', (item) => {
    const kind = errorClassFor[item.cause as Cause];
    // The addon reaches the C entry, whose text the public name replaces.
    const message = (item.c_message ?? item.message) as string;
    const text = literal(message.replace(/^zero_crossings: /, 'zeroCrossings: '));
    const samples = samplesFor(item);
    expectRefusal(() => addon.zeroCrossings(samples, 1e-10, false, true, true), kind, text);
  });

  it.each(refused)('refuses $id in the facade scan', (item) => {
    const samples = samplesFor(item);
    expectRefusal(() => zeroCrossings(samples, 22050), RangeError, /zeroCrossings: .*index/);
  });

  it('refuses a validate key', () => {
    const samples = samplesFor(accepted[0]);
    expectRefusal(
      () => zeroCrossings(samples, 22050, { validate: false } as never),
      TypeError,
      /zeroCrossings: unknown key validate \(expected /,
    );
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

  it.each(accepted)('accepts $id asynchronously', async ({ overrides }) => {
    expect((await masterAudioAsync({ samples, sampleRate, overrides })).samples).toEqual(samples);
    expect((await masterAudioAsync(samples, sampleRate, { overrides })).samples).toEqual(samples);
    const stereo = await masterAudioStereoAsync({
      left: samples,
      right: samples,
      sampleRate,
      overrides,
    });
    expect(stereo.left).toEqual(samples);
    expect(stereo.right).toEqual(samples);
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
      expectRefusal(call, kind, text);
    }
  });

  const asyncForms: Array<[string, () => Promise<unknown>]> = [
    ['masterAudioAsync', () => masterAudioAsync({ samples, sampleRate } as never)],
    ['masterAudioAsync', () => masterAudioAsync(samples, sampleRate, {})],
    [
      'masterAudioStereoAsync',
      () => masterAudioStereoAsync({ left: samples, right: samples, sampleRate }),
    ],
  ];

  it.each(refused)('rejects $id in every asynchronous form', async (item) => {
    const kind = errorClassFor[item.cause as Cause];
    for (const [name, call] of asyncForms) {
      const text = new RegExp(`^${literal(`${name}: ${item.message as string}`).source}$`);
      const error = await call().then(
        () => undefined,
        (reason: unknown) => reason,
      );
      expect(error, name).toBeInstanceOf(kind);
      expect((error as Error).message, name).toMatch(text);
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
      project.setMixerSceneJson(JSON.stringify(scene));
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
  it.each(refused)('refuses $id through $loader', ({ loader, scene, cause, code, message }) => {
    // A document's content is refused by the core's reader, never re-labelled as an argument.
    expect(cause).toBe('format');
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
