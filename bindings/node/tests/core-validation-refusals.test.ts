import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { mfccToAudio, mfccToMel, StreamingRetune } from '../src/index.js';
import { addon } from '../src/native.js';
import { expectRefusal } from './_helpers.js';

// The core function owns each refusal in this corpus. The addon calls the core
// directly, so the refusal is asserted on the raw addon as well as on the facade.

type Cause = 'argument';
type MfccInput = { n_mfcc: number; n_frames: number; fill: number };
type MfccCase = { id: string; n_mels: number; cause?: Cause; message?: string };
type MfccRoute = { input: MfccInput; accepted: MfccCase[]; refused: MfccCase[] };
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
) as { mfcc_to_mel: MfccRoute; streaming_retune_prepare: RetuneRoute };

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
