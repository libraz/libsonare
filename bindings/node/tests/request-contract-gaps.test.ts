import { describe, expect, it } from 'vitest';
import { noteSegments } from '../src/index.js';

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
    const nestedConfig = Object.freeze({
      segmentationThresholdCents: 30,
      minNoteMs: 10,
      referenceHz: 442,
    });

    const flatResult = noteSegments(flatRequest);
    const nestedResult = noteSegments(Object.freeze({ ...baseRequest, config: nestedConfig }));

    expect(flatResult).toEqual(nestedResult);
    expect(flatRequest).not.toHaveProperty('config');
    expect(nestedConfig).toEqual({
      segmentationThresholdCents: 30,
      minNoteMs: 10,
      referenceHz: 442,
    });
  });

  it('rejects duplicate flat and deprecated nested tuning options', () => {
    expect(() =>
      noteSegments({
        f0Hz: new Float32Array([440, 440]),
        voicedProb: new Float32Array([1, 1]),
        frameRate: 100,
        minNoteMs: 10,
        config: { minNoteMs: 20 },
      }),
    ).toThrow(RangeError);
  });
});
