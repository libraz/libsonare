/**
 * Cross-surface boundary parity for WASM wrappers that call the C++ core
 * directly (the C-ABI TU is not linked into WASM). Each case pins a guard to the
 * behaviour the C ABI / Node / Python surfaces already enforce, so the WASM
 * surface cannot silently diverge.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringChain,
  masteringChainStereo,
  meteringTruePeakDb,
  phaseVocoder,
  RealtimeEngine,
  remix,
  voiceChangeRealtime,
} from '../src/index';

const SR = 22050;

function sine(n = 2048): Float32Array {
  const buf = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    buf[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  return buf;
}

function withNaN(n = 2048): Float32Array {
  const buf = sine(n);
  buf[100] = Number.NaN;
  return buf;
}

beforeAll(async () => {
  await init();
});

describe('phaseVocoder rate guard matches the C ABI (no upper cap)', () => {
  it('accepts a fast rate above the former cap of 100 (C ABI has no upper bound)', () => {
    const out = phaseVocoder(sine(8192), SR, 150);
    expect(out.length).toBeGreaterThan(0);
    // rate > 1 compresses the timeline, so the output is much shorter than input.
    expect(out.length).toBeLessThan(8192);
  });
  it('still rejects a non-positive rate', () => {
    expect(() => phaseVocoder(sine(), SR, 0)).toThrow(/finite positive/);
    expect(() => phaseVocoder(sine(), SR, -1)).toThrow(/finite positive/);
  });
  it('still rejects a non-finite rate before allocating an enormous buffer', () => {
    expect(() => phaseVocoder(sine(), SR, Number.NaN)).toThrow(/finite positive/);
    expect(() => phaseVocoder(sine(), SR, Number.POSITIVE_INFINITY)).toThrow(/finite positive/);
  });
});

describe('remix validates its input like the C ABI run_offline path', () => {
  it('rejects non-finite samples', () => {
    expect(() => remix(withNaN(), new Int32Array([0, 1024]), SR)).toThrow();
  });
  it('rejects an empty buffer', () => {
    expect(() => remix(new Float32Array(0), new Int32Array([0, 0]), SR)).toThrow();
  });
  it('rejects an out-of-range sample rate', () => {
    expect(() => remix(sine(), new Int32Array([0, 1024]), 100)).toThrow();
  });
  it('accepts valid input and returns a buffer', () => {
    const out = remix(sine(), new Int32Array([0, 1024]), SR);
    expect(out.length).toBe(1024);
  });
});

describe('masteringChain validates input on every entry (mono + stereo)', () => {
  it('mono chain rejects non-finite samples', () => {
    expect(() => masteringChain({ samples: withNaN(), sampleRate: SR })).toThrow();
  });
  it('mono chain rejects an empty buffer', () => {
    expect(() => masteringChain({ samples: new Float32Array(0), sampleRate: SR })).toThrow();
  });
  it('stereo chain rejects non-finite samples', () => {
    expect(() =>
      masteringChainStereo({ left: withNaN(), right: sine(), sampleRate: SR }),
    ).toThrow();
  });
  it('mono chain accepts valid input', () => {
    const r = masteringChain({ samples: sine(SR), sampleRate: SR });
    expect(r.samples.length).toBe(SR);
  });
});

describe('meteringTruePeakDb oversample-factor guard', () => {
  it('accepts 0 (meaning the default 4) and valid powers of two', () => {
    expect(Number.isFinite(meteringTruePeakDb(sine(), SR, 0))).toBe(true);
    expect(Number.isFinite(meteringTruePeakDb(sine(), SR, 8))).toBe(true);
  });
  it('rejects a non-power-of-two factor', () => {
    expect(() => meteringTruePeakDb(sine(), SR, 3)).toThrow(/power of two/);
  });
  it('rejects a fractional factor', () => {
    expect(() => meteringTruePeakDb(sine(), SR, 2.5)).toThrow(/power of two/);
  });
  it('rejects a factor above 16', () => {
    expect(() => meteringTruePeakDb(sine(), SR, 32)).toThrow(/power of two/);
  });
});

describe('voiceChangeRealtime empty-input guard', () => {
  it('rejects an empty buffer', () => {
    expect(() => voiceChangeRealtime(new Float32Array(0), 48000, 'neutral-monitor')).toThrow();
  });
});

describe('addParameter range guard matches the C ABI', () => {
  const ordinary = () => ({
    id: 21,
    name: 'gain',
    unit: 'dB',
    minValue: -60,
    maxValue: 6,
    defaultValue: 0,
    rtSafe: true,
    defaultCurve: 1,
  });

  // Driven one field at a time: a descriptor carrying all three cannot say which
  // one the guard caught, and a NaN makes the inverted-range comparison false.
  for (const field of ['minValue', 'maxValue', 'defaultValue'] as const) {
    for (const [label, bad] of [
      ['NaN', Number.NaN],
      ['+Infinity', Number.POSITIVE_INFINITY],
      ['-Infinity', Number.NEGATIVE_INFINITY],
    ] as const) {
      it(`rejects ${label} on ${field}`, () => {
        const engine = new RealtimeEngine(48000, 128);
        const info = ordinary();
        info[field] = bad;
        expect(() => engine.addParameter(info)).toThrow(/finite/);
        expect(engine.parameterCount()).toBe(0);
      });
    }
  }

  it('still rejects an inverted finite range', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.addParameter({ ...ordinary(), minValue: 1, maxValue: 0 })).toThrow(
      /maxValue must be >= minValue/,
    );
    expect(engine.parameterCount()).toBe(0);
  });

  it('accepts a degenerate but equal finite range', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.addParameter({ ...ordinary(), minValue: 0.5, maxValue: 0.5, defaultValue: 0.5 });
    expect(engine.parameterCount()).toBe(1);
  });

  it('accepts a finite default outside the declared range', () => {
    // minValue/maxValue are descriptive metadata the engine never clamps to.
    const engine = new RealtimeEngine(48000, 128);
    engine.addParameter({ ...ordinary(), defaultValue: 12 });
    expect(engine.parameterCount()).toBe(1);
  });

  it('registers an ordinary parameter', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.addParameter(ordinary());
    expect(engine.parameterCount()).toBe(1);
  });
});

describe('setClipPagePrefetchFrames rejects a value the int64 cast cannot take', () => {
  // The guard's boundary. Written as powers of two because the decimal spellings
  // of these doubles do not round-trip through a source literal.
  const TWO_POW_63 = 2 ** 63;
  const LARGEST_BELOW_TWO_POW_63 = TWO_POW_63 - 1024;

  it('rejects +Infinity and NaN', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClipPagePrefetchFrames(Number.POSITIVE_INFINITY)).toThrow();
    expect(() => engine.setClipPagePrefetchFrames(Number.NaN)).toThrow();
  });
  it('rejects a finite value the int64 cast cannot take', () => {
    // INT64_MAX is not representable as a double, so a bound spelled from it
    // would admit exactly 2^63 — the first value whose cast is undefined.
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.setClipPagePrefetchFrames(TWO_POW_63)).toThrow();
    expect(() => engine.setClipPagePrefetchFrames(1e30)).toThrow();
  });

  it('accepts the largest double below 2^63', () => {
    // The double immediately below the bound: without this the rejection above
    // is satisfied by a guard set a power of two too low.
    const engine = new RealtimeEngine(48000, 128);
    engine.setClipPagePrefetchFrames(LARGEST_BELOW_TWO_POW_63);
    expect(engine.clipPagePrefetchFrames()).toBe(LARGEST_BELOW_TWO_POW_63);
  });

  it('accepts a finite non-negative window', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setClipPagePrefetchFrames(4096);
    expect(engine.clipPagePrefetchFrames()).toBe(4096);
  });
});
