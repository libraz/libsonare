import { describe, expect, it } from 'vitest';
import {
  masterAudio,
  masterAudioStereo,
  StreamingMasteringChain,
  streamingLoudnessGain,
  streamingLoudnessGainStereo,
} from '../src/index.js';
import { chainUnknownKeyText } from './_helpers.js';

const SR = 48000;

function program(amplitude: number, phase = 0): Float32Array {
  const out = new Float32Array(SR);
  for (let i = 0; i < out.length; i++) {
    const t = i / SR;
    out[i] =
      amplitude *
      (0.5 * Math.sin(2 * Math.PI * 220 * t + phase) + 0.3 * Math.sin(2 * Math.PI * 1760 * t));
  }
  return out;
}

const STAGED = {
  loudness: { targetLufs: -14 },
  eq: { tilt: { tiltDb: 3 } },
  dynamics: { compressor: { thresholdDb: -30, ratio: 4, makeupGainDb: 6 } },
};

describe('streamingLoudnessGain', () => {
  it('equals the gain the offline chain applies, stages before loudness included', () => {
    const samples = program(0.05);
    for (const config of [{ loudness: { targetLufs: -14 } }, STAGED]) {
      const gain = streamingLoudnessGain({ samples, sampleRate: SR, config });
      const offline = masterAudio({ samples, sampleRate: SR, overrides: config });
      expect(gain.loudnessStaticGainDb).toBe(offline.appliedGainDb);
      expect(Number.isFinite(gain.integratedLufs)).toBe(true);
      expect(Number.isFinite(gain.truePeakDbtp)).toBe(true);
    }
  });

  it('measures after the stages that precede loudness', () => {
    const samples = program(0.05);
    const staged = streamingLoudnessGain({ samples, sampleRate: SR, config: STAGED });
    const plain = streamingLoudnessGain({
      samples,
      sampleRate: SR,
      config: { loudness: { targetLufs: -14 } },
    });
    expect(staged.integratedLufs).toBeGreaterThan(plain.integratedLufs + 1);
  });

  it('has a stereo form that equals the offline stereo gain', () => {
    const left = program(0.04);
    const right = program(0.05, 1.3);
    const gain = streamingLoudnessGainStereo({ left, right, sampleRate: SR, config: STAGED });
    const offline = masterAudioStereo({ left, right, sampleRate: SR, overrides: STAGED });
    expect(gain.loudnessStaticGainDb).toBe(offline.appliedGainDb);
  });

  it('leaves the gain absent for silence, and the result constructs a chain that applies none', () => {
    const gain = streamingLoudnessGain({
      samples: new Float32Array(SR),
      sampleRate: SR,
      config: STAGED,
    });
    expect('loudnessStaticGainDb' in gain).toBe(false);
    expect(gain.integratedLufs).toBe(Number.NEGATIVE_INFINITY);
    expect(gain.truePeakDbtp).toBe(Number.NEGATIVE_INFINITY);

    // The silent result as the chain options, against an explicit 0 dB: both apply no gain.
    const block = program(0.05).subarray(0, 512);
    const fromSilence = new StreamingMasteringChain({
      ...STAGED,
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 512,
      loudnessStaticGainDb: gain.loudnessStaticGainDb,
      loudnessStaticGainPeakDbtp: gain.truePeakDbtp,
    });
    const unity = new StreamingMasteringChain({
      ...STAGED,
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 512,
      loudnessStaticGainDb: 0,
    });
    const lifted = new StreamingMasteringChain({
      ...STAGED,
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 512,
      loudnessStaticGainDb: 6,
    });
    const reference = Array.from(unity.processMono(block.slice()));
    expect(Array.from(fromSilence.processMono(block.slice()))).toEqual(reference);
    // The gain is read: 6 dB moves the same block.
    const moved = Array.from(lifted.processMono(block.slice()));
    expect(Math.max(...moved.map((value, i) => Math.abs(value - reference[i])))).toBeGreaterThan(
      0.01,
    );
  });

  it.each([
    ['loudnessStaticGainDb', Number.NaN],
    ['loudnessStaticGainDb', Number.POSITIVE_INFINITY],
    ['loudnessStaticGainPeakDbtp', Number.NaN],
    ['loudnessStaticGainPeakDbtp', Number.POSITIVE_INFINITY],
  ])('refuses a written non-finite %s (%s) by name', (key, value) => {
    const construct = () =>
      new StreamingMasteringChain({
        ...STAGED,
        sampleRate: SR,
        numChannels: 1,
        maxBlockSize: 512,
        loudnessStaticGainDb: 0,
        [key]: value,
      });
    expect(construct).toThrow(RangeError);
    expect(construct).toThrow(new RegExp(`\\b${key} must be finite`));
  });

  it('rejects mismatched channels and an unknown config key', () => {
    expect(() =>
      streamingLoudnessGainStereo({
        left: new Float32Array(100),
        right: new Float32Array(99),
        sampleRate: SR,
      }),
    ).toThrow(RangeError);
    expect(() =>
      streamingLoudnessGain({
        samples: new Float32Array(SR),
        sampleRate: SR,
        config: { 'no.such.key': 1 } as never,
      }),
    ).toThrow(
      new TypeError(chainUnknownKeyText('streamingLoudnessGain: unknown key config.no.such.key')),
    );
  });
});
