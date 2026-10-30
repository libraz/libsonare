/**
 * Request keys and option values the Node facade or addon must refuse by name
 * rather than accept and drop, and the error class each refusal carries.
 */

import { describe, expect, it } from 'vitest';
import {
  masterAudioAsync,
  masterAudioStereoAsync,
  masteringStreamingPreview,
  onsetEnvelope,
  onsetStrengthMulti,
  remix,
  remixAlignedIntervals,
} from '../src/index.js';
import { expectRefusal } from './_helpers.js';

const SR = 22050;

function tone(seconds = 0.5): Float32Array {
  const out = new Float32Array(Math.floor(SR * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  return out;
}

describe('remix intervals', () => {
  it('refuses an odd-length list as a RangeError on both entry points', () => {
    const samples = tone();
    expectRefusal(() => remix(samples, [0, 100, 200], SR), RangeError, /pairs/);
    expectRefusal(() => remix(samples, new Int32Array([0, 100, 200]), SR), RangeError, /pairs/);
    expectRefusal(
      () => remixAlignedIntervals({ samples, intervals: new Int32Array([0, 100, 200]) }),
      RangeError,
      /pairs/,
    );
  });
});

describe('masteringStreamingPreview platforms', () => {
  it('refuses a value that is neither absent nor an array', () => {
    const samples = tone();
    for (const platforms of [-1, 'x', {}]) {
      expectRefusal(
        () => masteringStreamingPreview({ samples, sampleRate: SR, platforms: platforms as never }),
        TypeError,
        /platforms/,
      );
    }
    expect(() => masteringStreamingPreview({ samples, sampleRate: SR })).not.toThrow();
  });
});

describe('request keys that the entry point does not honour', () => {
  it('rejects cancel and onProgress on the async mastering entries', async () => {
    const samples = tone(0.2);
    for (const key of ['cancel', 'onProgress']) {
      const extra = { [key]: () => true } as Record<string, unknown>;
      await expect(
        masterAudioAsync({ samples, sampleRate: 48000, ...extra }),
      ).rejects.toBeInstanceOf(TypeError);
      await expect(
        masterAudioStereoAsync({ left: samples, right: samples, sampleRate: 48000, ...extra }),
      ).rejects.toBeInstanceOf(TypeError);
    }
  });

  it('rejects fmin, fmax and htk on the onset requests', () => {
    const samples = tone();
    for (const key of ['fmin', 'fmax', 'htk']) {
      const extra = { [key]: 1 } as Record<string, unknown>;
      expectRefusal(
        () => onsetEnvelope({ samples, sampleRate: SR, ...extra }),
        TypeError,
        new RegExp(key),
      );
      expectRefusal(
        () => onsetStrengthMulti({ samples, sampleRate: SR, ...extra }),
        TypeError,
        new RegExp(key),
      );
    }
  });
});
