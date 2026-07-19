import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeImpulseResponse,
  analyzeRhythm,
  analyzeTimbre,
  analyzeWithProgress,
  bassChroma,
  chordFunctionalAnalysis,
  chroma,
  chromaCens,
  chromaCqt,
  cqt,
  cqtToAudio,
  decompose,
  decomposeWithInit,
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
  Mode,
  melSpectrogram,
  mfcc,
  onsetEnvelope,
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
  stftDb,
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

  it('keeps request-object and positional complete analysis equivalent', () => {
    expect(analyze({ samples, sampleRate })).toEqual(analyze(samples, sampleRate));
  });

  it('accepts the progress callback in the request object', () => {
    const stages: string[] = [];
    const result = analyzeWithProgress({
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
      chordFunctionalAnalysis({ samples, sampleRate, keyRoot: PitchClass.C, keyMode: Mode.Major }),
    ).toEqual(chordFunctionalAnalysis(samples, PitchClass.C, Mode.Major, sampleRate));
  });

  it('defaults functional chord analysis to major mode', () => {
    const request = chordFunctionalAnalysis({ samples, sampleRate, keyRoot: PitchClass.C });
    const explicit = chordFunctionalAnalysis(samples, PitchClass.C, Mode.Major, sampleRate);

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
    const options = { lengthM: 7, widthM: 5, heightM: 3, maxSeconds: 0.1 };
    expect(roomMorph({ samples: impulse, sampleRate: acousticRate, ...options })).toEqual(
      roomMorph(impulse, acousticRate, options),
    );
  });

  it('keeps pitch and resampling request objects equivalent', () => {
    const yin = { sampleRate, frameLength: 1024, hopLength: 256, fmin: 65, fmax: 1000 };
    expect(pitchYin({ samples, ...yin })).toEqual(
      pitchYin(samples, yin.sampleRate, yin.frameLength, yin.hopLength, yin.fmin, yin.fmax),
    );
    expect(pitchPyin({ samples, ...yin })).toEqual(
      pitchPyin(samples, yin.sampleRate, yin.frameLength, yin.hopLength, yin.fmin, yin.fmax),
    );
    expect(Array.from(resample({ samples, srcSr: sampleRate, targetSr: 16000 }))).toEqual(
      Array.from(resample(samples, sampleRate, 16000)),
    );
  });

  it('keeps Constant-Q transform request objects equivalent', () => {
    const options = { sampleRate, hopLength: 512, fmin: 65.406, nBins: 12, binsPerOctave: 12 };
    expect(cqt({ samples, ...options })).toEqual(
      cqt(
        samples,
        options.sampleRate,
        options.hopLength,
        options.fmin,
        options.nBins,
        options.binsPerOctave,
      ),
    );
    expect(pseudoCqt({ samples, ...options })).toEqual(
      pseudoCqt(
        samples,
        options.sampleRate,
        options.hopLength,
        options.fmin,
        options.nBins,
        options.binsPerOctave,
      ),
    );
    expect(hybridCqt({ samples, ...options })).toEqual(
      hybridCqt(
        samples,
        options.sampleRate,
        options.hopLength,
        options.fmin,
        options.nBins,
        options.binsPerOctave,
      ),
    );
    expect(vqt({ samples, ...options, gamma: 0 })).toEqual(
      vqt(
        samples,
        options.sampleRate,
        options.hopLength,
        options.fmin,
        options.nBins,
        options.binsPerOctave,
        0,
      ),
    );
  });

  it('keeps CQT inverse request objects equivalent', () => {
    const cqtOptions = { sampleRate, hopLength: 512, fmin: 65.406, nBins: 12, binsPerOctave: 12 };
    const result = cqt({ samples, ...cqtOptions });
    const inverse = {
      magnitude: result.magnitude,
      nBins: result.nBins,
      nFrames: result.nFrames,
      sampleRate,
      hopLength: 512,
      fmin: 65.406,
      binsPerOctave: 12,
      nIter: 1,
    };
    expect(Array.from(cqtToAudio(inverse))).toEqual(
      Array.from(
        cqtToAudio(
          inverse.magnitude,
          inverse.nBins,
          inverse.nFrames,
          inverse.sampleRate,
          inverse.hopLength,
          inverse.fmin,
          inverse.binsPerOctave,
          inverse.nIter,
        ),
      ),
    );
    expect(Array.from(vqtToAudio({ ...inverse, gamma: 0 }))).toEqual(
      Array.from(
        vqtToAudio(
          inverse.magnitude,
          inverse.nBins,
          inverse.nFrames,
          inverse.sampleRate,
          inverse.hopLength,
          inverse.fmin,
          inverse.binsPerOctave,
          0,
          inverse.nIter,
        ),
      ),
    );
  });

  it('keeps frame and spectrogram request objects equivalent', () => {
    const frame = { sampleRate, nFft: 1024, hopLength: 256 };
    for (const fn of [spectralCentroid, spectralBandwidth, spectralFlatness]) {
      expect(Array.from(fn({ samples, ...frame }))).toEqual(
        Array.from(fn(samples, frame.sampleRate, frame.nFft, frame.hopLength)),
      );
    }
    expect(Array.from(spectralRolloff({ samples, ...frame, rollPercent: 0.9 }))).toEqual(
      Array.from(spectralRolloff(samples, frame.sampleRate, frame.nFft, frame.hopLength, 0.9)),
    );
    const time = { sampleRate, frameLength: 1024, hopLength: 256 };
    for (const fn of [zeroCrossingRate, rmsEnergy]) {
      expect(Array.from(fn({ samples, ...time }))).toEqual(
        Array.from(fn(samples, time.sampleRate, time.frameLength, time.hopLength)),
      );
    }
    expect(stft({ samples, ...frame })).toEqual(
      stft(samples, frame.sampleRate, frame.nFft, frame.hopLength),
    );
    expect(stftDb({ samples, ...frame })).toEqual(
      stftDb(samples, frame.sampleRate, frame.nFft, frame.hopLength),
    );
    const chromaOptions = { sampleRate, hopLength: 256, nChroma: 12 };
    for (const fn of [chromaCens, chromaCqt, bassChroma]) {
      expect(fn({ samples, ...chromaOptions })).toEqual(
        fn(samples, chromaOptions.sampleRate, chromaOptions.hopLength, chromaOptions.nChroma),
      );
    }
    expect(chroma({ samples, ...frame })).toEqual(
      chroma(samples, frame.sampleRate, frame.nFft, frame.hopLength),
    );
  });

  it('keeps onset and tempogram request objects equivalent', () => {
    const onsetOptions = { sampleRate, nFft: 1024, hopLength: 256, nMels: 32 };
    const onset = onsetEnvelope({ samples, ...onsetOptions });
    expect(Array.from(onset)).toEqual(
      Array.from(
        onsetEnvelope(
          samples,
          onsetOptions.sampleRate,
          onsetOptions.nFft,
          onsetOptions.hopLength,
          onsetOptions.nMels,
        ),
      ),
    );
    expect(onsetStrengthMulti({ samples, ...onsetOptions, nBands: 2 })).toEqual(
      onsetStrengthMulti(
        samples,
        onsetOptions.sampleRate,
        onsetOptions.nFft,
        onsetOptions.hopLength,
        onsetOptions.nMels,
        2,
      ),
    );
    const tempogramOptions = { sampleRate, hopLength: 256, winLength: 64 };
    const tempogram = fourierTempogram({ onsetEnvelope: onset, ...tempogramOptions });
    expect(tempogram).toEqual(
      fourierTempogram(
        onset,
        tempogramOptions.sampleRate,
        tempogramOptions.hopLength,
        tempogramOptions.winLength,
      ),
    );
    expect(
      Array.from(tempogramRatio({ tempogramData: tempogram.data, ...tempogramOptions })),
    ).toEqual(
      Array.from(
        tempogramRatio(
          tempogram.data,
          tempogramOptions.winLength,
          tempogramOptions.sampleRate,
          tempogramOptions.hopLength,
        ),
      ),
    );
  });

  it('keeps decomposition request objects equivalent', () => {
    const matrix = new Float32Array([1, 2, 3, 4, 5, 6]);
    const options = { s: matrix, nFeatures: 2, nFrames: 3, nComponents: 1, nIter: 2, beta: 2 };
    expect(decompose(options)).toEqual(decompose(matrix, 2, 3, 1, 2, 2));
    expect(decomposeWithInit({ ...options, init: 'random' })).toEqual(
      decomposeWithInit(matrix, 2, 3, 1, 2, 2, 'random'),
    );
  });

  it('keeps Mel spectrogram request objects equivalent', () => {
    const options = { sampleRate, nFft: 1024, hopLength: 256, nMels: 32, fmin: 0, fmax: 8000 };
    expect(melSpectrogram({ samples, ...options })).toEqual(
      melSpectrogram(
        samples,
        options.sampleRate,
        options.nFft,
        options.hopLength,
        options.nMels,
        options.fmin,
        options.fmax,
      ),
    );
  });

  it('keeps MFCC request objects equivalent', () => {
    const options = { sampleRate, nFft: 1024, hopLength: 256, nMels: 32, nMfcc: 8 };
    expect(mfcc({ samples, ...options })).toEqual(
      mfcc(
        samples,
        options.sampleRate,
        options.nFft,
        options.hopLength,
        options.nMels,
        options.nMfcc,
      ),
    );
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
