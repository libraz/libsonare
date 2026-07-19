import { describe, expect, it } from 'vitest';
import {
  masterAudio,
  masterAudioAsync,
  masterAudioStereo,
  masterAudioStereoAsync,
  mastering,
  masteringAssistantSuggest,
  masteringAudioProfile,
  masteringChain,
  masteringChainStereo,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringProcess,
  masteringProcessStereo,
  masteringRepairDeclick,
  masteringRepairDeclip,
  masteringRepairDecrackle,
  masteringRepairDehum,
  masteringRepairDenoiseClassical,
  masteringRepairDereverbClassical,
  masteringRepairTrimSilence,
  masteringStereoAnalyze,
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
  it('mastering preserves the positional result', () => {
    const samples = signal();
    const positional = mastering(samples, sampleRate, { targetLufs: -18 });
    const request = mastering({ samples, sampleRate, targetLufs: -18 });

    expect(request).toEqual(positional);
  });

  it('named processors preserve positional results', () => {
    const samples = signal();
    const positional = masteringProcess('dynamics.compressor', samples, sampleRate, { ratio: 2 });
    const request = masteringProcess({
      processorName: 'dynamics.compressor',
      samples,
      sampleRate,
      params: { ratio: 2 },
    });
    expect(request).toEqual(positional);

    const left = signal();
    const right = signal();
    expect(
      masteringProcessStereo({
        processorName: 'stereo.imager',
        left,
        right,
        sampleRate,
        params: { width: 1 },
      }),
    ).toEqual(masteringProcessStereo('stereo.imager', left, right, sampleRate, { width: 1 }));
  });

  it('mastering chains preserve positional results', () => {
    const samples = signal();
    expect(masteringChain({ samples, sampleRate })).toEqual(masteringChain(samples, sampleRate));

    const left = signal();
    const right = signal();
    expect(masteringChainStereo({ left, right, sampleRate })).toEqual(
      masteringChainStereo(left, right, sampleRate),
    );
  });

  it('masterAudio preserves the positional result', () => {
    const samples = signal();
    const positional = masterAudio(samples, sampleRate, 'pop');
    const request = masterAudio({ samples, sampleRate, preset: 'pop' });

    expect(request).toEqual(positional);
  });

  it('masterAudioStereo preserves the positional result', () => {
    const left = signal();
    const right = signal();
    const positional = masterAudioStereo(left, right, sampleRate, 'pop');
    const request = masterAudioStereo({ left, right, sampleRate, preset: 'pop' });

    expect(request).toEqual(positional);
  });

  it('async preset mastering accepts the request form', async () => {
    const samples = signal();
    const positional = await masterAudioAsync(samples, sampleRate, 'pop');
    const request = await masterAudioAsync({ samples, sampleRate, preset: 'pop' });

    expect(request).toEqual(positional);
  });

  it('async stereo preset mastering accepts the request form', async () => {
    const left = signal();
    const right = signal();
    const positional = await masterAudioStereoAsync(left, right, sampleRate, 'pop');
    const request = await masterAudioStereoAsync({ left, right, sampleRate, preset: 'pop' });

    expect(request).toEqual(positional);
  });

  it('normalization and dynamics processors preserve positional results', () => {
    const samples = signal();
    expect(normalize({ samples, sampleRate, targetDb: -2 })).toEqual(
      normalize(samples, sampleRate, -2),
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
    expect(masteringRepairTrimSilence({ samples, sampleRate, threshold: 0.01 })).toEqual(
      masteringRepairTrimSilence(samples, sampleRate, { threshold: 0.01 }),
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
    const source = signal();
    const reference = signal();

    const soloPositional = captureThrow(() =>
      masteringProcess('does.not.exist' as never, samples, sampleRate),
    );
    const soloRequest = captureThrow(() =>
      masteringProcess({ processorName: 'does.not.exist' as never, samples, sampleRate }),
    );
    expect(soloPositional.threw).toBe(true);
    expect(soloRequest.threw).toBe(true);
    expect(soloRequest.message).toBe(soloPositional.message);

    const pairPositional = captureThrow(() =>
      masteringPairProcess('does.not.exist' as never, source, reference, sampleRate),
    );
    const pairRequest = captureThrow(() =>
      masteringPairProcess({
        processorName: 'does.not.exist' as never,
        source,
        reference,
        sampleRate,
      }),
    );
    expect(pairPositional.threw).toBe(true);
    expect(pairRequest.threw).toBe(true);
    expect(pairRequest.message).toBe(pairPositional.message);

    const presetPositional = captureThrow(() =>
      masterAudio(samples, sampleRate, 'not-a-preset' as never),
    );
    const presetRequest = captureThrow(() =>
      masterAudio({ samples, sampleRate, preset: 'not-a-preset' as never }),
    );
    expect(presetPositional.threw).toBe(true);
    expect(presetRequest.threw).toBe(true);
    expect(presetRequest.message).toBe(presetPositional.message);
  });

  it('pair, stereo, and recommendation calls preserve positional results', () => {
    const source = signal();
    const reference = signal();
    expect(
      masteringPairProcess({
        processorName: 'match.abCrossfade',
        source,
        reference,
        sampleRate,
        params: { mix: 0.5 },
      }),
    ).toEqual(
      masteringPairProcess('match.abCrossfade', source, reference, sampleRate, { mix: 0.5 }),
    );
    expect(
      masteringPairAnalyze({
        analysisName: 'match.referenceLoudness',
        source,
        reference,
        sampleRate,
      }),
    ).toEqual(masteringPairAnalyze('match.referenceLoudness', source, reference, sampleRate));
    expect(
      masteringStereoAnalyze({
        analysisName: 'stereo.monoCompatCheck',
        left: source,
        right: reference,
        sampleRate,
      }),
    ).toEqual(masteringStereoAnalyze('stereo.monoCompatCheck', source, reference, sampleRate));
    expect(masteringAssistantSuggest({ samples: source, sampleRate })).toEqual(
      masteringAssistantSuggest(source, sampleRate),
    );
    expect(masteringAudioProfile({ samples: source, sampleRate })).toEqual(
      masteringAudioProfile(source, sampleRate),
    );
    const platforms = [{ name: 'Test', targetLufs: -14, ceilingDb: -1 }];
    expect(masteringStreamingPreview({ samples: source, sampleRate, platforms })).toEqual(
      masteringStreamingPreview(source, sampleRate, platforms),
    );
  });
});
