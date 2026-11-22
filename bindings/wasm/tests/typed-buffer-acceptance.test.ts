/**
 * What a realtime buffer argument accepts.
 *
 * Input planes and sample blocks take a Float32Array or a plain array; output buffers are
 * written with `set` and take only a Float32Array. Anything else is refused by name before the
 * call reads or writes a sample, so a refused call leaves the output as it was and runs no block.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  Mixer,
  mixingScenePreset,
  RealtimeVoiceChanger,
  waveformPeakPyramid,
} from '../dist/index.js';
import { expectRefusalOf } from './_helpers';

const SR = 48000;
const BLOCK = 128;
const SENTINEL = 7;

beforeAll(async () => {
  await init();
});

const signal = (): Float32Array =>
  Float32Array.from({ length: BLOCK }, (_, i) => 0.25 * Math.sin((2 * Math.PI * 440 * i) / SR));
const sentinel = (): Float32Array => new Float32Array(BLOCK).fill(SENTINEL);

/** Wrong buffer kinds a caller might pass in place of a Float32Array. */
const WRONG_KINDS: Record<string, () => unknown> = {
  Float64Array: () => new Float64Array(BLOCK),
  Int16Array: () => new Int16Array(BLOCK),
  arrayLike: () => ({ length: BLOCK }),
};

function mixer(): Mixer {
  return Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
    sampleRate: SR,
    maxBlockSize: BLOCK,
  });
}

function changer(): RealtimeVoiceChanger {
  return new RealtimeVoiceChanger({ sampleRate: SR, maxBlockSize: BLOCK, numChannels: 2 });
}

describe('Mixer.processStereoInto', () => {
  for (const [kind, make] of Object.entries(WRONG_KINDS)) {
    it(`refuses a ${kind} input plane and leaves the outputs untouched`, () => {
      const m = mixer();
      const outL = sentinel();
      const outR = sentinel();
      try {
        expect(
          expectRefusalOf(
            () => m.processStereoInto([make() as unknown as Float32Array], [signal()], outL, outR),
            TypeError,
          ).message,
        ).toBe('Mixer.processStereoInto: leftInputs[0] must be a Float32Array');
        expect(outL.every((v) => v === SENTINEL)).toBe(true);
        expect(outR.every((v) => v === SENTINEL)).toBe(true);
      } finally {
        m.destroy();
      }
    });
  }

  for (const [kind, make] of Object.entries({
    ...WRONG_KINDS,
    array: () => Array(BLOCK).fill(0),
  })) {
    it(`refuses a ${kind} output buffer by name`, () => {
      const m = mixer();
      try {
        expect(
          expectRefusalOf(
            () =>
              m.processStereoInto(
                [signal()],
                [signal()],
                make() as unknown as Float32Array,
                new Float32Array(BLOCK),
              ),
            TypeError,
          ).message,
        ).toBe('Mixer.processStereoInto: outLeft must be a Float32Array');
      } finally {
        m.destroy();
      }
    });
  }

  it('still accepts a Float32Array or a plain array as an input plane', () => {
    const m = mixer();
    try {
      for (const input of [signal(), Array.from(signal())]) {
        const outL = sentinel();
        m.processStereoInto(
          [input as unknown as Float32Array],
          [signal()],
          outL,
          new Float32Array(BLOCK),
        );
        expect(outL.some((v) => v !== SENTINEL)).toBe(true);
      }
    } finally {
      m.destroy();
    }
  });
});

describe('RealtimeVoiceChanger block calls', () => {
  for (const [kind, make] of Object.entries(WRONG_KINDS)) {
    it(`refuses ${kind} samples, writes nothing and runs no block`, () => {
      const refused = changer();
      const fresh = changer();
      try {
        const output = sentinel();
        expect(
          expectRefusalOf(
            () => refused.processMonoInto(make() as unknown as Float32Array, output),
            TypeError,
          ).message,
        ).toBe('RealtimeVoiceChanger.processMonoInto: samples must be a Float32Array');
        expect(output.every((v) => v === SENTINEL)).toBe(true);
        expect(
          expectRefusalOf(() => refused.processMono(make() as unknown as Float32Array), TypeError)
            .message,
        ).toBe('RealtimeVoiceChanger.processMono: samples must be a Float32Array');
        // A refused call leaves the changer's state where a fresh one starts.
        const input = signal();
        expect(Array.from(refused.processMono(input))).toEqual(
          Array.from(fresh.processMono(input)),
        );
      } finally {
        refused.destroy();
        fresh.destroy();
      }
    });

    it(`refuses a ${kind} carrier by name`, () => {
      const c = changer();
      try {
        const output = sentinel();
        expect(
          expectRefusalOf(
            () => c.processInterleavedInto(signal(), 1, output, make() as unknown as Float32Array),
            TypeError,
          ).message,
        ).toBe('RealtimeVoiceChanger.processInterleavedInto: carrier must be a Float32Array');
        expect(output.every((v) => v === SENTINEL)).toBe(true);
      } finally {
        c.destroy();
      }
    });
  }

  for (const [kind, make] of Object.entries({
    ...WRONG_KINDS,
    array: () => Array(BLOCK).fill(0),
  })) {
    it(`refuses a ${kind} output buffer by name`, () => {
      const c = changer();
      try {
        expect(
          expectRefusalOf(
            () => c.processMonoInto(signal(), make() as unknown as Float32Array),
            TypeError,
          ).message,
        ).toBe('RealtimeVoiceChanger.processMonoInto: output must be a Float32Array');
        expect(
          expectRefusalOf(
            () => c.processInterleavedInto(signal(), 1, make() as unknown as Float32Array),
            TypeError,
          ).message,
        ).toBe('RealtimeVoiceChanger.processInterleavedInto: output must be a Float32Array');
      } finally {
        c.destroy();
      }
    });
  }

  it('still accepts a Float32Array or a plain array as samples', () => {
    const c = changer();
    try {
      for (const input of [signal(), Array.from(signal())]) {
        expect(c.processMono(input as unknown as Float32Array)).toHaveLength(BLOCK);
      }
    } finally {
      c.destroy();
    }
  });
});

describe('a numeric list argument', () => {
  const channels = [new Float32Array(4096).fill(0.5)];

  it('accepts a plain array and every typed array kind', () => {
    for (const levels of [
      [512],
      new Int32Array([512]),
      new Uint16Array([512]),
      new Float64Array([512]),
    ]) {
      expect(
        waveformPeakPyramid({ channels, samplesPerBucketLevels: levels as number[] }),
      ).toHaveLength(1);
    }
  });

  it('refuses an object that only carries a length by name', () => {
    expect(
      expectRefusalOf(
        () =>
          waveformPeakPyramid({
            channels,
            samplesPerBucketLevels: { length: 1, 0: 512 } as unknown as number[],
          }),
        TypeError,
      ).message,
    ).toBe('waveformPeakPyramid: samplesPerBucketLevels must be an array');
  });
});
