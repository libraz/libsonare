import {
  analyzeImpulseResponse,
  cyclicTempogram,
  detectAcoustic,
  detectChords,
  detectDownbeats,
  detectKey,
  detectKeyCandidates,
  masterAudio,
  masteringPresetNames,
} from '../../bindings/node/src/index.js';
import type {
  AcousticResult,
  ChordAnalysisResult,
  KeyCandidate,
  MasteringChainResult,
  MasteringPreset,
} from '../../bindings/node/src/types.js';

const samples = new Float32Array([0.0, 0.1, -0.1, 0.0]);
const irSamples = new Float32Array([1.0, 0.5, 0.25, 0.125, 0.0]);

const presets: MasteringPreset[] = masteringPresetNames();
const mastered: MasteringChainResult = masterAudio(samples, 22050, 'aiMusic');
const acoustic: AcousticResult = analyzeImpulseResponse(irSamples, 48000);
const blindAcoustic: AcousticResult = detectAcoustic(irSamples, 48000);
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
downbeats satisfies Float32Array;
chords.chords satisfies ChordAnalysisResult['chords'];
cyclic.data satisfies Float32Array;
key.shortName satisfies string;
keyCandidates[0].correlation satisfies number;
mastered.stages satisfies string[];
