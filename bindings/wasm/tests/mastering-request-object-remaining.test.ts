import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringPairProcess,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDenoiseClassical,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
  masteringStreamingPreview,
} from '../src/index';

const sampleRate = 22_050;
const samples = Float32Array.from({ length: 2048 }, (_, i) =>
  Math.sin((2 * Math.PI * 220 * i) / sampleRate),
);

beforeAll(async () => init());

describe('remaining WASM mastering request objects', () => {
  it('preserves dynamics results', () => {
    for (const [positional, request] of [
      [
        masteringDynamicsCompressor(samples, sampleRate, { thresholdDb: -18 }),
        masteringDynamicsCompressor({ samples, sampleRate, thresholdDb: -18 }),
      ],
      [
        masteringDynamicsGate(samples, sampleRate, { thresholdDb: -48 }),
        masteringDynamicsGate({ samples, sampleRate, thresholdDb: -48 }),
      ],
      [
        masteringDynamicsTransientShaper(samples, sampleRate, { attackGainDb: 2 }),
        masteringDynamicsTransientShaper({ samples, sampleRate, attackGainDb: 2 }),
      ],
    ] as const) {
      expect(request.latencySamples).toBe(positional.latencySamples);
      expect(request.samples).toEqual(positional.samples);
    }
  });

  it('preserves repair results', () => {
    const cases = [
      [
        masteringRepairDeclick(samples, sampleRate, { threshold: 3 }),
        masteringRepairDeclick({ samples, sampleRate, threshold: 3 }),
      ],
      [
        masteringRepairDenoiseClassical(samples, sampleRate, { nFft: 512 }),
        masteringRepairDenoiseClassical({ samples, sampleRate, nFft: 512 }),
      ],
      [
        masteringRepairDeclip(samples, sampleRate, { iterations: 1 }),
        masteringRepairDeclip({ samples, sampleRate, iterations: 1 }),
      ],
      [
        masteringRepairDecrackle(samples, sampleRate, { threshold: 3 }),
        masteringRepairDecrackle({ samples, sampleRate, threshold: 3 }),
      ],
      [
        masteringRepairDehum(samples, sampleRate, { fundamentalHz: 60 }),
        masteringRepairDehum({ samples, sampleRate, fundamentalHz: 60 }),
      ],
      [
        masteringRepairDereverbClassical(samples, sampleRate, { nFft: 512 }),
        masteringRepairDereverbClassical({ samples, sampleRate, nFft: 512 }),
      ],
      [
        masteringRepairTrimSilence(samples, sampleRate, { threshold: 0.001 }),
        masteringRepairTrimSilence({ samples, sampleRate, threshold: 0.001 }),
      ],
    ] as const;
    for (const [positional, request] of cases) {
      expect(request).toEqual(positional);
    }
  });

  it('preserves core helper results', () => {
    const reference = Float32Array.from(samples, (value) => value * 0.8);
    expect(
      masteringPairProcess({
        processorName: 'match.abCrossfade',
        source: samples,
        reference,
        sampleRate,
      }),
    ).toEqual(masteringPairProcess('match.abCrossfade', samples, reference, sampleRate));
    expect(masteringAssistantSuggest({ samples, sampleRate })).toBe(
      masteringAssistantSuggest(samples, sampleRate),
    );
    expect(masteringAudioProfile({ samples, sampleRate })).toBe(
      masteringAudioProfile(samples, sampleRate),
    );
    expect(masteringStreamingPreview({ samples, sampleRate })).toBe(
      masteringStreamingPreview(samples, sampleRate),
    );
  });
});

describe('WASM mastering request/positional error-path equivalence', () => {
  // Both call shapes share one private normalizer, so an invalid input must fail
  // identically either way — the success-path equivalence above does not prove
  // the error path stays in lockstep.
  function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
    try {
      fn();
      return { threw: false, message: '' };
    } catch (error) {
      return { threw: true, message: error instanceof Error ? error.message : String(error) };
    }
  }

  it('throws identically on an unknown processor name', () => {
    const reference = Float32Array.from(samples, (value) => value * 0.8);
    const positional = captureThrow(() =>
      masteringPairProcess('does.not.exist' as never, samples, reference, sampleRate),
    );
    const request = captureThrow(() =>
      masteringPairProcess({
        processorName: 'does.not.exist' as never,
        source: samples,
        reference,
        sampleRate,
      }),
    );
    expect(positional.threw).toBe(true);
    expect(request.threw).toBe(true);
    expect(request.message).toBe(positional.message);
  });

  it('throws identically on non-finite samples', () => {
    const bad = Float32Array.from(samples);
    bad[64] = Number.NaN;
    const positional = captureThrow(() =>
      masteringDynamicsCompressor(bad, sampleRate, { thresholdDb: -18 }),
    );
    const request = captureThrow(() =>
      masteringDynamicsCompressor({ samples: bad, sampleRate, thresholdDb: -18 }),
    );
    expect(positional.threw).toBe(true);
    expect(request.threw).toBe(true);
    expect(request.message).toBe(positional.message);
  });
});
