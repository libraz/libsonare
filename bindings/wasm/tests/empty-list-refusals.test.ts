/**
 * A list option is either omitted, which selects the default set, or names at least one
 * member: an empty list is refused by name rather than read as the default. The Node
 * package carries the same cases in its own copy of this file.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  detectKey,
  detectKeyCandidates,
  init,
  masteringStreamingPreview,
  masteringStreamingPreviewStereo,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const SR = 22050;

function tone(seconds = 0.5): Float32Array {
  const out = new Float32Array(Math.floor(SR * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * 261.6 * i) / SR);
  }
  return out;
}

const PLATFORM = { name: 'Target', targetLufs: -14, ceilingDbtp: -1 };

describe('key detection modes', () => {
  it('refuses an empty modes list on the request and positional forms', () => {
    const samples = tone();
    expect(() => detectKey({ samples, sampleRate: SR, modes: [] })).toThrow(
      new RangeError('detectKey: modes must not be empty; omit it for the default mode set'),
    );
    expect(() => detectKey(samples, SR, { modes: [] })).toThrow(
      new RangeError('detectKey: modes must not be empty; omit it for the default mode set'),
    );
    expect(() => detectKeyCandidates({ samples, sampleRate: SR, modes: [] })).toThrow(
      new RangeError(
        'detectKeyCandidates: modes must not be empty; omit it for the default mode set',
      ),
    );
    expect(() => detectKeyCandidates(samples, SR, { modes: [] })).toThrow(
      new RangeError(
        'detectKeyCandidates: modes must not be empty; omit it for the default mode set',
      ),
    );
  });

  it('reads an omitted and a null modes list as the default set', () => {
    const samples = tone();
    const omitted = detectKey({ samples, sampleRate: SR });
    expect(detectKey({ samples, sampleRate: SR, modes: null as never })).toEqual(omitted);
    expect(detectKey({ samples, sampleRate: SR, modes: ['major', 'minor'] }).mode).toMatch(
      /major|minor/,
    );
  });
});

describe('streaming preview platforms', () => {
  it('refuses an empty platforms list on the mono and stereo entries', () => {
    const samples = tone();
    expect(() => masteringStreamingPreview({ samples, sampleRate: SR, platforms: [] })).toThrow(
      new RangeError(
        'masteringStreamingPreview: platforms must not be empty; omit it for the built-in set',
      ),
    );
    expect(() => masteringStreamingPreview(samples, SR, { platforms: [] })).toThrow(
      new RangeError(
        'masteringStreamingPreview: platforms must not be empty; omit it for the built-in set',
      ),
    );
    expect(() =>
      masteringStreamingPreviewStereo({
        left: samples,
        right: samples,
        sampleRate: SR,
        platforms: [],
      }),
    ).toThrow(
      new RangeError(
        'masteringStreamingPreviewStereo: platforms must not be empty; omit it for the built-in set',
      ),
    );
  });

  it('refuses an entry that is not an object or lacks a typed field', () => {
    const samples = tone();
    const preview = (platforms: unknown) => () =>
      masteringStreamingPreview({ samples, sampleRate: SR, platforms: platforms as never });
    expect(preview(['spotify'])).toThrow(
      new TypeError('masteringStreamingPreview: platforms[0] must be an object'),
    );
    expect(preview([PLATFORM, null])).toThrow(
      new TypeError('masteringStreamingPreview: platforms[1] must be an object'),
    );
    expect(preview([{ targetLufs: -14, ceilingDbtp: -1 }])).toThrow(
      new TypeError('masteringStreamingPreview: platforms[0].name must be a string'),
    );
    expect(preview([{ name: 'Target', ceilingDbtp: -1 }])).toThrow(
      new TypeError('masteringStreamingPreview: platforms[0].targetLufs must be a number'),
    );
    expect(preview([{ name: 'Target', targetLufs: -14 }])).toThrow(
      new TypeError('masteringStreamingPreview: platforms[0].ceilingDbtp must be a number'),
    );
  });

  it('previews exactly the given platforms, and the built-in set when omitted', () => {
    const samples = tone();
    const given = masteringStreamingPreview({ samples, sampleRate: SR, platforms: [PLATFORM] });
    expect(given.platforms.map((p) => p.name)).toEqual(['Target']);
    const omitted = masteringStreamingPreview({ samples, sampleRate: SR });
    expect(omitted.platforms.length).toBeGreaterThan(1);
    expect(
      masteringStreamingPreview({ samples, sampleRate: SR, platforms: null as never }),
    ).toEqual(omitted);
  });
});
