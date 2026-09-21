/**
 * The three streaming processors the WASM module exposes: the mastering chain,
 * the equalizer and the retune, each driven block by block.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
} from '../dist/index.js';

describe('Sonare WASM Module', () => {
  beforeAll(async () => {
    await init();
  });

  describe('mastering', () => {
    it('should stream a mono block through StreamingMasteringChain', () => {
      const chain = new StreamingMasteringChain({
        eq: { tiltDb: 1.0 },
      });
      try {
        chain.prepare(44100, 512, 1);
        const block = new Float32Array(512);
        for (let i = 0; i < block.length; i += 1) {
          block[i] = 0.1;
        }
        const out = chain.processMono(block);
        expect(out).toBeInstanceOf(Float32Array);
        expect(out.length).toBe(block.length);
        // tilt EQ should modify the constant signal at least somewhere
        const stages = chain.stageNames();
        expect(stages).toContain('eq.tilt');
      } finally {
        chain.delete();
      }
    });

    it('flushes delayed StreamingMasteringChain output once', () => {
      const chain = new StreamingMasteringChain({
        maximizer: { truePeakLimiter: { enabled: true } },
      });
      try {
        chain.prepare(48000, 64, 1);
        expect(chain.latencySamples()).toBeGreaterThan(0);
        chain.processMono(new Float32Array(64).fill(0.5));
        const tail: number[] = [];
        for (;;) {
          const block = chain.flushMono();
          if (block.length === 0) {
            break;
          }
          tail.push(...block);
        }
        expect(tail.length).toBeGreaterThanOrEqual(chain.latencySamples());
        expect(chain.flushMono()).toHaveLength(0);
      } finally {
        chain.delete();
      }
    });

    it('uses the canonical flat parser for nested streaming-chain configuration', () => {
      const chain = new StreamingMasteringChain({
        repair: { denoise: { enabled: false } },
        eq: { tilt: { enabled: true, tiltDb: 2.0, pivotHz: 1500 } },
      });
      try {
        chain.prepare(44100, 512, 1);
        expect(chain.stageNames()).toEqual(['eq.tilt']);
      } finally {
        chain.delete();
      }
    });

    it('should construct a loudness-enabled StreamingMasteringChain with a static gain', () => {
      // A loudness-enabled config normally throws at construction (whole-signal
      // LUFS is unavailable while streaming). Supplying a precomputed
      // loudnessStaticGainDb must let it construct, prepare, and process.
      const chain = new StreamingMasteringChain({
        eq: { tiltDb: 1.0 },
        loudness: { targetLufs: -18, ceilingDb: -1, truePeakOversample: 4 },
        loudnessStaticGainDb: 3.0,
        loudnessStaticGainPeakDb: -6.0,
      });
      try {
        chain.prepare(44100, 512, 1);
        const block = new Float32Array(512);
        for (let i = 0; i < block.length; i += 1) {
          block[i] = 0.1;
        }
        const out = chain.processMono(block);
        expect(out).toBeInstanceOf(Float32Array);
        expect(out.length).toBe(block.length);
        // The loudness stage is wired in once a static gain is provided.
        expect(chain.stageNames()).toContain('loudness.optimize');
      } finally {
        chain.delete();
      }
    });

    it('should still throw for a loudness-enabled chain without a static gain', () => {
      expect(
        () =>
          new StreamingMasteringChain({
            loudness: { targetLufs: -18, ceilingDb: -1, truePeakOversample: 4 },
          }),
      ).toThrow();
    });

    it('should stream stereo blocks through StreamingEqualizer', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        eq.setBand(0, {
          type: 'HighShelf',
          frequencyHz: 8000,
          gainDb: 6,
          enabled: true,
        });
        eq.setGainScale(0.5);
        eq.setOutputGainDb(3);
        eq.setOutputPan(0);

        const length = 512;
        const left = new Float32Array(length);
        const right = new Float32Array(length);
        for (let i = 0; i < length; i += 1) {
          const value = Math.sin((2 * Math.PI * 1000 * i) / 48000) * 0.5;
          left[i] = value;
          right[i] = value;
        }

        const firstSeq = eq.spectrum().seq;
        const out = eq.processStereo(left, right);
        expect(out.left).toBeInstanceOf(Float32Array);
        expect(out.right).toBeInstanceOf(Float32Array);
        expect(out.left.length).toBe(length);
        expect(out.right.length).toBe(length);

        const snapshot = eq.spectrum();
        expect(snapshot.seq).toBeGreaterThan(firstSeq);
        expect(snapshot.bandGainDb.length).toBe(24);
        expect(snapshot.bandGainDb[0]).toBeGreaterThan(2.5);
        expect(snapshot.bandGainDb[0]).toBeLessThan(3.5);
        expect(snapshot.profileDb.length).toBe(16);
        expect(snapshot.preLeft.length).toBe(snapshot.postLeft.length);
        expect(eq.latencySamples()).toBeGreaterThanOrEqual(0);
      } finally {
        eq.delete();
      }
    });

    it('reports exactly one discard for the block that carried a non-finite sample', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        eq.setBand(0, {
          type: 'Peak',
          frequencyHz: 1000,
          gainDb: 6,
          enabled: true,
        });

        const length = 512;
        const left = new Float32Array(length);
        const right = new Float32Array(length);
        for (let i = 0; i < length; i += 1) {
          const value = Math.sin((2 * Math.PI * 1000 * i) / 48000) * 0.5;
          left[i] = value;
          right[i] = value;
        }

        // Clean block: the enabled band is actually filtering, so the zero
        // discard count read below is not vacuous.
        const cleanOut = eq.processStereo(left, right);
        expect(cleanOut.left.some((v, i) => v !== left[i])).toBe(true);
        expect(eq.nonFiniteDiscardCount()).toBe(0);

        // This EQ does not scrub its input at all (unlike the mixer's
        // process_stereo boundary), so a NaN sample reaches the band's IIR
        // state directly and discards within this same block.
        const poisonedLeft = left.slice();
        const poisonedRight = right.slice();
        poisonedLeft[100] = Number.NaN;
        poisonedRight[100] = Number.NaN;
        eq.processStereo(poisonedLeft, poisonedRight);
        // Exactly one: the unit is one processed block, never a channel --
        // both channels carried the poison in this same block.
        expect(eq.nonFiniteDiscardCount()).toBe(1);

        // A further clean block adds nothing more: the EQ has recovered.
        eq.processStereo(left, right);
        expect(eq.nonFiniteDiscardCount()).toBe(1);
      } finally {
        eq.delete();
      }
    });

    it('should draw the magnitude curve the bands actually apply', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        eq.setBand(0, {
          type: 'Peak',
          frequencyHz: 1000,
          gainDb: 6,
          q: 1.5,
          enabled: true,
        });
        const db = eq.magnitudeResponse(new Float32Array([100, 1000, 10000]));
        expect(db).toBeInstanceOf(Float32Array);
        expect(db.length).toBe(3);
        expect(db[1]).toBeGreaterThan(5.5);
        expect(db[1]).toBeLessThan(6.5);
        expect(Math.abs(db[0])).toBeLessThan(1);
        expect(Math.abs(db[2])).toBeLessThan(1);
      } finally {
        eq.delete();
      }
    });

    it('should keep a mid band off the left curve', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        eq.setBand(0, {
          type: 'Peak',
          frequencyHz: 1000,
          gainDb: 9,
          q: 1.5,
          enabled: true,
          placement: 'Mid',
        });
        const freqs = new Float32Array([1000]);
        expect(eq.magnitudeResponse(freqs, 'Mid')[0]).toBeGreaterThan(6);
        expect(Math.abs(eq.magnitudeResponse(freqs, 'Left')[0])).toBeLessThan(0.5);
        expect(() => eq.magnitudeResponse(freqs, 'Bogus' as unknown as 'Mid')).toThrow();
      } finally {
        eq.delete();
      }
    });

    it('should read an all-pass band as flat', () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        eq.setBand(0, { type: 'AllPass', frequencyHz: 1000, q: 0.7, enabled: true });
        const db = eq.magnitudeResponse(new Float32Array([100, 1000, 10000]));
        for (const value of db) {
          expect(Math.abs(value)).toBeLessThan(0.05);
        }
      } finally {
        eq.delete();
      }
    });

    it('should accept string phase modes for StreamingEqualizer', () => {
      const linearEq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      const aliasEq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      try {
        linearEq.setPhaseMode('linear');
        linearEq.setBand(0, {
          type: 'Peak',
          frequencyHz: 1000,
          gainDb: 3,
          q: 1,
          enabled: true,
        });
        expect(linearEq.latencySamples()).toBeGreaterThan(0);

        expect(() => aliasEq.setPhaseMode('zero-latency')).not.toThrow();
        aliasEq.setBand(0, {
          type: 'HighShelf',
          frequencyHz: 8000,
          gainDb: 2,
          enabled: true,
        });
        expect(aliasEq.latencySamples()).toBeGreaterThanOrEqual(0);
      } finally {
        linearEq.delete();
        aliasEq.delete();
      }
    });

    it('should use the prepared sample rate for StreamingEqualizer.match defaults', () => {
      const sampleRate = 44100;
      const length = Math.floor(sampleRate * 0.25);
      const source = new Float32Array(length);
      const reference = new Float32Array(length);
      for (let i = 0; i < length; i += 1) {
        source[i] = Math.sin((2 * Math.PI * 1000 * i) / sampleRate);
        reference[i] = Math.sin((2 * Math.PI * 2000 * i) / sampleRate);
      }
      const omitted = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      const explicit = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      try {
        omitted.match(source, reference, { maxBands: 6 });
        explicit.match(source, reference, { sampleRate, maxBands: 6 });
        const omittedGain = Array.from(omitted.spectrum().bandGainDb);
        const explicitGain = Array.from(explicit.spectrum().bandGainDb);
        expect(omittedGain.length).toBe(explicitGain.length);
        for (let i = 0; i < omittedGain.length; i += 1) {
          expect(omittedGain[i]).toBeCloseTo(explicitGain[i], 6);
        }
      } finally {
        omitted.delete();
        explicit.delete();
      }
    });

    it('should stream mono blocks through StreamingRetune', () => {
      const retune = new StreamingRetune({ semitones: 12, mix: 1, grainSize: 512 });
      try {
        retune.prepare(48000, 128);
        expect(retune.config().semitones).toBe(12);
        expect(retune.grainSize()).toBe(512);

        const block = new Float32Array(128);
        for (let i = 0; i < block.length; i += 1) {
          block[i] = Math.sin((2 * Math.PI * 220 * i) / 48000) * 0.5;
        }

        const out = retune.processMono(block);
        expect(out).toBeInstanceOf(Float32Array);
        expect(out.length).toBe(block.length);
        expect(Array.from(out).every(Number.isFinite)).toBe(true);

        retune.setConfig({ semitones: -5, mix: 0.5, grainSize: 512 });
        expect(retune.config().mix).toBeCloseTo(0.5, 6);
      } finally {
        retune.delete();
      }
    });
  });
});
