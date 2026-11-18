import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeMelody,
  analyzeSections,
  cyclicTempogram,
  deemphasis,
  ebur128LoudnessRangeLu,
  estimateTuning,
  fixFrames,
  fixLength,
  hpssWithResidual,
  init,
  lufs,
  lufsMultichannel,
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
  it('matches the request and positional forms of spectral and reconstruction calls', () => {
    expect(spectralContrast({ samples, sampleRate, nFft: 512, hopLength: 128 })).toEqual(
      spectralContrast(samples, sampleRate, { nFft: 512, hopLength: 128 }),
    );
    expect(polyFeatures({ samples, sampleRate, nFft: 512, hopLength: 128, order: 2 })).toEqual(
      polyFeatures(samples, sampleRate, { nFft: 512, hopLength: 128, order: 2 }),
    );
    expect(zeroCrossings({ samples, sampleRate, threshold: 1e-5 })).toEqual(
      zeroCrossings(samples, sampleRate, { threshold: 1e-5 }),
    );
    expect(estimateTuning({ samples, sampleRate, nFft: 512, hopLength: 128 })).toBe(
      estimateTuning(samples, sampleRate, { nFft: 512, hopLength: 128 }),
    );
    const freqs = Float32Array.from([220, 440, 660]);
    const tuning = pitchTuning({ frequencies: freqs });
    expect(tuning).toBeGreaterThanOrEqual(-50);
    expect(tuning).toBeLessThan(50);
    expect(pitchTuning({ frequencies: freqs, resolution: 0.01, binsPerOctave: 12 })).toBe(tuning);
    const filtered = nnFilter({ s: { data: new Float32Array(12).fill(1), rows: 3, cols: 4 } });
    expect([filtered.rows, filtered.cols]).toEqual([3, 4]);
    expect(Array.from(filtered.data).every((v) => Math.abs(v - 1) < 1e-6)).toBe(true);
    expect(
      phaseVocoder({ samples, speedRatio: 1.1, sampleRate, nFft: 512, hopLength: 128 }),
    ).toEqual(phaseVocoder(samples, sampleRate, { speedRatio: 1.1, nFft: 512, hopLength: 128 }));
    expect(hpssWithResidual({ samples, sampleRate })).toEqual(
      hpssWithResidual(samples, sampleRate),
    );
    expect(remix({ samples, sampleRate, intervals: new Int32Array([0, 100]) })).toEqual(
      remix(samples, sampleRate, { intervals: new Int32Array([0, 100]) }),
    );
  });

  it('answers the request forms of music, loudness, and core rhythm', () => {
    expect(analyzeSections({ samples, sampleRate })).toEqual(analyzeSections(samples, sampleRate));
    expect(analyzeMelody({ samples, sampleRate })).toEqual(analyzeMelody(samples, sampleRate));
    expect(lufs({ samples, sampleRate })).toEqual(lufs(samples, sampleRate));
    expect(momentaryLufs({ samples, sampleRate })).toEqual(momentaryLufs(samples, sampleRate));
    expect(shortTermLufs({ samples, sampleRate })).toEqual(shortTermLufs(samples, sampleRate));
    expect(() =>
      lufsMultichannel({ channels: [samples], sampleRate, validate: true } as never),
    ).toThrow("lufsMultichannel: unknown request key 'validate'");
    const onset = new Float32Array(512).fill(0.2);
    const tempoGrid = tempogram({
      onsetEnvelope: onset,
      sampleRate,
      hopLength: 128,
      winLength: 64,
    });
    expect(tempoGrid.rows).toBe(64);
    expect(tempoGrid.data.length).toBe(tempoGrid.rows * tempoGrid.cols);
    const cyclic = cyclicTempogram({
      onsetEnvelope: onset,
      sampleRate,
      hopLength: 128,
      winLength: 64,
    });
    expect(cyclic.cols).toBe(tempoGrid.cols);
    expect(cyclic.data.length).toBe(cyclic.rows * cyclic.cols);
    // The defaults are the 60..200 BPM range.
    expect(plp({ onsetEnvelope: onset, sampleRate, hopLength: 128, winLength: 64 })).toEqual(
      plp({
        onsetEnvelope: onset,
        sampleRate,
        hopLength: 128,
        bpmMin: 60,
        bpmMax: 200,
        winLength: 64,
      }),
    );
    const normalized = pcen({ values: { data: new Float32Array(12).fill(1), rows: 3, cols: 4 } });
    expect([normalized.rows, normalized.cols]).toEqual([3, 4]);
    expect(trim({ samples, sampleRate })).toEqual(trim(samples, sampleRate));
  });

  it('computes the remaining feature utility operations from request objects', () => {
    const emphasisInput = new Float32Array([1, 1, 1]);
    const emphasized = preemphasis({ samples: emphasisInput, sampleRate, coef: 0.5, zi: 0 });
    expect(emphasized).toEqual(preemphasis(emphasisInput, sampleRate, { coef: 0.5, zi: 0 }));
    expect(Array.from(emphasized)).toEqual([1, 0.5, 0.5]);
    const restored = deemphasis({ samples: emphasized, sampleRate, coef: 0.5, zi: 0 });
    expect(restored).toEqual(deemphasis(emphasized, sampleRate, { coef: 0.5, zi: 0 }));
    expect(Array.from(restored)).toEqual([1, 1, 1]);
    const vector = new Float32Array([1, 2]);
    expect(Array.from(padCenter({ values: vector, targetSize: 4, padValue: -1 }))).toEqual([
      -1, 1, 2, -1,
    ]);
    expect(Array.from(fixLength({ values: vector, targetSize: 4, padValue: -1 }))).toEqual([
      1, 2, -1, -1,
    ]);
    const frames = new Int32Array([2, 4]);
    expect(Array.from(fixFrames({ frames, xMin: 0, xMax: 5, pad: true }))).toEqual([0, 2, 4, 5]);
    const peaks = new Float32Array([0, 1, 0, 2, 0]);
    expect(
      Array.from(
        peakPick({
          values: peaks,
          preMax: 1,
          postMax: 1,
          preAvg: 1,
          postAvg: 1,
          delta: 0,
          wait: 0,
        }),
      ),
    ).toEqual([0, 1, 3]);
    const normalized = new Float32Array([3, 4]);
    const unit = vectorNormalize({ values: normalized, norm: 'l2', threshold: 1e-12 });
    expect(unit[0]).toBeCloseTo(0.6, 6);
    expect(unit[1]).toBeCloseTo(0.8, 6);
    const chroma = { data: new Float32Array(24).fill(1), rows: 12, cols: 2 };
    const centroid = tonnetz({ chromagram: chroma });
    expect([centroid.rows, centroid.cols]).toEqual([6, 2]);
  });

  it('preserves request forms for NNLS chroma and EBU loudness range', () => {
    expect(nnlsChroma({ samples, sampleRate })).toEqual(nnlsChroma(samples, sampleRate));
    expect(ebur128LoudnessRangeLu({ samples, sampleRate })).toBe(
      ebur128LoudnessRangeLu(samples, sampleRate),
    );
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const posRate = captureThrow(() =>
      phaseVocoder(samples, sampleRate, { speedRatio: 0, nFft: 512, hopLength: 128 }),
    );
    const reqRate = captureThrow(() =>
      phaseVocoder({ samples, speedRatio: 0, sampleRate, nFft: 512, hopLength: 128 }),
    );
    // A missing speedRatio is refused in both forms, never defaulted to a no-op stretch.
    const posMissing = captureThrow(() =>
      (phaseVocoder as (...args: unknown[]) => unknown)(samples, sampleRate),
    );
    const reqMissing = captureThrow(() => phaseVocoder({ samples, sampleRate } as never));
    expect(posMissing.threw).toBe(true);
    expect(reqMissing.message).toBe(posMissing.message);
    const dirty = Float32Array.from(samples);
    dirty[3] = Number.NaN;
    const scanned = captureThrow(() =>
      phaseVocoder({ samples: dirty, speedRatio: 1.1, sampleRate }),
    );
    expect(scanned.message).toMatch(/contains NaN/);
    const unknown = captureThrow(() =>
      phaseVocoder({ samples: dirty, speedRatio: 1.1, sampleRate, validate: false } as never),
    );
    expect(unknown.message).toBe("phaseVocoder: unknown request key 'validate'");
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
