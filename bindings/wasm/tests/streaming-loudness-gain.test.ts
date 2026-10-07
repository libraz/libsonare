import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringChain,
  masteringChainStereo,
  StreamingMasteringChain,
  streamingLoudnessGain,
  streamingLoudnessGainStereo,
} from '../dist/index.js';

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

describe('streamingLoudnessGain (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('equals the gain the offline chain applies, stages before loudness included', () => {
    const samples = program(0.05);
    for (const config of [{ loudness: { targetLufs: -14 } }, STAGED]) {
      const gain = streamingLoudnessGain({ samples, sampleRate: SR, config });
      const offline = masteringChain({ samples, sampleRate: SR, config });
      expect(gain.loudnessStaticGainDb).toBe(offline.appliedGainDb);
      expect(Number.isFinite(gain.integratedLufs)).toBe(true);
      expect(Number.isFinite(gain.truePeakDb)).toBe(true);
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
    const offline = masteringChainStereo({ left, right, sampleRate: SR, config: STAGED });
    expect(gain.loudnessStaticGainDb).toBe(offline.appliedGainDb);
  });

  it('yields 0 dB for silence and the numbers construct a streaming chain', () => {
    const gain = streamingLoudnessGain({
      samples: new Float32Array(SR),
      sampleRate: SR,
      config: STAGED,
    });
    expect(gain.loudnessStaticGainDb).toBe(0);
    expect(Number.isFinite(gain.integratedLufs)).toBe(false);
    const chain = new StreamingMasteringChain({
      ...STAGED,
      loudnessStaticGainDb: gain.loudnessStaticGainDb,
      loudnessStaticGainPeakDb: gain.truePeakDb,
    });
    try {
      chain.prepare(SR, 512, 1);
      expect(chain.processMono(new Float32Array(512)).length).toBe(512);
    } finally {
      chain.delete();
    }
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
        config: { 'no.such.key': 1 },
      }),
    ).toThrow();
  });
});
