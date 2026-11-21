import { describe, expect, it, StreamingMasteringChain, setupWorklet } from './_worklet_helpers';

// The chain used to be reachable only from the main-thread entry, so a live
// preview had to round-trip audio out of the AudioWorklet realm. These cases
// pin the worklet entry actually carrying the class, and the contract a host
// has to honour once it does.
describe('StreamingMasteringChain from the worklet entry', () => {
  setupWorklet();

  const block = (frames: number): Float32Array => {
    const samples = new Float32Array(frames);
    for (let i = 0; i < frames; i += 1) {
      samples[i] = Math.sin((i / frames) * Math.PI * 8) * 0.25;
    }
    return samples;
  };

  it('prepares and processes inside the worklet realm', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 256,
      eq: { tilt: { tiltDb: 1.5 } },
    });
    try {
      expect(chain.stageNames()).toContain('eq.tilt');
      const input = block(256);
      const processed = chain.processMono(input);
      expect(processed).toHaveLength(256);
      expect(processed.some((value, i) => value !== input[i])).toBe(true);
      expect(processed.every((value) => Number.isFinite(value))).toBe(true);
      expect(chain.latencySamples).toBeGreaterThanOrEqual(0);
      const flushed = chain.flushMono();
      expect(flushed.every((value) => Number.isFinite(value))).toBe(true);
    } finally {
      chain.destroy();
    }
  });

  it('rejects a loudness stage without a precomputed static gain', () => {
    // Whole-signal integrated LUFS cannot be measured block by block, so the
    // host must supply the offline-measured gain or the chain refuses to build.
    expect(
      () =>
        new StreamingMasteringChain({
          sampleRate: 48000,
          numChannels: 1,
          loudness: { targetLufs: -14 },
        }),
    ).toThrow();
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      maxBlockSize: 256,
      numChannels: 2,
      loudness: { targetLufs: -14 },
      loudnessStaticGainDb: -3,
      loudnessStaticGainPeakDbtp: -1,
    });
    try {
      const left = block(256);
      const right = block(256);
      const processed = chain.processStereo(left, right);
      expect(processed.left).toHaveLength(256);
      expect(processed.right).toHaveLength(256);
      expect(processed.left.every((value) => Number.isFinite(value))).toBe(true);
    } finally {
      chain.destroy();
    }
  });
});
