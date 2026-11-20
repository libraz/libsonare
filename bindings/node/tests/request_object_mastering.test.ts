import { describe, expect, it } from 'vitest';
import {
  masterAudio,
  masterAudioAsync,
  masterAudioStereo,
  masterAudioStereoAsync,
  masteringAnalyzePair,
  masteringAnalyzeStereo,
  masteringApplyPairProcessor,
  masteringApplyProcessor,
  masteringApplyProcessorStereo,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringNormalizeLoudness,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDenoiseClassical,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
  masteringStreamingPreview,
  normalize,
} from '../src/index.js';

const sampleRate = 22050;

function signal(): Float32Array {
  const samples = new Float32Array(sampleRate / 4);
  for (let i = 0; i < samples.length; i++) {
    samples[i] = 0.15 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
  }
  return samples;
}

describe('mastering request-object compatibility', () => {
  it('masteringNormalizeLoudness preserves the positional result', () => {
    const samples = signal();
    const positional = masteringNormalizeLoudness(samples, sampleRate, { targetLufs: -18 });
    const request = masteringNormalizeLoudness({ samples, sampleRate, targetLufs: -18 });

    expect(request).toEqual(positional);
  });

  it('named processors preserve positional results', () => {
    const samples = signal();
    const positional = masteringApplyProcessor(samples, sampleRate, {
      processor: 'dynamics.compressor',
      params: { ratio: 2 },
    });
    const request = masteringApplyProcessor({
      processor: 'dynamics.compressor',
      samples,
      sampleRate,
      params: { ratio: 2 },
    });
    expect(request).toEqual(positional);

    const left = signal();
    const right = signal().map((v, i) => v * (i % 3 === 0 ? -0.5 : 0.8));
    // A width away from the default 1, so a dropped params field cannot pass.
    const widened = masteringApplyProcessorStereo({
      processor: 'stereo.imager',
      left,
      right,
      sampleRate,
      params: { width: 1.6 },
    });
    expect(widened).not.toEqual(
      masteringApplyProcessorStereo({ processor: 'stereo.imager', left, right, sampleRate }),
    );
  });

  it('masterAudio without a preset runs the overrides alone, in either call shape', () => {
    const samples = signal();
    const empty = {};
    expect(masterAudio({ samples, sampleRate, overrides: empty })).toEqual(
      masterAudio(samples, sampleRate, { overrides: empty }),
    );
    const overrides = { loudness: { targetLufs: -14, ceilingDbtp: -1 } };
    expect(masterAudio({ samples, sampleRate, overrides })).toEqual(
      masterAudio(samples, sampleRate, { overrides }),
    );
  });

  it('exposes the before and after mastering report', () => {
    const result = masterAudio({
      samples: signal(),
      sampleRate,
      overrides: { loudness: { targetLufs: -14, ceilingDbtp: -1 } },
    });
    expect(result.report.before.integratedLufs).toBe(result.inputLufs);
    expect(result.report.after.integratedLufs).toBe(result.outputLufs);
    expect(result.report.after.truePeakDbtp).toBe(result.outputTruePeakDbtp);
    expect(result.report.bandEnergyDeltaDb).toBeInstanceOf(Float32Array);
    expect(result.report.bandEnergyDeltaDb).toHaveLength(32);
    expect(result.report.maxGainReductionDb).toBeLessThanOrEqual(0);
  });

  it('masterAudio preserves the positional result', () => {
    const samples = signal();
    const positional = masterAudio(samples, sampleRate, { preset: 'pop' });
    const request = masterAudio({ samples, sampleRate, preset: 'pop' });

    expect(request).toEqual(positional);
  });

  it('masterAudio takes a dotted override path as its nested spelling', () => {
    const samples = signal();
    const nested = masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      overrides: { loudness: { targetLufs: -20 } },
    });
    expect(nested.outputLufs).toBeCloseTo(-20, 1);
    const dotted = masterAudio({
      samples,
      sampleRate,
      preset: 'pop',
      overrides: { 'loudness.targetLufs': -20 } as never,
    });
    expect(dotted).toEqual(nested);
  });

  it('rejects an unknown override key', () => {
    const samples = signal();
    expect(() =>
      masterAudio({
        samples,
        sampleRate,
        preset: 'pop',
        overrides: { loudness: { nope: 1 } } as never,
      }),
    ).toThrow(/^masterAudio: unknown key overrides\.loudness\.nope$/);
  });

  it('masterAudioStereo takes a request object only', () => {
    const left = signal();
    const right = signal();
    const request = masterAudioStereo({ left, right, sampleRate, preset: 'pop' });
    expect(request.left).toHaveLength(left.length);
    expect(() =>
      (masterAudioStereo as (...args: unknown[]) => unknown)(left, right, sampleRate),
    ).toThrow(TypeError);
  });

  it('async preset mastering accepts the request form', async () => {
    const samples = signal();
    const positional = await masterAudioAsync(samples, sampleRate, { preset: 'pop' });
    const request = await masterAudioAsync({ samples, sampleRate, preset: 'pop' });

    expect(request).toEqual(positional);
  });

  it('async stereo preset mastering takes a request object only', async () => {
    const left = signal();
    const right = signal();
    const request = await masterAudioStereoAsync({ left, right, sampleRate, preset: 'pop' });

    expect(request.left).toHaveLength(left.length);
  });

  it('normalization and dynamics processors preserve positional results', () => {
    const samples = signal();
    expect(normalize({ samples, sampleRate, targetDb: -2 })).toEqual(
      normalize(samples, sampleRate, { targetDb: -2 }),
    );
    expect(
      masteringDynamicsCompressor({ samples, sampleRate, thresholdDb: -20, ratio: 2 }),
    ).toEqual(masteringDynamicsCompressor(samples, sampleRate, { thresholdDb: -20, ratio: 2 }));
    expect(masteringDynamicsGate({ samples, sampleRate, thresholdDb: -45 })).toEqual(
      masteringDynamicsGate(samples, sampleRate, { thresholdDb: -45 }),
    );
    expect(masteringDynamicsTransientShaper({ samples, sampleRate, attackGainDb: 2 })).toEqual(
      masteringDynamicsTransientShaper(samples, sampleRate, { attackGainDb: 2 }),
    );
  });

  it('repair processors preserve positional results', () => {
    const samples = signal();
    expect(masteringRepairDeclick({ samples, sampleRate, threshold: 0.2 })).toEqual(
      masteringRepairDeclick(samples, sampleRate, { threshold: 0.2 }),
    );
    expect(
      masteringRepairDenoiseClassical({ samples, sampleRate, nFft: 512, hopLength: 128 }),
    ).toEqual(masteringRepairDenoiseClassical(samples, sampleRate, { nFft: 512, hopLength: 128 }));
    expect(masteringRepairDeclip({ samples, sampleRate, iterations: 1 })).toEqual(
      masteringRepairDeclip(samples, sampleRate, { iterations: 1 }),
    );
    expect(masteringRepairDecrackle({ samples, sampleRate, threshold: 2 })).toEqual(
      masteringRepairDecrackle(samples, sampleRate, { threshold: 2 }),
    );
    expect(masteringRepairDehum({ samples, sampleRate, harmonics: 2 })).toEqual(
      masteringRepairDehum(samples, sampleRate, { harmonics: 2 }),
    );
    expect(
      masteringRepairDereverbClassical({ samples, sampleRate, nFft: 512, hopLength: 128 }),
    ).toEqual(masteringRepairDereverbClassical(samples, sampleRate, { nFft: 512, hopLength: 128 }));
    expect(masteringRepairTrimSilence({ samples, sampleRate, thresholdDb: -40 })).toEqual(
      masteringRepairTrimSilence(samples, sampleRate, { thresholdDb: -40 }),
    );
  });

  // The positional and request forms funnel through one private normalizer, so an
  // invalid input must fail identically either way. These assert the error path —
  // not just the success path above — stays equivalent across both call shapes.
  function captureThrow(fn: () => unknown): { threw: boolean; message: string } {
    try {
      fn();
      return { threw: false, message: '' };
    } catch (error) {
      return { threw: true, message: error instanceof Error ? error.message : String(error) };
    }
  }

  it('positional and request forms throw identically on invalid input', () => {
    const samples = signal();

    const soloPositional = captureThrow(() =>
      masteringApplyProcessor(samples, sampleRate, { processor: 'does.not.exist' as never }),
    );
    const soloRequest = captureThrow(() =>
      masteringApplyProcessor({ processor: 'does.not.exist' as never, samples, sampleRate }),
    );
    expect(soloPositional.threw).toBe(true);
    expect(soloRequest.threw).toBe(true);
    expect(soloRequest.message).toBe(soloPositional.message);

    const presetPositional = captureThrow(() =>
      masterAudio(samples, sampleRate, { preset: 'not-a-preset' as never }),
    );
    const presetRequest = captureThrow(() =>
      masterAudio({ samples, sampleRate, preset: 'not-a-preset' as never }),
    );
    expect(presetPositional.threw).toBe(true);
    expect(presetRequest.threw).toBe(true);
    expect(presetRequest.message).toBe(presetPositional.message);
  });

  it('pair processors and analyses are request-only and refuse an unknown name', () => {
    const source = signal();
    const reference = signal();
    const pair = captureThrow(() =>
      masteringApplyPairProcessor({
        processor: 'does.not.exist' as never,
        source,
        reference,
        sampleRate,
      }),
    );
    expect(pair.threw).toBe(true);
    expect(() =>
      (masteringApplyPairProcessor as (...args: unknown[]) => unknown)(
        'match.abCrossfade',
        source,
        reference,
        sampleRate,
      ),
    ).toThrow(TypeError);
    expect(() =>
      (masteringAnalyzePair as (...args: unknown[]) => unknown)(
        'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      ),
    ).toThrow(TypeError);
  });

  it('pair, stereo, and recommendation calls preserve positional results', () => {
    const source = signal();
    const reference = signal();
    expect(
      masteringApplyPairProcessor({
        processor: 'match.abCrossfade',
        source,
        reference,
        sampleRate,
        params: { mix: 0.5 },
      }).samples,
    ).toHaveLength(source.length);
    expect(
      masteringAnalyzePair({
        analysis: 'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      }),
    ).toHaveProperty('sourceLufs');
    expect(
      masteringAnalyzeStereo({
        analysis: 'stereo.monoCompatCheck',
        left: source,
        right: reference,
        sampleRate,
      }),
    ).toHaveProperty('correlation');
    expect(masteringAssistantSuggest({ samples: source, sampleRate })).toEqual(
      masteringAssistantSuggest(source, sampleRate),
    );
    expect(masteringAudioProfile({ samples: source, sampleRate })).toEqual(
      masteringAudioProfile(source, sampleRate),
    );
    const platforms = [{ name: 'Test', targetLufs: -14, ceilingDbtp: -1 }];
    expect(masteringStreamingPreview({ samples: source, sampleRate, platforms })).toEqual(
      masteringStreamingPreview(source, sampleRate, { platforms }),
    );
  });

  it('takes the true-peak ceiling as ceilingDbtp', () => {
    const hot = signal();
    const peakOf = (ceilingDbtp: number) =>
      Math.max(
        ...masteringNormalizeLoudness({ samples: hot, sampleRate, targetLufs: 0, ceilingDbtp })
          .samples,
      );
    expect(peakOf(-6)).toBeLessThan(peakOf(-1));
    expect(() =>
      masteringNormalizeLoudness({ samples: hot, sampleRate, ceilingDb: -1 } as never),
    ).toThrow(TypeError);
    expect(() =>
      masteringStreamingPreview({
        samples: hot,
        sampleRate,
        platforms: [{ name: 'Test', targetLufs: -14, ceilingDb: -1 } as never],
      }),
    ).toThrow(/ceilingDbtp/);
  });
});
