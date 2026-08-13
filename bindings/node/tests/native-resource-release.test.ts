/**
 * Every public class that owns a native handle must offer a deterministic way to
 * release it. Without one, a long-lived process that creates one per request
 * (an upload endpoint building a StreamAnalyzer, a preview socket building a
 * StreamingEqualizer) accumulates native memory that no JS-side action can
 * reclaim — the wrapper is garbage but uncollected, and the C++ object with it.
 *
 * The per-instance native footprint is large enough that the leak is measurable
 * rather than theoretical, so these tests compare RSS between a create+destroy
 * loop and a create-and-retain loop of the same size. `retainCount` is tuned per
 * class so the retain phase peaks around a few hundred MB.
 */

import { describe, expect, it } from 'vitest';
import {
  RealtimeVoiceChanger,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
} from '../src/index.js';

interface Releasable {
  destroy(): void;
  [Symbol.dispose](): void;
}

interface ReleasableCase {
  name: string;
  /** Builds an instance and exercises it, so the native buffers are allocated. */
  make: () => Releasable;
  /** A method that must throw once the handle is released. */
  useAfterDestroy: (instance: Releasable) => void;
  retainCount: number;
}

const block = (n: number): Float32Array =>
  new Float32Array(n).map((_, i) => 0.25 * Math.sin((2 * Math.PI * 440 * i) / 48000));

const CASES: readonly ReleasableCase[] = [
  {
    name: 'StreamAnalyzer',
    make: () => {
      const analyzer = new StreamAnalyzer({ sampleRate: 48000 });
      analyzer.process(block(4096));
      return analyzer;
    },
    useAfterDestroy: (instance) => (instance as StreamAnalyzer).process(block(256)),
    retainCount: 100,
  },
  {
    name: 'StreamingMasteringChain',
    make: () => {
      const chain = new StreamingMasteringChain({
        eq: { tilt: { tiltDb: 1.0 } },
        dynamics: { compressor: { thresholdDb: -18, ratio: 2 } },
        saturation: { tape: { driveDb: 2 } },
        stereo: { imager: { width: 1.1 } },
        maximizer: { truePeakLimiter: { ceilingDb: -1 } },
      });
      chain.prepare(48000, 1024, 2);
      chain.processStereo(block(1024), block(1024));
      return chain;
    },
    useAfterDestroy: (instance) =>
      (instance as StreamingMasteringChain).processStereo(block(1024), block(1024)),
    retainCount: 300,
  },
  {
    name: 'StreamingEqualizer',
    make: () => {
      const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
      eq.processMono(block(512));
      return eq;
    },
    useAfterDestroy: (instance) => (instance as StreamingEqualizer).processMono(block(512)),
    retainCount: 20,
  },
  {
    name: 'RealtimeVoiceChanger',
    make: () => {
      const changer = new RealtimeVoiceChanger({ sampleRate: 48000, maxBlockSize: 512 });
      changer.processMono(block(512));
      return changer;
    },
    useAfterDestroy: (instance) => (instance as RealtimeVoiceChanger).processMono(block(512)),
    retainCount: 400,
  },
];

const rss = (): number => process.memoryUsage().rss;

describe('native handles are released deterministically', () => {
  for (const testCase of CASES) {
    it(`${testCase.name}: destroy() is idempotent and disables the instance`, () => {
      const instance = testCase.make();
      instance.destroy();
      // A second release must not throw, so `finally { destroy() }` around a
      // `using` block or an early return stays safe.
      expect(() => {
        instance.destroy();
      }).not.toThrow();
      // The handle is really gone, not merely flagged: the native side rejects
      // any further use rather than touching freed state.
      expect(() => testCase.useAfterDestroy(instance)).toThrow();
    });

    it(`${testCase.name}: Symbol.dispose releases the same way`, () => {
      const instance = testCase.make();
      instance[Symbol.dispose]();
      expect(() => testCase.useAfterDestroy(instance)).toThrow();
    });

    it(`${testCase.name}: create+destroy does not accumulate native memory`, () => {
      const count = testCase.retainCount;
      // Warm up so first-touch allocation is not billed to the measured phase.
      for (let i = 0; i < 10; i++) {
        testCase.make().destroy();
      }

      const beforeDestroyed = rss();
      for (let i = 0; i < count; i++) {
        testCase.make().destroy();
      }
      const destroyedDelta = rss() - beforeDestroyed;

      let retained: Releasable[] = [];
      const beforeRetained = rss();
      for (let i = 0; i < count; i++) {
        retained.push(testCase.make());
      }
      const retainedDelta = rss() - beforeRetained;
      for (const instance of retained) {
        instance.destroy();
      }
      retained = [];

      // Self-calibrating: the retained loop shows what one loop's worth of live
      // native state costs, so the assertion does not hard-code a byte budget.
      // Measured margins are 30x-4000x, so a 4x factor is well clear of noise
      // while leaving room for allocator slack.
      expect(retainedDelta, `${testCase.name} retained too little to measure`).toBeGreaterThan(
        8 * 1024 * 1024,
      );
      expect(
        destroyedDelta,
        `${testCase.name}: create+destroy grew RSS by ${(destroyedDelta / 1e6).toFixed(1)} MB ` +
          `vs ${(retainedDelta / 1e6).toFixed(1)} MB retained — destroy() is not releasing`,
      ).toBeLessThan(retainedDelta / 4);
    });
  }
});
