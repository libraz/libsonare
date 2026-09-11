/**
 * Percussive-event extraction and rendering on the WASM surface.
 *
 * The editing model rests on one property: a set of events whose edits are all
 * identity renders back to the input bit for bit. Everything else here is an
 * edit applied to exactly one event's span, checked against the untouched
 * neighbour, or a request field checked by the difference it makes to the
 * result.
 *
 * The fixtures are synthesised at 22050 Hz on the extractor's default framing
 * (2048-point FFT, 512 hop), so every span position is reasoned in whole hops.
 * Each hit carries its own noise seed and its own peak level: on a flat fixture
 * a span that read a neighbour's audio measures the right number anyway.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  extractPercussiveEvents,
  init,
  isSonareError,
  type PercussiveEvent,
  type PercussiveEventInput,
  renderPercussiveEvents,
} from '../src/index';

const sampleRate = 22050;
const hopLength = 512;

/** 60 ms at 22050 Hz, exactly; the synthesised length of one hit. */
const hitSamples = 1323;
const hitDecayMs = 12;

/** Where `threeHits` writes its hits, and at what peak level. */
const threeHitStarts = [4410, 13230, 22050];
const threeHitPeaks = [0.5, 0.34, 0.22];
const noteStart = 8820;
const lateHitStart = 52920;
const moveHitStarts = [6615, 35280];

/**
 * How far in front of its transient an onset may legitimately sit: the
 * detector's own backtrack bound (10 frames), at the framing the fixtures use.
 */
const backtrackReach = 10 * hopLength;

/** Deterministic uniform noise in `[-1, 1)`, one stream per seed. */
function noise(seed: number, samples: number): Float64Array {
  const out = new Float64Array(samples);
  let state = (Math.imul(seed, 2654435761) + 1) >>> 0;
  for (let i = 0; i < samples; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = (state >>> 9) / 4194304 - 1;
  }
  return out;
}

/**
 * An exponentially decaying noise burst: the fixtures' struck sound. Normalised
 * so the peak is exactly `amplitude`, which makes the synthesised level an
 * anchor for `peakAmplitude` and for the lifted signal's size.
 */
function hit(seed: number, amplitude: number): Float64Array {
  const burst = noise(seed, hitSamples);
  const decay = hitDecayMs * 0.001 * sampleRate;
  let worst = 0;
  for (let i = 0; i < burst.length; i++) {
    burst[i] *= Math.exp(-i / decay);
    worst = Math.max(worst, Math.abs(burst[i]));
  }
  const scale = amplitude / worst;
  for (let i = 0; i < burst.length; i++) {
    burst[i] *= scale;
  }
  return burst;
}

interface HitSpec {
  start: number;
  seed: number;
  amplitude: number;
}

function addHits(into: Float32Array, specs: readonly HitSpec[]): void {
  for (const spec of specs) {
    const burst = hit(spec.seed, spec.amplitude);
    for (let i = 0; i < burst.length && spec.start + i < into.length; i++) {
      into[spec.start + i] += burst[i];
    }
  }
}

/**
 * A sustained sine over `[start, end)` with a 2 ms raised-cosine edge at each
 * end: short enough that the attack still trips the detector, long enough that
 * the buffer carries no step the separation would read as one long click.
 */
function addNote(
  into: Float32Array,
  start: number,
  end: number,
  frequencyHz: number,
  amplitude: number,
): void {
  const edgeSamples = 44; // 2 ms at 22050 Hz.
  for (let i = start; i < end; i++) {
    const position = i - start;
    const remaining = end - 1 - i;
    let envelope = 1;
    if (position < edgeSamples) {
      envelope = 0.5 - 0.5 * Math.cos((Math.PI * position) / edgeSamples);
    } else if (remaining < edgeSamples) {
      envelope = 0.5 - 0.5 * Math.cos((Math.PI * remaining) / edgeSamples);
    }
    into[i] += amplitude * envelope * Math.sin((2 * Math.PI * frequencyHz * position) / sampleRate);
  }
}

/**
 * Three isolated hits at descending, distinct levels, 400 ms apart, followed by
 * a tail longer than the default 500 ms cap.
 */
function threeHits(): Float32Array {
  const samples = new Float32Array(44100); // 2.0 s
  addHits(samples, [
    { start: threeHitStarts[0], seed: 1, amplitude: threeHitPeaks[0] },
    { start: threeHitStarts[1], seed: 2, amplitude: threeHitPeaks[1] },
    { start: threeHitStarts[2], seed: 3, amplitude: threeHitPeaks[2] },
  ]);
  return samples;
}

/** Two isolated hits inside one second: the compact fixture the sweeps run on. */
function twoHits(): Float32Array {
  const samples = new Float32Array(22050); // 1.0 s
  addHits(samples, [
    { start: 3528, seed: 31, amplitude: 0.5 },
    { start: 12348, seed: 32, amplitude: 0.3 },
  ]);
  return samples;
}

/**
 * A sustained note and, well after it, one isolated hit. The two onsets are the
 * same kind of event to the detector and opposite kinds to the separation, which
 * is what `percussiveRatio` has to tell apart.
 */
function noteThenHit(): Float32Array {
  const samples = new Float32Array(66150); // 3.0 s
  addNote(samples, noteStart, 44100, 330, 0.8);
  addHits(samples, [{ start: lateHitStart, seed: 11, amplitude: 0.5 }]);
  return samples;
}

/**
 * A quiet hit sitting on top of a loud sustained note, the same note with no hit
 * on it, and the same hit with nothing under it.
 *
 * The source peak over the hit's span is the note's, an order above the hit's
 * own, so a `peakAmplitude` measured on the source and one measured on the
 * percussive component cannot be confused. `hitOnly` is the identical hit with
 * nothing sustaining through it, which is the only thing `percussiveRatio`
 * responds to between the two.
 */
function layeredFixture() {
  const hitStart = 22050;
  const hitPeak = 0.15;
  const noteAmplitude = 0.8;
  const noteOnly = new Float32Array(44100); // 2.0 s
  addNote(noteOnly, 0, 44100, 440, noteAmplitude);
  const mixed = Float32Array.from(noteOnly);
  addHits(mixed, [{ start: hitStart, seed: 21, amplitude: hitPeak }]);
  const hitOnly = new Float32Array(noteOnly.length);
  addHits(hitOnly, [{ start: hitStart, seed: 21, amplitude: hitPeak }]);
  return { mixed, noteOnly, hitOnly, hitStart, hitPeak, noteAmplitude };
}

function rms(data: Float32Array, lo: number, hi: number): number {
  const end = Math.min(hi, data.length);
  let sum = 0;
  for (let i = lo; i < end; i++) {
    sum += data[i] * data[i];
  }
  return Math.sqrt(sum / Math.max(1, end - lo));
}

function peak(data: Float32Array, lo = 0, hi = data.length): number {
  const end = Math.min(hi, data.length);
  let highest = 0;
  for (let i = lo; i < end; i++) {
    highest = Math.max(highest, Math.abs(data[i]));
  }
  return highest;
}

function maxAbsDifference(a: Float32Array, b: Float32Array, lo = 0, hi = a.length): number {
  const end = Math.min(hi, Math.min(a.length, b.length));
  let worst = 0;
  for (let i = lo; i < end; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

/**
 * `a - b`, sample by sample. The renderer writes `source + (gain - 1) * lifted`,
 * so a difference against the source is the lifted signal at a known scale, and
 * two differences at two gains are the same signal at two scales.
 */
function difference(a: Float32Array, b: Float32Array): Float32Array {
  const out = new Float32Array(a.length);
  for (let i = 0; i < a.length; i++) {
    out[i] = a[i] - b[i];
  }
  return out;
}

/** `max |a + scale * b|` over the whole buffer. */
function maxResidual(a: Float32Array, b: Float32Array, scale: number): number {
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] + scale * b[i]));
  }
  return worst;
}

/** Asserts the two buffers agree sample for sample over `[lo, hi)`. */
function expectIdenticalOver(a: Float32Array, b: Float32Array, lo: number, hi: number): void {
  expect(Float32Array.from(a.subarray(lo, hi))).toEqual(Float32Array.from(b.subarray(lo, hi)));
}

/** Index of the event whose onset sits closest to `sample`. */
function nearestEvent(events: readonly PercussiveEvent[], sample: number): number {
  expect(events.length).toBeGreaterThan(0);
  let best = 0;
  let bestDistance = Math.abs(events[0].onsetSample - sample);
  for (let i = 1; i < events.length; i++) {
    const distance = Math.abs(events[i].onsetSample - sample);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

/**
 * Asserts the span opens in front of the transient at `start` and closes past
 * it. Not a symmetric tolerance: peak-picking lands after a transient starts, so
 * an onset at or after `start` means the span opened inside its own hit, and the
 * early bound is the detector's own limit on how far backtracking may travel.
 */
function expectSpanCovers(event: PercussiveEvent, start: number, coverage = hitSamples): void {
  expect(event.onsetSample).toBeLessThanOrEqual(start);
  expect(event.onsetSample).toBeGreaterThan(start - backtrackReach);
  expect(event.offsetSample).toBeGreaterThan(start + coverage);
}

/** The event nearest `start`, required to actually be at `start`. */
function eventAt(events: readonly PercussiveEvent[], start: number): PercussiveEvent {
  const event = events[nearestEvent(events, start)];
  expectSpanCovers(event, start);
  return event;
}

/** Whether any span covers `start`; false for an empty set rather than failing. */
function anySpanCovers(events: readonly PercussiveEvent[], start: number): boolean {
  return events.some((event) => event.onsetSample <= start && event.offsetSample > start);
}

function expectWellFormed(events: readonly PercussiveEvent[], samples: Float32Array): void {
  let previousOffset = 0;
  for (const event of events) {
    expect(event.onsetSample).toBeGreaterThanOrEqual(0);
    expect(event.offsetSample).toBeGreaterThan(event.onsetSample);
    expect(event.offsetSample).toBeLessThanOrEqual(samples.length);
    // Ascending and non-overlapping, which is also what makes the set renderable.
    expect(event.onsetSample).toBeGreaterThanOrEqual(previousOffset);
    previousOffset = event.offsetSample;
    expect(Number.isFinite(event.strength)).toBe(true);
    expect(Number.isFinite(event.peakAmplitude)).toBe(true);
    expect(event.peakAmplitude).toBeGreaterThanOrEqual(0);
    expect(event.percussiveRatio).toBeGreaterThanOrEqual(0);
    expect(event.percussiveRatio).toBeLessThanOrEqual(1);
    expect(event.edit).toEqual({ timeOffsetSamples: 0, gainDb: 0, muted: false });
  }
}

/** Asserts that `action` throws a SonareError carrying InvalidParameter. */
function expectInvalidParameter(action: () => void): void {
  let caught: unknown;
  try {
    action();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  if (isSonareError(caught)) {
    expect(caught.code).toBe(ErrorCode.InvalidParameter);
  }
}

/**
 * Framings that break constant overlap-add. Each row is on the far side of
 * exactly one bound — `nFft` even and at least 2, `hopLength` in
 * `(0, nFft / 2]` — so a rejection cannot come from a second defect in the row.
 * A negative value is rejected by the binding before the sentinel promotion; the
 * rest reach the core.
 */
const brokenFramings: readonly { nFft: number; hopLength: number }[] = [
  { nFft: 2047, hopLength: 512 }, // odd
  { nFft: 1023, hopLength: 256 }, // odd, at another size
  { nFft: 1, hopLength: 1 }, // odd and below the minimum
  { nFft: -2048, hopLength: 512 }, // negative
  { nFft: 2048, hopLength: -512 }, // negative advance
  { nFft: 2048, hopLength: 1025 }, // one sample past half the window
  { nFft: 1024, hopLength: 513 }, // the same bound at another size
  { nFft: 2048, hopLength: 2048 }, // no overlap at all
];

/** Framings inside the rule, including its inclusive end. */
const validFramings: readonly { nFft: number; hopLength: number }[] = [
  { nFft: 2048, hopLength: 512 }, // the default
  { nFft: 2048, hopLength: 1024 }, // exactly half the window
  { nFft: 1024, hopLength: 512 }, // the same boundary at another size
];

let three: Float32Array;
let compact: Float32Array;
let threeEvents: PercussiveEvent[];

beforeAll(async () => {
  await init();
  three = threeHits();
  compact = twoHits();
  threeEvents = extractPercussiveEvents({ samples: three, sampleRate });
});

describe('extractPercussiveEvents', () => {
  it('finds every hit, in order, with an identity edit', () => {
    expect(threeEvents).toHaveLength(3);
    expectWellFormed(threeEvents, three);

    for (let i = 0; i < 3; i++) {
      expectSpanCovers(threeEvents[i], threeHitStarts[i]);
      expect(threeEvents[i].strength).toBeGreaterThan(0);
    }
    expect(threeEvents[0].onsetSample).toBeLessThan(threeEvents[1].onsetSample);
    expect(threeEvents[1].onsetSample).toBeLessThan(threeEvents[2].onsetSample);

    // These are struck sounds in silence, so the separation has to call them
    // percussive. The complementary half — that the figure is not simply pinned
    // at its ceiling for everything — is the layered case below.
    for (const event of threeEvents) {
      expect(event.percussiveRatio).toBeGreaterThan(0.5);
    }
  });

  it('surfaces exactly the six documented event fields', () => {
    expect(Object.keys(threeEvents[0]).sort()).toEqual([
      'edit',
      'offsetSample',
      'onsetSample',
      'peakAmplitude',
      'percussiveRatio',
      'strength',
    ]);
    // No pitch, no curve: a struck sound has no steady F0 to edit, so the edit
    // axes are time and amplitude and nothing else.
    expect(Object.keys(threeEvents[0].edit).sort()).toEqual([
      'gainDb',
      'muted',
      'timeOffsetSamples',
    ]);
  });

  it('closes a span on the next onset and caps the last one', () => {
    // Adjacent, exactly: the interior spans are closed by the following onset and
    // nothing else, so there is no gap and no rounding to absorb.
    expect(threeEvents[0].offsetSample).toBe(threeEvents[1].onsetSample);
    expect(threeEvents[1].offsetSample).toBe(threeEvents[2].onsetSample);

    // The hits are 400 ms apart, inside the 500 ms cap, so the cap binds on the
    // last span only. 500 ms is 11025 samples at this rate, exactly.
    const lengths = threeEvents.map((event) => event.offsetSample - event.onsetSample);
    expect(lengths[0]).toBeLessThan(11025);
    expect(lengths[1]).toBeLessThan(11025);
    expect(lengths[2]).toBe(11025);
    expect(threeEvents[2].offsetSample).toBeLessThan(three.length);
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

    // Separation is close to linear in the source level, so the measured ratios
    // track the synthesised ones.
    const measured = threeEvents[0].peakAmplitude / threeEvents[1].peakAmplitude;
    expect(Math.abs(measured - threeHitPeaks[0] / threeHitPeaks[1])).toBeLessThanOrEqual(0.45);
  });

  it('measures peakAmplitude on the percussive component, not the source', () => {
    const fixture = layeredFixture();
    const events = extractPercussiveEvents({ samples: fixture.mixed, sampleRate });
    const event = eventAt(events, fixture.hitStart);

    // The source over this span is dominated by the 0.8 note, so the two
    // candidate signals are an order apart and the answer says which was read.
    expect(peak(fixture.mixed, event.onsetSample, event.offsetSample)).toBeGreaterThan(0.75);
    expect(event.peakAmplitude).toBeGreaterThan(0);
    expect(event.peakAmplitude).toBeLessThan(0.35);
  });

  it('separates a struck sound from a sustained attack', () => {
    // Both onsets are detected; what tells them apart is how much of the span the
    // separation called percussive. A figure pinned at 1 for everything fails the
    // ordering here, not merely the bounds.
    const samples = noteThenHit();
    const events = extractPercussiveEvents({ samples, sampleRate });
    expect(events.length).toBeGreaterThanOrEqual(2);
    expectWellFormed(events, samples);

    const noteIndex = nearestEvent(events, noteStart);
    const hitIndex = nearestEvent(events, lateHitStart);
    expect(noteIndex).not.toBe(hitIndex);
    expectSpanCovers(events[noteIndex], noteStart);
    expectSpanCovers(events[hitIndex], lateHitStart);

    const noteRatio = events[noteIndex].percussiveRatio;
    const hitRatio = events[hitIndex].percussiveRatio;
    expect(hitRatio).toBeGreaterThan(noteRatio);
    // Discriminating rather than merely ordered, from both ends.
    expect(noteRatio).toBeLessThan(0.9);
    expect(hitRatio).toBeGreaterThan(0.5);
    expect(noteRatio).toBeLessThan(0.5 * hitRatio);
  });

  it('reads a hit under a sustain as barely percussive', () => {
    // The figure is a property of the span's energy, not evidence about the
    // onset that opened it. A genuine hit over a loud sustain reads near 0
    // because the sustain owns the span, so a low ratio is not "no hit here".
    // These are the same samples twice, differing only in what sustains through
    // them.
    const fixture = layeredFixture();
    const buried = extractPercussiveEvents({ samples: fixture.mixed, sampleRate });
    const underSustain = eventAt(buried, fixture.hitStart);
    const alone = extractPercussiveEvents({ samples: fixture.hitOnly, sampleRate });
    const isolated = eventAt(alone, fixture.hitStart);

    // Opposite ends of the figure, for the identical hit.
    expect(underSustain.percussiveRatio).toBeLessThan(0.05);
    expect(isolated.percussiveRatio).toBeGreaterThan(0.5);

    // The buried one is a real hit all the same: the separation measured a peak
    // on it the sustain cannot account for. Without this the case would only be
    // saying the ratio is small, which silence also satisfies.
    expect(underSustain.peakAmplitude).toBeGreaterThan(0.3 * fixture.hitPeak);
  });

  it('selects on minPercussiveRatio, which 0 does not turn back into a default', () => {
    // 0 is both this field's default and its own meaning, so it is the one field
    // the zero-is-default rule must not claim.
    const fixture = layeredFixture();
    const buried = extractPercussiveEvents({ samples: fixture.mixed, sampleRate });
    const isolated = extractPercussiveEvents({ samples: fixture.hitOnly, sampleRate });
    const threshold =
      0.5 *
      (eventAt(buried, fixture.hitStart).percussiveRatio +
        eventAt(isolated, fixture.hitStart).percussiveRatio);

    expect(anySpanCovers(buried, fixture.hitStart)).toBe(true);
    expect(
      anySpanCovers(
        extractPercussiveEvents({ samples: fixture.mixed, sampleRate, minPercussiveRatio: 0 }),
        fixture.hitStart,
      ),
    ).toBe(true);
    // A threshold between the two readings drops a hit that is genuinely there,
    // while keeping the identical hit in silence.
    expect(
      anySpanCovers(
        extractPercussiveEvents({
          samples: fixture.mixed,
          sampleRate,
          minPercussiveRatio: threshold,
        }),
        fixture.hitStart,
      ),
    ).toBe(false);
    expect(
      anySpanCovers(
        extractPercussiveEvents({
          samples: fixture.hitOnly,
          sampleRate,
          minPercussiveRatio: threshold,
        }),
        fixture.hitStart,
      ),
    ).toBe(true);
  });

  it('binds maxEventMs on every span once it is short enough', () => {
    // 40 ms is 882 samples at 22050 Hz, exactly, and shorter than the 400 ms
    // between hits, so it now closes every span including the interior ones.
    const capped = extractPercussiveEvents({ samples: three, sampleRate, maxEventMs: 40 });
    expect(capped).toHaveLength(threeEvents.length);
    for (let i = 0; i < capped.length; i++) {
      // The cap moves an offset and never an onset.
      expect(capped[i].onsetSample).toBe(threeEvents[i].onsetSample);
      expect(capped[i].offsetSample - capped[i].onsetSample).toBe(882);
    }

    // Raised past the tail: the last span is no longer cut at 500 ms, and still
    // stops at the end of the audio.
    const loose = extractPercussiveEvents({ samples: three, sampleRate, maxEventMs: 5000 });
    expectWellFormed(loose, three);
    expect(loose.at(-1)?.offsetSample).toBe(three.length);
  });

  it('honours onsetWait and onsetDelta', () => {
    const events = extractPercussiveEvents({ samples: compact, sampleRate });
    expect(events).toHaveLength(2);

    // The two hits are 8820 samples apart, which is 17 hops, so a 40-frame
    // minimum spacing cannot keep both.
    expect(extractPercussiveEvents({ samples: compact, sampleRate, onsetWait: 40 })).toHaveLength(
      1,
    );

    // Raising the threshold offset finds fewer, stronger hits. No onset envelope
    // clears an offset this far above it, so the request field reaching the
    // detector is the difference between two hits and none — which is also the
    // empty result being reported rather than raised as an error.
    expect(extractPercussiveEvents({ samples: compact, sampleRate, onsetDelta: 1e6 })).toEqual([]);

    // And the other direction: exactly zero is the one unselectable value, so a
    // negative offset is a value rather than an error, and it puts the threshold
    // below the default instead of above it.
    const lowered = extractPercussiveEvents({ samples: compact, sampleRate, onsetDelta: -0.5 });
    expectWellFormed(lowered, compact);
    expect(lowered.length).toBeGreaterThanOrEqual(2);
  });

  it('reads the separation framing and the two kernels', () => {
    // Each field is checked by the difference it makes, not merely by being
    // accepted. The layered fixture is the sensitive one for the kernels: its
    // ratio sits near the floor rather than saturated at the ceiling.
    const fixture = layeredFixture();
    const base = { samples: fixture.mixed, sampleRate };
    const defaults = extractPercussiveEvents(base);

    const finerFraming = extractPercussiveEvents({ ...base, nFft: 1024, hopLength: 256 });
    expectWellFormed(finerFraming, fixture.mixed);
    expect(finerFraming).not.toEqual(defaults);

    for (const options of [{ hpssKernelHarmonic: 3 }, { hpssKernelPercussive: 3 }]) {
      const changed = extractPercussiveEvents({ ...base, ...options });
      expectWellFormed(changed, fixture.mixed);
      expect(changed.map((event) => event.percussiveRatio)).not.toEqual(
        defaults.map((event) => event.percussiveRatio),
      );
    }
  });

  it('takes every default at 0 and at absent, and at the documented value', () => {
    // The zero-is-default rule, and what the defaults actually are: a zeroed
    // request, an absent one and the documented values are one result.
    const zeroed = extractPercussiveEvents({
      samples: three,
      sampleRate,
      nFft: 0,
      hopLength: 0,
      hpssKernelHarmonic: 0,
      hpssKernelPercussive: 0,
      onsetWait: 0,
      onsetDelta: 0,
      maxEventMs: 0,
      minPercussiveRatio: 0,
    });
    expect(zeroed).toEqual(threeEvents);

    const spelled = extractPercussiveEvents({
      samples: three,
      sampleRate,
      nFft: 2048,
      hopLength: 512,
      hpssKernelHarmonic: 31,
      hpssKernelPercussive: 31,
      onsetWait: 1,
      onsetDelta: 0.06,
      maxEventMs: 500,
      minPercussiveRatio: 0,
    });
    expect(spelled).toEqual(threeEvents);

    // Non-vacuity: these fields do decide the result, so the equalities above are
    // the defaults being applied rather than a request nobody read.
    expect(extractPercussiveEvents({ samples: three, sampleRate, maxEventMs: 40 })).not.toEqual(
      threeEvents,
    );
  });

  it('reads a plain number array exactly as it reads a Float32Array', () => {
    // The samples are copied into WASM memory either way, so the two spellings
    // are the same audio rather than merely similar.
    expect(extractPercussiveEvents({ samples: [...compact], sampleRate })).toEqual(
      extractPercussiveEvents({ samples: compact, sampleRate }),
    );
  });

  it('rejects a malformed request', () => {
    const base = { samples: compact, sampleRate };
    expect(() => extractPercussiveEvents({ samples: new Float32Array(0), sampleRate })).toThrow(
      RangeError,
    );
    expect(() => extractPercussiveEvents({ samples: compact, sampleRate: 7999 })).toThrow(
      RangeError,
    );
    expect(() =>
      extractPercussiveEvents({ samples: Float32Array.from([Number.NaN, 0.5]), sampleRate }),
    ).toThrow(RangeError);

    // Negative counts are rejected before the zero-is-default promotion, so none
    // of them is quietly swallowed as "keep the default".
    for (const options of [
      { nFft: -2048 },
      { hopLength: -512 },
      { hpssKernelHarmonic: -1 },
      { hpssKernelPercussive: -1 },
      { onsetWait: -1 },
    ]) {
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, ...options }));
    }

    // A framing that cannot be overlap-added back is an error, because the
    // separation inverts an STFT.
    for (const framing of brokenFramings) {
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, ...framing }));
    }

    // A median kernel is rejected rather than rounded when it is even, and 0 is
    // the only even value that means anything here.
    for (const options of [
      { hpssKernelHarmonic: 4 },
      { hpssKernelPercussive: 4 },
      { hpssKernelHarmonic: 32 },
      { hpssKernelPercussive: 2 },
    ]) {
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, ...options }));
    }

    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, onsetDelta: bad }));
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, maxEventMs: bad }));
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, minPercussiveRatio: bad }));
    }
    expectInvalidParameter(() => extractPercussiveEvents({ ...base, maxEventMs: -1 }));
    for (const bad of [-0.01, -1, 1.01, 2]) {
      expectInvalidParameter(() => extractPercussiveEvents({ ...base, minPercussiveRatio: bad }));
    }

    // Positive controls: both ends of the documented ratio range are values, an
    // odd kernel at the minimum is one, a hop of exactly half the window is
    // inside the overlap rule, and 0 is the default rather than the rejected "no
    // span at all" the core reads it as.
    for (const good of [0, 1]) {
      expect(() => extractPercussiveEvents({ ...base, minPercussiveRatio: good })).not.toThrow();
    }
    expect(() =>
      extractPercussiveEvents({ ...base, hpssKernelHarmonic: 3, hpssKernelPercussive: 3 }),
    ).not.toThrow();
    for (const framing of validFramings) {
      expect(() => extractPercussiveEvents({ ...base, ...framing })).not.toThrow();
    }
    expect(() => extractPercussiveEvents({ ...base, maxEventMs: 0 })).not.toThrow();
  });
});

describe('renderPercussiveEvents', () => {
  it('reproduces the input bit for bit when every edit is the identity', () => {
    // Hand-built spans first, so the property does not depend on the extractor.
    const handBuilt: PercussiveEventInput[] = [
      { onsetSample: 0, offsetSample: 22050 },
      { onsetSample: 22050, offsetSample: 44100 },
    ];
    expect(renderPercussiveEvents({ samples: three, sampleRate, events: handBuilt })).toEqual(
      three,
    );

    // Then the shape a caller actually holds between an extract and a render.
    const rendered = renderPercussiveEvents({ samples: three, sampleRate, events: threeEvents });
    expect(rendered).toEqual(three);

    // And the emptiest set there is.
    expect(renderPercussiveEvents({ samples: three, sampleRate, events: [] })).toEqual(three);
  });

  it('mutes a hit and leaves the note under it sounding', () => {
    // This is the property the whole separation exists for, so both halves are
    // asserted: the hit goes and the note stays, at its own level.
    const fixture = layeredFixture();
    const events = extractPercussiveEvents({ samples: fixture.mixed, sampleRate });
    const event = eventAt(events, fixture.hitStart);
    const rendered = renderPercussiveEvents({
      samples: fixture.mixed,
      sampleRate,
      events: [{ ...event, edit: { ...event.edit, muted: true } }],
    });
    expect(rendered).toHaveLength(fixture.mixed.length);

    const hitEnd = fixture.hitStart + hitSamples;
    // Half one: over the hit's own window, what is left of the hit is a fraction
    // of what was there. The "before" figure is the synthesised peak exactly,
    // recovered by subtracting the note-only reference.
    const before = maxAbsDifference(fixture.mixed, fixture.noteOnly, fixture.hitStart, hitEnd);
    expect(before).toBeCloseTo(fixture.hitPeak, 4);
    const after = maxAbsDifference(rendered, fixture.noteOnly, fixture.hitStart, hitEnd);
    expect(after).toBeLessThan(0.5 * before);

    // Half two: the note is still there at its own level, bounded from both sides
    // rather than merely "not silent". A 0.8 sine has RMS 0.8 / sqrt(2).
    const noteRms = 0.8 / Math.SQRT2;
    expect(rms(rendered, event.onsetSample, event.offsetSample)).toBeGreaterThan(0.9 * noteRms);
    expect(rms(rendered, event.onsetSample, event.offsetSample)).toBeLessThan(1.1 * noteRms);

    // Nothing outside the span moved at all.
    expectIdenticalOver(rendered, fixture.mixed, 0, event.onsetSample);
    expectIdenticalOver(rendered, fixture.mixed, event.offsetSample, fixture.mixed.length);
  });

  it('moves a hit to its new position', () => {
    // Two isolated hits 1.3 s apart. The first is shifted by its own span length,
    // so source and destination are adjacent and disjoint.
    const samples = new Float32Array(48510); // 2.2 s
    addHits(samples, [
      { start: moveHitStarts[0], seed: 41, amplitude: 0.5 },
      { start: moveHitStarts[1], seed: 42, amplitude: 0.32 },
    ]);
    const events = extractPercussiveEvents({ samples, sampleRate });
    expect(events).toHaveLength(2);
    expectSpanCovers(events[0], moveHitStarts[0]);
    const span = events[0].offsetSample - events[0].onsetSample;
    expect(span).toBe(11025); // the 500 ms cap, the next onset being further off

    const renderWith = (edit: PercussiveEventInput['edit']): Float32Array =>
      renderPercussiveEvents({ samples, sampleRate, events: [{ ...events[0], edit }] });
    const moved = renderWith({ timeOffsetSamples: span });
    const silenced = renderWith({ muted: true });

    // The span's own edges decide what is written; where the hit was synthesised
    // decides where to listen. They are not the same sample, because the span
    // opens in front of the transient.
    const oldStart = moveHitStarts[0];
    const newStart = oldStart + span;
    const sourceLevel = rms(samples, oldStart, oldStart + hitSamples);
    expect(sourceLevel).toBeGreaterThan(0);

    // Energy leaves the old position ...
    expect(rms(moved, oldStart, oldStart + hitSamples)).toBeLessThan(0.4 * sourceLevel);
    // ... and arrives at the new one, at the level it left with. Bounded above as
    // well: an arrival at ten times the level is not a move.
    expect(rms(moved, newStart, newStart + hitSamples)).toBeGreaterThan(0.5 * sourceLevel);
    expect(rms(moved, newStart, newStart + hitSamples)).toBeLessThan(1.4 * sourceLevel);

    // The moved signal is the lifted one translated, sample for sample, rather
    // than a signal separated afresh at the destination.
    let worst = 0;
    for (let i = events[0].onsetSample; i < events[0].offsetSample; i++) {
      const destination = i + span;
      worst = Math.max(
        worst,
        Math.abs(moved[destination] - silenced[destination] - (samples[i] - silenced[i])),
      );
    }
    expect(worst).toBeLessThan(1e-5);
    // ... and the translated signal is the hit, not a sliver of it.
    expect(maxAbsDifference(samples, silenced, oldStart, oldStart + hitSamples)).toBeGreaterThan(
      0.15,
    );

    // Only the two spans are written, so the second hit is untouched bit for bit.
    expectIdenticalOver(moved, samples, 0, events[0].onsetSample);
    expectIdenticalOver(moved, samples, events[0].offsetSample + span, samples.length);
  });

  it('truncates a shift that runs past either end', () => {
    const renderShifted = (timeOffsetSamples: number): Float32Array =>
      renderPercussiveEvents({
        samples: three,
        sampleRate,
        events: [{ ...threeEvents[0], edit: { timeOffsetSamples } }],
      });
    const silenced = renderPercussiveEvents({
      samples: three,
      sampleRate,
      events: [{ ...threeEvents[0], edit: { muted: true } }],
    });

    // Pushed clean past an end nothing arrives, so the result is the muted render
    // exactly. That is also the statement that nothing wrapped around.
    expect(renderShifted(100000)).toEqual(silenced);
    expect(renderShifted(-100000)).toEqual(silenced);
    // Non-vacuity: the muted render is not the source, so the two equalities
    // above are a truncated move rather than a pass-through.
    expect(silenced).not.toEqual(three);
  });

  it('scales a hit by its gain', () => {
    const renderWith = (gainDb: number, muted: boolean): Float32Array =>
      renderPercussiveEvents({
        samples: three,
        sampleRate,
        events: [{ ...threeEvents[0], edit: { gainDb, muted } }],
      });

    const louder = renderWith(6.0206, false);
    const quieter = renderWith(-6.0206, false);
    const silenced = renderWith(0, true);

    // The render is source + (gain - 1) * lifted, so a difference against the
    // source is the lifted signal at a known scale: +1 at +6.02 dB, -1/2 at
    // -6.02 dB, -1 when muted. Those relations are exact whatever the lifted
    // signal turns out to be, so the tolerance is float rounding and nothing
    // more.
    const up = difference(louder, three);
    const down = difference(quieter, three);
    const gone = difference(silenced, three);
    expect(maxResidual(up, down, 2)).toBeLessThan(1e-5);
    expect(maxResidual(up, gone, 1)).toBeLessThan(1e-5);

    // The lifted signal is the hit and not a sliver of it, against the peak the
    // fixture synthesised: an isolated struck sound is nearly all percussive.
    expect(peak(up)).toBeGreaterThan(0.3 * threeHitPeaks[0]);
    expect(peak(up)).toBeLessThan(1.2 * threeHitPeaks[0]);

    // Direction and rough size over the hit itself, from both sides.
    const window = threeHitStarts[0];
    const sourceLevel = rms(three, window, window + hitSamples);
    expect(rms(louder, window, window + hitSamples) / sourceLevel).toBeGreaterThan(1.3);
    expect(rms(louder, window, window + hitSamples) / sourceLevel).toBeLessThan(2.05);
    expect(rms(quieter, window, window + hitSamples) / sourceLevel).toBeGreaterThan(0.45);
    expect(rms(quieter, window, window + hitSamples) / sourceLevel).toBeLessThan(0.85);
  });

  it('reads only the span and the edit of a hand-built event', () => {
    // The three measured figures are not read at all, so an event carrying
    // nothing but its span renders exactly as the extracted one does.
    const event = threeEvents[0];
    const fromExtracted = renderPercussiveEvents({
      samples: three,
      sampleRate,
      events: [{ ...event, edit: { gainDb: -6.0206 } }],
    });
    const fromHandBuilt = renderPercussiveEvents({
      samples: three,
      sampleRate,
      events: [
        {
          onsetSample: event.onsetSample,
          offsetSample: event.offsetSample,
          edit: { gainDb: -6.0206 },
        },
      ],
    });
    expect(fromHandBuilt).toEqual(fromExtracted);
    // Non-vacuity: the gain was applied, so the equality is not two
    // pass-throughs.
    expect(fromExtracted).not.toEqual(three);
  });

  it('reads fadeMs, the separation framing and the two kernels', () => {
    const edited: PercussiveEventInput[] = [{ ...threeEvents[0], edit: { muted: true } }];
    const base = { samples: three, sampleRate, events: edited };
    const defaults = renderPercussiveEvents(base);

    // The fade shapes the tail of the lifted span, so a longer one leaves more of
    // the hit behind at the span's end.
    expect(renderPercussiveEvents({ ...base, fadeMs: 50 })).not.toEqual(defaults);
    // 0 and the documented 5 ms are both the default.
    expect(renderPercussiveEvents({ ...base, fadeMs: 0 })).toEqual(defaults);
    expect(renderPercussiveEvents({ ...base, fadeMs: 5 })).toEqual(defaults);

    // The separation the events are lifted with, which a render must be given
    // what the extraction was called with.
    for (const options of [
      { nFft: 1024, hopLength: 256 },
      { hpssKernelHarmonic: 3 },
      { hpssKernelPercussive: 3 },
    ]) {
      const changed = renderPercussiveEvents({ ...base, ...options });
      expect(changed).toHaveLength(three.length);
      expect(changed).not.toEqual(defaults);
    }
    // And the zero-is-default rule on the same four fields.
    expect(
      renderPercussiveEvents({
        ...base,
        nFft: 0,
        hopLength: 0,
        hpssKernelHarmonic: 0,
        hpssKernelPercussive: 0,
      }),
    ).toEqual(defaults);
  });

  it('rejects a malformed request', () => {
    const length = three.length;
    const edited = (onsetSample: number, offsetSample: number): PercussiveEventInput => ({
      onsetSample,
      offsetSample,
      edit: { gainDb: -3 },
    });
    const render = (events: readonly PercussiveEventInput[], extra = {}): Float32Array =>
      renderPercussiveEvents({ samples: three, sampleRate, events, ...extra });

    expect(() =>
      renderPercussiveEvents({ samples: new Float32Array(0), sampleRate, events: [] }),
    ).toThrow(RangeError);
    expect(() => renderPercussiveEvents({ samples: three, sampleRate: 7999, events: [] })).toThrow(
      RangeError,
    );

    // An event that is not one: a missing or non-numeric span bound is rejected
    // here rather than read as 0.
    expectInvalidParameter(() => render([{ offsetSample: 8192 } as PercussiveEventInput]));
    expectInvalidParameter(() => render([{ onsetSample: 0 } as PercussiveEventInput]));
    expectInvalidParameter(() =>
      render([{ onsetSample: '0', offsetSample: 8192 } as unknown as PercussiveEventInput]),
    );
    expectInvalidParameter(() => render(undefined as unknown as readonly PercussiveEventInput[]));

    // An empty span has nothing to lift, a reversed one is not a span, and
    // neither end may sit outside the audio.
    expectInvalidParameter(() => render([edited(4410, 4410)]));
    expectInvalidParameter(() => render([edited(15435, 4410)]));
    expectInvalidParameter(() => render([edited(-512, 4410)]));
    expectInvalidParameter(() => render([edited(4410, length + 1)]));
    expectInvalidParameter(() => render([edited(length, length + 4410)]));

    // Overlapping source spans are not a renderable set; touching ones are.
    expectInvalidParameter(() => render([edited(0, 22050), edited(11025, 33075)]));
    expect(() => render([edited(0, 22050), edited(22050, 33075)])).not.toThrow();

    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expectInvalidParameter(() =>
        render([{ onsetSample: 4410, offsetSample: 15435, edit: { gainDb: bad } }]),
      );
      expectInvalidParameter(() => render([edited(4410, 15435)], { fadeMs: bad }));
    }
    expectInvalidParameter(() => render([edited(4410, 15435)], { fadeMs: -1 }));
    for (const options of [
      { nFft: -2048 },
      { hopLength: -512 },
      { hpssKernelHarmonic: -1 },
      { hpssKernelPercussive: -1 },
      { hpssKernelHarmonic: 4 },
      { hpssKernelPercussive: 4 },
    ]) {
      expectInvalidParameter(() => render([edited(4410, 15435)], options));
    }

    // A zero-length fade is the default here rather than the hard cut the core
    // rejects, and the span it is applied to still renders.
    expect(() => render([edited(4410, 15435)], { fadeMs: 0 })).not.toThrow();
    expect(() => render([edited(4410, 15435)])).not.toThrow();
  });

  it('rejects a broken framing even for an all-identity and an empty set', () => {
    // Two promises pull against each other here: an all-identity set is a
    // bit-exact pass-through that runs no separation, and the framing is checked
    // anyway. An implementation that validates the framing where it builds the
    // STFT returns the input instead of throwing, and only this case sees it.
    const identity: PercussiveEventInput[] = [
      { onsetSample: 0, offsetSample: 22050 },
      { onsetSample: 22050, offsetSample: 44100 },
    ];
    expect(renderPercussiveEvents({ samples: three, sampleRate, events: identity })).toEqual(three);

    for (const framing of brokenFramings) {
      expectInvalidParameter(() =>
        renderPercussiveEvents({ samples: three, sampleRate, events: identity, ...framing }),
      );
      expectInvalidParameter(() =>
        renderPercussiveEvents({ samples: three, sampleRate, events: [], ...framing }),
      );
    }

    // Inside the rule the pass-through still holds, including at exactly half the
    // window, so the rejections are not simply "any non-default framing".
    for (const framing of validFramings) {
      expect(
        renderPercussiveEvents({ samples: three, sampleRate, events: identity, ...framing }),
      ).toEqual(three);
      expect(
        renderPercussiveEvents({ samples: three, sampleRate, events: [], ...framing }),
      ).toEqual(three);
    }
  });

  it('validates an event whose edit is the identity', () => {
    // An unrenderable set is unrenderable whether or not this call would touch
    // it, so the all-identity fast path does not get to skip the checks.
    const span = (onsetSample: number, offsetSample: number): PercussiveEventInput => ({
      onsetSample,
      offsetSample,
    });
    const render = (events: readonly PercussiveEventInput[]): Float32Array =>
      renderPercussiveEvents({ samples: three, sampleRate, events });

    expectInvalidParameter(() => render([span(4410, 4410)]));
    expectInvalidParameter(() => render([span(15435, 4410)]));
    expectInvalidParameter(() => render([span(-512, 4410)]));
    expectInvalidParameter(() => render([span(4410, three.length + 1)]));
    expectInvalidParameter(() => render([span(0, 22050), span(11025, 33075)]));
    // One bad event poisons a set that is otherwise renderable and otherwise
    // entirely identity.
    expectInvalidParameter(() => render([span(0, 11025), span(22050, 11025)]));
  });

  it('round-trips an extracted set through an edit and back', () => {
    // The whole point of the model: extract, change nothing but the edit, render.
    const events = extractPercussiveEvents({ samples: compact, sampleRate });
    expect(events).toHaveLength(2);
    expect(renderPercussiveEvents({ samples: compact, sampleRate, events })).toEqual(compact);

    const edited = events.map((event, index) =>
      index === 0 ? { ...event, edit: { ...event.edit, gainDb: -12 } } : event,
    );
    const rendered = renderPercussiveEvents({ samples: compact, sampleRate, events: edited });
    expect(rendered).toHaveLength(compact.length);
    // Pinned on both sides: the edited span moved and the untouched one did not,
    // and the result is still a signal rather than silence or a blow-up.
    expect(rms(rendered, 3528, 3528 + hitSamples)).toBeLessThan(
      0.6 * rms(compact, 3528, 3528 + hitSamples),
    );
    expect(rms(rendered, 3528, 3528 + hitSamples)).toBeGreaterThan(0);
    expectIdenticalOver(rendered, compact, events[1].onsetSample, compact.length);
  });

  it('skips only the JS pre-scan when validate is false', () => {
    // The flag drops the indexed O(n) scan, not the check: the native layer
    // re-validates the buffer, so a non-finite sample is still rejected — with
    // the native error in place of the RangeError that names the index.
    const poisoned = Float32Array.from(compact);
    poisoned[100] = Number.NaN;
    expect(() => extractPercussiveEvents({ samples: poisoned, sampleRate })).toThrow(RangeError);
    expectInvalidParameter(() =>
      extractPercussiveEvents({ samples: poisoned, sampleRate, validate: false }),
    );
    expectInvalidParameter(() =>
      renderPercussiveEvents({ samples: poisoned, sampleRate, events: [], validate: false }),
    );

    // And on a clean buffer it changes nothing about the result.
    expect(extractPercussiveEvents({ samples: compact, sampleRate, validate: false })).toEqual(
      extractPercussiveEvents({ samples: compact, sampleRate }),
    );
    expect(
      renderPercussiveEvents({ samples: compact, sampleRate, events: [], validate: false }),
    ).toEqual(compact);
  });
});
