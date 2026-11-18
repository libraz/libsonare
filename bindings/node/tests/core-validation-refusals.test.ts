import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  melToAudio,
  mfccToAudio,
  mfccToMel,
  pitchShift,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  timeStretch,
} from '../src/index.js';
import { addon } from '../src/native.js';
import { expectRefusal } from './_helpers.js';

// The core function owns each refusal in this corpus. The addon calls the core
// directly, so the refusal is asserted on the raw addon as well as on the facade.

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

const errorClassFor: Record<Cause, ErrorConstructor> = { argument: RangeError };
const literal = (text: string) => new RegExp(text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));

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
    const text = new RegExp(message as string);
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
    const text = new RegExp(message as string);
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
    const text = new RegExp(message as string);
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

  it.each(refused)('refuses $id', ({ n_fft, hop_length, cause, message }) => {
    const kind = errorClassFor[cause as Cause];
    const text = new RegExp(message as string);
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

describe('streaming mastering chain prepare refusals come from the core', () => {
  const { input, accepted, refused } = corpus.streaming_mastering_chain_prepare;
  const channels = input.num_channels;

  it.each(accepted)('accepts $id', ({ sample_rate, max_block_size }) => {
    const chain = new StreamingMasteringChain({
      sampleRate: sample_rate,
      numChannels: channels,
      maxBlockSize: max_block_size,
    });
    const raw = new addon.StreamingMasteringChain({});
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
    const raw = new addon.StreamingMasteringChain({});
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
