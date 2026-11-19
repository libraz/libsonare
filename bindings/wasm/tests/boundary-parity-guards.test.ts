/**
 * Cross-surface boundary parity for WASM wrappers that call the C++ core
 * directly (the C-ABI TU is not linked into WASM). Each case pins a guard to the
 * behaviour the C ABI / Node / Python surfaces already enforce, so the WASM
 * surface cannot silently diverge.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masterAudio,
  masterAudioStereo,
  meteringTruePeakDbtp,
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

describe('phaseVocoder speedRatio guard matches the C ABI (no upper cap)', () => {
  it('accepts a fast speedRatio above the former cap of 100 (C ABI has no upper bound)', () => {
    const out = phaseVocoder({ samples: sine(8192), sampleRate: SR, speedRatio: 150 });
    expect(out.length).toBeGreaterThan(0);
    // speedRatio > 1 compresses the timeline, so the output is much shorter than input.
    expect(out.length).toBeLessThan(8192);
  });
  it('still rejects a non-positive speedRatio', () => {
    // Finite, so it gets past the boundary reader and the domain guard answers
    // it. The wording is that guard's own and says only what it checked: the
    // finiteness of the rate is a separate question, answered below.
    expect(() => phaseVocoder({ samples: sine(), sampleRate: SR, speedRatio: 0 })).toThrow(
      /phaseVocoder: speedRatio must be finite and greater than 0$/,
    );
    expect(() => phaseVocoder({ samples: sine(), sampleRate: SR, speedRatio: -1 })).toThrow(
      /phaseVocoder: speedRatio must be finite and greater than 0$/,
    );
  });
  it('still rejects a non-finite speedRatio before allocating an enormous buffer', () => {
    // Answered by the facade rather than the core, which is what the addon does
    // with the same value: the wording is the shared assertFiniteScalar's, not
    // the core's "finite positive". Pinning the core's wording here asserted a
    // message no other surface produces, which is the divergence this file
    // exists to prevent.
    expect(() => phaseVocoder({ samples: sine(), sampleRate: SR, speedRatio: Number.NaN })).toThrow(
      /phaseVocoder: speedRatio must be finite/,
    );
    expect(() =>
      phaseVocoder({ samples: sine(), sampleRate: SR, speedRatio: Number.POSITIVE_INFINITY }),
    ).toThrow(/phaseVocoder: speedRatio must be finite/);
  });
});

describe('remix validates its input like the C ABI run_offline path', () => {
  it('rejects non-finite samples', () => {
    expect(() =>
      remix({ samples: withNaN(), intervals: new Int32Array([0, 1024]), sampleRate: SR }),
    ).toThrow();
  });
  it('rejects an empty buffer', () => {
    expect(() =>
      remix({ samples: new Float32Array(0), intervals: new Int32Array([0, 0]), sampleRate: SR }),
    ).toThrow();
  });
  it('rejects an out-of-range sample rate', () => {
    expect(() =>
      remix({ samples: sine(), intervals: new Int32Array([0, 1024]), sampleRate: 100 }),
    ).toThrow();
  });
  it('accepts valid input and returns a buffer', () => {
    const out = remix({
      samples: sine(),
      intervals: new Int32Array([0, 1024]),
      sampleRate: SR,
      alignZeros: false,
    });
    expect(out.length).toBe(1024);
  });

  // The boundary is an ordinal, so folding one onto the next legal index cuts
  // the slice somewhere the caller never named and nothing downstream can tell
  // the two apart. The control is in the same case: without it a refusal would
  // pass here even if the argument had stopped selecting anything.
  it('refuses a fractional boundary rather than truncating it onto a legal index', () => {
    expect(
      remix({ samples: sine(), intervals: [0, 1024], sampleRate: SR, alignZeros: false }).length,
    ).toBe(1024);
    expect(
      remix({ samples: sine(), intervals: [0, 512], sampleRate: SR, alignZeros: false }).length,
    ).toBe(512);
    expect(() => remix({ samples: sine(), intervals: [0, 1024.7], sampleRate: SR })).toThrow(
      /intervals\[1\] must be an integer/,
    );
  });

  it('refuses a boundary past the native int range, naming the range', () => {
    expect(() => remix({ samples: sine(), intervals: [0, 2 ** 31], sampleRate: SR })).toThrow(
      /intervals\[1\] must be an integer in \[-2147483648, 2147483647\]/,
    );
  });

  // A non-number entry is the wrong type rather than the wrong domain, and the
  // class is the only thing carrying that difference — the message is the same
  // one a fraction gets. The fractional case repeats here as the discriminator:
  // without it, `toThrow(TypeError)` would also pass if everything threw one.
  it('reports a non-number boundary as a type error, not a range error', () => {
    expect(
      remix({ samples: sine(), intervals: [0, 1024], sampleRate: SR, alignZeros: false }).length,
    ).toBe(1024);
    expect(() =>
      remix({ samples: sine(), intervals: [0, '1024'] as unknown as number[], sampleRate: SR }),
    ).toThrow(TypeError);
    expect(() => remix({ samples: sine(), intervals: [0, 1024.7], sampleRate: SR })).toThrow(
      RangeError,
    );
  });
});

describe('masterAudio without a preset validates input on every entry (mono + stereo)', () => {
  it('mono chain rejects non-finite samples', () => {
    expect(() => masterAudio({ samples: withNaN(), sampleRate: SR, overrides: {} })).toThrow();
  });
  it('mono chain rejects an empty buffer', () => {
    expect(() =>
      masterAudio({ samples: new Float32Array(0), sampleRate: SR, overrides: {} }),
    ).toThrow();
  });
  it('stereo chain rejects non-finite samples', () => {
    expect(() =>
      masterAudioStereo({ left: withNaN(), right: sine(), sampleRate: SR, overrides: {} }),
    ).toThrow();
  });
  it('mono chain accepts valid input', () => {
    const r = masterAudio({ samples: sine(SR), sampleRate: SR, overrides: {} });
    expect(r.samples.length).toBe(SR);
  });
});

describe('meteringTruePeakDbtp oversample-factor guard', () => {
  it('takes the default when omitted and accepts valid powers of two', () => {
    expect(Number.isFinite(meteringTruePeakDbtp(sine(), SR))).toBe(true);
    expect(Number.isFinite(meteringTruePeakDbtp(sine(), SR, { oversampleFactor: 8 }))).toBe(true);
  });
  it('rejects an explicit 0 instead of reading it as the default', () => {
    expect(() => meteringTruePeakDbtp(sine(), SR, { oversampleFactor: 0 })).toThrow(RangeError);
  });
  it('rejects a non-power-of-two factor', () => {
    expect(() => meteringTruePeakDbtp(sine(), SR, { oversampleFactor: 3 })).toThrow(/power of two/);
  });
  it('rejects a fractional factor', () => {
    expect(() => meteringTruePeakDbtp(sine(), SR, { oversampleFactor: 2.5 })).toThrow(
      /power of two/,
    );
  });
  it('rejects a factor above 16', () => {
    expect(() => meteringTruePeakDbtp(sine(), SR, { oversampleFactor: 32 })).toThrow(
      /power of two/,
    );
  });
});

describe('voiceChangeRealtime empty-input guard', () => {
  it('rejects an empty buffer', () => {
    expect(() =>
      voiceChangeRealtime({
        channels: [new Float32Array(0)],
        sampleRate: 48000,
        config: 'neutralMonitor',
      }),
    ).toThrow();
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
    defaultCurve: 'exponential' as const,
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
        const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
        const info = ordinary();
        info[field] = bad;
        expect(() => engine.addParameter(info)).toThrow(/finite/);
        expect(engine.parameters().length).toBe(0);
      });
    }
  }

  it('still rejects an inverted finite range', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.addParameter({ ...ordinary(), minValue: 1, maxValue: 0 })).toThrow(
      /maxValue must be >= minValue/,
    );
    expect(engine.parameters().length).toBe(0);
  });

  it('accepts a degenerate but equal finite range', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.addParameter({ ...ordinary(), minValue: 0.5, maxValue: 0.5, defaultValue: 0.5 });
    expect(engine.parameters().length).toBe(1);
  });

  it('accepts a finite default outside the declared range', () => {
    // minValue/maxValue are descriptive metadata the engine never clamps to.
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.addParameter({ ...ordinary(), defaultValue: 12 });
    expect(engine.parameters().length).toBe(1);
  });

  it('registers an ordinary parameter', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.addParameter(ordinary());
    expect(engine.parameters().length).toBe(1);
  });
});

describe('setClipPagePrefetchFrames rejects a value the int64 cast cannot take', () => {
  // The guard's boundary. Written as powers of two because the decimal spellings
  // of these doubles do not round-trip through a source literal.
  const TWO_POW_63 = 2 ** 63;
  const LARGEST_BELOW_TWO_POW_63 = TWO_POW_63 - 1024;

  it('rejects +Infinity and NaN', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setClipPagePrefetchFrames(Number.POSITIVE_INFINITY)).toThrow();
    expect(() => engine.setClipPagePrefetchFrames(Number.NaN)).toThrow();
  });
  it('rejects a finite value the int64 cast cannot take', () => {
    // INT64_MAX is not representable as a double, so a bound spelled from it
    // would admit exactly 2^63 — the first value whose cast is undefined.
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    expect(() => engine.setClipPagePrefetchFrames(TWO_POW_63)).toThrow();
    expect(() => engine.setClipPagePrefetchFrames(1e30)).toThrow();
  });

  it('accepts the largest double below 2^63', () => {
    // The double immediately below the bound: without this the rejection above
    // is satisfied by a guard set a power of two too low.
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setClipPagePrefetchFrames(LARGEST_BELOW_TWO_POW_63);
    expect(engine.clipPagePrefetchFrames).toBe(LARGEST_BELOW_TWO_POW_63);
  });

  it('accepts a finite non-negative window', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.setClipPagePrefetchFrames(4096);
    expect(engine.clipPagePrefetchFrames).toBe(4096);
  });
});
