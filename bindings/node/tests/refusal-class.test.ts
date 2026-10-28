import { describe, expect, it } from 'vitest';
import {
  hzToNote,
  isSonareError,
  masteringProcess,
  masteringProcessStereo,
  noteToHz,
} from '../src/index.js';

/**
 * The failure class follows the cause: every refused caller argument is a plain
 * RangeError, whichever layer refused it, and never a coded SonareError.
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

function expectRefusal(caught: unknown): void {
  expect(caught).toBeInstanceOf(RangeError);
  expect(isSonareError(caught)).toBe(false);
  expect((caught as { code?: unknown }).code).toBeUndefined();
}

describe('a refused argument is a RangeError', () => {
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
      masteringProcess('dynamics.compressor', samples, SR, { notAParameter: 1 } as never),
    );
    expectRefusal(mono);
    expect((mono as Error).message).toMatch(/notAParameter/);
    const stereo = capture(() =>
      masteringProcessStereo('dynamics.compressor', samples, samples, SR, {
        notAParameter: 1,
      } as never),
    );
    expectRefusal(stereo);
    expect((stereo as Error).message).toMatch(/notAParameter/);
  });
});
