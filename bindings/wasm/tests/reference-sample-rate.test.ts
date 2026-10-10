/**
 * A reference (or track) at another rate is resampled onto the source rate.
 *
 * Each case compares a call given the reference at its own rate against the same
 * call given the reference pre-resampled by this surface's `resample`. Both reach
 * one resampler, so the two agree exactly; the control reads the raw reference at
 * the source rate, which a call ignoring the rate would also do.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringAbMatchLoudness,
  masteringAbMatchLoudnessStereo,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessStereo,
  resample,
  StreamingEqualizer,
  suggestMixSceneJson,
} from '../src/index';

beforeAll(async () => {
  await init();
});

const SR = 48000;
const REF_SR = 32000;

function noise(length: number, seed: number, gain: number): Float32Array {
  const out = new Float32Array(length);
  let state = seed >>> 0;
  for (let i = 0; i < length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = gain * ((state >>> 8) / (1 << 24) - 0.5);
  }
  return out;
}

function sine(freq: number, sampleRate: number, seconds: number, amp: number): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / sampleRate);
  }
  return out;
}

const SOURCE = noise(24000, 7, 0.4);
const REFERENCE = noise(16000, 11, 0.6);
let RESAMPLED: Float32Array;

beforeAll(() => {
  RESAMPLED = resample(REFERENCE, REF_SR, SR);
});

describe('pair entries resample a reference at another rate', () => {
  it('mono processor', () => {
    const params = { mix: 0.5 };
    const expected = masteringPairProcess({
      processorName: 'match.abCrossfade',
      source: SOURCE,
      reference: RESAMPLED,
      sampleRate: SR,
      params,
    });
    const actual = masteringPairProcess({
      processorName: 'match.abCrossfade',
      source: SOURCE,
      reference: REFERENCE,
      sampleRate: SR,
      referenceSampleRate: REF_SR,
      params,
    });
    expect(actual.sampleRate).toBe(SR);
    expect(actual.samples).toEqual(expected.samples);
    const misread = masteringPairProcess('match.abCrossfade', SOURCE, REFERENCE, SR, params);
    expect(misread.samples).not.toEqual(expected.samples);
  });

  it('stereo processor', () => {
    const base = {
      processorName: 'match.abCrossfade' as const,
      sourceLeft: SOURCE,
      sourceRight: SOURCE,
      sampleRate: SR,
      params: { mix: 0.25 },
    };
    const expected = masteringPairProcessStereo({
      ...base,
      referenceLeft: RESAMPLED,
      referenceRight: RESAMPLED,
    });
    const actual = masteringPairProcessStereo({
      ...base,
      referenceLeft: REFERENCE,
      referenceRight: REFERENCE,
      referenceSampleRate: REF_SR,
    });
    expect(actual.left).toEqual(expected.left);
    expect(actual.right).toEqual(expected.right);
    const misread = masteringPairProcessStereo({
      ...base,
      referenceLeft: REFERENCE,
      referenceRight: REFERENCE,
    });
    expect(misread.left).not.toEqual(expected.left);
  });

  it.each(['match.referenceLoudness', 'match.matchEqCurve'] as const)('analysis %s', (name) => {
    const expected = masteringPairAnalyze({
      analysisName: name,
      source: SOURCE,
      reference: RESAMPLED,
      sampleRate: SR,
    });
    const actual = masteringPairAnalyze({
      analysisName: name,
      source: SOURCE,
      reference: REFERENCE,
      sampleRate: SR,
      referenceSampleRate: REF_SR,
    });
    expect(actual).toBe(expected);
    expect(masteringPairAnalyze(name, SOURCE, REFERENCE, SR)).not.toBe(expected);
  });

  it('reads a delay back in source-rate samples', () => {
    const signal = noise(16000, 23, 0.5);
    const reference = new Float32Array(signal.length + 100);
    reference.set(signal, 100);
    const json = masteringPairAnalyze({
      analysisName: 'match.estimateReferenceDelaySamples',
      source: resample(signal, REF_SR, SR),
      reference,
      sampleRate: SR,
      referenceSampleRate: REF_SR,
    });
    const { delaySamples } = JSON.parse(json) as { delaySamples: number };
    expect(Math.abs(delaySamples - (100 * SR) / REF_SR)).toBeLessThanOrEqual(1);
  });

  it('equal rates are the omitted rate bit for bit', () => {
    const request = {
      analysisName: 'match.referenceLoudness' as const,
      source: SOURCE,
      reference: REFERENCE,
      sampleRate: SR,
    };
    expect(masteringPairAnalyze({ ...request, referenceSampleRate: SR })).toBe(
      masteringPairAnalyze(request),
    );
  });

  it('A/B loudness match, mono and stereo', () => {
    const source = sine(220, SR, 1, 0.05);
    const reference = noise(48000, 5, 0.8);
    const resampled = resample(reference, REF_SR, SR);
    const expected = masteringAbMatchLoudness({ source, reference: resampled, sampleRate: SR });
    const actual = masteringAbMatchLoudness({
      source,
      reference,
      sampleRate: SR,
      referenceSampleRate: REF_SR,
    });
    expect(actual).toEqual(expected);
    expect(masteringAbMatchLoudness({ source, reference, sampleRate: SR }).referenceLufs).not.toBe(
      expected.referenceLufs,
    );

    const right = source.map((v) => -0.5 * v);
    const stereoExpected = masteringAbMatchLoudnessStereo({
      sourceLeft: source,
      sourceRight: right,
      referenceLeft: resampled,
      referenceRight: resampled,
      sampleRate: SR,
    });
    const stereo = masteringAbMatchLoudnessStereo({
      sourceLeft: source,
      sourceRight: right,
      referenceLeft: reference,
      referenceRight: reference,
      sampleRate: SR,
      referenceSampleRate: REF_SR,
    });
    expect(stereo).toEqual(stereoExpected);
  });

  it('refuses a reference rate the way it refuses the sample rate', () => {
    const call = (sampleRate: number, referenceSampleRate?: number) => () =>
      masteringPairAnalyze({
        analysisName: 'match.referenceLoudness',
        source: SOURCE,
        reference: REFERENCE,
        sampleRate,
        referenceSampleRate,
      });
    for (const bad of [0, -1]) {
      expect(call(bad)).toThrow(RangeError);
      expect(call(SR, bad)).toThrow(RangeError);
      expect(call(SR, bad)).toThrow(/referenceSampleRate/);
    }
  });
});

describe('StreamingEqualizer.match resamples a reference at another rate', () => {
  const probe = (reference: Float32Array, options: Record<string, number> = {}) => {
    const eq = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 4096 });
    try {
      eq.match(sine(1000, SR, 1, 0.1), reference, { maxBands: 6, ...options });
      return eq.processMono(noise(4096, 99, 0.5));
    } finally {
      eq.destroy();
    }
  };

  it('equals the pre-resampled reference, and unequal lengths are accepted', () => {
    const reference = noise(20000, 31, 0.7);
    const expected = probe(resample(reference, REF_SR, SR));
    expect(probe(reference, { referenceSampleRate: REF_SR })).toEqual(expected);
    expect(probe(reference)).not.toEqual(expected);
    expect(probe(reference, { sampleRate: SR, referenceSampleRate: SR })).toEqual(probe(reference));
  });

  it('refuses a non-positive reference rate', () => {
    for (const bad of [0, -1]) {
      expect(() => probe(REFERENCE, { referenceSampleRate: bad })).toThrow();
    }
  });
});

describe('mixing assistant resamples a track at another rate', () => {
  const tracks = (keys: Float32Array, sampleRate?: number) => [
    { id: 'bass', name: 'bass', left: sine(110, SR, 0.8, 0.3) },
    { id: 'keys', name: 'keys', left: keys, sampleRate },
  ];

  it('equals the pre-resampled track', () => {
    const keys = sine(660, REF_SR, 0.8, 0.2);
    const expected = suggestMixSceneJson({
      tracks: tracks(resample(keys, REF_SR, SR)),
      sampleRate: SR,
    });
    expect(suggestMixSceneJson({ tracks: tracks(keys, REF_SR), sampleRate: SR })).toBe(expected);
    expect(suggestMixSceneJson({ tracks: tracks(keys), sampleRate: SR })).not.toBe(expected);
    expect(suggestMixSceneJson({ tracks: tracks(keys, SR), sampleRate: SR })).toBe(
      suggestMixSceneJson({ tracks: tracks(keys), sampleRate: SR }),
    );
  });

  it('refuses a track rate the way it refuses the sample rate', () => {
    const keys = sine(660, SR, 0.8, 0.2);
    for (const bad of [0, -1]) {
      expect(() => suggestMixSceneJson({ tracks: tracks(keys), sampleRate: bad })).toThrow(
        RangeError,
      );
      expect(() => suggestMixSceneJson({ tracks: tracks(keys, bad), sampleRate: SR })).toThrow(
        /tracks\[1\]\.sampleRate/,
      );
    }
  });
});
