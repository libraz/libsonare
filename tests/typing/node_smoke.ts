import {
  amplitudeToDb,
  analyzeImpulseResponse,
  cyclicTempogram,
  detectAcoustic,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  estimateRoom,
  fixFrames,
  frameSignal,
  masterAudio,
  masterAudioAsync,
  masterAudioStereoAsync,
  masteringPresetNames,
  pcen,
  peakPick,
  plp,
  powerToDb,
  roomMorph,
  roomMorphStereo,
  splitSilence,
  synthesizeRir,
  tempogram,
  trimSilence,
} from '../../bindings/node/src/index.js';
import * as nodeFacade from '../../bindings/node/src/index.js';
import type {
  AcousticResult,
  ChordAnalysisResult,
  KeyCandidate,
  Matrix2D,
  MasteringChainResult,
  MasteringPreset,
  RirResult,
  RoomEstimateResult,
  RoomMorphResult,
  RoomMorphStereoResult,
} from '../../bindings/node/src/types.js';

const samples = new Float32Array([0.0, 0.1, -0.1, 0.0]);
const irSamples = new Float32Array([1.0, 0.5, 0.25, 0.125, 0.0]);

const presets: MasteringPreset[] = masteringPresetNames();
const mastered: MasteringChainResult = masterAudio({
  samples,
  sampleRate: 22050,
  preset: 'aiMusic',
});
const acoustic: AcousticResult = analyzeImpulseResponse({
  samples: irSamples,
  sampleRate: 48000,
});
const blindAcoustic: AcousticResult = detectAcoustic({
  samples: irSamples,
  sampleRate: 48000,
});
const rir: RirResult = synthesizeRir({
  sampleRate: 48000,
  lengthM: 6.0,
  widthM: 5.0,
  heightM: 3.0,
  absorption: 0.2,
});
const roomEstimate: RoomEstimateResult = estimateRoom({
  samples: irSamples,
  sampleRate: 48000,
  preferEyring: true,
});
const morphed: RoomMorphResult = roomMorph({
  samples,
  sampleRate: 22050,
  lengthM: 6.0,
  widthM: 5.0,
  heightM: 3.0,
  wet: 0.4,
});
const morphedStereo: RoomMorphStereoResult = roomMorphStereo({
  left: samples,
  right: samples,
  sampleRate: 22050,
  lengthM: 6.0,
  widthM: 5.0,
  heightM: 3.0,
  receiverSpacingM: 0.5,
});
const downbeats: Float32Array = detectDownbeats({ samples, sampleRate: 22050 });
const chords: ChordAnalysisResult = detectChords({
  samples,
  sampleRate: 22050,
  minDurationSec: 0.3,
  smoothingWindowSec: 2.0,
  threshold: 0.5,
  useTriadsOnly: false,
  nFft: 2048,
  hopLength: 512,
  useBeatSync: true,
  useHmm: true,
  hmmBeamWidth: 8,
  useKeyContext: true,
  keyRoot: 0,
  keyMode: 'major',
  detectInversions: true,
  chromaMethod: 'nnls',
});
const cyclic = cyclicTempogram({ onsetEnvelope: samples, sampleRate: 22050 });

// Canonical request-object forms of the request-object-migrated feature functions.
const onset = new Float32Array([0.0, 0.5, 1.0, 0.5, 0.0]);
const frameIndices = new Int32Array([0, 2, 4]);
const samplesMatrix: Matrix2D = { data: samples, rows: 1, cols: samples.length };
tempogram({ onsetEnvelope: onset, sampleRate: 22050 }).data satisfies Float32Array;
plp({ onsetEnvelope: onset, sampleRate: 22050 }) satisfies Float32Array;
powerToDb({ values: onset, ref: 1.0, amin: 1e-10, topDb: 80.0 }) satisfies Float32Array;
amplitudeToDb({ values: onset, ref: 1.0, amin: 1e-5, topDb: 80.0 }) satisfies Float32Array;
trimSilence({
  samples,
  sampleRate: 22050,
  topDb: 60.0,
  frameLength: 2048,
  hopLength: 512,
}).audio satisfies Float32Array;
splitSilence({
  samples,
  sampleRate: 22050,
  topDb: 60.0,
  frameLength: 2048,
  hopLength: 512,
}) satisfies Int32Array;
frameSignal({ samples, sampleRate: 22050, frameLength: 2, hopLength: 1 }) satisfies Matrix2D;
peakPick({
  values: onset,
  preMax: 1,
  postMax: 1,
  preAvg: 1,
  postAvg: 1,
  delta: 0.1,
  wait: 1,
}) satisfies Int32Array;
pcen({ values: { data: onset, rows: 1, cols: onset.length } }) satisfies Matrix2D;
fixFrames({ frames: frameIndices, xMin: 0, pad: true }) satisfies Int32Array;

// Every request-object feature is called in its canonical request form.
nodeFacade.stft({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512 });
nodeFacade.stftDb({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512 });
nodeFacade.melSpectrogram({
  samples,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  nMels: 128,
  fminHz: 0,
  fmaxHz: 8000,
  htk: false,
});
nodeFacade.mfcc({
  samples,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  nMels: 128,
  nMfcc: 13,
  fminHz: 0,
  fmaxHz: 8000,
  htk: false,
  lifter: 0,
});
nodeFacade.chroma({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512 });
nodeFacade.chromaCens({
  samples,
  sampleRate: 22050,
  hopLength: 512,
  nChroma: 12,
  binsPerOctave: 36,
});
nodeFacade.chromaCqt({
  samples,
  sampleRate: 22050,
  hopLength: 512,
  nChroma: 12,
  binsPerOctave: 36,
});
nodeFacade.bassChroma({ samples, sampleRate: 22050, hopLength: 512, nChroma: 12 });
nodeFacade.cqt({
  samples,
  sampleRate: 22050,
  hopLength: 512,
  fminHz: 32.7,
  nBins: 84,
  binsPerOctave: 12,
});
nodeFacade.pseudoCqt({
  samples,
  sampleRate: 22050,
  hopLength: 512,
  fminHz: 32.7,
  nBins: 84,
  binsPerOctave: 12,
});
nodeFacade.hybridCqt({
  samples,
  sampleRate: 22050,
  hopLength: 512,
  fminHz: 32.7,
  nBins: 84,
  binsPerOctave: 12,
});
nodeFacade.vqt({
  samples,
  sampleRate: 22050,
  hopLength: 512,
  fminHz: 32.7,
  nBins: 84,
  binsPerOctave: 12,
  gamma: 0,
});
// gamma changes variable-Q bandwidth and must be impossible to pass to a
// constant-Q request object. This fixture is compiled by CI; Vitest test
// sources are intentionally excluded from the package tsconfig.
// @ts-expect-error gamma is VQT-only.
nodeFacade.cqt({ samples, sampleRate: 22050, gamma: 20 });
// @ts-expect-error gamma is VQT-only.
nodeFacade.pseudoCqt({ samples, sampleRate: 22050, gamma: 20 });
// @ts-expect-error gamma is VQT-only.
nodeFacade.hybridCqt({ samples, sampleRate: 22050, gamma: 20 });
nodeFacade.vqt({ samples, sampleRate: 22050, gamma: 20 }).magnitude satisfies Matrix2D;
masterAudio({ samples, sampleRate: 48000, cancel: () => true, onProgress: () => {} }).stages satisfies string[];
// @ts-expect-error callbacks cannot cross the async worker boundary.
masterAudioAsync({ samples, sampleRate: 48000, cancel: () => true });
// @ts-expect-error callbacks cannot cross the async worker boundary.
masterAudioAsync({ samples, sampleRate: 48000, onProgress: () => {} });
// @ts-expect-error callbacks cannot cross the async worker boundary.
masterAudioStereoAsync({ left: samples, right: samples, sampleRate: 48000, cancel: () => true });
// @ts-expect-error callbacks cannot cross the async worker boundary.
masterAudioStereoAsync({ left: samples, right: samples, sampleRate: 48000, onProgress: () => {} });
nodeFacade.cqtToAudio({
  magnitude: samplesMatrix,
  sampleRate: 22050,
  hopLength: 512,
  fminHz: 32.7,
  binsPerOctave: 12,
  nIter: 1,
});
nodeFacade.vqtToAudio({
  magnitude: samplesMatrix,
  sampleRate: 22050,
  hopLength: 512,
  fminHz: 32.7,
  binsPerOctave: 12,
  gamma: 0,
  nIter: 1,
});
nodeFacade.melToStft({
  power: samplesMatrix,
  sampleRate: 22050,
  nFft: 2048,
  fminHz: 0,
  fmaxHz: 8000,
  htk: false,
});
nodeFacade.melToAudio({
  power: samplesMatrix,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  fminHz: 0,
  fmaxHz: 8000,
  nIter: 1,
  htk: false,
});
nodeFacade.mfccToMel({ coefficients: samplesMatrix, nMels: 128, lifter: 0 });
nodeFacade.mfccToAudio({
  coefficients: samplesMatrix,
  nMels: 128,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  fminHz: 0,
  fmaxHz: 8000,
  nIter: 1,
  htk: false,
  lifter: 0,
});
nodeFacade.spectralCentroid({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512 });
nodeFacade.spectralContrast({
  samples,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  nBands: 6,
  fminHz: 200,
  quantile: 0.02,
});
nodeFacade.polyFeatures({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512, order: 1 });
nodeFacade.zeroCrossings({
  samples,
  sampleRate: 22050,
  threshold: 1e-10,
  refMagnitude: false,
  pad: true,
  zeroPos: true,
});
nodeFacade.pitchTuning({ frequencies: samples, resolution: 0.01, binsPerOctave: 12 });
nodeFacade.estimateTuning({
  samples,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  resolution: 0.01,
  binsPerOctave: 12,
});
nodeFacade.decompose({
  s: samplesMatrix,
  nComponents: 1,
  nIter: 1,
  beta: 2,
  init: 'random',
});
nodeFacade.nnFilter({ s: samplesMatrix, aggregate: 'mean', k: 7, width: 1 });
nodeFacade.remix({
  samples,
  sampleRate: 22050,
  intervals: new Int32Array([0, samples.length]),
  alignZeros: false,
});
nodeFacade.hpss({
  samples,
  sampleRate: 22050,
  kernelHarmonic: 31,
  kernelPercussive: 31,
  withResidual: true,
});
nodeFacade.spectralBandwidth({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512 });
nodeFacade.spectralRolloff({
  samples,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  rollPercent: 0.85,
});
nodeFacade.spectralFlatness({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512 });
nodeFacade.zeroCrossingRate({ samples, sampleRate: 22050, frameLength: 2048, hopLength: 512 });
nodeFacade.rmsEnergy({ samples, sampleRate: 22050, frameLength: 2048, hopLength: 512 });
nodeFacade.pitchYin({
  samples,
  sampleRate: 22050,
  frameLength: 2048,
  hopLength: 512,
  f0MinHz: 50,
  f0MaxHz: 1000,
  threshold: 0.1,
});
nodeFacade.pitchPyin({
  samples,
  sampleRate: 22050,
  frameLength: 2048,
  hopLength: 512,
  f0MinHz: 50,
  f0MaxHz: 1000,
  threshold: 0.1,
  fillNa: true,
});
nodeFacade.onsetStrength({ samples, sampleRate: 22050, nFft: 2048, hopLength: 512, nMels: 128 });
nodeFacade.onsetStrengthMulti({
  samples,
  sampleRate: 22050,
  nFft: 2048,
  hopLength: 512,
  nMels: 128,
  nBands: 3,
});
nodeFacade.fourierTempogram({
  onsetEnvelope: onset,
  sampleRate: 22050,
  hopLength: 512,
  winLength: 384,
});
nodeFacade.tempogramRatio({
  tempogram: { data: samples, rows: 384, cols: 1 },
  sampleRate: 22050,
  hopLength: 512,
  factors: new Float32Array([1]),
});
const declick = nodeFacade.masteringRepairDeclick({ samples, sampleRate: 22050, threshold: 0.5 });
declick satisfies nodeFacade.DeclickResult;
declick.samples satisfies Float32Array;
declick.report satisfies nodeFacade.DeclickReport;
const denoise = nodeFacade.masteringRepairDenoiseClassical({
  samples,
  sampleRate: 22050,
  mode: 'logMmse',
});
denoise satisfies nodeFacade.DenoiseResult;
const declip = nodeFacade.masteringRepairDeclip({
  samples,
  sampleRate: 22050,
  clipThresholdDb: -0.4455,
});
declip satisfies nodeFacade.DeclipResult;
const decrackle = nodeFacade.masteringRepairDecrackle({
  samples,
  sampleRate: 22050,
  mode: 'median',
});
decrackle satisfies nodeFacade.DecrackleResult;
const dehum = nodeFacade.masteringRepairDehum({ samples, sampleRate: 22050, fundamentalHz: 60 });
dehum satisfies nodeFacade.DehumResult;
const dereverb = nodeFacade.masteringRepairDereverbClassical({
  samples,
  sampleRate: 22050,
  threshold: 0.1,
});
dereverb satisfies nodeFacade.DereverbResult;
const trimmed = nodeFacade.masteringRepairTrimSilence({ samples, sampleRate: 22050, mode: 'peak' });
trimmed satisfies nodeFacade.TrimSilenceResult;
trimmed.report satisfies nodeFacade.TrimReport;
nodeFacade.masteringDynamicsCompressor({ samples, sampleRate: 22050, ratio: 2 });
nodeFacade.masteringDynamicsGate({ samples, sampleRate: 22050, thresholdDb: -40 });
nodeFacade.masteringDynamicsTransientShaper({ samples, sampleRate: 22050, attackGainDb: 2 });
const key = detectKey({
  samples,
  sampleRate: 22050,
  chromaHighPassHz: 80.0,
  useHpss: false,
  modes: 'all',
  profile: 'faraldoEdma',
  genreHint: 'edm',
});
const keyCandidates: KeyCandidate[] = detectKeyCandidates({
  samples,
  sampleRate: 22050,
  chromaHighPassHz: 80.0,
  modes: ['major', 'dorian'],
  profile: 'faraldoEdma',
});

// @ts-expect-error invalid preset identifiers must be rejected at compile time.
masterAudio({ samples, sampleRate: 22050, preset: 'invalidPreset' });

presets satisfies MasteringPreset[];
acoustic.rt60Bands satisfies Float32Array;
blindAcoustic.isBlind satisfies boolean;
rir.rir satisfies Float32Array;
rir.diagnostics satisfies RirResult['diagnostics'];
roomEstimate.rt60Bands satisfies Float32Array;
roomEstimate.confidence satisfies number;
morphed.audio satisfies Float32Array;
downbeats satisfies Float32Array;
chords.chords satisfies ChordAnalysisResult['chords'];
cyclic.data satisfies Float32Array;
key.shortName satisfies string;
keyCandidates[0].correlation satisfies number;
mastered.stages satisfies string[];
