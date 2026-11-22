import { describe, expect, it } from 'vitest';
import {
  type MasteringAssistantResult,
  type MasteringAudioProfile,
  type MasteringStreamingPreviewResult,
  masteringAnalyzePair,
  masteringAnalyzeStereo,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringStreamingPreview,
  type PairAnalysis,
  type PairAnalysisResultMap,
  type StereoAnalysis,
  type StereoAnalysisResultMap,
} from '../src/index.js';

const SR = 22050;

function tones(low: number, high: number, amplitude: number): Float32Array {
  const out = new Float32Array(SR);
  for (let i = 0; i < out.length; i++) {
    const t = i / SR;
    out[i] =
      amplitude * (0.6 * Math.sin(2 * Math.PI * low * t) + 0.4 * Math.sin(2 * Math.PI * high * t));
  }
  return out;
}

// Both maps cover every analysis name; a name added to the union without a result type fails to compile.
const pairMapIsComplete: Exclude<PairAnalysis, keyof PairAnalysisResultMap> extends never
  ? true
  : false = true;
const stereoMapIsComplete: Exclude<StereoAnalysis, keyof StereoAnalysisResultMap> extends never
  ? true
  : false = true;

describe('typed results of the explainable-mastering helpers', () => {
  const source = tones(220, 1760, 0.2);
  const reference = tones(330, 2200, 0.4);

  it('covers every analysis name', () => {
    expect(pairMapIsComplete && stereoMapIsComplete).toBe(true);
  });

  it('types the assistant suggestion', () => {
    const result: MasteringAssistantResult = masteringAssistantSuggest({
      samples: source,
      sampleRate: SR,
    });
    expect(typeof result.chainConfig).toBe('object');
    expect(result.explanation.length).toBeGreaterThan(0);
    expect(typeof result.profile.durationSec).toBe('number');
  });

  it('types the streaming preview', () => {
    const result: MasteringStreamingPreviewResult = masteringStreamingPreview({
      samples: source,
      sampleRate: SR,
    });
    expect(result.platforms.length).toBeGreaterThan(0);
    expect(typeof result.platforms[0]?.ceilingRisk).toBe('boolean');
  });

  it('types the audio profile', () => {
    const result: MasteringAudioProfile = masteringAudioProfile({
      samples: source,
      sampleRate: SR,
    });
    expect(typeof result.loudness.integratedLufs).toBe('number');
  });

  it('states the integrated loudness of a silent take as its infinite end', () => {
    const result = masteringAudioProfile({ samples: new Float32Array(SR), sampleRate: SR });
    expect(result.loudness.integratedLufs).toBe(Number.NEGATIVE_INFINITY);
    expect(result.loudness.truePeakDbtp).toBe(Number.NEGATIVE_INFINITY);
  });

  it('keys the pair analysis result on the analysis name literal', () => {
    const loudness = masteringAnalyzePair({
      analysis: 'match.referenceLoudness',
      source,
      reference,
      sampleRate: SR,
    });
    expect(typeof loudness.gainToMatchDb).toBe('number');

    const curve = masteringAnalyzePair({
      analysis: 'match.matchEqCurve',
      source,
      reference,
      sampleRate: SR,
    });
    expect(curve.frequencies.length).toBe(curve.gainDb.length);

    const bands = masteringAnalyzePair({
      analysis: 'match.tonalBalance',
      source,
      reference,
      sampleRate: SR,
    });
    expect(bands.bands[0]?.deviationDb).toBeTypeOf('number');
  });

  it('keys the stereo analysis result on the analysis name literal', () => {
    const result = masteringAnalyzeStereo({
      analysis: 'stereo.monoCompatCheck',
      left: source,
      right: reference,
      sampleRate: SR,
    });
    expect(typeof result.likelyMonoCompatible).toBe('boolean');
  });
});
