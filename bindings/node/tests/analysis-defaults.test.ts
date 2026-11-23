import { describe, expect, it } from 'vitest';
import type { VoiceChangeOptions } from '../src/effects_mastering.js';
import {
  analyzeBpm,
  analyzeDynamics,
  analyzeMelody,
  analyzeRhythm,
  analyzeSections,
  analyzeTimbre,
  detectAcoustic,
  masteringNormalizeLoudness,
  meteringDetectClipping,
  meteringDynamicRange,
  noteStretch,
  voiceChange,
} from '../src/index.js';
import type {
  MeteringDetectClippingOptions,
  MeteringDynamicRangeOptions,
} from '../src/metering.js';
import type {
  AcousticOptions,
  AnalyzeBpmOptions,
  AnalyzeDynamicsOptions,
  AnalyzeRhythmOptions,
  AnalyzeSectionsOptions,
  AnalyzeTimbreOptions,
  MasteringNormalizeLoudnessOptions,
  MelodyOptions,
} from '../src/types.js';

/**
 * Pins the resolved defaults of the options-bag analysis entry points.
 *
 * Once a facade folds its scalar params into an `options` bag, the per-param
 * default literal moves into the function body (`options.nFft ?? 2048`), which
 * the cross-binding parity checker cannot read — it can only reconcile defaults
 * declared as positional parameters. WASM and Python still expose these as
 * positional params, so parity keeps them anchored to the C core; this test
 * re-anchors the Node side by asserting that calling with NO options produces a
 * byte-identical result to calling with the documented defaults spelled out.
 * If any `?? <default>` drifts from the value mirrored here, the no-options and
 * explicit-options results diverge and the matching case fails.
 */
const SR = 22050;

function testSignal(): Float32Array {
  // Deterministic mixed-tone signal, long enough for framed analysis.
  const n = SR; // 1 second
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] =
      0.3 *
      (Math.sin((2 * Math.PI * 110 * i) / SR) +
        Math.sin((2 * Math.PI * 220 * i) / SR) +
        Math.sin((2 * Math.PI * 440 * i) / SR));
  }
  return out;
}

describe('analysis options-bag defaults are pinned to the documented values', () => {
  const x = testSignal();

  it('analyzeBpm', () => {
    const defaults: Required<AnalyzeBpmOptions> = {
      bpmMin: 60.0,
      bpmMax: 200.0,
      nFft: 2048,
      hopLength: 512,
      maxCandidates: 5,
    };
    expect(analyzeBpm(x, SR)).toEqual(analyzeBpm(x, SR, defaults));
  });

  it('analyzeRhythm', () => {
    const defaults: Required<AnalyzeRhythmOptions> = {
      bpmMin: 60.0,
      bpmMax: 200.0,
      startBpm: 120.0,
      nFft: 2048,
      hopLength: 512,
    };
    expect(analyzeRhythm(x, SR)).toEqual(analyzeRhythm(x, SR, defaults));
  });

  it('analyzeDynamics', () => {
    const defaults: Required<AnalyzeDynamicsOptions> = {
      windowSec: 0.4,
      hopLength: 512,
      compressionThresholdDb: 6.0,
    };
    expect(analyzeDynamics(x, SR)).toEqual(analyzeDynamics(x, SR, defaults));
  });

  it('detectAcoustic', () => {
    const defaults: Required<AcousticOptions> = {
      nOctaveBands: 6,
      nThirdOctaveSubbands: 24,
      minDecayDb: 30.0,
      noiseFloorMarginDb: 10.0,
    };
    expect(detectAcoustic(x, SR)).toEqual(detectAcoustic(x, SR, defaults));
  });

  it('analyzeTimbre', () => {
    const defaults: Required<AnalyzeTimbreOptions> = {
      nFft: 2048,
      hopLength: 512,
      nMels: 128,
      nMfcc: 13,
      windowSec: 0.5,
    };
    expect(analyzeTimbre(x, SR)).toEqual(analyzeTimbre(x, SR, defaults));
  });

  it('analyzeSections', () => {
    const defaults: Required<AnalyzeSectionsOptions> = {
      nFft: 2048,
      hopLength: 512,
      minSectionSec: 4.0,
      threshold: 0.22,
    };
    expect(analyzeSections(x, SR)).toEqual(analyzeSections(x, SR, defaults));
  });

  it('analyzeMelody', () => {
    const defaults: Required<MelodyOptions> = {
      f0MinHz: 65.0,
      f0MaxHz: 2093.0,
      frameLength: 2048,
      hopLength: 256,
      threshold: 0.1,
      usePyin: false,
      center: true,
    };
    expect(analyzeMelody(x, SR)).toEqual(analyzeMelody(x, SR, defaults));
  });

  it('voiceChange', () => {
    const defaults: Required<VoiceChangeOptions> = {
      pitchSemitones: 0.0,
      formantFactor: 1.0,
      formantMode: 'relative',
    };
    expect(voiceChange(x, SR)).toEqual(voiceChange(x, SR, defaults));
  });

  it('masteringNormalizeLoudness', () => {
    const defaults: Required<MasteringNormalizeLoudnessOptions> = {
      targetLufs: -14.0,
      ceilingDbtp: -1.0,
      truePeakOversample: 4,
      releaseMs: 50,
      applyGainAtInputRate: false,
    };
    expect(masteringNormalizeLoudness(x, SR)).toEqual(masteringNormalizeLoudness(x, SR, defaults));
  });

  it('noteStretch (stretchRatio is required; the region defaults to the whole input)', () => {
    const region = { onsetSample: 0, offsetSample: x.length };
    expect(() => noteStretch(x, SR, region as never)).toThrow(TypeError);
    expect(noteStretch(x, SR, { stretchRatio: 1.0 })).toEqual(
      noteStretch(x, SR, { ...region, stretchRatio: 1.0 }),
    );
  });

  it('meteringDetectClipping', () => {
    const defaults: Required<MeteringDetectClippingOptions> = {
      thresholdDb: 20 * Math.log10(0.999),
      minRegionSamples: 1,
    };
    expect(meteringDetectClipping(x, SR)).toEqual(meteringDetectClipping(x, SR, defaults));
  });

  it('meteringDynamicRange', () => {
    const defaults: Required<MeteringDynamicRangeOptions> = {
      windowSec: 3,
      hopSec: 1,
      lowPercentile: 0.1,
      highPercentile: 0.95,
    };
    expect(meteringDynamicRange(x, SR)).toEqual(meteringDynamicRange(x, SR, defaults));
  });
});
