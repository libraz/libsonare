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
  masteringPresetNames,
  pcen,
  peakPick,
  plp,
  powerToDb,
  roomMorph,
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
  MasteringChainResult,
  MasteringPreset,
  RirResult,
  RoomEstimateResult,
} from '../../bindings/node/src/types.js';

const samples = new Float32Array([0.0, 0.1, -0.1, 0.0]);
const irSamples = new Float32Array([1.0, 0.5, 0.25, 0.125, 0.0]);

const presets: MasteringPreset[] = masteringPresetNames();
const mastered: MasteringChainResult = masterAudio(samples, 22050, 'aiMusic');
const acoustic: AcousticResult = analyzeImpulseResponse(irSamples, 48000);
const blindAcoustic: AcousticResult = detectAcoustic(irSamples, 48000);
const rir: RirResult = synthesizeRir({
  lengthM: 6.0,
  widthM: 5.0,
  heightM: 3.0,
  absorption: 0.2,
});
const roomEstimate: RoomEstimateResult = estimateRoom(irSamples, 48000, {
  preferEyring: true,
});
const morphed: Float32Array = roomMorph(samples, 22050, {
  lengthM: 6.0,
  widthM: 5.0,
  heightM: 3.0,
  wet: 0.4,
});
const downbeats: Float32Array = detectDownbeats(samples, 22050);
const chords: ChordAnalysisResult = detectChords(
  samples,
  22050,
  0.3,
  2.0,
  0.5,
  false,
  2048,
  512,
  true,
  true,
  8,
  true,
  0,
  0,
  true,
  'nnls',
);
const cyclic = cyclicTempogram(samples, 22050);

// Positional compatibility overloads for the request-object-migrated feature
// functions must stay callable (mirrors the WASM surface).
const onset = new Float32Array([0.0, 0.5, 1.0, 0.5, 0.0]);
const frameIndices = new Int32Array([0, 2, 4]);
tempogram(onset, 22050).data satisfies Float32Array;
plp(onset, 22050) satisfies Float32Array;
powerToDb(onset, 1.0, 1e-10, 80.0) satisfies Float32Array;
amplitudeToDb(onset, 1.0, 1e-5, 80.0) satisfies Float32Array;
trimSilence(samples, 60.0, 2048, 512).audio satisfies Float32Array;
splitSilence(samples, 60.0, 2048, 512) satisfies Int32Array;
frameSignal(samples, 2, 1).frames satisfies Float32Array;
peakPick(onset, 1, 1, 1, 1, 0.1, 1) satisfies Int32Array;
pcen(onset, 1, onset.length) satisfies Float32Array;
fixFrames(frameIndices, 0, -1, true) satisfies Int32Array;

// Every request-object feature keeps a positional overload for compatibility.
// This fixture is compiled, not run; it pins each public overload independently.
nodeFacade.stft(samples, 22050, 2048, 512);
nodeFacade.stftDb(samples, 22050, 2048, 512);
nodeFacade.melSpectrogram(samples, 22050, 2048, 512, 128, 0, 8000, false);
nodeFacade.mfcc(samples, 22050, 2048, 512, 128, 13, 0, 8000, false, 0);
nodeFacade.chroma(samples, 22050, 2048, 512);
nodeFacade.chromaCens(samples, 22050, 512, 12, 36);
nodeFacade.chromaCqt(samples, 22050, 512, 12, 36);
nodeFacade.bassChroma(samples, 22050, 512, 12);
nodeFacade.cqt(samples, 22050, 512, 32.7, 84, 12);
nodeFacade.pseudoCqt(samples, 22050, 512, 32.7, 84, 12);
nodeFacade.hybridCqt(samples, 22050, 512, 32.7, 84, 12);
nodeFacade.vqt(samples, 22050, 512, 32.7, 84, 12, 0);
nodeFacade.cqtToAudio(samples, 1, samples.length, 22050, 512, 32.7, 12, 1);
nodeFacade.vqtToAudio(samples, 1, samples.length, 22050, 512, 32.7, 12, 0, 1);
nodeFacade.melToStft(samples, 1, samples.length, 22050, 2048, 0, 8000, false);
nodeFacade.melToAudio(samples, 1, samples.length, 22050, 2048, 512, 0, 8000, 1, false);
nodeFacade.mfccToMel(samples, 1, samples.length, 128, 0);
nodeFacade.mfccToAudio(samples, 1, samples.length, 128, 22050, 2048, 512, 0, 8000, 1, false, 0);
nodeFacade.spectralCentroid(samples, 22050, 2048, 512);
nodeFacade.spectralContrast(samples, 22050, 2048, 512, 6, 200, 0.02);
nodeFacade.polyFeatures(samples, 22050, 2048, 512, 1);
nodeFacade.zeroCrossings(samples, 1e-10, false, true, true);
nodeFacade.pitchTuning(samples, 0.01, 12);
nodeFacade.estimateTuning(samples, 22050, 2048, 512, 0.01, 12);
nodeFacade.decompose(samples, 1, samples.length, 1, 1, 2, 'random');
nodeFacade.nnFilter(samples, 1, samples.length, 'mean', 7, 1);
nodeFacade.remix(samples, new Int32Array([0, samples.length]), 22050, false);
nodeFacade.hpssWithResidual(samples, 22050, 31, 31);
nodeFacade.spectralBandwidth(samples, 22050, 2048, 512);
nodeFacade.spectralRolloff(samples, 22050, 2048, 512, 0.85);
nodeFacade.spectralFlatness(samples, 22050, 2048, 512);
nodeFacade.zeroCrossingRate(samples, 22050, 2048, 512);
nodeFacade.rmsEnergy(samples, 22050, 2048, 512);
nodeFacade.pitchYin(samples, 22050, 2048, 512, 50, 1000, 0.1, true);
nodeFacade.pitchPyin(samples, 22050, 2048, 512, 50, 1000, 0.1, true);
nodeFacade.onsetEnvelope(samples, 22050, 2048, 512, 128);
nodeFacade.onsetStrengthMulti(samples, 22050, 2048, 512, 128, 3);
nodeFacade.fourierTempogram(onset, 22050, 512, 384);
nodeFacade.tempogramRatio(samples, 384, 22050, 512, new Float32Array([1]));
nodeFacade.masteringRepairDeclick(samples, 22050, { threshold: 0.5 });
nodeFacade.masteringRepairDenoiseClassical(samples, 22050, { mode: 'logMmse' });
nodeFacade.masteringRepairDeclip(samples, 22050, { clipThreshold: 0.95 });
nodeFacade.masteringRepairDecrackle(samples, 22050, { mode: 'median' });
nodeFacade.masteringRepairDehum(samples, 22050, { fundamentalHz: 60 });
nodeFacade.masteringRepairDereverbClassical(samples, 22050, { threshold: 0.1 });
nodeFacade.masteringRepairTrimSilence(samples, 22050, { mode: 'peak' });
nodeFacade.masteringDynamicsCompressor(samples, 22050, { ratio: 2 });
nodeFacade.masteringDynamicsGate(samples, 22050, { thresholdDb: -40 });
nodeFacade.masteringDynamicsTransientShaper(samples, 22050, { attackGainDb: 2 });
const key = detectKey(samples, 22050, {
  highPassHz: 80.0,
  useHpss: false,
  modes: 'all',
  profile: 'edma',
  genreHint: 'edm',
});
const keyCandidates: KeyCandidate[] = detectKeyCandidates(samples, 22050, {
  highPassHz: 80.0,
  modes: ['major', 'dorian'],
  profile: 4,
});

// @ts-expect-error invalid preset identifiers must be rejected at compile time.
masterAudio(samples, 22050, 'invalidPreset');

presets satisfies MasteringPreset[];
acoustic.rt60Bands satisfies Float32Array;
blindAcoustic.isBlind satisfies boolean;
rir.rir satisfies Float32Array;
rir.hasError satisfies boolean;
roomEstimate.rt60Bands satisfies Float32Array;
roomEstimate.confidence satisfies number;
morphed satisfies Float32Array;
downbeats satisfies Float32Array;
chords.chords satisfies ChordAnalysisResult['chords'];
cyclic.data satisfies Float32Array;
key.shortName satisfies string;
keyCandidates[0].correlation satisfies number;
mastered.stages satisfies string[];
