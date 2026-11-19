/**
 * A stereo entry refuses channels of unequal length by naming both fields it
 * compared, in the caller's spelling.
 */

import { describe, expect, it } from 'vitest';
import {
  masterAudioStereo,
  masterAudioStereoAsync,
  masteringAbMatchLoudnessStereo,
  masteringApplyProcessorStereo,
  streamingLoudnessGainStereo,
} from '../src/index.js';

const SR = 48000;
const long = new Float32Array(4800).fill(0.1);
const short = new Float32Array(2400).fill(0.1);

describe('stereo channel length refusals', () => {
  it.each([
    [
      'masterAudioStereo',
      () => masterAudioStereo({ left: long, right: short, sampleRate: SR, preset: 'pop' }),
    ],
    [
      'streamingLoudnessGainStereo',
      () => streamingLoudnessGainStereo({ left: long, right: short, sampleRate: SR }),
    ],
    [
      'masteringApplyProcessorStereo',
      () =>
        masteringApplyProcessorStereo({
          processor: 'dynamics.compressor',
          left: long,
          right: short,
          sampleRate: SR,
        }),
    ],
  ])('%s names left and right', (entry, call) => {
    expect(call).toThrow(new RangeError(`${entry}: left and right must have the same length`));
  });

  it('masterAudioStereoAsync rejects with the same text', async () => {
    await expect(
      masterAudioStereoAsync({ left: long, right: short, sampleRate: SR, preset: 'pop' }),
    ).rejects.toThrow(
      new RangeError('masterAudioStereoAsync: left and right must have the same length'),
    );
  });

  it('masteringAbMatchLoudnessStereo names the source pair and the reference pair', () => {
    expect(() =>
      masteringAbMatchLoudnessStereo({
        sourceLeft: long,
        sourceRight: short,
        referenceLeft: long,
        referenceRight: long,
        sampleRate: SR,
      }),
    ).toThrow(
      new RangeError(
        'masteringAbMatchLoudnessStereo: sourceLeft and sourceRight must have the same length',
      ),
    );
    expect(() =>
      masteringAbMatchLoudnessStereo({
        sourceLeft: long,
        sourceRight: long,
        referenceLeft: long,
        referenceRight: short,
        sampleRate: SR,
      }),
    ).toThrow(
      new RangeError(
        'masteringAbMatchLoudnessStereo: referenceLeft and referenceRight must have the same length',
      ),
    );
  });
});
