import { beforeAll, describe, expect, it } from 'vitest';
import { init, spectralEdit } from '../src/index';
import { sine } from './_helpers';

describe('spectralEdit nFft', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([6, 1000, 2])('refuses %i naming nFft and the power-of-two rule', (nFft) => {
    const samples = sine(440, 0.1, { sampleRate: 22050 });
    expect(() => spectralEdit(samples, 22050, [{ mode: 'mute' }], { nFft })).toThrow(
      /nFft must be a power of two/,
    );
  });
});
