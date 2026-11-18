import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { init, melToAudio, mfccToAudio, mfccToMel, StreamingRetune } from '../src/index';
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
