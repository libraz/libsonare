import { describe, expect, it } from 'vitest';
import type { AlignTakeToReferenceRequest } from '../src/index.js';
import { alignTakeToReference, ErrorCode, isSonareError, Project } from '../src/index.js';
import { glide } from './_helpers.js';

const SR = 22050;

/** The take runs half again as long as the reference, so the axes cannot be confused. */
const reference = glide(261.63, 0.5, { sampleRate: SR });
const take = glide(261.63, 0.75, { sampleRate: SR });

const request = (
  overrides: Partial<AlignTakeToReferenceRequest> = {},
): AlignTakeToReferenceRequest => ({
  reference,
  take,
  sampleRate: SR,
  ...overrides,
});

describe('alignTakeToReference', () => {
  it('returns anchors the project warp map accepts, oriented for the take', () => {
    const { anchors, alignment } = alignTakeToReference(request());

    expect(anchors.length).toBeGreaterThanOrEqual(2);
    for (let i = 1; i < anchors.length; i++) {
      const previous = anchors[i - 1] as { warpSample: number; sourceSample: number };
      const current = anchors[i] as { warpSample: number; sourceSample: number };
      expect(current.warpSample).toBeGreaterThan(previous.warpSample);
      expect(current.sourceSample).toBeGreaterThan(previous.sourceSample);
    }

    // Orientation: warpSample is on the reference timeline, sourceSample in the
    // take. The two lengths differ by half again, so a swap shows up here.
    const last = anchors[anchors.length - 1] as { warpSample: number; sourceSample: number };
    expect(last.warpSample).toBeLessThan(reference.length);
    expect(last.sourceSample).toBeLessThan(take.length);
    expect(last.sourceSample).toBeGreaterThan(last.warpSample);

    // The frame counts follow the arguments this entry point took, not the ones
    // it passes on, so a swap inside the wrapper shows up too.
    expect(alignment.referenceFrames).toBeGreaterThan(0);
    expect(alignment.takeFrames).toBeGreaterThan(alignment.referenceFrames);
    expect(Number.isFinite(alignment.meanResidualFrames)).toBe(true);

    // The assertion the whole reduction is for: a repeated coordinate is refused
    // as a warp map, so anchors that install prove they were reduced.
    const project = Project.create();
    try {
      project.setWarpMap({ id: 101, name: 'aligned take', anchors });
    } finally {
      project.destroy();
    }
  });

  it('reads hopLength, so a finer hop yields more anchors', () => {
    // A domain check would only prove the value was read off the request object.
    // Two calls differing in one field, required to answer differently, is what
    // shows it reached the measurement: a hop dropped on the floor gives the
    // same count both times.
    const coarse = alignTakeToReference(request({ hopLength: 1024 })).anchors.length;
    const fine = alignTakeToReference(request({ hopLength: 256 })).anchors.length;
    expect(fine).toBeGreaterThan(coarse);
  });

  it('forwards binsPerOctave far enough for its domain to refuse the value', () => {
    // The cheapest non-vacuity check for this key: the *Property reader sees only
    // the type and the magnitude, so a value refused for being the wrong SHAPE
    // was refused by the chroma front-end, which means it arrived there. The
    // chromagram folds onto 12 pitch classes, so a non-multiple of 12 has no
    // grid to land on.
    for (const binsPerOctave of [13, 18]) {
      let caught: unknown;
      try {
        alignTakeToReference(request({ binsPerOctave }));
      } catch (e) {
        caught = e;
      }
      expect(isSonareError(caught), `binsPerOctave ${binsPerOctave} should be refused`).toBe(true);
      expect((caught as { code: number }).code).toBe(ErrorCode.InvalidParameter);
    }
    // The control: the neighbouring multiples this brackets are accepted at this
    // sample rate, so the refusals above are the divisibility rule rather than a
    // ceiling every value past 12 would hit.
    expect(alignTakeToReference(request({ binsPerOctave: 12 })).anchors.length).toBeGreaterThan(1);
    expect(alignTakeToReference(request({ binsPerOctave: 24 })).anchors.length).toBeGreaterThan(1);
  });

  it('reads binsPerOctave, which moves the path rather than the anchor count', () => {
    // The same argument as above for the other config key, on the observable it
    // actually has: the hop sets the frame rate and so the count, while a finer
    // chroma resolution changes where the path goes. `24` is deliberately not the
    // comparison -- two bins per semitone fold onto the same 12 pitch classes and
    // answer identically, so it would prove nothing.
    const path = (anchors: { warpSample: number; sourceSample: number }[]) =>
      anchors.map((a) => `${a.warpSample}:${a.sourceSample}`).join(',');
    const twelve = alignTakeToReference(request({ binsPerOctave: 12 }));
    const finer = alignTakeToReference(request({ binsPerOctave: 36 }));
    expect(path(twelve.anchors)).not.toBe(path(finer.anchors));
    expect(path(twelve.anchors)).toBe(path(alignTakeToReference(request()).anchors));
  });

  it('treats an omitted hopLength as the library value', () => {
    const omitted = alignTakeToReference(request()).anchors.length;
    const explicitUndefined = alignTakeToReference(
      request({ hopLength: undefined, binsPerOctave: undefined }),
    ).anchors.length;
    expect(explicitUndefined).toBe(omitted);
    // 512 is the library hop; naming it must not change the answer, which is what
    // says the omitted key selected the default rather than some other value.
    expect(alignTakeToReference(request({ hopLength: 512 })).anchors.length).toBe(omitted);
  });

  it('refuses a config key by name rather than substituting a default', () => {
    expect(() =>
      alignTakeToReference(request({ hopLength: '512' as unknown as number })),
    ).toThrowError(/hopLength must be a number/);
    expect(() =>
      alignTakeToReference(request({ binsPerOctave: {} as unknown as number })),
    ).toThrowError(/binsPerOctave must be a number/);
    // A fraction would select the library default and report success, which is a
    // different mistake from a magnitude one step off, so it is named as such.
    expect(() => alignTakeToReference(request({ hopLength: 512.5 }))).toThrowError(
      /hopLength must be a whole number/,
    );
  });

  it('refuses unusable audio', () => {
    // Named per buffer, not as one `samples`: the two are refused by the argument
    // the caller actually spelled.
    expect(() => alignTakeToReference(request({ reference: new Float32Array(0) }))).toThrowError(
      /reference must not be empty/,
    );
    expect(() => alignTakeToReference(request({ take: new Float32Array(0) }))).toThrowError(
      /take must not be empty/,
    );
    const withNan = Float32Array.from(reference);
    withNan[100] = Number.NaN;
    expect(() => alignTakeToReference(request({ reference: withNan }))).toThrowError(
      /reference contains NaN or Inf/,
    );
  });

  it('refuses a sample rate outside the supported range', () => {
    expect(() => alignTakeToReference(request({ sampleRate: 0 }))).toThrowError(
      /sampleRate out of supported range/,
    );
    expect(() => alignTakeToReference(request({ sampleRate: -SR }))).toThrowError(
      /sampleRate out of supported range/,
    );
    expect(() => alignTakeToReference(request({ sampleRate: SR + 0.5 }))).toThrowError(
      /sampleRate must be an integer/,
    );
  });

  it('reports an unalignable pair rather than answering with an unusable map', () => {
    // One chroma frame per signal cannot yield two distinct anchors, and the
    // library says so instead of handing back a map setWarpMap would refuse.
    let caught: unknown;
    try {
      alignTakeToReference(request({ hopLength: 1_000_000 }));
    } catch (e) {
      caught = e;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as { code: number }).code).toBe(ErrorCode.InvalidParameter);
  });

  describe('a take recorded at another rate', () => {
    const REF_RATE = 22050;
    const TAKE_RATE = 32000;
    const HOP = 512;
    const LEAD_SEC = 0.5;
    // The take is generated directly at its own rate (no resampler in the
    // oracle). 32000/22050 is 6.44 semitones, not an octave, so a wrong rate
    // cannot be absorbed by the chroma fold.
    const ref = glide(261.63, 1.0, { sampleRate: REF_RATE });
    const body = glide(261.63, 1.0, { sampleRate: TAKE_RATE });
    const lead = Math.round(LEAD_SEC * TAKE_RATE);
    const longTake = new Float32Array(lead + body.length);
    longTake.set(body, lead);
    const at = (takeSampleRate?: number) =>
      alignTakeToReference({
        reference: ref,
        take: longTake,
        sampleRate: REF_RATE,
        takeSampleRate,
      });

    it('counts take frames at the reference rate', () => {
      const { alignment } = at(TAKE_RATE);
      const expected = Math.ceil((longTake.length * REF_RATE) / TAKE_RATE / HOP);
      expect(Math.abs(alignment.takeFrames - expected)).toBeLessThanOrEqual(1);
      // Declared at the reference rate instead, the take is read as 1.45x longer.
      expect(at(REF_RATE).alignment.takeFrames).toBeGreaterThan(expected * 1.3);
    });

    it('puts both anchor axes in reference-rate samples', () => {
      const { anchors } = at(TAKE_RATE);
      const offset = LEAD_SEC * REF_RATE;
      // The silence run collapses to one boundary anchor at the reference start.
      const rest = anchors.slice(1);
      expect(rest.length).toBeGreaterThan(2);
      for (const a of rest) {
        expect(Math.abs(a.sourceSample - a.warpSample - offset)).toBeLessThanOrEqual(HOP);
      }
      const wrong = at(REF_RATE).anchors.slice(1);
      const off = wrong.filter((a) => Math.abs(a.sourceSample - a.warpSample - offset) > HOP);
      expect(off.length).toBeGreaterThan(wrong.length / 2);
    });

    it('equals the reference rate when takeSampleRate is omitted', () => {
      const omitted = at(undefined);
      const explicit = at(REF_RATE);
      expect(omitted).toEqual(explicit);
    });

    it('refuses a take rate the way it refuses sampleRate, naming the field', () => {
      for (const bad of [0, -TAKE_RATE, 7999, 384001]) {
        expect(() => at(bad)).toThrowError(/takeSampleRate out of supported range/);
      }
      expect(() => at(TAKE_RATE + 0.5)).toThrowError(/takeSampleRate must be an integer/);
      expect(() => at('x' as unknown as number)).toThrowError(/takeSampleRate must be a number/);
    });
  });
});
