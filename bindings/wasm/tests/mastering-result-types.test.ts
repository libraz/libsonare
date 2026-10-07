import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  type JsonResult,
  type MasteringAssistantResult,
  type MasteringAudioProfile,
  type MasteringStreamingPreviewResult,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringPairAnalyze,
  masteringStereoAnalyze,
  masteringStreamingPreview,
  type PairAnalysis,
  type PairAnalysisResultMap,
  type StereoAnalysis,
  type StereoAnalysisResultMap,
} from '../dist/index.js';

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

describe('typed JSON results of the explainable-mastering helpers (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  const source = tones(220, 1760, 0.2);
  const reference = tones(330, 2200, 0.4);

  it('covers every analysis name', () => {
    expect(pairMapIsComplete && stereoMapIsComplete).toBe(true);
  });

  it('types the assistant suggestion', () => {
    const json = masteringAssistantSuggest({ samples: source, sampleRate: SR });
    const result: MasteringAssistantResult = JSON.parse(json) as JsonResult<typeof json>;
    expect(result.chainConfig.version).toBeGreaterThanOrEqual(1);
    expect(result.explanation.length).toBeGreaterThan(0);
    expect(typeof result.profile.durationSec).toBe('number');
  });

  it('types the streaming preview', () => {
    const json = masteringStreamingPreview({ samples: source, sampleRate: SR });
    const result: MasteringStreamingPreviewResult = JSON.parse(json) as JsonResult<typeof json>;
    expect(result.platforms.length).toBeGreaterThan(0);
    expect(typeof result.platforms[0]?.ceilingRisk).toBe('boolean');
  });

  it('types the audio profile', () => {
    const json = masteringAudioProfile({ samples: source, sampleRate: SR });
    const result: MasteringAudioProfile = JSON.parse(json) as JsonResult<typeof json>;
    expect(typeof result.loudness.integratedLufs).toBe('number');
  });

  it('keys the pair analysis result on the analysis name literal', () => {
    const loudness = masteringPairAnalyze({
      analysisName: 'match.referenceLoudness',
      source,
      reference,
      sampleRate: SR,
    });
    const gain = (JSON.parse(loudness) as JsonResult<typeof loudness>).gainToMatchDb;
    expect(typeof gain).toBe('number');

    const curve = masteringPairAnalyze('match.matchEqCurve', source, reference, SR);
    const parsed = JSON.parse(curve) as JsonResult<typeof curve>;
    expect(parsed.frequencies.length).toBe(parsed.gainDb.length);

    const bands = masteringPairAnalyze({
      analysisName: 'match.tonalBalance',
      source,
      reference,
      sampleRate: SR,
    });
    expect((JSON.parse(bands) as JsonResult<typeof bands>).bands[0]?.deviationDb).toBeTypeOf(
      'number',
    );
  });

  it('keys the stereo analysis result on the analysis name literal', () => {
    const json = masteringStereoAnalyze({
      analysisName: 'stereo.monoCompatCheck',
      left: source,
      right: reference,
      sampleRate: SR,
    });
    const result = JSON.parse(json) as JsonResult<typeof json>;
    expect(typeof result.likelyMonoCompatible).toBe('boolean');
  });
});
