import { execFileSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  amplitudeToDb,
  analyze,
  analyzeBpm,
  analyzeDynamics,
  analyzeMelody,
  analyzeRhythm,
  analyzeSections,
  analyzeTimbre,
  analyzeWithProgress,
  chordFunctionalAnalysis,
  chroma,
  cqt,
  dbToAmplitude,
  dbToPower,
  deemphasis,
  detectBeats,
  detectBpm,
  detectChords,
  detectKey,
  detectKeyCandidates,
  detectOnsets,
  fixFrames,
  fixLength,
  fourierTempogram,
  frameSignal,
  framesToSamples,
  framesToTime,
  harmonic,
  hasFfmpegSupport,
  hpss,
  hzToMel,
  hzToMidi,
  hzToNote,
  lufs,
  Mixer,
  mastering,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringChain,
  masteringPairAnalysisNames,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
  masteringProcessStereo,
  masteringStereoAnalysisNames,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  melSpectrogram,
  melToHz,
  mfcc,
  midiToHz,
  mixingScenePresetJson,
  momentaryLufs,
  nnlsChroma,
  normalize,
  noteToHz,
  onsetEnvelope,
  pcen,
  peakPick,
  percussive,
  pitchPyin,
  pitchShift,
  pitchYin,
  plp,
  powerToDb,
  preemphasis,
  RealtimeEngine,
  resample,
  rmsEnergy,
  StreamingEqualizer,
  StreamingMasteringChain,
  samplesToFrames,
  shortTermLufs,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  splitSilence,
  stft,
  stftDb,
  tempogram,
  tempogramRatio,
  timeStretch,
  timeToFrames,
  tonnetz,
  trim,
  trimSilence,
  vectorNormalize,
  version,
  vqt,
  zeroCrossingRate,
} from '../src/index.js';

function findFfmpegCli(): string | null {
  try {
    const result = execFileSync('which', ['ffmpeg'], { encoding: 'utf-8' }).trim();
    return result || null;
  } catch {
    return null;
  }
}

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('RealtimeEngine', () => {
  it('processWithMonitor returns output and monitor buses', () => {
    const engine = new RealtimeEngine(48000, 16);
    try {
      const result = engine.processWithMonitor([
        new Float32Array(16).fill(0.25),
        new Float32Array(16).fill(-0.25),
      ]);
      expect(result.output).toHaveLength(2);
      expect(result.monitor).toHaveLength(2);
      expect(result.output[0][0]).toBeCloseTo(0.25);
      expect(result.output[1][0]).toBeCloseTo(-0.25);
      expect(result.monitor[0][0]).toBeCloseTo(0);
      expect(result.monitor[1][0]).toBeCloseTo(0);
    } finally {
      engine.destroy();
    }
  });
});

describe('Mixer (scene-based routing)', () => {
  it('routes a preset scene and schedules insert automation', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), 48000, 512);
    try {
      mixer.compile();
      expect(mixer.stripCount()).toBeGreaterThan(0);

      // Strip 0 (vocal) carries pre-fader inserts; schedule a no-throw event.
      expect(() => mixer.scheduleInsertAutomation(0, 0, 0, 0, 0.0)).not.toThrow();
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, 0, 48000, 1.0, 'exponential'),
      ).not.toThrow();

      // Out-of-range strip index must throw.
      expect(() => mixer.scheduleInsertAutomation(999, 0, 0, 0, 0.0)).toThrow();

      const block = 512;
      const vocalL = new Float32Array(block);
      const vocalR = new Float32Array(block);
      vocalL[0] = 1.0;
      vocalR[0] = 1.0;
      const silentL = new Float32Array(block);
      const silentR = new Float32Array(block);
      const out = mixer.processStereo([vocalL, silentL], [vocalR, silentR]);
      expect(out.left.length).toBe(block);
      expect(out.sampleRate).toBe(48000);

      const scene = mixer.toSceneJson();
      expect(scene).toContain('vocal-verb');
    } finally {
      mixer.destroy();
    }
  });
});
