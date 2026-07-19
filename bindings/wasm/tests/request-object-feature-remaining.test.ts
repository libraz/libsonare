import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeMelody,
  analyzeSections,
  cyclicTempogram,
  deemphasis,
  ebur128LoudnessRange,
  estimateTuning,
  fixFrames,
  fixLength,
  hpssWithResidual,
  init,
  lufs,
  lufsInterleaved,
  momentaryLufs,
  nnFilter,
  nnlsChroma,
  padCenter,
  pcen,
  peakPick,
  phaseVocoder,
  pitchTuning,
  plp,
  polyFeatures,
  preemphasis,
  remix,
  shortTermLufs,
  spectralContrast,
  tempogram,
  tonnetz,
  trim,
  vectorNormalize,
  zeroCrossings,
} from '../src/index';

const sampleRate = 22050;
const samples = Float32Array.from({ length: 8192 }, (_, i) =>
  Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => {
  await init();
});

describe('remaining feature request-object compatibility', () => {
  it('preserves positional spectral and reconstruction forms', () => {
    expect(spectralContrast({ samples, sampleRate, nFft: 512, hopLength: 128 })).toEqual(
      spectralContrast(samples, sampleRate, 512, 128),
    );
    expect(polyFeatures({ samples, sampleRate, nFft: 512, hopLength: 128, order: 2 })).toEqual(
      polyFeatures(samples, sampleRate, 512, 128, 2),
    );
    expect(zeroCrossings({ samples, threshold: 1e-5 })).toEqual(zeroCrossings(samples, 1e-5));
    expect(estimateTuning({ samples, sampleRate, nFft: 512, hopLength: 128 })).toBe(
      estimateTuning(samples, sampleRate, 512, 128),
    );
    const freqs = Float32Array.from([220, 440, 660]);
    expect(pitchTuning({ frequencies: freqs })).toBe(pitchTuning(freqs));
    expect(nnFilter({ s: new Float32Array(12).fill(1), nFeatures: 3, nFrames: 4 })).toEqual(
      nnFilter(new Float32Array(12).fill(1), 3, 4),
    );
    expect(phaseVocoder({ samples, rate: 1.1, sampleRate, nFft: 512, hopLength: 128 })).toEqual(
      phaseVocoder(samples, 1.1, sampleRate, 512, 128),
    );
    expect(hpssWithResidual({ samples, sampleRate })).toEqual(
      hpssWithResidual(samples, sampleRate),
    );
    expect(remix({ samples, intervals: new Int32Array([0, 100]) })).toEqual(
      remix(samples, new Int32Array([0, 100])),
    );
  });

  it('preserves request forms for music, loudness, and core rhythm', () => {
    expect(analyzeSections({ samples, sampleRate })).toEqual(analyzeSections(samples, sampleRate));
    expect(analyzeMelody({ samples, sampleRate })).toEqual(analyzeMelody(samples, sampleRate));
    expect(lufs({ samples, sampleRate })).toEqual(lufs(samples, sampleRate));
    expect(momentaryLufs({ samples, sampleRate })).toEqual(momentaryLufs(samples, sampleRate));
    expect(shortTermLufs({ samples, sampleRate })).toEqual(shortTermLufs(samples, sampleRate));
    expect(lufsInterleaved({ samples, channels: 1, sampleRate })).toEqual(
      lufsInterleaved(samples, 1, sampleRate),
    );
    const onset = new Float32Array(512).fill(0.2);
    expect(tempogram({ onsetEnvelope: onset, sampleRate, hopLength: 128, winLength: 64 })).toEqual(
      tempogram(onset, sampleRate, 128, 64),
    );
    expect(
      cyclicTempogram({ onsetEnvelope: onset, sampleRate, hopLength: 128, winLength: 64 }),
    ).toEqual(cyclicTempogram(onset, sampleRate, 128, 64));
    expect(plp({ onsetEnvelope: onset, sampleRate, hopLength: 128, winLength: 64 })).toEqual(
      plp(onset, sampleRate, 128, 30, 300, 64),
    );
    expect(pcen({ values: new Float32Array(12).fill(1), nBins: 3, nFrames: 4 })).toEqual(
      pcen(new Float32Array(12).fill(1), 3, 4),
    );
    expect(trim({ samples, sampleRate })).toEqual(trim(samples, sampleRate));
  });

  it('preserves request forms for remaining feature utility operations', () => {
    const emphasisInput = new Float32Array([1, 1, 1]);
    expect(preemphasis({ samples: emphasisInput, coef: 0.5, zi: 0 })).toEqual(
      preemphasis(emphasisInput, 0.5, 0),
    );
    const emphasized = preemphasis(emphasisInput, 0.5, 0);
    expect(deemphasis({ samples: emphasized, coef: 0.5, zi: 0 })).toEqual(
      deemphasis(emphasized, 0.5, 0),
    );
    const vector = new Float32Array([1, 2]);
    expect(padCenter({ values: vector, targetSize: 4, padValue: -1 })).toEqual(
      padCenter(vector, 4, -1),
    );
    expect(fixLength({ values: vector, targetSize: 4, padValue: -1 })).toEqual(
      fixLength(vector, 4, -1),
    );
    const frames = new Int32Array([2, 4]);
    expect(fixFrames({ frames, xMin: 0, xMax: 5, pad: true })).toEqual(
      fixFrames(frames, 0, 5, true),
    );
    const peaks = new Float32Array([0, 1, 0, 2, 0]);
    expect(
      peakPick({ values: peaks, preMax: 1, postMax: 1, preAvg: 1, postAvg: 1, delta: 0, wait: 0 }),
    ).toEqual(peakPick(peaks, 1, 1, 1, 1, 0, 0));
    const normalized = new Float32Array([3, 4]);
    expect(vectorNormalize({ values: normalized, normType: 2, threshold: 1e-12 })).toEqual(
      vectorNormalize(normalized, 2, 1e-12),
    );
    const chroma = new Float32Array(24).fill(1);
    expect(tonnetz({ chromagram: chroma, nChroma: 12, nFrames: 2 })).toEqual(
      tonnetz(chroma, 12, 2),
    );
  });

  it('preserves request forms for NNLS chroma and EBU loudness range', () => {
    expect(nnlsChroma({ samples, sampleRate })).toEqual(nnlsChroma(samples, sampleRate));
    expect(ebur128LoudnessRange({ samples, sampleRate })).toBe(
      ebur128LoudnessRange(samples, sampleRate),
    );
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const posRate = captureThrow(() => phaseVocoder(samples, 0, sampleRate, 512, 128));
    const reqRate = captureThrow(() =>
      phaseVocoder({ samples, rate: 0, sampleRate, nFft: 512, hopLength: 128 }),
    );
    expect(posRate.threw).toBe(true);
    expect(reqRate.threw).toBe(true);
    expect(reqRate.message).toBe(posRate.message);

    const empty = new Float32Array(0);
    const posLufs = captureThrow(() => lufs(empty, sampleRate));
    const reqLufs = captureThrow(() => lufs({ samples: empty, sampleRate }));
    expect(posLufs.threw).toBe(true);
    expect(reqLufs.threw).toBe(true);
    expect(reqLufs.message).toBe(posLufs.message);
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
