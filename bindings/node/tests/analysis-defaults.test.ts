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
  mastering,
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
  MasteringOptions,
  MelodyOptions,
  NoteStretchOptions,
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
      bpmMin: 30.0,
      bpmMax: 300.0,
      startBpm: 120.0,
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
      compressionThreshold: 6.0,
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
    };
    expect(analyzeSections(x, SR)).toEqual(analyzeSections(x, SR, defaults));
  });

  it('analyzeMelody', () => {
    const defaults: Required<MelodyOptions> = {
      fmin: 65.0,
      fmax: 2093.0,
      frameLength: 2048,
      hopLength: 256,
      threshold: 0.1,
      usePyin: false,
      center: true,
    };
    expect(analyzeMelody(x, SR)).toEqual(analyzeMelody(x, SR, defaults));
  });

  it('voiceChange', () => {
    const defaults: Required<Omit<VoiceChangeOptions, 'validate'>> = {
      pitchSemitones: 0.0,
      formantFactor: 1.0,
    };
    expect(voiceChange(x, SR)).toEqual(voiceChange(x, SR, defaults));
  });

  it('mastering', () => {
    const defaults: Required<MasteringOptions> = {
      targetLufs: -14.0,
      ceilingDb: -1.0,
      truePeakOversample: 4,
    };
    expect(mastering(x, SR)).toEqual(mastering(x, SR, defaults));
  });

  it('noteStretch (stretchRatio default; onset/offset are required inputs)', () => {
    // onsetSample/offsetSample select WHICH note, so their 0/0 defaults are
    // placeholders (a zero-length region throws). Pin only the real default,
    // stretchRatio, against an explicit valid region.
    const region: NoteStretchOptions = { onsetSample: 0, offsetSample: x.length };
    expect(noteStretch(x, SR, region)).toEqual(
      noteStretch(x, SR, { ...region, stretchRatio: 1.0 }),
    );
  });

  it('meteringDetectClipping', () => {
    const defaults: Required<Omit<MeteringDetectClippingOptions, 'validate'>> = {
      threshold: 0.999,
      minRegionSamples: 1,
    };
    expect(meteringDetectClipping(x, SR)).toEqual(meteringDetectClipping(x, SR, defaults));
  });

  it('meteringDynamicRange', () => {
    const defaults: Required<Omit<MeteringDynamicRangeOptions, 'validate'>> = {
      windowSec: 0,
      hopSec: 0,
      lowPercentile: -1,
      highPercentile: -1,
    };
    expect(meteringDynamicRange(x, SR)).toEqual(meteringDynamicRange(x, SR, defaults));
  });
});
