/**
 * A stereo entry refuses channels of unequal length by naming both fields it
 * compared, in the caller's spelling.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masterAudioStereo,
  masteringAbMatchLoudnessStereo,
  masteringApplyProcessorStereo,
  normalizeStereo,
  StreamingEqualizer,
  StreamingMasteringChain,
  streamingLoudnessGainStereo,
} from '../src/index';

const SR = 48000;
const long = new Float32Array(4800).fill(0.1);
const short = new Float32Array(2400).fill(0.1);

beforeAll(async () => {
  await init();
});

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
    ['normalizeStereo', () => normalizeStereo({ left: long, right: short, sampleRate: SR })],
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

  it('the streaming processors name left and right on every stereo method', () => {
    const block = new Float32Array(128).fill(0.1);
    const half = new Float32Array(64).fill(0.1);
    const eq = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 128 });
    const chain = new StreamingMasteringChain({
      sampleRate: SR,
      numChannels: 2,
      maxBlockSize: 128,
    });
    try {
      expect(() => eq.processStereo(block, half)).toThrow(
        new RangeError(
          'StreamingEqualizer.processStereo: left and right must have the same length',
        ),
      );
      expect(() => eq.setSidechainStereo(block, half)).toThrow(
        new RangeError(
          'StreamingEqualizer.setSidechainStereo: left and right must have the same length',
        ),
      );
      expect(() => chain.processStereo(block, half)).toThrow(
        new RangeError(
          'StreamingMasteringChain.processStereo: left and right must have the same length',
        ),
      );
    } finally {
      eq.destroy();
      chain.destroy();
    }
  });
});
