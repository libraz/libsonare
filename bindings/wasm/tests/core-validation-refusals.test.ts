import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  melToAudio,
  mfccToAudio,
  mfccToMel,
  pitchShift,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  timeStretch,
} from '../src/index';
import { getSonareModule } from '../src/module_state';
import { expectRefusalOf } from './_helpers';

// The core function owns each refusal in this corpus. The embind wrappers call the
// core directly, so the refusal is asserted on the raw module as well as on the facade.

type Cause = 'argument';
type MfccInput = { n_mfcc: number; n_frames: number; fill: number };
type MfccCase = { id: string; n_mels: number; cause?: Cause; message?: string };
type MfccRoute = { input: MfccInput; accepted: MfccCase[]; refused: MfccCase[] };
type MelInput = { n_mels: number; n_frames: number; fill: number };
type MelCase = { id: string; n_iter: number; cause?: Cause; message?: string };
type MelRoute = { input: MelInput; accepted: MelCase[]; refused: MelCase[] };
type MfccAudioInput = { n_mfcc: number; n_frames: number; n_mels: number; fill: number };
type MfccAudioRoute = { input: MfccAudioInput; accepted: MelCase[]; refused: MelCase[] };
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
};
type StretchRoute = { input: StretchInput; accepted: StretchCase[]; refused: StretchCase[] };
type RateBlockCase = {
  id: string;
  sample_rate: number;
  max_block_size: number;
  cause?: Cause;
  message?: string;
};
type RateBlockRoute = {
  input: { num_channels: number };
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

const corpus = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/core_validation_refusals.json', import.meta.url),
    'utf8',
  ),
) as {
  mfcc_to_mel: MfccRoute;
  mel_to_audio: MelRoute;
  mfcc_to_audio: MfccAudioRoute;
  native_spectral_stretch: StretchRoute;
  streaming_mastering_chain_prepare: RateBlockRoute;
  equalizer_prepare: EqualizerRoute;
  streaming_retune_prepare: RetuneRoute;
};

const errorClassFor: Record<Cause, typeof RangeError> = { argument: RangeError };
const literal = (text: string) => new RegExp(text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));

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
  melToAudio(...args: unknown[]): Float32Array;
  timeStretchEx(...args: unknown[]): Float32Array;
  pitchShiftEx(...args: unknown[]): Float32Array;
  createStreamingMasteringChain(config: Record<string, unknown>): {
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
    const text = new RegExp(message as string);
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
    const text = new RegExp(message as string);
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
    const text = new RegExp(message as string);
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
    const viaStretch = raw().timeStretchEx(samples, rate, input.speed_ratio, n_fft, hop_length);
    const viaShift = raw().pitchShiftEx(samples, rate, input.pitch_semitones, n_fft, hop_length);
    expect(viaStretch.length).toBeGreaterThan(0);
    expect(viaShift.length).toBeGreaterThan(0);
  });

  it.each(refused)('refuses $id', ({ n_fft, hop_length, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = new RegExp(message as string);
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
      () => raw().timeStretchEx(samples, rate, input.speed_ratio, n_fft, hop_length),
      () => raw().pitchShiftEx(samples, rate, input.pitch_semitones, n_fft, hop_length),
    ];
    for (const call of rawCalls) {
      expect(expectRefusalOf(call, kind).message).toMatch(text);
    }
  });
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
    const handle = raw().createStreamingMasteringChain({ __flatParams: {} });
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
    const handle = raw().createStreamingMasteringChain({ __flatParams: {} });
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
