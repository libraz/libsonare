import { describe, expect, it } from 'vitest';
import { analyze, analyzeBpm } from '../src/index.js';

const sampleRate = 22050;
// Request-versus-positional equivalence for these functions lives in the
// table-driven request_object_equivalence.test.ts; this file keeps the
// request-shape behaviour that table cannot express.
const tone = new Float32Array(sampleRate * 2).map(
  (_, i) => 0.3 * Math.sin((2 * Math.PI * 440 * i) / sampleRate),
);

describe('analysis request-object compatibility', () => {
  it('refuses a beat-tracking prior on BPM-only analysis', () => {
    expect(() => analyzeBpm({ samples: tone, sampleRate, startBpm: 55 } as never)).toThrow(
      TypeError,
    );
    expect(() => analyzeBpm(tone, sampleRate, { startBpm: 55 } as never)).toThrow(TypeError);
  });

  it('accepts complete analyzer options', () => {
    const options = {
      bpmMin: 30,
      bpmMax: 90,
      startBpm: 55,
      useHpss: false,
      useChordHmm: true,
      detectChordInversions: true,
    };
    const result = analyze({ samples: tone, sampleRate, ...options });
    expect(result.bpm).toBeGreaterThanOrEqual(30);
    expect(result.bpm).toBeLessThanOrEqual(90);
    // The control: the bounds reached the tempo search rather than the input
    // landing inside them on its own.
    expect(analyze({ samples: tone, sampleRate, bpmMin: 190, bpmMax: 210 }).bpm).not.toBe(
      result.bpm,
    );
  });

  it('honours the analyzer options on a request that also carries onProgress', () => {
    const flatRequest = { samples: tone, sampleRate, bpmMin: 190, bpmMax: 210 };
    const viaAnalyze = analyze(flatRequest);
    const viaProgress = analyze({ ...flatRequest, onProgress: () => {} });
    expect(viaProgress.bpm).toBeGreaterThanOrEqual(190);
    expect(viaProgress.bpm).toBeLessThanOrEqual(210);
    expect(viaProgress.bpm).toBe(viaAnalyze.bpm);
    // The positive control: without the bounds actually reaching the reader,
    // this input's natural tempo does not land in [190, 210] on its own.
    const withoutBounds = analyze({ samples: tone, sampleRate, onProgress: () => {} });
    expect(withoutBounds.bpm).not.toBe(viaProgress.bpm);
  });

  it('refuses a nested `options` field beside the flattened ones', () => {
    const request = {
      samples: tone,
      sampleRate,
      bpmMin: 190,
      bpmMax: 210,
      onProgress: () => {},
      options: { bpmMin: 80, bpmMax: 90 },
    };
    expect(() => analyze(request as never)).toThrow(/options/);
  });
});
