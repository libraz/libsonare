/**
 * The optional stage counts a mastering result carries: `declipUnreconstructedRuns` is present
 * only when a declip stage ran, `quantizeClippedSamples` only when a quantizing stage ran.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masterAudio,
  masterAudioStereo,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
} from '../src/index';

const SR = 48000;
const CLIP_THRESHOLD_DB = 20 * Math.log10(0.47);

/** A tone clipped flat at 0.5, with one held run longer than declip reconstructs. */
function clippedToneWithLongRun(): Float32Array {
  const out = new Float32Array(SR / 4);
  for (let i = 0; i < out.length; i++) {
    out[i] = Math.max(-0.5, Math.min(0.5, Math.sin((2 * Math.PI * 220 * i) / SR)));
  }
  out.fill(0.5, 2000, 2800);
  return out;
}

const hot = (): Float32Array => Float32Array.from([0.25, 1.5, -2.0, 1.0]);

function expectCount(value: number | undefined): void {
  expect(Number.isInteger(value)).toBe(true);
  expect(value as number).toBeGreaterThanOrEqual(0);
}

describe('optional stage counts on mastering results', () => {
  beforeAll(async () => {
    await init();
  });

  it('a declip processor reports its unreconstructed runs', () => {
    const params = { clipThresholdDb: CLIP_THRESHOLD_DB };
    const samples = clippedToneWithLongRun();
    const mono = masteringApplyProcessor({
      processor: 'repair.declip',
      samples,
      sampleRate: SR,
      params,
    });
    expectCount(mono.declipUnreconstructedRuns);
    expect(mono.declipUnreconstructedRuns).toBeGreaterThanOrEqual(1);
    expect('quantizeClippedSamples' in mono).toBe(false);

    const stereo = masteringApplyProcessorStereo({
      processor: 'repair.declip',
      left: samples,
      right: samples.slice(),
      sampleRate: SR,
      params,
    });
    expectCount(stereo.declipUnreconstructedRuns);
    expect(stereo.declipUnreconstructedRuns).toBeGreaterThanOrEqual(2);
    expect('quantizeClippedSamples' in stereo).toBe(false);
  });

  it('a quantizing processor counts samples past full scale', () => {
    const processor = 'final.bitDepth';
    const mono = masteringApplyProcessor({ processor, samples: hot(), sampleRate: SR });
    expectCount(mono.quantizeClippedSamples);
    // Exactly 1.0 is not counted.
    expect(mono.quantizeClippedSamples).toBe(2);
    expect('declipUnreconstructedRuns' in mono).toBe(false);

    const stereo = masteringApplyProcessorStereo({
      processor,
      left: hot(),
      right: hot(),
      sampleRate: SR,
    });
    expectCount(stereo.quantizeClippedSamples);
    expect(stereo.quantizeClippedSamples).toBe(4);
    expect('declipUnreconstructedRuns' in stereo).toBe(false);
  });

  it('a dither of type none leaves the quantize count absent', () => {
    const params = { type: 0 } as never;
    const mono = masteringApplyProcessor({
      processor: 'final.dither',
      samples: hot(),
      sampleRate: SR,
      params,
    });
    expect('quantizeClippedSamples' in mono).toBe(false);
    const stereo = masteringApplyProcessorStereo({
      processor: 'final.dither',
      left: hot(),
      right: hot(),
      sampleRate: SR,
      params,
    });
    expect('quantizeClippedSamples' in stereo).toBe(false);
  });

  it('a chain reports the declip count only when declip ran', () => {
    const samples = clippedToneWithLongRun();
    const on = { repair: { declip: { enabled: true } } };
    const off = { repair: { declip: { enabled: false } } };

    const mono = masterAudio({ samples, sampleRate: SR, overrides: on });
    expectCount(mono.declipUnreconstructedRuns);
    expect('quantizeClippedSamples' in mono).toBe(false);
    expect(
      'declipUnreconstructedRuns' in masterAudio({ samples, sampleRate: SR, overrides: off }),
    ).toBe(false);

    const left = samples;
    const right = samples.slice();
    const stereo = masterAudioStereo({ left, right, sampleRate: SR, overrides: on });
    expectCount(stereo.declipUnreconstructedRuns);
    expect('quantizeClippedSamples' in stereo).toBe(false);
    expect(
      'declipUnreconstructedRuns' in
        masterAudioStereo({ left, right, sampleRate: SR, overrides: off }),
    ).toBe(false);
  });
});
