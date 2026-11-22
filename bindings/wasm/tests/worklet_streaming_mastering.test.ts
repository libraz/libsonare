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

  it('builds a loudness stage without a static gain and applies no gain', () => {
    // The static gain is measured offline and is absent for a take with no measurable
    // loudness; a chain built without one applies none, exactly as a 0 dB gain does.
    const config = {
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 256,
      loudness: { targetLufs: -14 },
    };
    const ungained = new StreamingMasteringChain(config);
    const unity = new StreamingMasteringChain({ ...config, loudnessStaticGainDb: 0 });
    try {
      for (let b = 0; b < 8; b++) {
        const input = Float32Array.from(
          { length: 256 },
          (_, k) => 0.25 * Math.sin((b * 256 + k) * 0.03),
        );
        expect(Array.from(ungained.processMono(input))).toEqual(
          Array.from(unity.processMono(input)),
        );
      }
      // Control: a real gain moves the same input, so the equality above is not vacuous.
      const gained = new StreamingMasteringChain({ ...config, loudnessStaticGainDb: -12 });
      const probe = new Float32Array(256).fill(0.25);
      let a = probe;
      let c = probe;
      for (let b = 0; b < 16; b++) {
        a = ungained.processMono(probe);
        c = gained.processMono(probe);
      }
      expect(Math.abs(c[255])).toBeLessThan(Math.abs(a[255]) * 0.5);
      gained.destroy();
    } finally {
      ungained.destroy();
      unity.destroy();
    }
  });

  it('applies a precomputed static gain', () => {
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
