/**
 * The repair detectors: measure without repairing.
 *
 * Every case opens against a fixture whose defect is constructed, and pairs it
 * with the same fixture without the defect, because a detector that always
 * answers zero and one that found nothing read the same from one call.
 *
 * The two contrasts these entries exist to keep straight are pinned here rather
 * than described: `detectNoiseFloor` REFUSES a buffer shorter than `nFft` while
 * `detectReverb` pads the same buffer, and a silent channel contributes no edge
 * to `detectTrimRangeStereo`'s union -- a naive min/max over the two channel
 * ranges passes every other assertion in this file and fails that one.
 */

import { describe, expect, it } from 'vitest';
import {
  masteringRepairDetectClicks,
  masteringRepairDetectClipping,
  masteringRepairDetectCrackle,
  masteringRepairDetectHum,
  masteringRepairDetectNoiseFloor,
  masteringRepairDetectReverb,
  masteringRepairDetectTrimRange,
  masteringRepairDetectTrimRangeStereo,
} from '../src/index.js';
import { addon } from '../src/native.js';

const SR = 22050;
const LENGTH = 4410;

function tone(length: number, hz: number, amp = 0.3): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i += 1) {
    out[i] = amp * Math.sin((2 * Math.PI * hz * i) / SR);
  }
  return out;
}

/** A deterministic pseudo-noise run, so a floor measured here is reproducible. */
function noise(length: number, amp: number): Float32Array {
  const out = new Float32Array(length);
  let state = 12345;
  for (let i = 0; i < length; i += 1) {
    state = (state * 1103515245 + 12345) & 0x7fffffff;
    out[i] = amp * (state / 0x3fffffff - 1);
  }
  return out;
}

/**
 * A run of alternating +/-amp, so every sample in it has |x| exactly `amp`.
 *
 * A tone would put samples arbitrarily close to zero next to each edge, which
 * makes the trim scan's first/last indices depend on where the zero crossings
 * landed; with this the answer is the run's own bounds.
 */
function run(begin: number, end: number, amp: number): Float32Array {
  const out = new Float32Array(LENGTH);
  for (let i = begin; i < end; i += 1) {
    out[i] = i % 2 === 0 ? amp : -amp;
  }
  return out;
}

function clicky(): Float32Array {
  const out = tone(LENGTH, 440);
  for (const i of [500, 1500, 2500]) {
    out[i] = 0.99;
  }
  return out;
}

function crackly(): Float32Array {
  const out = tone(LENGTH, 440);
  for (let i = 200; i < LENGTH; i += 200) {
    out[i] += 0.8;
  }
  return out;
}

function hummy(): Float32Array {
  const out = tone(LENGTH, 440, 0.2);
  for (let i = 0; i < LENGTH; i += 1) {
    out[i] += 0.3 * Math.sin((2 * Math.PI * 50 * i) / SR);
  }
  return out;
}

function clipped(): Float32Array {
  const out = tone(LENGTH, 440, 1.5);
  for (let i = 0; i < LENGTH; i += 1) {
    out[i] = Math.max(-1, Math.min(1, out[i]));
  }
  return out;
}

describe('masteringRepairDetectClicks', () => {
  it('counts the clicks it was given, and none in the same material without them', () => {
    const detected = masteringRepairDetectClicks({ samples: clicky(), sampleRate: SR });
    expect(detected.count).toBe(3);
    expect(detected.rejected).toBe(0);
    expect(detected.longestRunSamples).toBe(1);
    expect(detected.perSecond).toBeCloseTo(3 / (LENGTH / SR), 5);
    // The control: the same tone, undamaged. Without it a detector wired to a
    // constant would pass the line above.
    expect(masteringRepairDetectClicks({ samples: tone(LENGTH, 440), sampleRate: SR }).count).toBe(
      0,
    );
  });

  it('counts by the declicker criteria the options select', () => {
    // A neighbour ratio nothing can meet rejects every run, which is what says
    // the options bag reaches the analysis rather than being read and dropped.
    expect(
      masteringRepairDetectClicks({ samples: clicky(), sampleRate: SR, neighborRatio: 1e6 }).count,
    ).toBe(0);
  });
});

describe('masteringRepairDetectNoiseFloor', () => {
  it('reads the floor of the noise it was given, and its shape in 32 bands', () => {
    const quiet = masteringRepairDetectNoiseFloor({ samples: noise(8192, 0.01), sampleRate: SR });
    const loud = masteringRepairDetectNoiseFloor({ samples: noise(8192, 0.1), sampleRate: SR });
    expect(quiet.bandFloorDbfs).toHaveLength(32);
    // Ten times the amplitude is 20 dB, so the pair measures the input rather
    // than reporting a constant.
    expect(loud.floorDbfs - quiet.floorDbfs).toBeCloseTo(20, 0);
  });

  it('refuses a buffer shorter than nFft, where detectReverb pads the same one', () => {
    const short = tone(512, 440);
    expect(() =>
      masteringRepairDetectNoiseFloor({ samples: short, sampleRate: SR, nFft: 1024 }),
    ).toThrow(/at least n_fft/);
    // The same buffer, the same nFft, through the entry that pads instead.
    const padded = masteringRepairDetectReverb({ samples: short, sampleRate: SR, nFft: 1024 });
    expect(Number.isFinite(padded.lateDecayRatioDb)).toBe(true);
    // And the refusal is the length rather than the buffer: an nFft it does
    // reach is accepted.
    expect(
      Number.isFinite(
        masteringRepairDetectNoiseFloor({ samples: short, sampleRate: SR, nFft: 512 }).floorDbfs,
      ),
    ).toBe(true);
  });
});

describe('masteringRepairDetectClipping', () => {
  it('counts samples at or past clipThreshold, and none in unclipped material', () => {
    const detected = masteringRepairDetectClipping({ samples: clipped(), sampleRate: SR });
    expect(detected.sampleCount).toBeGreaterThan(0);
    expect(detected.sampleFraction).toBeCloseTo(detected.sampleCount / LENGTH, 5);
    expect(detected.runCount).toBeGreaterThan(0);
    expect(detected.longestRunSamples).toBeGreaterThan(0);
    expect(
      masteringRepairDetectClipping({ samples: tone(LENGTH, 440), sampleRate: SR }).sampleCount,
    ).toBe(0);
  });

  it('reads clipThreshold and nothing else off the config', () => {
    const base = masteringRepairDetectClipping({ samples: clipped(), sampleRate: SR });
    const raised = masteringRepairDetectClipping({
      samples: clipped(),
      sampleRate: SR,
      clipThreshold: 0.999999,
    });
    expect(raised.sampleCount).toBeLessThan(base.sampleCount);
    // The reconstruction fields describe a repair that does not run here, so
    // they are accepted and change nothing.
    expect(
      masteringRepairDetectClipping({
        samples: clipped(),
        sampleRate: SR,
        lpcOrder: 8,
        iterations: 7,
        lpcBlend: 0.1,
      }),
    ).toEqual(base);
  });
});

describe('masteringRepairDetectCrackle', () => {
  it('counts crackle by the median criterion, and none in undamaged material', () => {
    const detected = masteringRepairDetectCrackle({ samples: crackly(), sampleRate: SR });
    expect(detected.sampleCount).toBeGreaterThan(0);
    expect(detected.sampleFraction).toBeCloseTo(detected.sampleCount / LENGTH, 5);
    expect(detected.perSecond).toBeCloseTo(detected.sampleCount / (LENGTH / SR), 4);
    expect(
      masteringRepairDetectCrackle({ samples: tone(LENGTH, 440), sampleRate: SR }).sampleCount,
    ).toBe(0);
  });

  it('measures by that criterion whatever mode is configured', () => {
    // Wavelet shrinkage removes crackle without ever deciding a sample is
    // crackle, so the two modes must report the same counts -- these numbers do
    // not describe what wavelet mode would have repaired.
    const median = masteringRepairDetectCrackle({
      samples: crackly(),
      sampleRate: SR,
      mode: 'median',
    });
    const wavelet = masteringRepairDetectCrackle({
      samples: crackly(),
      sampleRate: SR,
      mode: 'waveletShrinkage',
    });
    expect(wavelet).toEqual(median);
    // The control for that equality: a knob the criterion does read.
    expect(
      masteringRepairDetectCrackle({ samples: crackly(), sampleRate: SR, threshold: 2 })
        .sampleCount,
    ).toBe(0);
  });
});

describe('masteringRepairDetectHum', () => {
  it('finds the mains fundamental it was given, and no harmonics without one', () => {
    const detected = masteringRepairDetectHum({ samples: hummy(), sampleRate: SR });
    expect(detected.fundamentalHz).toBeCloseTo(50, 5);
    expect(detected.harmonics).toBeGreaterThan(0);
    expect(detected.harmonicDbfs).toHaveLength(16);
    expect(
      masteringRepairDetectHum({ samples: tone(LENGTH, 440, 0.2), sampleRate: SR }).harmonics,
    ).toBe(0);
  });

  it('runs the estimation path whatever adaptive says', () => {
    // The fixed path notches the configured frequency without ever looking for
    // hum, so a detector following the flag would hand back its own input.
    const on = masteringRepairDetectHum({ samples: hummy(), sampleRate: SR, adaptive: true });
    const off = masteringRepairDetectHum({ samples: hummy(), sampleRate: SR, adaptive: false });
    expect(on).toEqual(off);
    // The control: a config field the estimator does read moves the answer. The
    // search starts at 60 and searchRangeHz bounds how far down it reaches.
    expect(
      masteringRepairDetectHum({ samples: hummy(), sampleRate: SR, fundamentalHz: 60 })
        .fundamentalHz,
    ).toBeCloseTo(58, 5);
  });
});

describe('masteringRepairDetectReverb', () => {
  it('reads a reverberant take higher than the same take dry', () => {
    // A decaying note rather than a sustained tone: a tone sustains across the
    // module's late lag on its own, which leaves the statistic nothing to
    // separate.
    const dry = new Float32Array(SR);
    for (let i = 0; i < dry.length; i += 1) {
      const gate = i % LENGTH < LENGTH / 2 ? 1 : 0;
      dry[i] = Math.sin((2 * Math.PI * 440 * i) / SR) * Math.exp(-i / 1000) * gate;
    }
    const wet = new Float32Array(dry.length);
    let tail = 0;
    for (let i = 0; i < dry.length; i += 1) {
      tail = tail * 0.9995 + dry[i];
      wet[i] = 0.5 * dry[i] + 0.02 * tail;
    }
    // Less negative means the material sustains across the module's late lag,
    // which a tail does and a dry offset does not.
    expect(
      masteringRepairDetectReverb({ samples: wet, sampleRate: SR }).lateDecayRatioDb,
    ).toBeGreaterThan(
      masteringRepairDetectReverb({ samples: dry, sampleRate: SR }).lateDecayRatioDb,
    );
  });

  it('runs the WPE analysis only under wpeEnabled', () => {
    const samples = tone(LENGTH, 440);
    expect(masteringRepairDetectReverb({ samples, sampleRate: SR }).latePredictability).toBe(0);
    expect(
      masteringRepairDetectReverb({ samples, sampleRate: SR, wpeEnabled: true }).latePredictability,
    ).toBeGreaterThan(0);
  });
});

describe('masteringRepairDetectTrimRange', () => {
  it('returns the range the trimmer would cut to, padding included', () => {
    const samples = run(1000, 2000, 0.5);
    expect(masteringRepairDetectTrimRange({ samples, sampleRate: SR })).toEqual({
      first: 1000,
      lastExclusive: 2000,
    });
    // The padding is INSIDE the range: this is what the repair would keep, not
    // the detected extent of the signal.
    expect(
      masteringRepairDetectTrimRange({ samples, sampleRate: SR, paddingSamples: 100 }),
    ).toEqual({ first: 900, lastExclusive: 2100 });
  });

  it('reports (length, length) when nothing is above the threshold', () => {
    expect(
      masteringRepairDetectTrimRange({ samples: new Float32Array(LENGTH), sampleRate: SR }),
    ).toEqual({ first: LENGTH, lastExclusive: LENGTH });
  });
});

describe('masteringRepairDetectTrimRangeStereo', () => {
  const left = () => run(1000, 2000, 0.5);
  const right = () => run(1500, 3000, 0.5);

  it('unions the two channels ranges', () => {
    expect(
      masteringRepairDetectTrimRangeStereo({ left: left(), right: right(), sampleRate: SR }),
    ).toEqual({ first: 1000, lastExclusive: 3000 });
  });

  it('takes no edge from a channel with nothing above the threshold', () => {
    // THE ASSERTION A NAIVE min/max FAILS. A silent channel's own range is
    // (LENGTH, LENGTH), so unioning it as an interval pushes lastExclusive out
    // to 4410 and the pair keeps the whole buffer. It contributes nothing
    // instead, and the answer is the active channel's range exactly.
    const silent = new Float32Array(LENGTH);
    expect(
      masteringRepairDetectTrimRangeStereo({ left: left(), right: silent, sampleRate: SR }),
    ).toEqual({ first: 1000, lastExclusive: 2000 });
    expect(
      masteringRepairDetectTrimRangeStereo({ left: silent, right: right(), sampleRate: SR }),
    ).toEqual({ first: 1500, lastExclusive: 3000 });
  });

  it('reports (length, length) when neither channel carries signal', () => {
    const silent = new Float32Array(LENGTH);
    expect(
      masteringRepairDetectTrimRangeStereo({ left: silent, right: silent, sampleRate: SR }),
    ).toEqual({ first: LENGTH, lastExclusive: LENGTH });
  });

  it('pads the union rather than each channel', () => {
    expect(
      masteringRepairDetectTrimRangeStereo({
        left: left(),
        right: right(),
        sampleRate: SR,
        paddingSamples: 200,
      }),
    ).toEqual({ first: 800, lastExclusive: 3200 });
  });

  it('refuses a mismatched pair by name', () => {
    expect(() =>
      masteringRepairDetectTrimRangeStereo({
        left: left(),
        right: new Float32Array(10),
        sampleRate: SR,
      }),
    ).toThrow(/same length/);
  });
});

/**
 * Every entry, called through the addon with NO options argument at all -- the
 * C ABI's own "pass NULL for library defaults" path, which the facade never
 * reaches because it always hands the request object down.
 */
describe('an omitted config is the library defaults', () => {
  const cases: [string, () => unknown, () => unknown][] = [
    [
      'masteringRepairDetectClicks',
      () => addon.masteringRepairDetectClicks(clicky(), SR),
      () => masteringRepairDetectClicks({ samples: clicky(), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectNoiseFloor',
      () => addon.masteringRepairDetectNoiseFloor(noise(8192, 0.01), SR),
      () => masteringRepairDetectNoiseFloor({ samples: noise(8192, 0.01), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectClipping',
      () => addon.masteringRepairDetectClipping(clipped(), SR),
      () => masteringRepairDetectClipping({ samples: clipped(), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectCrackle',
      () => addon.masteringRepairDetectCrackle(crackly(), SR),
      () => masteringRepairDetectCrackle({ samples: crackly(), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectHum',
      () => addon.masteringRepairDetectHum(hummy(), SR),
      () => masteringRepairDetectHum({ samples: hummy(), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectReverb',
      () => addon.masteringRepairDetectReverb(tone(LENGTH, 440), SR),
      () => masteringRepairDetectReverb({ samples: tone(LENGTH, 440), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectTrimRange',
      () => addon.masteringRepairDetectTrimRange(run(1000, 2000, 0.5), SR),
      () => masteringRepairDetectTrimRange({ samples: run(1000, 2000, 0.5), sampleRate: SR }),
    ],
    [
      'masteringRepairDetectTrimRangeStereo',
      () =>
        addon.masteringRepairDetectTrimRangeStereo(run(1000, 2000, 0.5), run(1500, 3000, 0.5), SR),
      () =>
        masteringRepairDetectTrimRangeStereo({
          left: run(1000, 2000, 0.5),
          right: run(1500, 3000, 0.5),
          sampleRate: SR,
        }),
    ],
  ];

  for (const [name, omitted, spelled] of cases) {
    it(`${name} answers the same with no config as with an empty one`, () => {
      expect(omitted()).toEqual(spelled());
    });
  }
});

describe('the detectors refuse what they cannot measure', () => {
  const nanned = () => {
    const out = clicky();
    out[3] = Number.NaN;
    return out;
  };
  const mono: [string, (samples: Float32Array, sampleRate: number) => unknown][] = [
    [
      'masteringRepairDetectClicks',
      (s, r) => masteringRepairDetectClicks({ samples: s, sampleRate: r }),
    ],
    [
      'masteringRepairDetectNoiseFloor',
      (s, r) => masteringRepairDetectNoiseFloor({ samples: s, sampleRate: r }),
    ],
    [
      'masteringRepairDetectClipping',
      (s, r) => masteringRepairDetectClipping({ samples: s, sampleRate: r }),
    ],
    [
      'masteringRepairDetectCrackle',
      (s, r) => masteringRepairDetectCrackle({ samples: s, sampleRate: r }),
    ],
    ['masteringRepairDetectHum', (s, r) => masteringRepairDetectHum({ samples: s, sampleRate: r })],
    [
      'masteringRepairDetectReverb',
      (s, r) => masteringRepairDetectReverb({ samples: s, sampleRate: r }),
    ],
    [
      'masteringRepairDetectTrimRange',
      (s, r) => masteringRepairDetectTrimRange({ samples: s, sampleRate: r }),
    ],
    [
      'masteringRepairDetectTrimRangeStereo',
      (s, r) => masteringRepairDetectTrimRangeStereo({ left: s, right: s, sampleRate: r }),
    ],
  ];

  for (const [name, call] of mono) {
    it(`${name} refuses an empty buffer, a non-finite sample and a bad sample rate`, () => {
      expect(() => call(new Float32Array(0), SR)).toThrow();
      expect(() => call(nanned(), SR)).toThrow();
      expect(() => call(clicky(), 0)).toThrow();
      expect(() => call(clicky(), -SR)).toThrow();
      // The control: the same call on the same fixture succeeds, so the four
      // above are refusals rather than an entry that always throws.
      expect(() => call(clicky(), SR)).not.toThrow();
    });
  }

  it('refuses a wrong-typed option by name rather than substituting a default', () => {
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: the point of the case is a value the type rejects.
      masteringRepairDetectClicks({ samples: clicky(), sampleRate: SR, threshold: '0.5' as any }),
    ).toThrow(/threshold must be a number/);
  });

  it('refuses an unknown mode string by name', () => {
    expect(() =>
      // biome-ignore lint/suspicious/noExplicitAny: the point of the case is a value the type rejects.
      masteringRepairDetectCrackle({ samples: crackly(), sampleRate: SR, mode: 'nope' as any }),
    ).toThrow(/unknown decrackle mode/);
  });

  it('refuses a negative padding count by name', () => {
    expect(() =>
      masteringRepairDetectTrimRange({
        samples: run(1000, 2000, 0.5),
        sampleRate: SR,
        paddingSamples: -1,
      }),
    ).toThrow(/paddingSamples/);
  });
});
