import { describe, expect, it } from 'vitest';
import type { PercussiveEvent, PercussiveEventInput, SonareError } from '../src/index.js';
import {
  ErrorCode,
  extractPercussiveEvents,
  isSonareError,
  renderPercussiveEvents,
} from '../src/index.js';

const SR = 22050;
/** The default framing, which every fixture position below is reasoned in. */
const HOP = 512;
/** The detector's own bound on how far backtracking may travel, in samples. */
const BACKTRACK_REACH = 10 * HOP;

const HIT_SAMPLES = 1323; // 60 ms at 22050 Hz, exactly.
const HIT_DECAY_MS = 12;

// Three isolated hits at descending, distinct levels, 400 ms apart, in a buffer
// whose tail runs past the default 500 ms span cap.
const THREE_HIT_STARTS = [4410, 13230, 22050];
const THREE_HIT_PEAKS = [0.5, 0.34, 0.22];

// Two hits 1.3 s apart, so a hit shifted by its own span length lands on empty
// timeline rather than on its neighbour.
const MOVE_HIT_STARTS = [6615, 35280];

/**
 * Deterministic uniform noise in `[-1, 1)`. The seed is per hit so no two
 * synthesised hits carry the same samples: a span that read a neighbour's audio
 * cannot then match.
 */
function noise(seed: number, length: number): Float32Array {
  const output = new Float32Array(length);
  let state = (Math.imul(seed, 2654435761) + 1) >>> 0;
  for (let i = 0; i < length; i += 1) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    output[i] = (state >>> 9) / 4194304 - 1;
  }
  return output;
}

/**
 * An exponentially decaying noise burst: the fixtures' struck sound. Normalised
 * so the peak is exactly `amplitude`, which makes the synthesised level an
 * analytic anchor for `peakAmplitude` and for the lifted signal's size.
 */
function hit(seed: number, amplitude: number): Float32Array {
  const burst = noise(seed, HIT_SAMPLES);
  const decay = HIT_DECAY_MS * 0.001 * SR;
  let worst = 0;
  for (let i = 0; i < HIT_SAMPLES; i += 1) {
    burst[i] *= Math.exp(-i / decay);
    worst = Math.max(worst, Math.abs(burst[i]));
  }
  const scale = amplitude / worst;
  for (let i = 0; i < HIT_SAMPLES; i += 1) {
    burst[i] *= scale;
  }
  return burst;
}

function addHit(into: Float32Array, start: number, seed: number, amplitude: number): void {
  const burst = hit(seed, amplitude);
  for (let i = 0; i < burst.length && start + i < into.length; i += 1) {
    into[start + i] += burst[i];
  }
}

/**
 * A sustained sine over `[start, end)` with a 2 ms raised-cosine edge at each
 * end: short enough that the attack still trips the detector, long enough that
 * the buffer carries no step discontinuity, so the span reads as harmonic rather
 * than as one long click.
 */
function addNote(
  into: Float32Array,
  start: number,
  end: number,
  frequencyHz: number,
  amplitude: number,
): void {
  const edge = 44; // 2 ms at 22050 Hz.
  for (let i = start; i < end; i += 1) {
    const position = i - start;
    const remaining = end - 1 - i;
    let envelope = 1;
    if (position < edge) {
      envelope = 0.5 - 0.5 * Math.cos((Math.PI * position) / edge);
    } else if (remaining < edge) {
      envelope = 0.5 - 0.5 * Math.cos((Math.PI * remaining) / edge);
    }
    into[i] += amplitude * envelope * Math.sin((2 * Math.PI * frequencyHz * position) / SR);
  }
}

function threeHits(): Float32Array {
  const samples = new Float32Array(44100); // 2.0 s
  for (const [index, start] of THREE_HIT_STARTS.entries()) {
    addHit(samples, start, index + 1, THREE_HIT_PEAKS[index]);
  }
  return samples;
}

/** Two isolated hits inside one second: the compact fixture the sweeps run on. */
function twoHits(): Float32Array {
  const samples = new Float32Array(22050);
  addHit(samples, 3528, 31, 0.5);
  addHit(samples, 12348, 32, 0.3);
  return samples;
}

function movableHits(): Float32Array {
  const samples = new Float32Array(48510); // 2.2 s
  addHit(samples, MOVE_HIT_STARTS[0], 41, 0.5);
  addHit(samples, MOVE_HIT_STARTS[1], 42, 0.32);
  return samples;
}

// A quiet hit on top of a loud sustained note, the same note with no hit on it,
// and the same hit with nothing under it.
//
// The source peak over the hit's span is the note's, an order above the hit's
// own peak, so a `peakAmplitude` measured on the source and one measured on the
// percussive component cannot be confused. The two single-component buffers are
// what "the harmonic content is still sounding" and "the identical hit in
// silence" are asserted against.
const LAYERED_HIT_START = 22050;
const LAYERED_HIT_PEAK = 0.15;
const LAYERED_NOTE_AMPLITUDE = 0.8;

function layeredFixture(): { mixed: Float32Array; noteOnly: Float32Array; hitOnly: Float32Array } {
  const noteOnly = new Float32Array(44100); // 2.0 s
  addNote(noteOnly, 0, 44100, 440, LAYERED_NOTE_AMPLITUDE);
  const mixed = Float32Array.from(noteOnly);
  addHit(mixed, LAYERED_HIT_START, 21, LAYERED_HIT_PEAK);
  const hitOnly = new Float32Array(noteOnly.length);
  addHit(hitOnly, LAYERED_HIT_START, 21, LAYERED_HIT_PEAK);
  return { mixed, noteOnly, hitOnly };
}

/** Index of the first differing sample, or -1 when the two agree everywhere. */
function firstMismatch(a: Float32Array, b: Float32Array, lo = 0, hi = a.length): number {
  if (a.length !== b.length) {
    return 0;
  }
  const end = Math.min(hi, a.length);
  for (let i = lo; i < end; i += 1) {
    if (a[i] !== b[i]) {
      return i;
    }
  }
  return -1;
}

/** Bit-for-bit equality, reported as one index rather than as two buffers. */
function expectIdentical(rendered: Float32Array, source: Float32Array): void {
  expect(rendered).toBeInstanceOf(Float32Array);
  expect(rendered).toHaveLength(source.length);
  expect(firstMismatch(rendered, source)).toBe(-1);
}

function rms(samples: Float32Array, lo: number, hi: number): number {
  const end = Math.min(hi, samples.length);
  let acc = 0;
  for (let i = lo; i < end; i += 1) {
    acc += samples[i] * samples[i];
  }
  return Math.sqrt(acc / Math.max(1, end - lo));
}

function peak(samples: Float32Array, lo = 0, hi = samples.length): number {
  const end = Math.min(hi, samples.length);
  let highest = 0;
  for (let i = lo; i < end; i += 1) {
    highest = Math.max(highest, Math.abs(samples[i]));
  }
  return highest;
}

function maxDifference(a: Float32Array, b: Float32Array, lo = 0, hi = a.length): number {
  const end = Math.min(hi, a.length, b.length);
  let worst = 0;
  for (let i = lo; i < end; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

/** `a - b`, sample by sample. */
function difference(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length);
  for (let i = 0; i < a.length; i += 1) {
    out[i] = a[i] - b[i];
  }
  return out;
}

/** `max |a + scale * b|` over the whole buffer. */
function maxResidual(a: Float32Array, b: Float32Array, scale: number): number {
  let worst = 0;
  for (let i = 0; i < a.length; i += 1) {
    worst = Math.max(worst, Math.abs(a[i] + scale * b[i]));
  }
  return worst;
}

/**
 * Asserts that an edit meant to change the sound did: the output moved away from
 * `source`, and it is still a signal. The amplitude band is what a bare
 * difference check misses — silence differs from the source by its own peak, so
 * it passes one, and a blow-up passes it too.
 */
function expectEdited(out: Float32Array, source: Float32Array, floor = 0.5): void {
  expect(out).toHaveLength(source.length);
  expect(maxDifference(source, out)).toBeGreaterThan(0.02);
  const sourcePeak = peak(source);
  expect(peak(out)).toBeGreaterThan(floor * sourcePeak);
  expect(peak(out)).toBeLessThan(2 * sourcePeak);
}

/**
 * Every event is a well-formed, renderable span with an identity edit: ascending
 * and non-overlapping, inside the audio, and carrying finite measurements.
 */
function expectWellFormed(events: PercussiveEvent[], samples: Float32Array): void {
  let previousOffset = 0;
  for (const [index, event] of events.entries()) {
    const at = `event ${index}`;
    expect(event.onsetSample, at).toBeGreaterThanOrEqual(0);
    expect(event.offsetSample, at).toBeGreaterThan(event.onsetSample);
    expect(event.offsetSample, at).toBeLessThanOrEqual(samples.length);
    expect(event.onsetSample, at).toBeGreaterThanOrEqual(previousOffset);
    previousOffset = event.offsetSample;
    expect(Number.isFinite(event.strength), at).toBe(true);
    expect(Number.isFinite(event.peakAmplitude), at).toBe(true);
    expect(event.peakAmplitude, at).toBeGreaterThanOrEqual(0);
    expect(event.percussiveRatio, at).toBeGreaterThanOrEqual(0);
    expect(event.percussiveRatio, at).toBeLessThanOrEqual(1);
    expect(event.edit, at).toEqual({ timeOffsetSamples: 0, gainDb: 0, muted: false });
  }
}

/**
 * The span opens in front of the transient at `start` and closes past it.
 *
 * Not a symmetric tolerance on the onset, because the two directions are not the
 * same claim. Peak-picking lands after a transient starts, so an onset at or
 * after `start` means the span opened inside its own hit: it would then measure
 * the next hit's peak and keep its own attack when muted. Backtracking is what
 * moves the edge in front, and the early bound is the detector's own limit on
 * that travel, so an onset cannot have wandered back into the previous hit.
 */
function expectSpanCovers(event: PercussiveEvent, start: number, coverage = HIT_SAMPLES): void {
  expect(event.onsetSample).toBeLessThanOrEqual(start);
  expect(event.onsetSample).toBeGreaterThan(start - BACKTRACK_REACH);
  expect(event.offsetSample).toBeGreaterThan(start + coverage);
}

/** The event whose onset sits closest to `start`, required to actually cover it. */
function eventAt(events: PercussiveEvent[], start: number): PercussiveEvent {
  expect(events.length).toBeGreaterThan(0);
  let best = events[0];
  for (const event of events) {
    if (Math.abs(event.onsetSample - start) < Math.abs(best.onsetSample - start)) {
      best = event;
    }
  }
  expectSpanCovers(best, start);
  return best;
}

/** Whether any span covers `start`; false for an empty set rather than failing. */
function anySpanCovers(events: PercussiveEvent[], start: number): boolean {
  return events.some((event) => event.onsetSample <= start && event.offsetSample > start);
}

/** Every field a separation produced, so two runs can be compared as a whole. */
function measurements(event: PercussiveEvent): number[] {
  return [
    event.onsetSample,
    event.offsetSample,
    event.strength,
    event.peakAmplitude,
    event.percussiveRatio,
  ];
}

function measuredSet(events: PercussiveEvent[]): number[][] {
  return events.map(measurements);
}

/** The error a call raises, or `undefined` when it does not raise one. */
function capture(run: () => unknown): unknown {
  try {
    run();
    return undefined;
  } catch (error) {
    return error;
  }
}

function expectInvalidParameter(run: () => unknown): void {
  const error = capture(run);
  expect(isSonareError(error)).toBe(true);
  expect((error as SonareError).code).toBe(ErrorCode.InvalidParameter);
  expect((error as SonareError).codeName).toBe('InvalidParameter');
}

/**
 * Framings on the far side of exactly one bound of the overlap-add rule (`nFft`
 * even and at least 2, `hopLength` in `(0, nFft / 2]`), so a rejection cannot
 * come from a second defect in the row.
 */
const BROKEN_FRAMINGS = [
  { nFft: 2047, hopLength: 512 }, // odd
  { nFft: -2048, hopLength: 512 }, // negative
  { nFft: 2048, hopLength: 2048 }, // no overlap at all
  { nFft: 2048, hopLength: 1025 }, // one sample past half the window
  { nFft: 2048, hopLength: -512 }, // negative advance
];

/** Framings inside the rule, including the inclusive end of it. */
const VALID_FRAMINGS = [
  { nFft: 2048, hopLength: 1024 }, // exactly half the window
  { nFft: 1024, hopLength: 512 }, // the same boundary at another size
];

const three = threeHits();
const two = twoHits();
const movable = movableHits();
const layered = layeredFixture();

const threeEvents = extractPercussiveEvents({ samples: three, sampleRate: SR });
const twoEvents = extractPercussiveEvents({ samples: two, sampleRate: SR });
const movableEvents = extractPercussiveEvents({ samples: movable, sampleRate: SR });
const layeredEvents = extractPercussiveEvents({ samples: layered.mixed, sampleRate: SR });
const isolatedEvents = extractPercussiveEvents({ samples: layered.hitOnly, sampleRate: SR });

describe('extractPercussiveEvents', () => {
  it('finds every hit, in order, with an identity edit', () => {
    expect(threeEvents).toHaveLength(3);
    expectWellFormed(threeEvents, three);

    for (const [index, start] of THREE_HIT_STARTS.entries()) {
      expectSpanCovers(threeEvents[index], start);
      expect(threeEvents[index].strength, `hit ${index}`).toBeGreaterThan(0);
    }
    expect(threeEvents[0].onsetSample).toBeLessThan(threeEvents[1].onsetSample);
    expect(threeEvents[1].onsetSample).toBeLessThan(threeEvents[2].onsetSample);

    // Adjacent, exactly: an interior span is closed by the following onset and
    // nothing else, so there is no gap and no rounding to absorb.
    expect(threeEvents[0].offsetSample).toBe(threeEvents[1].onsetSample);
    expect(threeEvents[1].offsetSample).toBe(threeEvents[2].onsetSample);

    // The hits are 400 ms apart, inside the 500 ms cap, so the cap binds on the
    // last span only. 500 ms is 11025 samples at this rate, exactly.
    expect(threeEvents[0].offsetSample - threeEvents[0].onsetSample).toBeLessThan(11025);
    expect(threeEvents[2].offsetSample - threeEvents[2].onsetSample).toBe(11025);
  });

  it('measures each hit on its own percussive component', () => {
    // The three hits are synthesised at 0.50, 0.34 and 0.22 peak with different
    // noise seeds, so a span that measured a neighbour, or measured the whole
    // buffer, cannot reproduce this ordering.
    expect(threeEvents[0].peakAmplitude).toBeGreaterThan(threeEvents[1].peakAmplitude);
    expect(threeEvents[1].peakAmplitude).toBeGreaterThan(threeEvents[2].peakAmplitude);

    // An isolated hit is nearly all percussive, so the measured peak sits just
    // under the synthesised one rather than anywhere below it.
    expect(threeEvents[0].peakAmplitude).toBeGreaterThan(0.25);
    expect(threeEvents[0].peakAmplitude).toBeLessThanOrEqual(0.55);

    // Struck sounds in silence, so the separation has to call them percussive.
    // The complementary half — that the figure is not simply pinned at its
    // ceiling for everything — is the buried-hit case below.
    for (const event of threeEvents) {
      expect(event.percussiveRatio).toBeGreaterThan(0.5);
    }
  });

  it('reads a hit under a sustain as barely percussive', () => {
    // percussiveRatio is a property of the span's energy, not evidence about the
    // onset that opened it. These two extractions are the same hit, same seed
    // and same level, differing only in what sustains through it.
    const underSustain = eventAt(layeredEvents, LAYERED_HIT_START);
    const isolated = eventAt(isolatedEvents, LAYERED_HIT_START);

    // Opposite ends of the figure, for the identical hit.
    expect(underSustain.percussiveRatio).toBeLessThan(0.05);
    expect(isolated.percussiveRatio).toBeGreaterThan(0.5);

    // The buried one is a real hit all the same: the separation measured a peak
    // on it the sustain cannot account for. Without this the case would only be
    // saying the ratio is small, which silence also satisfies.
    expect(underSustain.peakAmplitude).toBeGreaterThan(0.3 * LAYERED_HIT_PEAK);

    // And it is measured on the percussive component, not on the source: the
    // source peak over that span is the 0.8 note's, an order above the answer.
    expect(
      peak(layered.mixed, underSustain.onsetSample, underSustain.offsetSample),
    ).toBeGreaterThan(0.75);
    expect(underSustain.peakAmplitude).toBeLessThan(0.35);
  });

  it('returns an empty array when nothing is detected', () => {
    expect(extractPercussiveEvents({ samples: new Float32Array(22050), sampleRate: SR })).toEqual(
      [],
    );
  });

  it('caps a span with maxEventMs without moving an onset', () => {
    // 40 ms is 882 samples at 22050 Hz, exactly, and shorter than the 400 ms
    // between hits, so it now closes every span including the interior ones.
    const capped = extractPercussiveEvents({ samples: three, sampleRate: SR, maxEventMs: 40 });
    expect(capped).toHaveLength(threeEvents.length);
    for (const [index, event] of capped.entries()) {
      expect(event.onsetSample, `event ${index}`).toBe(threeEvents[index].onsetSample);
      expect(event.offsetSample - event.onsetSample, `event ${index}`).toBe(882);
    }

    // Raised past the tail, the last span is no longer cut at 500 ms — and still
    // stops at the end of the audio, whatever the cap says.
    const loose = extractPercussiveEvents({ samples: three, sampleRate: SR, maxEventMs: 5000 });
    expect(loose).toHaveLength(threeEvents.length);
    expectWellFormed(loose, three);
    expect(loose[2].offsetSample - loose[2].onsetSample).toBeGreaterThan(11025);
    expect(loose[2].offsetSample).toBe(three.length);
  });

  it('spaces consecutive onsets by onsetWait', () => {
    // The hits are 400 ms apart, about 17 frames at the default hop, so a wait
    // of 100 frames swallows the second and third. The default (1) finding all
    // three is the positive control that keeps this from passing vacuously.
    const sparse = extractPercussiveEvents({ samples: three, sampleRate: SR, onsetWait: 100 });
    expect(sparse).toHaveLength(1);
    expectSpanCovers(sparse[0], THREE_HIT_STARTS[0]);
    expect(threeEvents).toHaveLength(3);
  });

  it('raises the detector threshold with onsetDelta', () => {
    // The onset envelope is unnormalised spectral flux, so an offset this far
    // above any flux the fixture can produce leaves nothing over the threshold.
    expect(
      extractPercussiveEvents({ samples: three, sampleRate: SR, onsetDelta: 1e9 }),
    ).toHaveLength(0);
    // 0 selects the documented default rather than a detector with no offset.
    expect(
      measuredSet(extractPercussiveEvents({ samples: three, sampleRate: SR, onsetDelta: 0 })),
    ).toEqual(measuredSet(threeEvents));
  });

  it('selects on minPercussiveRatio without moving a span', () => {
    // The buried hit reads near 0 and the identical isolated one near 1, so a
    // threshold between them drops a hit that is genuinely there while keeping
    // the one in silence. That is also why 0 is the default.
    const underSustain = eventAt(layeredEvents, LAYERED_HIT_START);
    const isolated = eventAt(isolatedEvents, LAYERED_HIT_START);
    const between = 0.5 * (underSustain.percussiveRatio + isolated.percussiveRatio);
    expect(between).toBeGreaterThan(underSustain.percussiveRatio);
    expect(between).toBeLessThan(isolated.percussiveRatio);

    const selective = extractPercussiveEvents({
      samples: layered.mixed,
      sampleRate: SR,
      minPercussiveRatio: between,
    });
    expect(anySpanCovers(selective, LAYERED_HIT_START)).toBe(false);
    expect(
      anySpanCovers(
        extractPercussiveEvents({
          samples: layered.hitOnly,
          sampleRate: SR,
          minPercussiveRatio: between,
        }),
        LAYERED_HIT_START,
      ),
    ).toBe(true);

    // Selection happens after measurement, so a survivor is the very same event
    // rather than one whose span grew over a dropped neighbour.
    expect(measuredSet(selective)).toEqual(
      measuredSet(layeredEvents.filter((event) => event.percussiveRatio >= between)),
    );

    // 0 is this field's own meaning as well as its default, so it must keep
    // everything rather than read as "unset" and then as something else.
    const keepAll = extractPercussiveEvents({
      samples: layered.mixed,
      sampleRate: SR,
      minPercussiveRatio: 0,
    });
    expect(measuredSet(keepAll)).toEqual(measuredSet(layeredEvents));
    expect(anySpanCovers(keepAll, LAYERED_HIT_START)).toBe(true);
    expect(selective.length).toBeLessThan(keepAll.length);
  });

  it('separates on the framing nFft and hopLength name', () => {
    for (const framing of VALID_FRAMINGS) {
      const at = JSON.stringify(framing);
      const events = extractPercussiveEvents({ samples: two, sampleRate: SR, ...framing });
      expectWellFormed(events, two);
      expect(events.length, at).toBeGreaterThan(0);
      // A different framing is a different separation, so the measurements move
      // — which is the evidence that both fields reached the C struct.
      expect(measuredSet(events), at).not.toEqual(measuredSet(twoEvents));
      // And the set it produced is renderable, on its own framing.
      expectIdentical(
        renderPercussiveEvents({ samples: two, sampleRate: SR, events, ...framing }),
        two,
      );
    }

    for (const framing of BROKEN_FRAMINGS) {
      expectInvalidParameter(() =>
        extractPercussiveEvents({ samples: two, sampleRate: SR, ...framing }),
      );
    }
  });

  it('runs the separation with the kernel lengths the request names', () => {
    // Each kernel has to land in its own slot, so all three comparisons matter:
    // a kernel that was ignored collapses one of the first two, and two kernels
    // written to one field collapse the third. (A straight swap of the two is
    // not visible from here — nothing on this surface separates independently.)
    const extractWith = (options: Record<string, number>): number[][] =>
      measuredSet(extractPercussiveEvents({ samples: layered.mixed, sampleRate: SR, ...options }));

    const shortHarmonic = extractWith({ hpssKernelHarmonic: 3 });
    const shortPercussive = extractWith({ hpssKernelPercussive: 3 });

    expect(shortHarmonic).not.toEqual(measuredSet(layeredEvents));
    expect(shortPercussive).not.toEqual(measuredSet(layeredEvents));
    expect(shortHarmonic).not.toEqual(shortPercussive);
  });

  it('rejects malformed audio and config', () => {
    expectInvalidParameter(() =>
      extractPercussiveEvents({ samples: new Float32Array(0), sampleRate: SR }),
    );
    expectInvalidParameter(() =>
      extractPercussiveEvents({ samples: Float32Array.from([0, Number.NaN, 0]), sampleRate: SR }),
    );
    expect(() => extractPercussiveEvents({ samples: two, sampleRate: 0 })).toThrow(RangeError);
    expect(() => extractPercussiveEvents({ samples: two, sampleRate: 1e9 })).toThrow(RangeError);

    for (const maxEventMs of [Number.NaN, Number.POSITIVE_INFINITY, -1]) {
      expectInvalidParameter(() =>
        extractPercussiveEvents({ samples: two, sampleRate: SR, maxEventMs }),
      );
    }
    for (const minPercussiveRatio of [-0.01, 1.01, Number.NaN, Number.POSITIVE_INFINITY]) {
      expectInvalidParameter(() =>
        extractPercussiveEvents({ samples: two, sampleRate: SR, minPercussiveRatio }),
      );
    }
    for (const onsetWait of [-1, -100]) {
      expectInvalidParameter(() =>
        extractPercussiveEvents({ samples: two, sampleRate: SR, onsetWait }),
      );
    }
    expectInvalidParameter(() =>
      extractPercussiveEvents({ samples: two, sampleRate: SR, onsetDelta: Number.NaN }),
    );
    for (const hpssKernelHarmonic of [-1, -31]) {
      expectInvalidParameter(() =>
        extractPercussiveEvents({ samples: two, sampleRate: SR, hpssKernelHarmonic }),
      );
    }
    expectInvalidParameter(() =>
      extractPercussiveEvents({ samples: two, sampleRate: SR, hpssKernelPercussive: -1 }),
    );

    // Both ends of the documented ratio range are inside it, and a zeroed knob
    // is the default rather than an error.
    for (const minPercussiveRatio of [0, 1]) {
      expect(() =>
        extractPercussiveEvents({ samples: two, sampleRate: SR, minPercussiveRatio }),
      ).not.toThrow();
    }
    expect(
      measuredSet(
        extractPercussiveEvents({
          samples: two,
          sampleRate: SR,
          nFft: 0,
          hopLength: 0,
          hpssKernelHarmonic: 0,
          hpssKernelPercussive: 0,
          onsetWait: 0,
          onsetDelta: 0,
          maxEventMs: 0,
          minPercussiveRatio: 0,
        }),
      ),
    ).toEqual(measuredSet(twoEvents));
  });

  it('accepts a plain number array as its audio', () => {
    expect(
      measuredSet(extractPercussiveEvents({ samples: Array.from(two), sampleRate: SR })),
    ).toEqual(measuredSet(twoEvents));
  });
});

describe('renderPercussiveEvents', () => {
  it('reproduces the input bit for bit for an identity set', () => {
    // Hand-built spans first, so the property does not depend on the extractor.
    const handBuilt: PercussiveEventInput[] = [
      { onsetSample: 0, offsetSample: 22050 },
      { onsetSample: 22050, offsetSample: 44100 },
    ];
    expectIdentical(
      renderPercussiveEvents({ samples: three, sampleRate: SR, events: handBuilt }),
      three,
    );

    // Then the shape a caller actually holds between an extract and a render.
    expectIdentical(
      renderPercussiveEvents({ samples: three, sampleRate: SR, events: threeEvents }),
      three,
    );

    // And the emptiest set there is.
    expectIdentical(renderPercussiveEvents({ samples: three, sampleRate: SR, events: [] }), three);
  });

  it('renders a set whose edits were changed and nothing else', () => {
    // The round trip a host performs: extract, touch only `edit`, hand the same
    // objects back. Every other field travels through untouched.
    const edited = threeEvents.map((event, index) =>
      index === 1 ? { ...event, edit: { ...event.edit, gainDb: -6.0206 } } : event,
    );
    const rendered = renderPercussiveEvents({ samples: three, sampleRate: SR, events: edited });
    expectEdited(rendered, three);

    // Only the edited event's span was written, so its neighbours are bit for
    // bit what they were — and that span is not.
    expect(firstMismatch(three, rendered, 0, threeEvents[1].onsetSample)).toBe(-1);
    expect(firstMismatch(three, rendered, threeEvents[1].offsetSample, three.length)).toBe(-1);
    expect(
      firstMismatch(three, rendered, threeEvents[1].onsetSample, threeEvents[1].offsetSample),
    ).not.toBe(-1);

    // The set that produced it is unchanged, so a host may keep holding it.
    expect(measuredSet(threeEvents)).toEqual(
      measuredSet(extractPercussiveEvents({ samples: three, sampleRate: SR })),
    );
  });

  it('mutes a hit and leaves the note under it sounding', () => {
    // This is the property the whole separation exists for, so both halves are
    // asserted: the hit goes and the note stays, at its own level.
    const target = eventAt(layeredEvents, LAYERED_HIT_START);
    const rendered = renderPercussiveEvents({
      samples: layered.mixed,
      sampleRate: SR,
      events: [{ ...target, edit: { muted: true } }],
    });
    expect(rendered).toHaveLength(layered.mixed.length);

    const hitEnd = LAYERED_HIT_START + HIT_SAMPLES;
    // Half one: over the hit's own window, what is left of it is a fraction of
    // what was there. The "before" figure is the synthesised peak exactly,
    // recovered by subtracting the note-only reference.
    const before = maxDifference(layered.mixed, layered.noteOnly, LAYERED_HIT_START, hitEnd);
    expect(before).toBeCloseTo(LAYERED_HIT_PEAK, 4);
    const after = maxDifference(rendered, layered.noteOnly, LAYERED_HIT_START, hitEnd);
    expect(after).toBeLessThan(0.5 * before);

    // Half two: the note is still there at its own level, bounded from both
    // sides rather than merely "not silent". A 0.8 sine has RMS 0.8 / sqrt(2).
    const noteRms = LAYERED_NOTE_AMPLITUDE / Math.SQRT2;
    const renderedRms = rms(rendered, target.onsetSample, target.offsetSample);
    expect(renderedRms).toBeGreaterThan(0.9 * noteRms);
    expect(renderedRms).toBeLessThan(1.1 * noteRms);

    // Nothing outside the span moved at all.
    expect(firstMismatch(layered.mixed, rendered, 0, target.onsetSample)).toBe(-1);
    expect(firstMismatch(layered.mixed, rendered, target.offsetSample, layered.mixed.length)).toBe(
      -1,
    );
  });

  it('moves a hit to its new position', () => {
    // Two isolated hits 1.3 s apart. The first is shifted by its own span
    // length, so source and destination are adjacent and disjoint, and the
    // second hit is nowhere near either.
    expect(movableEvents).toHaveLength(2);
    const target = movableEvents[0];
    expectSpanCovers(target, MOVE_HIT_STARTS[0]);
    const span = target.offsetSample - target.onsetSample;
    expect(span).toBe(11025); // the 500 ms cap, the next onset being further off

    const moved = renderPercussiveEvents({
      samples: movable,
      sampleRate: SR,
      events: [{ ...target, edit: { timeOffsetSamples: span } }],
    });
    const silenced = renderPercussiveEvents({
      samples: movable,
      sampleRate: SR,
      events: [{ ...target, edit: { muted: true } }],
    });

    // The span's own edges decide what is written; where the hit was synthesised
    // decides where to listen. They are not the same sample, because the span
    // opens in front of the transient.
    const oldStart = MOVE_HIT_STARTS[0];
    const newStart = oldStart + span;
    const sourceLevel = rms(movable, oldStart, oldStart + HIT_SAMPLES);
    expect(sourceLevel).toBeGreaterThan(0);

    // Energy leaves the old position ...
    expect(rms(moved, oldStart, oldStart + HIT_SAMPLES)).toBeLessThan(0.4 * sourceLevel);
    // ... and arrives at the new one, at the level it left with. Bounded above
    // as well: an arrival at ten times the level is not a move.
    expect(rms(moved, newStart, newStart + HIT_SAMPLES)).toBeGreaterThan(0.5 * sourceLevel);
    expect(rms(moved, newStart, newStart + HIT_SAMPLES)).toBeLessThan(1.4 * sourceLevel);

    // The moved signal is the lifted one translated, sample for sample, and not
    // a signal separated afresh at the destination. `movable - silenced` is the
    // lifted signal where it sat, `moved - silenced` is it where it landed.
    expect(target.offsetSample + span).toBeLessThanOrEqual(movable.length);
    let worst = 0;
    for (let i = target.onsetSample; i < target.offsetSample; i += 1) {
      const destination = i + span;
      worst = Math.max(
        worst,
        Math.abs(moved[destination] - silenced[destination] - (movable[i] - silenced[i])),
      );
    }
    expect(worst).toBeLessThan(1e-5);
    // ... and the translated signal is the hit, not a sliver of it.
    expect(maxDifference(movable, silenced, oldStart, oldStart + HIT_SAMPLES)).toBeGreaterThan(
      0.15,
    );

    // Only the two spans are written, so the second hit is untouched.
    expect(firstMismatch(movable, moved, 0, target.onsetSample)).toBe(-1);
    expect(firstMismatch(movable, moved, target.offsetSample + span, movable.length)).toBe(-1);
  });

  it('truncates a shift that runs past either end', () => {
    const target = movableEvents[0];
    const silenced = renderPercussiveEvents({
      samples: movable,
      sampleRate: SR,
      events: [{ ...target, edit: { muted: true } }],
    });

    // Pushed clean past an end nothing arrives, so the result is the muted
    // render exactly. That is also the statement that nothing wrapped around.
    for (const timeOffsetSamples of [100000, -100000]) {
      const rendered = renderPercussiveEvents({
        samples: movable,
        sampleRate: SR,
        events: [{ ...target, edit: { timeOffsetSamples } }],
      });
      expect(rendered).toHaveLength(movable.length);
      expect(firstMismatch(rendered, silenced), `offset ${timeOffsetSamples}`).toBe(-1);
    }
  });

  it('scales a hit by its gain', () => {
    const renderWith = (gainDb: number, muted: boolean): Float32Array =>
      renderPercussiveEvents({
        samples: three,
        sampleRate: SR,
        events: [{ ...threeEvents[0], edit: { gainDb, muted } }],
      });

    const louder = renderWith(6.0206, false);
    const quieter = renderWith(-6.0206, false);
    const silenced = renderWith(0, true);

    // The render is source + (gain - 1) * lifted, so a difference against the
    // source is the lifted signal at a known scale: +1 at +6.02 dB, -1/2 at
    // -6.02 dB, -1 when muted. Those relations are exact, whatever the lifted
    // signal turns out to be, so the tolerance is float rounding and no more.
    const up = difference(louder, three);
    const down = difference(quieter, three);
    const gone = difference(silenced, three);
    expect(maxResidual(up, down, 2)).toBeLessThan(1e-5);
    expect(maxResidual(up, gone, 1)).toBeLessThan(1e-5);

    // The lifted signal is the hit and not a sliver of it, against the peak the
    // fixture synthesised: an isolated struck sound is nearly all percussive.
    expect(peak(up)).toBeGreaterThan(0.3 * THREE_HIT_PEAKS[0]);
    expect(peak(up)).toBeLessThan(1.2 * THREE_HIT_PEAKS[0]);

    // Direction and rough size over the hit itself. The window is anchored where
    // the hit was written, not on the span's opening edge — the span opens in
    // front of the transient, so an edge-anchored window ends before the hit
    // does and misses the attack.
    const window = THREE_HIT_STARTS[0];
    const sourceLevel = rms(three, window, window + HIT_SAMPLES);
    expect(rms(louder, window, window + HIT_SAMPLES) / sourceLevel).toBeGreaterThan(1.3);
    expect(rms(louder, window, window + HIT_SAMPLES) / sourceLevel).toBeLessThan(2.05);
    expect(rms(quieter, window, window + HIT_SAMPLES) / sourceLevel).toBeGreaterThan(0.45);
    expect(rms(quieter, window, window + HIT_SAMPLES) / sourceLevel).toBeLessThan(0.85);
  });

  it('shapes the tail of the lifted span with fadeMs', () => {
    // A hand-built span, so the closing edge sits at a sample this case chose:
    // 4200 is inside the first hit's decay, where the fade shapes real signal.
    // A span closing in silence would have every fade length multiplying zeros,
    // and the comparison below would hold for an implementation that ignored the
    // field entirely.
    const FADE_SPAN_END = 4200;
    const renderWith = (fadeMs?: number): Float32Array =>
      renderPercussiveEvents({
        samples: two,
        sampleRate: SR,
        events: [{ onsetSample: 3000, offsetSample: FADE_SPAN_END, edit: { muted: true } }],
        ...(fadeMs === undefined ? {} : { fadeMs }),
      });

    const byDefault = renderWith();
    const short = renderWith(1);
    const long = renderWith(20);

    // 0 selects the documented 5 ms rather than a hard cut, so it is the default
    // render exactly and neither of the other two.
    expectIdentical(renderWith(0), byDefault);
    expect(firstMismatch(short, byDefault)).not.toBe(-1);
    expect(firstMismatch(long, byDefault)).not.toBe(-1);
    expect(firstMismatch(short, long)).not.toBe(-1);

    // A longer fade holds back more of what the mute would have removed, so more
    // of the hit survives over the span's last 20 ms — bounded above as well,
    // since "more" also describes an output that was never edited at all.
    const tail = [FADE_SPAN_END - 441, FADE_SPAN_END] as const; // 20 ms at 22050 Hz.
    const sourceLevel = rms(two, tail[0], tail[1]);
    expect(sourceLevel).toBeGreaterThan(0);
    expect(rms(short, tail[0], tail[1])).toBeLessThan(rms(long, tail[0], tail[1]));
    expect(rms(long, tail[0], tail[1])).toBeLessThan(sourceLevel);

    for (const rendered of [short, long]) {
      // The second hit is never edited, so the output is neither silent nor
      // blown up whatever the fade did to the first.
      expectEdited(rendered, two, 0.55);
    }
  });

  it('lifts the signal with the separation the request names', () => {
    const renderWith = (options: Record<string, number>): Float32Array =>
      renderPercussiveEvents({
        samples: two,
        sampleRate: SR,
        events: [{ ...twoEvents[0], edit: { muted: true } }],
        ...options,
      });

    const byDefault = renderWith({});
    // Each field has to land in its own slot; see the extraction case above for
    // why all three comparisons are needed.
    const shortHarmonic = renderWith({ hpssKernelHarmonic: 3 });
    const shortPercussive = renderWith({ hpssKernelPercussive: 3 });
    expect(firstMismatch(shortHarmonic, byDefault)).not.toBe(-1);
    expect(firstMismatch(shortPercussive, byDefault)).not.toBe(-1);
    expect(firstMismatch(shortHarmonic, shortPercussive)).not.toBe(-1);
    for (const rendered of [byDefault, shortHarmonic, shortPercussive]) {
      expectEdited(rendered, two, 0.55);
    }

    for (const framing of VALID_FRAMINGS) {
      const at = JSON.stringify(framing);
      const rendered = renderWith(framing);
      expectEdited(rendered, two, 0.55);
      expect(firstMismatch(rendered, byDefault), at).not.toBe(-1);
    }
  });

  it('rejects a broken framing even for an all-identity set', () => {
    // Two promises pull against each other here: an all-identity set is a
    // bit-exact pass-through that runs no separation, and the framing is checked
    // anyway. An implementation that validates the framing where it builds the
    // STFT returns the input instead of throwing, and only this sees it.
    for (const framing of BROKEN_FRAMINGS) {
      expectInvalidParameter(() =>
        renderPercussiveEvents({ samples: two, sampleRate: SR, events: twoEvents, ...framing }),
      );
      // And on the emptiest set there is: an unusable config is an error on
      // every set, not on the ones that happen to reach the separation.
      expectInvalidParameter(() =>
        renderPercussiveEvents({ samples: two, sampleRate: SR, events: [], ...framing }),
      );
    }

    // Inside the rule the pass-through still holds, so the rejections are not
    // simply "any non-default framing".
    for (const framing of VALID_FRAMINGS) {
      expectIdentical(
        renderPercussiveEvents({ samples: two, sampleRate: SR, events: [], ...framing }),
        two,
      );
    }
  });

  it('validates an event whose edit is the identity', () => {
    // An unrenderable set is unrenderable whether or not this call would touch
    // it, so the all-identity fast path does not get to skip the checks.
    const render = (events: PercussiveEventInput[]): Float32Array =>
      renderPercussiveEvents({ samples: three, sampleRate: SR, events });
    const length = three.length;

    // An empty span has nothing to lift, and a reversed one is not a span.
    expectInvalidParameter(() => render([{ onsetSample: 4410, offsetSample: 4410 }]));
    expectInvalidParameter(() => render([{ onsetSample: 15435, offsetSample: 4410 }]));
    // Outside the audio at either end.
    expectInvalidParameter(() => render([{ onsetSample: -512, offsetSample: 4410 }]));
    expectInvalidParameter(() => render([{ onsetSample: 4410, offsetSample: length + 1 }]));
    expectInvalidParameter(() => render([{ onsetSample: length, offsetSample: length + 4410 }]));
    // Overlapping source spans are not a renderable set; touching ones are.
    expectInvalidParameter(() =>
      render([
        { onsetSample: 0, offsetSample: 22050 },
        { onsetSample: 11025, offsetSample: 33075 },
      ]),
    );
    expectIdentical(
      render([
        { onsetSample: 0, offsetSample: 22050 },
        { onsetSample: 22050, offsetSample: 33075 },
      ]),
      three,
    );
  });

  it('rejects malformed audio, edits and config', () => {
    const edited: PercussiveEventInput[] = [
      { onsetSample: 4410, offsetSample: 15435, edit: { gainDb: -3 } },
    ];

    expectInvalidParameter(() =>
      renderPercussiveEvents({ samples: new Float32Array(0), sampleRate: SR, events: edited }),
    );
    expect(() => renderPercussiveEvents({ samples: three, sampleRate: 0, events: edited })).toThrow(
      RangeError,
    );
    expect(() =>
      renderPercussiveEvents({
        samples: three,
        sampleRate: SR,
        events: undefined as unknown as PercussiveEventInput[],
      }),
    ).toThrow(TypeError);
    expect(() =>
      renderPercussiveEvents({
        samples: three,
        sampleRate: SR,
        events: [42 as unknown as PercussiveEventInput],
      }),
    ).toThrow(/each event must be a plain object/);
    expect(() =>
      renderPercussiveEvents({
        samples: three,
        sampleRate: SR,
        events: [{ onsetSample: 0, offsetSample: 4410, edit: 7 as unknown as { gainDb: number } }],
      }),
    ).toThrow(/event\.edit must be a plain object/);

    for (const gainDb of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expectInvalidParameter(() =>
        renderPercussiveEvents({
          samples: three,
          sampleRate: SR,
          events: [{ onsetSample: 4410, offsetSample: 15435, edit: { gainDb } }],
        }),
      );
    }

    // A negative or non-finite fade is rejected; 0 is the default rather than a
    // hard cut, which the fadeMs case above pins.
    for (const fadeMs of [Number.NaN, Number.POSITIVE_INFINITY, -1]) {
      expectInvalidParameter(() =>
        renderPercussiveEvents({ samples: three, sampleRate: SR, events: edited, fadeMs }),
      );
    }
    for (const hpssKernelHarmonic of [-1, -31]) {
      expectInvalidParameter(() =>
        renderPercussiveEvents({
          samples: three,
          sampleRate: SR,
          events: edited,
          hpssKernelHarmonic,
        }),
      );
    }
    expectInvalidParameter(() =>
      renderPercussiveEvents({
        samples: three,
        sampleRate: SR,
        events: edited,
        hpssKernelPercussive: -1,
      }),
    );

    expect(() =>
      renderPercussiveEvents({ samples: three, sampleRate: SR, events: edited }),
    ).not.toThrow();
  });

  it('accepts a plain number array as its audio', () => {
    expectIdentical(
      renderPercussiveEvents({ samples: Array.from(two), sampleRate: SR, events: twoEvents }),
      two,
    );
  });
});
