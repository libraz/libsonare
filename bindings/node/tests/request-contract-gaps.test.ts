import { describe, expect, it } from 'vitest';
import { noteSegments } from '../src/index.js';
import { unknownKeyText } from './_helpers.js';

describe('request contract gaps', () => {
  it('normalizes canonical flat noteSegments tuning fields without mutation', () => {
    const baseRequest = {
      f0Hz: new Float32Array([440, 440, 445, 445, 440, 440]),
      voicedProb: new Float32Array([1, 1, 1, 1, 1, 1]),
      frameRate: 100,
    };
    const flatRequest = Object.freeze({
      ...baseRequest,
      segmentationThresholdCents: 30,
      minNoteMs: 10,
      referenceHz: 442,
    });

    const flatResult = noteSegments(flatRequest);

    expect(flatResult.length).toBeGreaterThan(0);
    expect(flatRequest).not.toHaveProperty('config');
    expect(flatRequest).toEqual({
      ...baseRequest,
      segmentationThresholdCents: 30,
      minNoteMs: 10,
      referenceHz: 442,
    });
  });

  it('refuses a nested config object as an unknown request key', () => {
    expect(() =>
      noteSegments({
        f0Hz: new Float32Array([440, 440]),
        voicedProb: new Float32Array([1, 1]),
        frameRate: 100,
        minNoteMs: 10,
        config: { minNoteMs: 20 },
      } as never),
    ).toThrow(unknownKeyText('noteSegments: unknown key config'));
  });
});
