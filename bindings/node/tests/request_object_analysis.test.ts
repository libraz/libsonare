import { describe, expect, it } from 'vitest';
import { analyze, analyzeWithProgress } from '../src/index.js';

const sampleRate = 22050;
// Request-versus-positional equivalence for these functions lives in the
// table-driven request_object_equivalence.test.ts; this file keeps the
// request-shape behaviour that table cannot express.
const tone = new Float32Array(sampleRate * 2).map(
  (_, i) => 0.3 * Math.sin((2 * Math.PI * 440 * i) / sampleRate),
);

describe('analysis request-object compatibility', () => {
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

  it('honours MusicAnalyzeOptions flattened onto an analyzeWithProgress request', () => {
    // A request built in analyze/analyzeAsync's flattened MusicAnalyzeRequest
    // shape (bpmMin/bpmMax as top-level fields, not nested under `options`) is
    // structurally assignable to AnalyzeWithProgressRequest too -- every field
    // it carries is optional there -- so a caller reusing one across both used
    // to have the flattened bounds silently dropped by analyzeWithProgress,
    // which only ever read request.options.
    const flatRequest = { samples: tone, sampleRate, bpmMin: 190, bpmMax: 210 };
    const viaAnalyze = analyze(flatRequest);
    const viaProgress = analyzeWithProgress({ ...flatRequest, onProgress: () => {} });
    expect(viaProgress.bpm).toBeGreaterThanOrEqual(190);
    expect(viaProgress.bpm).toBeLessThanOrEqual(210);
    expect(viaProgress.bpm).toBe(viaAnalyze.bpm);
    // The positive control: without the bounds actually reaching the reader,
    // this input's natural tempo does not land in [190, 210] on its own.
    const withoutBounds = analyzeWithProgress({ samples: tone, sampleRate, onProgress: () => {} });
    expect(withoutBounds.bpm).not.toBe(viaProgress.bpm);
  });

  it('lets a nested `options` field win over the same field flattened onto the request', () => {
    const request = {
      samples: tone,
      sampleRate,
      bpmMin: 190, // flattened -- must lose
      bpmMax: 210, // flattened -- must lose
      onProgress: () => {},
      options: { bpmMin: 80, bpmMax: 90 }, // nested -- must win
    };
    const result = analyzeWithProgress(request);
    expect(result.bpm).toBeGreaterThanOrEqual(80);
    expect(result.bpm).toBeLessThanOrEqual(90);
  });
});
