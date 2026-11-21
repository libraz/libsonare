import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeRhythm,
  analyzeTimbre,
  bassChroma,
  chordFunctionalAnalysis,
  chroma,
  chromaCens,
  chromaCqt,
  cqt,
  cqtToAudio,
  decompose,
  detectAcoustic,
  detectBeats,
  detectBpm,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  estimateRoom,
  fourierTempogram,
  hybridCqt,
  init,
  melSpectrogram,
  mfcc,
  onsetStrength,
  onsetStrengthMulti,
  PitchClass,
  pitchPyin,
  pitchYin,
  pseudoCqt,
  resample,
  rmsEnergy,
  roomMorph,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  tempogram,
  tempogramRatio,
  vqt,
  vqtToAudio,
  zeroCrossingRate,
} from '../src/index';
import { sine } from './_helpers';

const sampleRate = 22050;
const samples = sine(440, 2, { amp: 0.25, sampleRate });

describe('WASM basic analysis request objects', () => {
  beforeAll(async () => {
    await init();
  });

  it('keeps request-object and positional quick detectors equivalent', () => {
    expect(detectBpm({ samples, sampleRate })).toBe(detectBpm(samples, sampleRate));
    expect(detectKey({ samples, sampleRate, useHpss: true })).toEqual(
      detectKey(samples, sampleRate, { useHpss: true }),
    );
    expect(detectKeyCandidates({ samples, sampleRate, useHpss: true })).toEqual(
      detectKeyCandidates(samples, sampleRate, { useHpss: true }),
    );
    expect(Array.from(detectBeats({ samples, sampleRate }))).toEqual(
      Array.from(detectBeats(samples, sampleRate)),
    );
    expect(Array.from(detectDownbeats({ samples, sampleRate }))).toEqual(
      Array.from(detectDownbeats(samples, sampleRate)),
    );
    expect(Array.from(detectOnsets({ samples, sampleRate }))).toEqual(
      Array.from(detectOnsets(samples, sampleRate)),
    );
  });

  it('keeps onset peak-picking options equivalent', () => {
    const options = { nFft: 256, hopLength: 64, delta: 0.02, backtrack: true };
    expect(Array.from(detectOnsets({ samples, sampleRate, ...options }))).toEqual(
      Array.from(detectOnsets(samples, sampleRate, options)),
    );
  });

  it('keeps request-object and positional complete analysis equivalent', () => {
    expect(analyze({ samples, sampleRate })).toEqual(analyze(samples, sampleRate));
  });

  it('accepts complete analyzer options', () => {
    const result = analyze({
      samples,
      sampleRate,
      bpmMin: 30,
      bpmMax: 90,
      startBpm: 55,
      useHpss: false,
      useChordHmm: true,
      detectChordInversions: true,
    });
    expect(result.bpm).toBeGreaterThanOrEqual(30);
    expect(result.bpm).toBeLessThanOrEqual(90);
  });

  it('accepts the progress callback in the request object', () => {
    const stages: string[] = [];
    const result = analyze({
      samples,
      sampleRate,
      onProgress: (_progress, stage) => stages.push(stage),
    });

    expect(result).toEqual(analyze(samples, sampleRate));
    expect(stages.length).toBeGreaterThan(0);
  });

  it('keeps advanced music-analysis request objects equivalent', () => {
    expect(analyzeBpm({ samples, sampleRate, maxCandidates: 2 })).toEqual(
      analyzeBpm(samples, sampleRate, { maxCandidates: 2 }),
    );
    expect(analyzeRhythm({ samples, sampleRate })).toEqual(analyzeRhythm(samples, sampleRate));
    expect(analyzeDynamics({ samples, sampleRate })).toEqual(analyzeDynamics(samples, sampleRate));
    expect(analyzeTimbre({ samples, sampleRate })).toEqual(analyzeTimbre(samples, sampleRate));
    expect(detectChords({ samples, sampleRate })).toEqual(detectChords(samples, sampleRate));
    expect(
      chordFunctionalAnalysis({ samples, sampleRate, keyRoot: PitchClass.C, keyMode: 'major' }),
    ).toEqual(
      chordFunctionalAnalysis(samples, sampleRate, { keyRoot: PitchClass.C, keyMode: 'major' }),
    );
  });

  it('defaults functional chord analysis to major mode', () => {
    const request = chordFunctionalAnalysis({ samples, sampleRate, keyRoot: PitchClass.C });
    const explicit = chordFunctionalAnalysis(samples, sampleRate, {
      keyRoot: PitchClass.C,
      keyMode: 'major',
    });

    expect(request).toEqual(explicit);
  });

  it('keeps acoustic-analysis request objects equivalent', () => {
    const impulse = new Float32Array(4000);
    impulse[0] = 1;
    const acousticRate = 48000;
    expect(analyzeImpulseResponse({ samples: impulse, sampleRate: acousticRate })).toEqual(
      analyzeImpulseResponse(impulse, acousticRate),
    );
    expect(detectAcoustic({ samples: impulse, sampleRate: acousticRate })).toEqual(
      detectAcoustic(impulse, acousticRate),
    );
    expect(estimateRoom({ samples: impulse, sampleRate: acousticRate })).toEqual(
      estimateRoom(impulse, acousticRate),
    );
    const options = { lengthM: 7, widthM: 5, heightM: 3, maxSec: 0.1 };
    expect(roomMorph({ samples: impulse, sampleRate: acousticRate, ...options })).toEqual(
      roomMorph(impulse, acousticRate, options),
    );
  });

  it('keeps pitch and resampling request objects equivalent', () => {
    const yin = { frameLength: 1024, hopLength: 256, f0MinHz: 65, f0MaxHz: 1000 };
    expect(pitchYin({ samples, sampleRate, ...yin })).toEqual(pitchYin(samples, sampleRate, yin));
    expect(pitchPyin({ samples, sampleRate, ...yin })).toEqual(pitchPyin(samples, sampleRate, yin));
    expect(Array.from(resample({ samples, sampleRate, targetSampleRate: 16000 }))).toEqual(
      Array.from(resample(samples, sampleRate, 16000)),
    );
  });

  it('keeps Constant-Q transform request objects equivalent', () => {
    const options = { hopLength: 512, fminHz: 65.406, nBins: 12, binsPerOctave: 12 };
    expect(cqt({ samples, sampleRate, ...options })).toEqual(cqt(samples, sampleRate, options));
    expect(pseudoCqt({ samples, sampleRate, ...options })).toEqual(
      pseudoCqt(samples, sampleRate, options),
    );
    expect(hybridCqt({ samples, sampleRate, ...options })).toEqual(
      hybridCqt(samples, sampleRate, options),
    );
    expect(vqt({ samples, sampleRate, ...options, gamma: 0 })).toEqual(
      vqt(samples, sampleRate, { ...options, gamma: 0 }),
    );
  });

  it('runs the CQT inverse requests on the matrix a forward transform returns', () => {
    const result = cqt({
      samples,
      sampleRate,
      hopLength: 512,
      fminHz: 65.406,
      nBins: 12,
      binsPerOctave: 12,
    });
    const inverse = {
      magnitude: result.magnitude,
      sampleRate,
      hopLength: 512,
      fminHz: 65.406,
      binsPerOctave: 12,
      nIter: 1,
    };
    expect(result.magnitude.rows).toBe(12);
    const audio = cqtToAudio(inverse);
    expect(audio.length).toBeGreaterThan(0);
    expect(Array.from(cqtToAudio(inverse))).toEqual(Array.from(audio));
    const vqtAudio = vqtToAudio({ ...inverse, gamma: 0 });
    expect(vqtAudio.length).toBeGreaterThan(0);
    expect(Array.from(vqtToAudio({ ...inverse, gamma: 0 }))).toEqual(Array.from(vqtAudio));
  });

  it('keeps frame and spectrogram request objects equivalent', () => {
    const frame = { nFft: 1024, hopLength: 256 };
    for (const fn of [spectralCentroid, spectralBandwidth, spectralFlatness]) {
      expect(Array.from(fn({ samples, sampleRate, ...frame }))).toEqual(
        Array.from(fn(samples, sampleRate, frame)),
      );
    }
    expect(
      Array.from(spectralRolloff({ samples, sampleRate, ...frame, rollPercent: 0.9 })),
    ).toEqual(Array.from(spectralRolloff(samples, sampleRate, { ...frame, rollPercent: 0.9 })));
    const time = { frameLength: 1024, hopLength: 256 };
    for (const fn of [zeroCrossingRate, rmsEnergy]) {
      expect(Array.from(fn({ samples, sampleRate, ...time }))).toEqual(
        Array.from(fn(samples, sampleRate, time)),
      );
    }
    expect(stft({ samples, sampleRate, ...frame })).toEqual(stft(samples, sampleRate, frame));
    expect(stft({ samples, sampleRate, ...frame, db: true })).toEqual(
      stft(samples, sampleRate, { ...frame, db: true }),
    );
    const chromaOptions = { hopLength: 256, nChroma: 12 };
    // A union of the overloaded functions keeps only the request-object call
    // signature, so the positional side names the shared one it drops.
    type PositionalChromaFn = (
      samples: Float32Array,
      sampleRate: number,
      options?: { hopLength?: number; nChroma?: number },
    ) => ReturnType<typeof chromaCens>;
    for (const fn of [chromaCens, chromaCqt, bassChroma]) {
      expect(fn({ samples, sampleRate, ...chromaOptions })).toEqual(
        (fn as PositionalChromaFn)(samples, sampleRate, chromaOptions),
      );
    }
    expect(chroma({ samples, sampleRate, ...frame })).toEqual(chroma(samples, sampleRate, frame));
  });

  it('keeps onset request objects equivalent and shapes the tempogram requests', () => {
    const onsetOptions = { nFft: 1024, hopLength: 256, nMels: 32 };
    const onset = onsetStrength({ samples, sampleRate, ...onsetOptions });
    expect(Array.from(onset)).toEqual(Array.from(onsetStrength(samples, sampleRate, onsetOptions)));
    expect(onsetStrengthMulti({ samples, sampleRate, ...onsetOptions, nBands: 2 })).toEqual(
      onsetStrengthMulti(samples, sampleRate, { ...onsetOptions, nBands: 2 }),
    );
    const winLength = 64;
    const fourier = fourierTempogram({
      onsetEnvelope: onset,
      sampleRate,
      hopLength: 256,
      winLength,
      center: false,
    });
    const autocorrelation = tempogram({
      onsetEnvelope: onset,
      sampleRate,
      hopLength: 256,
      winLength,
      center: false,
      norm: false,
    });
    expect(fourier.cols).toBeGreaterThan(0);
    expect(autocorrelation.rows).toBe(winLength);
    expect(autocorrelation.cols).toBe(fourier.cols);
    const ratio = tempogramRatio({ tempogram: fourier, sampleRate, hopLength: 256 });
    expect(ratio.length).toBeGreaterThan(0);
    expect(ratio.every(Number.isFinite)).toBe(true);
  });

  it('keeps the default decomposition init equal to an explicit random init', () => {
    const matrix = new Float32Array([1, 2, 3, 4, 5, 6]);
    const options = {
      s: { data: matrix, rows: 2, cols: 3 },
      nComponents: 1,
      nIter: 2,
      beta: 2,
    };
    expect(decompose(options)).toEqual(decompose({ ...options, init: 'random' }));
  });

  it('keeps Mel spectrogram request objects equivalent', () => {
    const options = { nFft: 1024, hopLength: 256, nMels: 32, fminHz: 0, fmaxHz: 8000 };
    expect(melSpectrogram({ samples, sampleRate, ...options })).toEqual(
      melSpectrogram(samples, sampleRate, options),
    );
  });

  it('keeps MFCC request objects equivalent', () => {
    const options = { nFft: 1024, hopLength: 256, nMels: 32, nMfcc: 8 };
    expect(mfcc({ samples, sampleRate, ...options })).toEqual(mfcc(samples, sampleRate, options));
  });

  // Both call shapes funnel through one private normalizer, so an invalid input
  // must fail identically either way — the positive-path equivalence above does
  // not prove the error path stays in lockstep.
  it('throws identically on invalid input in both call forms', () => {
    const empty = new Float32Array(0);
    const posBpm = captureThrow(() => detectBpm(empty, sampleRate));
    const reqBpm = captureThrow(() => detectBpm({ samples: empty, sampleRate }));
    expect(posBpm.threw).toBe(true);
    expect(reqBpm.threw).toBe(true);
    expect(reqBpm.message).toBe(posBpm.message);

    const posTimbre = captureThrow(() => analyzeTimbre(empty, sampleRate));
    const reqTimbre = captureThrow(() => analyzeTimbre({ samples: empty, sampleRate }));
    expect(posTimbre.threw).toBe(true);
    expect(reqTimbre.threw).toBe(true);
    expect(reqTimbre.message).toBe(posTimbre.message);
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
