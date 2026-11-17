import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  chordFunctions,
  detectKey,
  detectKeyCandidates,
  init,
  scaleMaskForMode,
} from '../src/index';
import { sine } from './_helpers';

const SR = 22050;
const MODE_NAMES = ['major', 'minor', 'dorian', 'phrygian', 'lydian', 'mixolydian', 'locrian'];

beforeAll(async () => {
  await init();
});

describe('key modes are reported by name', () => {
  const samples = sine(440, 3, { sampleRate: SR });

  it('detectKey, detectKeyCandidates and analyze return the mode name', () => {
    expect(MODE_NAMES).toContain(detectKey(samples, SR).mode);
    const candidates = detectKeyCandidates({ samples, sampleRate: SR, modes: 'all' });
    expect(candidates.length).toBeGreaterThan(1);
    for (const candidate of candidates) {
      expect(MODE_NAMES).toContain(candidate.key.mode);
    }
    expect(MODE_NAMES).toContain(analyze(samples, SR).key.mode);
  });

  it('a reported key feeds the name-taking helpers unchanged', () => {
    const key = detectKey(samples, SR);
    expect(() => scaleMaskForMode(key.root, key.mode)).not.toThrow();
    expect(chordFunctions({ chords: [], key })).toEqual([]);
  });

  it('scaleMaskForMode refuses an ordinal', () => {
    for (const name of MODE_NAMES) {
      expect(scaleMaskForMode(0, name as 'major')).toBeGreaterThan(0);
    }
    expect(() => scaleMaskForMode(0, 0 as never)).toThrow(TypeError);
  });
});
