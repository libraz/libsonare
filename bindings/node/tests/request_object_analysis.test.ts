import { describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeAsync,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeMelody,
  analyzeRhythm,
  analyzeSections,
  analyzeTimbre,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  detectAcoustic,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  estimateRoom,
  roomMorph,
} from '../src/index.js';

const sampleRate = 22050;
const samples = new Float32Array(sampleRate / 2);

describe('analysis request-object compatibility', () => {
  it('preserves basic analysis calls', () => {
    expect(detectBpm({ samples, sampleRate })).toEqual(detectBpm(samples, sampleRate));
    expect(detectBeats({ samples, sampleRate })).toEqual(detectBeats(samples, sampleRate));
    expect(detectDownbeats({ samples, sampleRate })).toEqual(detectDownbeats(samples, sampleRate));
    expect(detectOnsets({ samples, sampleRate })).toEqual(detectOnsets(samples, sampleRate));
    expect(analyze({ samples, sampleRate })).toEqual(analyze(samples, sampleRate));
  });

  it('preserves key options', () => {
    const options = { useHpss: true, highPassHz: 80 };
    expect(detectKey({ samples, sampleRate, ...options })).toEqual(
      detectKey(samples, sampleRate, options),
    );
    expect(detectKeyCandidates({ samples, sampleRate, ...options })).toEqual(
      detectKeyCandidates(samples, sampleRate, options),
    );
  });

  it('preserves async and progress calls', async () => {
    await expect(analyzeAsync({ samples, sampleRate })).resolves.toEqual(
      await analyzeAsync(samples, sampleRate),
    );
    let positionalCalls = 0;
    let requestCalls = 0;
    expect(
      analyzeWithProgress(samples, sampleRate, () => {
        positionalCalls++;
      }),
    ).toEqual(
      analyzeWithProgress({
        samples,
        sampleRate,
        onProgress: () => {
          requestCalls++;
        },
      }),
    );
    expect(requestCalls).toBe(positionalCalls);
  });

  it('preserves advanced analysis option calls', () => {
    expect(analyzeSections({ samples, sampleRate, minSectionSec: 1 })).toEqual(
      analyzeSections(samples, sampleRate, { minSectionSec: 1 }),
    );
    expect(analyzeMelody({ samples, sampleRate, fmin: 80, usePyin: false })).toEqual(
      analyzeMelody(samples, sampleRate, { fmin: 80, usePyin: false }),
    );
    expect(analyzeBpm({ samples, sampleRate, bpmMin: 60 })).toEqual(
      analyzeBpm(samples, sampleRate, { bpmMin: 60 }),
    );
    expect(analyzeRhythm({ samples, sampleRate, bpmMin: 70 })).toEqual(
      analyzeRhythm(samples, sampleRate, { bpmMin: 70 }),
    );
    expect(analyzeDynamics({ samples, sampleRate, windowSec: 0.2 })).toEqual(
      analyzeDynamics(samples, sampleRate, { windowSec: 0.2 }),
    );
    expect(analyzeTimbre({ samples, sampleRate, nMels: 32 })).toEqual(
      analyzeTimbre(samples, sampleRate, { nMels: 32 }),
    );
  });

  it('preserves acoustic and chord request calls', () => {
    expect(analyzeImpulseResponse({ samples, sampleRate, nOctaveBands: 4 })).toEqual(
      analyzeImpulseResponse(samples, sampleRate, 4),
    );
    expect(detectAcoustic({ samples, sampleRate, nOctaveBands: 4 })).toEqual(
      detectAcoustic(samples, sampleRate, { nOctaveBands: 4 }),
    );
    const chordOptions = { minDuration: 0.1, nFft: 512, hopLength: 128 };
    expect(detectChords({ samples, sampleRate, ...chordOptions })).toEqual(
      detectChords(samples, sampleRate, chordOptions),
    );
    expect(
      chordFunctionalAnalysis({ samples, sampleRate, keyRoot: 0, keyMode: 0, ...chordOptions }),
    ).toEqual(chordFunctionalAnalysis(samples, 0, 0, sampleRate, chordOptions));
  });

  it('preserves room request calls', () => {
    const estimateOptions = { referenceAbsorption: 0.2 };
    expect(estimateRoom({ samples, sampleRate, ...estimateOptions })).toEqual(
      estimateRoom(samples, sampleRate, estimateOptions),
    );
    const morphOptions = { targetRt60: 0.2, mix: 0.25 };
    expect(roomMorph({ samples, sampleRate, ...morphOptions })).toEqual(
      roomMorph(samples, sampleRate, morphOptions),
    );
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const pos = captureThrow(() => detectBpm(empty, sampleRate));
    const req = captureThrow(() => detectBpm({ samples: empty, sampleRate }));
    expect(pos.threw).toBe(true);
    expect(req.threw).toBe(true);
    expect(req.message).toBe(pos.message);

    const posT = captureThrow(() => analyzeTimbre(empty, sampleRate, { nMels: 32 }));
    const reqT = captureThrow(() => analyzeTimbre({ samples: empty, sampleRate, nMels: 32 }));
    expect(posT.threw).toBe(true);
    expect(reqT.threw).toBe(true);
    expect(reqT.message).toBe(posT.message);
  });
});

function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
  try {
    fn();
    return { threw: false, message: '' };
  } catch (error) {
    return { threw: true, message: error instanceof Error ? error.message : String(error) };
  }
}
