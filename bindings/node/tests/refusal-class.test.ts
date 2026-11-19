import { describe, expect, it } from 'vitest';
import {
  hzToNote,
  isSonareError,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  noteToHz,
} from '../src/index.js';

/**
 * The failure class follows the cause: a refused caller argument is a plain
 * RangeError when its value is out of domain and a TypeError when its shape is
 * wrong (an undeclared key), whichever layer refused it, and never a coded SonareError.
 */
const SR = 22050;
const capture = (run: () => unknown): unknown => {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
};

function expectRefusal(
  caught: unknown,
  kind: typeof RangeError | typeof TypeError = RangeError,
): void {
  expect(caught).toBeInstanceOf(kind);
  expect(isSonareError(caught)).toBe(false);
  expect((caught as { code?: unknown }).code).toBeUndefined();
}

describe('a refused argument is a RangeError or a TypeError, never a SonareError', () => {
  it('hzToNote refuses a negative frequency', () => {
    const caught = capture(() => hzToNote(-1));
    expectRefusal(caught);
    expect((caught as Error).message).toMatch(/hz/);
  });

  it('noteToHz refuses a malformed name', () => {
    const caught = capture(() => noteToHz('H4'));
    expectRefusal(caught);
    expect((caught as Error).message).toMatch(/H4/);
  });

  it('a one-shot named processor refuses a key it never read', () => {
    const samples = new Float32Array(SR / 2).fill(0.1);
    const mono = capture(() =>
      masteringApplyProcessor({
        processor: 'dynamics.compressor',
        samples,
        sampleRate: SR,
        params: { notAParameter: 1 } as never,
      }),
    );
    expectRefusal(mono, TypeError);
    expect((mono as Error).message).toMatch(/notAParameter/);
    const stereo = capture(() =>
      masteringApplyProcessorStereo({
        processor: 'dynamics.compressor',
        left: samples,
        right: samples,
        sampleRate: SR,
        params: { notAParameter: 1 } as never,
      }),
    );
    expectRefusal(stereo, TypeError);
    expect((stereo as Error).message).toMatch(/notAParameter/);
  });
});
