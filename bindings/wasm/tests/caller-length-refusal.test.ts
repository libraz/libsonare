/**
 * A length the caller supplies is read through the shared array-like reader, so a
 * value that is not an array is refused by name instead of reading as zero items.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ClipPageProvider, init, Mixer, mixingScenePreset } from '../dist/index.js';
import { expectRefusalOf } from './_helpers';

const SR = 48000;
const BLOCK = 128;

beforeAll(async () => {
  await init();
});

describe('Mixer.processStereoInto', () => {
  const mixer = () =>
    Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
      sampleRate: SR,
      maxBlockSize: BLOCK,
    });

  it('refuses input lists that are not arrays instead of mixing nothing', () => {
    const outL = new Float32Array(BLOCK);
    const outR = new Float32Array(BLOCK);
    const notAList = {} as unknown as Float32Array[];
    expect(
      expectRefusalOf(() => mixer().processStereoInto(notAList, notAList, outL, outR), TypeError)
        .message,
    ).toBe('Mixer.processStereoInto: leftInputs and rightInputs length must be a number');
  });

  it('refuses an input channel that is not an array by name', () => {
    const outL = new Float32Array(BLOCK);
    const outR = new Float32Array(BLOCK);
    const notAChannel = [{}] as unknown as Float32Array[];
    expect(
      expectRefusalOf(
        () => mixer().processStereoInto(notAChannel, [new Float32Array(BLOCK)], outL, outR),
        TypeError,
      ).message,
    ).toBe('Mixer.processStereoInto: leftInputs element length must be a number');
  });
});

describe('ClipPageProvider.supply', () => {
  it('refuses a channel list that is not an array instead of counting it as empty', () => {
    const provider = new ClipPageProvider({ numChannels: 1, numSamples: 8, pageFrames: 4 });
    expect(
      expectRefusalOf(() => provider.supply(0, {} as unknown as Float32Array[]), TypeError).message,
    ).toBe('ClipPageProvider.supply: channels length must be a number');
  });
});
