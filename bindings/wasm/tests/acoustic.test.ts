/**
 * Geometric room-acoustics tests for the WASM binding. Mirrors the Node /
 * Python synthesizeRir / estimateRoom / roomMorph surfaces.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  detectAcoustic,
  estimateRoom,
  init,
  roomGeometryFromEstimate,
  roomMorph,
  roomMorphStereo,
  synthesizeRir,
} from '../src/index';
import { expectRefusalOf } from './_helpers';

beforeAll(async () => {
  await init();
});

describe('geometric room acoustics', () => {
  it('synthesizes a decaying RIR from geometry', () => {
    const result = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    expect(result.sampleRate).toBe(48000);
    expect(result.rir.length).toBeGreaterThan(0);
    expect(Array.from(result.rir).some((s) => Math.abs(s) > 0)).toBe(true);
  });

  it('throws a RangeError for invalid geometry', () => {
    const attempt = () =>
      synthesizeRir({
        sampleRate: 48000,
        lengthM: 7,
        widthM: 5,
        heightM: 3,
        source: { x: 99, y: 1, z: 1 },
      });
    expect(attempt).toThrow(/source\.x must be finite and in \[0, 7\]/);
    expect(attempt).toThrow(RangeError);
  });

  it('round-trips a known shoebox within tolerance', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    const est = estimateRoom(rir.rir, 48000, {
      aspectHintLw: 7 / 5,
      aspectHintLh: 7 / 3,
      referenceAbsorption: 0.15,
    });
    expect(est.volume).toBeGreaterThan(105 * 0.8);
    expect(est.volume).toBeLessThan(105 * 1.2);
    expect(est.confidence).toBeGreaterThan(0);
    expect(est.rt60Bands?.length ?? 0).toBeGreaterThanOrEqual(4);
    expect(Number.isFinite(est.drrDb)).toBe(true);
  });

  it('morphs toward a target room and is deterministic', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const opts = { lengthM: 12, widthM: 9, heightM: 5, absorption: 0.08, wet: 0.7 };
    const a = roomMorph(samples, 48000, opts);
    const b = roomMorph(samples, 48000, opts);
    expect(a.audio.length).toBeGreaterThan(samples.length);
    expect(a.sampleRate).toBe(48000);
    expect(Array.from(a.audio)).toEqual(Array.from(b.audio));
  });

  it('refuses an out-of-domain RIR field naming the key the caller wrote', () => {
    const room = { lengthM: 7, widthM: 5, heightM: 3, maxSec: 0.2 };
    const impulse = new Float32Array(4000);
    impulse[0] = 1.0;
    for (const [key, value] of [
      ['ismOrder', -1],
      ['maxSec', -1],
      ['mixingTimeMs', -1],
      ['crossfadeMs', -5],
      ['lengthM', Number.NaN],
      ['heightM', 0],
    ] as const) {
      const refusal = expect.objectContaining({
        name: 'RangeError',
        message: expect.stringContaining(key),
      });
      expect(() => synthesizeRir({ sampleRate: 48000, ...room, [key]: value })).toThrow(refusal);
      expect(() => roomMorph(impulse, 48000, { ...room, [key]: value })).toThrow(refusal);
    }
    expect(() => synthesizeRir({ sampleRate: 48000, ...room, ismOrder: 'x' as never })).toThrow(
      TypeError,
    );
  });

  it('refuses a seed that is not a uint32 instead of taking the default', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const opts = { lengthM: 12, widthM: 9, heightM: 5, maxSec: 0.2 };
    expect(() => roomMorph(samples, 48000, { ...opts, seed: 'abc' as never })).toThrow(TypeError);
    for (const seed of [-1, 2 ** 32, 0.5]) {
      expect(() => roomMorph(samples, 48000, { ...opts, seed })).toThrow(RangeError);
    }
    expect(roomMorph(samples, 48000, { ...opts, seed: 2 ** 32 - 1 }).sampleRate).toBe(48000);
  });

  it('states the supported sample-rate range when refusing a room morph', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const opts = { lengthM: 12, widthM: 9, heightM: 5, maxSec: 0.2 };
    for (const rate of [7999, 384001]) {
      expect(() => roomMorph(samples, rate, opts)).toThrow(
        /sampleRate must be an integer in \[8000, 384000\]$/,
      );
    }
  });

  it("publishes the target room synthesis's own diagnostics", () => {
    // The morph synthesizes its target RIR with the same code synthesizeRir
    // uses, so the same clamp fires — and it used to be dropped, leaving a morph
    // through a room the caller did not ask for indistinguishable from one
    // through the room they did.
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const base = {
      lengthM: 12,
      widthM: 9,
      heightM: 5,
      absorption: 0.4,
      wet: 1.0,
      maxSec: 0.3,
    };
    // Well above the synthesizer's safe maximum rather than one past it: this
    // case is about the channel the warning travels on, not about where the
    // clamp sits.
    const clamped = roomMorph(samples, 48000, { ...base, ismOrder: 99 });
    expect(clamped.diagnostics.map((d) => d.code)).toContain('acoustic.ism_order_clamped');
    expect(clamped.diagnostics.every((d) => d.severity === 'warning')).toBe(true);
    // An order the synthesizer does not clamp leaves the list clear of it, so
    // the entry above is the clamp's rather than one every call carries.
    const unclamped = roomMorph(samples, 48000, { ...base, ismOrder: 2 });
    expect(unclamped.diagnostics.map((d) => d.code)).not.toContain('acoustic.ism_order_clamped');
  });

  it('rejects out-of-range sample rates', () => {
    expect(() => synthesizeRir({ sampleRate: 0 })).toThrow();
    expect(() => synthesizeRir({ sampleRate: 500000 })).toThrow();
    const samples = new Float32Array(1000);
    samples[0] = 1.0;
    expect(() => estimateRoom(samples, 0)).toThrow();
    expect(() => roomMorph(samples, 500000, { lengthM: 5, widthM: 4, heightM: 3 })).toThrow();
  });

  it('rejects empty and non-finite input buffers', () => {
    expect(() => estimateRoom(new Float32Array(0), 48000)).toThrow();
    const bad = new Float32Array(1000);
    bad[10] = Number.NaN;
    expect(() => estimateRoom(bad, 48000)).toThrow();
    expect(() => roomMorph(bad, 48000, { lengthM: 5, widthM: 4, heightM: 3 })).toThrow();
  });

  it.each([
    [{ absorption: 2 }, 'absorption'],
    [{ bandAbsorption: new Float32Array([0.2, 1.5, 0.3]) }, 'bandAbsorption'],
    [
      { bandAbsorption: new Float32Array([0.2, 0.3]), bandScattering: new Float32Array([0, -0.5]) },
      'bandScattering',
    ],
  ])('names the material coefficient outside [0, 1] for %o', (fields, name) => {
    expect(() => synthesizeRir({ sampleRate: 48000, ...fields })).toThrow(
      new RangeError(`synthesizeRir: ${name} must be finite and in [0, 1]`),
    );
  });

  it('names a room-morph absorption outside [0, 1]', () => {
    expect(() => roomMorph(new Float32Array(4800).fill(0.1), 48000, { absorption: 1.01 })).toThrow(
      new RangeError('roomMorph: absorption must be finite and in [0, 1]'),
    );
  });

  it('rejects the same invalid room/material inputs the C ABI refuses', () => {
    // Over the 64-band material cap.
    expect(() =>
      synthesizeRir({ sampleRate: 48000, bandAbsorption: new Float32Array(65).fill(0.2) }),
    ).toThrow();
    // Per-band absorption/scattering outside [0, 1] (or non-finite) are rejected,
    // matching the C-ABI oracle's `unit` predicate rather than being silently
    // clamped only on this surface.
    expect(() =>
      synthesizeRir({ sampleRate: 48000, bandAbsorption: new Float32Array([0.2, 1.5, 0.3]) }),
    ).toThrow();
    expect(() =>
      synthesizeRir({ sampleRate: 48000, bandAbsorption: new Float32Array([0.2, -0.2, 0.3]) }),
    ).toThrow();
    expect(() =>
      synthesizeRir({
        sampleRate: 48000,
        bandAbsorption: new Float32Array([0.1, Number.NaN, 0.3]),
      }),
    ).toThrow();
    expect(() =>
      synthesizeRir({
        sampleRate: 48000,
        bandAbsorption: new Float32Array([0.2, 0.3]),
        bandScattering: new Float32Array([0, -0.5]),
      }),
    ).toThrow();
    // An in-range per-band table is accepted.
    expect(() =>
      synthesizeRir({
        sampleRate: 48000,
        bandAbsorption: new Float32Array([0.1, 0.3, 0.5]),
        maxSec: 0.1,
      }),
    ).not.toThrow();
    // The scalar absorption path is still rejected out of [0, 1] (both surfaces).
    expect(() => synthesizeRir({ sampleRate: 48000, absorption: 2 })).toThrow();
    // Non-finite geometry and timing.
    expect(() => synthesizeRir({ sampleRate: 48000, lengthM: Number.NaN })).toThrow();
    expect(() =>
      synthesizeRir({ sampleRate: 48000, source: { x: Number.POSITIVE_INFINITY, y: 1, z: 1 } }),
    ).toThrow();
    expect(() => synthesizeRir({ sampleRate: 48000, maxSec: 100000 })).toThrow();
    // An in-range absorption of exactly 1.0 is accepted (matches the C ABI's
    // [0, 1] bound, not the old 0.999 clamp).
    expect(() => synthesizeRir({ sampleRate: 48000, absorption: 1, maxSec: 0.1 })).not.toThrow();
  });

  it('rejects invalid estimation priors and room-morph configuration', () => {
    const samples = new Float32Array(2000);
    samples[0] = 1;
    expect(() => estimateRoom(samples, 48000, { referenceAbsorption: Number.NaN })).toThrow();
    expect(() =>
      estimateRoom(samples, 48000, { aspectHintLw: Number.POSITIVE_INFINITY }),
    ).toThrow();
    expect(() => roomMorph(samples, 48000, { lengthM: -1, widthM: 4, heightM: 3 })).toThrow();
    expect(() =>
      roomMorph(samples, 48000, {
        lengthM: 5,
        widthM: 4,
        heightM: 3,
        source: { x: 99, y: 1, z: 1 },
      }),
    ).toThrow();
    expect(() =>
      roomMorph(samples, 48000, { lengthM: 5, widthM: 4, heightM: 3, wet: Number.NaN }),
    ).toThrow();
    expect(() =>
      roomMorph(samples, 48000, {
        lengthM: 5,
        widthM: 4,
        heightM: 3,
        maxSec: Number.POSITIVE_INFINITY,
      }),
    ).toThrow();
  });

  it('rejects a negative third-octave subband count like the C ABI', () => {
    const samples = new Float32Array(2000);
    samples[0] = 1.0;
    expect(() =>
      detectAcoustic({ samples, sampleRate: 48000, nThirdOctaveSubbands: -1 }),
    ).toThrow();
    expect(() => detectAcoustic({ samples, sampleRate: 48000, nOctaveBands: -1 })).toThrow();
  });

  it('honors the late-tail model selector', () => {
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.4, maxSec: 0.3 };
    const sabine = synthesizeRir({ sampleRate: 48000, ...base, preferEyring: false });
    const eyring = synthesizeRir({ sampleRate: 48000, ...base, preferEyring: true });
    expect(Array.from(sabine.rir)).not.toEqual(Array.from(eyring.rir));
  });

  it('reads crossfadeMs 0 as a hard switch and takes 5 ms when it is omitted', () => {
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2, seed: 123, maxSec: 0.3 };
    const rirOf = (extra: object): number[] =>
      Array.from(synthesizeRir({ sampleRate: 48000, ...base, ...extra }).rir);
    expect(rirOf({})).toEqual(rirOf({ crossfadeMs: 5 }));
    expect(rirOf({ crossfadeMs: 0 })).not.toEqual(rirOf({}));

    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const morphBase = { ...base, wet: 0.7 };
    const morphOmitted = Array.from(roomMorph(samples, 48000, morphBase).audio);
    expect(
      Array.from(roomMorph(samples, 48000, { ...morphBase, crossfadeMs: 0 }).audio),
    ).not.toEqual(morphOmitted);
  });

  it('reads seed 0 as a seed of its own and takes seed 1 when it is omitted', () => {
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2, maxSec: 0.3 };
    const rirOf = (seed?: number): number[] =>
      Array.from(
        synthesizeRir({ sampleRate: 48000, ...base, ...(seed === undefined ? {} : { seed }) }).rir,
      );
    expect(rirOf()).toEqual(rirOf(1));
    expect(rirOf()).not.toEqual(rirOf(2));
    expect(rirOf(0)).not.toEqual(rirOf());
    expect(rirOf(0)).toEqual(rirOf(0));
  });

  it('reaches the whole uint32 seed space the C ABI accepts', () => {
    // The binding read the seed through intProperty, whose embind `as<int>()`
    // SATURATES rather than wrapping: measured against the pre-fix module, every
    // seed from 2^31 up — including 2^32 and 2^40, which are outside the C ABI's
    // range entirely — produced one identical RIR, the one belonging to INT_MAX.
    // So the whole upper half of the seed space collapsed onto a single seed
    // instead of being reachable or rejected. (The Node addon had the same gap
    // for a different reason: Int32Value wraps, so its high seeds fell back to
    // the default. Hence the distinctness and rejection assertions below rather
    // than only a comparison against the default.)
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2, maxSec: 0.3 };
    const asArray = (seed: number): number[] =>
      Array.from(synthesizeRir({ sampleRate: 48000, ...base, seed }).rir);
    const defaultRir = asArray(1);
    // Positive control: a seed inside the int32 range already varied the tail,
    // so "differs from the default" is not free.
    expect(asArray(2)).not.toEqual(defaultRir);

    for (const seed of [2 ** 31, 2 ** 31 + 1, 3_000_000_000, 4_294_967_295]) {
      expect(asArray(seed), `seed ${seed}`).not.toEqual(defaultRir);
      // Deterministic: the same high seed reproduces its own RIR exactly.
      expect(asArray(seed), `seed ${seed} repeat`).toEqual(asArray(seed));
    }
    // Distinct high seeds stay distinct rather than collapsing onto each other.
    expect(asArray(2 ** 31)).not.toEqual(asArray(4_294_967_295));

    // Past the uint32 range there is no C-ABI seed to reach, so it is rejected
    // rather than silently answered with the default RIR.
    expect(() => synthesizeRir({ sampleRate: 48000, ...base, seed: 2 ** 32 })).toThrow();
    expect(() => synthesizeRir({ sampleRate: 48000, ...base, seed: 2 ** 40 })).toThrow();
  });

  it('refuses a fractional seed rather than truncating it onto a neighbour', () => {
    // The range check reaches 1.5 and passes it, so the cast decided the seed:
    // 1.5 and 1.9 both landed on seed 1 and reported success, which is the same
    // silent fold as the wrap above from the other end.
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2, maxSec: 0.3 };
    const asArray = (seed: number): number[] =>
      Array.from(synthesizeRir({ sampleRate: 48000, ...base, seed }).rir);
    // Non-vacuity: 1 and 2 must actually differ, or "1.5 is refused" would pass
    // for an entry point that ignores its seed entirely.
    expect(asArray(1)).not.toEqual(asArray(2));
    expect(() => synthesizeRir({ sampleRate: 48000, ...base, seed: 1.5 })).toThrow(
      /seed must be an integer/,
    );
    expect(() => synthesizeRir({ sampleRate: 48000, ...base, seed: 1.9 })).toThrow(
      /seed must be an integer/,
    );
    // The neighbours it used to truncate onto stay accepted.
    expect(asArray(1).length).toBeGreaterThan(0);
    expect(asArray(2).length).toBeGreaterThan(0);
  });

  it('honors per-band wall scattering', () => {
    const base = {
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      bandAbsorption: new Float32Array([0.2, 0.22, 0.24, 0.26]),
      maxSec: 0.3,
      seed: 123,
    };
    const mirror = synthesizeRir({
      sampleRate: 48000,
      ...base,
      bandScattering: new Float32Array([0, 0, 0, 0]),
    });
    const diffuse = synthesizeRir({
      sampleRate: 48000,
      ...base,
      bandScattering: new Float32Array([0.8, 0.8, 0.8, 0.8]),
    });
    expect(Array.from(diffuse.rir)).not.toEqual(Array.from(mirror.rir));
  });

  it('refuses an explicit zero referenceAbsorption and takes the default when omitted', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    expect(() => estimateRoom(rir.rir, 48000, { referenceAbsorption: 0 })).toThrow(RangeError);
    const dflt = estimateRoom(rir.rir, 48000);
    // Non-vacuity: the analyzer's 0.01 floor is far smaller than the default.
    const floored = estimateRoom(rir.rir, 48000, { referenceAbsorption: 0.01 });
    expect(floored.volume).toBeLessThan((dflt.volume ?? 0) / 1000);
  });

  it('emits absorption and rt60 bands at the same length', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    const est = estimateRoom(rir.rir, 48000);
    expect(est.bandAbsorption?.length).toBe(est.rt60Bands?.length);
  });

  // The diagnostic list is transcribed by the WASM wrapper independently of the
  // C ABI, so Node passing says nothing about this surface. Until these two, the
  // only WASM assertion on the RIR result was the band-length invariant above.
  it('names the refused value and its range in the thrown error, and carries warnings on success', () => {
    expect(() =>
      synthesizeRir({
        sampleRate: 48000,
        lengthM: 7,
        widthM: 5,
        heightM: 3,
        source: { x: 99, y: 1, z: 1 },
      }),
    ).toThrow(/source\.x must be finite and in \[0, 7\]/);

    const clean = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    expect(clean).not.toHaveProperty('hasError');
    expect(clean).not.toHaveProperty('errorMessage');
    expect(clean.diagnostics).toEqual([]);
  });

  it('surfaces a maxSec tail clamp as a warning on a successful call', () => {
    // A clamped tail used to be indistinguishable from an untruncated RIR: the
    // convolution just sounded wrong, with nothing thrown.
    const clamped = synthesizeRir({
      sampleRate: 48000,
      lengthM: 20,
      widthM: 15,
      heightM: 8,
      absorption: 0.03,
      maxSec: 0.2,
    });
    const warning = clamped.diagnostics.find((d) => d.code === 'acoustic.rir_length_clamped');
    expect(warning).toBeDefined();
    expect(warning?.severity).toBe('warning');
  });

  // Cross-surface parity fixes: these paths previously diverged from the C ABI.
  it('rejects a negative ISM order instead of clamping it to zero', () => {
    expect(() =>
      synthesizeRir({
        sampleRate: 48000,
        lengthM: 7,
        widthM: 5,
        heightM: 3,
        absorption: 0.15,
        ismOrder: -1,
      }),
    ).toThrow();
  });

  it('refuses an explicit zero aspect hint', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    expect(() => estimateRoom(rir.rir, 48000, { aspectHintLw: 0 })).toThrow(RangeError);
    expect(() => estimateRoom(rir.rir, 48000, { aspectHintLh: 0 })).toThrow(RangeError);
    const est = estimateRoom(rir.rir, 48000);
    expect(est.rt60Bands?.length ?? 0).toBeGreaterThanOrEqual(4);
  });
  // The ISO 9613-1 atmospheric term reached only the streaming insert
  // ("effects.reverb.room") until these options existed; the offline entry
  // point could not request it at all. Each assertion below pins one of the
  // three fields, so dropping any single one on the way to the core fails here
  // rather than hiding behind the other two.
  const airHall = {
    lengthM: 30,
    widthM: 24,
    heightM: 15,
    source: { x: 3, y: 3, z: 1.5 },
    listener: { x: 10, y: 8, z: 1.7 },
    absorption: 0.2,
    maxSec: 0.5,
    ismOrder: 2,
    seed: 3,
  };
  // Energy past the ~100 ms crossover for this room, i.e. the statistical tail
  // the air term reshapes (sample 9600 is 200 ms at 48 kHz).
  const tailEnergy = (rir: Float32Array) => {
    let sum = 0;
    for (let i = 9600; i < rir.length; i += 1) {
      sum += rir[i] * rir[i];
    }
    return sum;
  };

  it('routes air absorption into the synthesized tail', () => {
    const off = synthesizeRir({ sampleRate: 48000, ...airHall, airAbsorptionEnabled: false });
    const iso = synthesizeRir({
      sampleRate: 48000,
      ...airHall,
      airAbsorptionEnabled: true,
      airTemperatureC: 20,
      airHumidityPercent: 50,
    });
    expect(Array.from(iso.rir)).not.toEqual(Array.from(off.rir));
    // Air absorption can only take energy out of the tail.
    expect(tailEnergy(iso.rir)).toBeLessThan(tailEnergy(off.rir));

    // An omitted climate resolves to the ISO reference.
    const implicitIso = synthesizeRir({
      sampleRate: 48000,
      ...airHall,
      airAbsorptionEnabled: true,
    });
    expect(Array.from(implicitIso.rir)).toEqual(Array.from(iso.rir));

    // Air absorption is on by default.
    expect(Array.from(synthesizeRir({ sampleRate: 48000, ...airHall }).rir)).toEqual(
      Array.from(iso.rir),
    );

    // Both climate values are read individually.
    const warm = synthesizeRir({
      sampleRate: 48000,
      ...airHall,
      airAbsorptionEnabled: true,
      airTemperatureC: 35,
      airHumidityPercent: 50,
    });
    const humid = synthesizeRir({
      sampleRate: 48000,
      ...airHall,
      airAbsorptionEnabled: true,
      airTemperatureC: 20,
      airHumidityPercent: 90,
    });
    expect(Array.from(warm.rir)).not.toEqual(Array.from(iso.rir));
    expect(Array.from(humid.rir)).not.toEqual(Array.from(iso.rir));
    expect(Array.from(warm.rir)).not.toEqual(Array.from(humid.rir));

    // The climate is ignored while the flag is off.
    const ignored = synthesizeRir({
      sampleRate: 48000,
      ...airHall,
      airAbsorptionEnabled: false,
      airTemperatureC: 35,
      airHumidityPercent: 90,
    });
    expect(Array.from(ignored.rir)).toEqual(Array.from(off.rir));
  });

  it('throws on an implausible air climate, naming the refused value', () => {
    expect(() =>
      synthesizeRir({
        sampleRate: 48000,
        ...airHall,
        maxSec: 0.2,
        airAbsorptionEnabled: true,
        airTemperatureC: -500,
      }),
    ).toThrow(/airTemperatureC must be finite and greater than -273\.15/);
  });

  it('routes air absorption into the room-morph target room', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const target = { ...airHall, maxSec: 0.3, wet: 1.0, sourceTailSuppression: 0.5 };
    const off = roomMorph(samples, 48000, { ...target, airAbsorptionEnabled: false });
    const on = roomMorph(samples, 48000, {
      ...target,
      airAbsorptionEnabled: true,
      airTemperatureC: 20,
      airHumidityPercent: 50,
    });
    expect(on.audio.length).toBe(off.audio.length);
    expect(Array.from(on.audio)).not.toEqual(Array.from(off.audio));
    // The morph validates its config rather than diagnosing, so this throws.
    expect(() =>
      roomMorph(samples, 48000, {
        ...target,
        airAbsorptionEnabled: true,
        airHumidityPercent: 150,
      }),
    ).toThrow();
  });
});

describe('named selectors and estimate-to-geometry', () => {
  const room = { maxSec: 0.1, sampleRate: 22050, ismOrder: 2 };
  const presets = ['none', 'concrete', 'wood', 'curtain', 'carpet', 'glass'] as const;

  it('builds a different RIR for each preset name', () => {
    const rirs = presets.map((name) =>
      Array.from(synthesizeRir({ ...room, materialPreset: name }).rir),
    );
    expect(new Set(rirs.map((rir) => JSON.stringify(rir))).size).toBe(presets.length);
  });

  it('refuses a number for a preset or mode, naming the valid set', () => {
    expect(() => synthesizeRir({ sampleRate: 48000, materialPreset: 2 as never })).toThrow(
      TypeError,
    );
    expect(() => synthesizeRir({ sampleRate: 48000, materialPreset: 2 as never })).toThrow(
      'materialPreset must be one of none, concrete, wood, curtain, carpet, glass',
    );
    expect(() => roomMorph(new Float32Array(100), 48000, { materialPreset: 1 as never })).toThrow(
      TypeError,
    );
    expect(() => estimateRoom(new Float32Array(1000), 48000, { mode: 1 as never })).toThrow(
      TypeError,
    );
    expect(() => estimateRoom(new Float32Array(1000), 48000, { mode: 1 as never })).toThrow(
      'mode must be one of auto, blind, impulseResponse',
    );
  });

  it('refuses an unknown preset or mode name, naming the valid set', () => {
    expect(() => synthesizeRir({ sampleRate: 48000, materialPreset: 'marble' as never })).toThrow(
      RangeError,
    );
    expect(() => synthesizeRir({ sampleRate: 48000, materialPreset: 'marble' as never })).toThrow(
      'materialPreset must be one of none, concrete, wood, curtain, carpet, glass',
    );
    expect(() =>
      roomMorph(new Float32Array(100), 48000, { materialPreset: 'Concrete' as never }),
    ).toThrow(RangeError);
    expect(() => estimateRoom(new Float32Array(1000), 48000, { mode: 'blinds' as never })).toThrow(
      'mode must be one of auto, blind, impulseResponse',
    );
  });

  it('feeds an estimate to synthesizeRir exactly as the hand-mapped call does', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    const estimate = estimateRoom(rir.rir, 48000, {
      aspectHintLw: 7 / 5,
      aspectHintLh: 7 / 3,
      referenceAbsorption: 0.15,
    });
    const source = { x: 1, y: 1, z: 1.2 };
    const listener = { x: 3, y: 2, z: 1.7 };
    const geometry = roomGeometryFromEstimate({ estimate, source, listener });
    expect(Object.keys(geometry).sort()).toEqual(
      ['absorption', 'bandAbsorption', 'heightM', 'lengthM', 'listener', 'source', 'widthM'].sort(),
    );
    expect(geometry.lengthM).toBeCloseTo(estimate.lengthM ?? Number.NaN, 5);
    const options = { maxSec: 0.1, ismOrder: 2 };
    const mapped = synthesizeRir({ sampleRate: 48000, ...geometry, ...options });
    const byHand = synthesizeRir({
      sampleRate: 48000,
      lengthM: estimate.lengthM,
      widthM: estimate.widthM,
      heightM: estimate.heightM,
      bandAbsorption: estimate.bandAbsorption,
      source,
      listener,
      ...options,
    });
    expect(Array.from(mapped.rir)).toEqual(Array.from(byHand.rir));
    // Omitted positions are scaled into the estimated room.
    const scaled = roomGeometryFromEstimate({ estimate });
    expect(Object.keys(scaled)).toContain('source');
    expect(scaled.absorption).toBeGreaterThan(0);
  });

  it('places a small estimated room inside it so synthesizeRir accepts it', () => {
    const estimate = {
      lengthM: 1.33,
      widthM: 1.33,
      heightM: 1.33,
      bandAbsorption: [0.2, 0.3, Number.NaN],
    };
    const geometry = roomGeometryFromEstimate({ estimate });
    expect(geometry.bandAbsorption).toBeUndefined();
    expect(geometry.absorption).toBeCloseTo(0.25, 6);
    for (const placement of [geometry.source, geometry.listener]) {
      for (const value of [placement?.x, placement?.y, placement?.z]) {
        expect(value).toBeGreaterThan(0);
        expect(value).toBeLessThan(1.33);
      }
    }
    expect(() =>
      synthesizeRir({ sampleRate: 48000, ...geometry, maxSec: 0.1, ismOrder: 2 }),
    ).not.toThrow();
    const full = roomGeometryFromEstimate({ estimate: { lengthM: 7, widthM: 5, heightM: 3 } });
    expect([full.source?.x, full.source?.y, full.source?.z]).toEqual([
      1,
      1,
      expect.closeTo(1.2, 6),
    ]);
    expect([full.listener?.x, full.listener?.y]).toEqual([5, 4]);
    expect(full.absorption).toBeUndefined();
  });

  it('refuses an estimate with no measurable room', () => {
    const silent = estimateRoom(new Float32Array(48000), 48000);
    expect(silent.lengthM).toBeUndefined();
    // The library refuses an estimate with no dimensions as a refused argument.
    expect(() => roomGeometryFromEstimate({ estimate: silent })).toThrow(RangeError);
    expect(() => roomGeometryFromEstimate({ estimate: silent })).toThrow(
      /^roomGeometryFromEstimate: room estimate has no measurable dimensions/,
    );
  });
});

describe('roomMorphStereo', () => {
  const room = { lengthM: 8, widthM: 6, heightM: 3, absorption: 0.2, wet: 1.0, maxSec: 0.3 };
  const impulse = () => {
    const x = new Float32Array(2000);
    x[0] = 1.0;
    return x;
  };

  it('returns two equal-length channels longer than the input', () => {
    const r = roomMorphStereo({ left: impulse(), right: impulse(), sampleRate: 48000, ...room });
    expect(r.left.length).toBe(r.right.length);
    expect(r.left.length).toBeGreaterThan(2000);
    expect(r.sampleRate).toBe(48000);
    expect(Array.isArray(r.diagnostics)).toBe(true);
  });

  it('decorrelates the channels for identical inputs', () => {
    const r = roomMorphStereo({ left: impulse(), right: impulse(), sampleRate: 48000, ...room });
    expect(Array.from(r.left)).not.toEqual(Array.from(r.right));
  });

  it('treats an omitted spacing as 0.5 m', () => {
    const base = { left: impulse(), right: impulse(), sampleRate: 48000, ...room };
    const a = roomMorphStereo(base);
    const b = roomMorphStereo({ ...base, receiverSpacingM: 0.5 });
    expect(Array.from(a.left)).toEqual(Array.from(b.left));
    expect(Array.from(a.right)).toEqual(Array.from(b.right));
  });

  it.each([0, -1, 4.5, Number.NaN, 1e40])(
    'rejects receiverSpacingM %s with a RangeError',
    (value) => {
      const attempt = () =>
        roomMorphStereo({
          left: impulse(),
          right: impulse(),
          sampleRate: 48000,
          ...room,
          receiverSpacingM: value,
        });
      expect(attempt).toThrow(RangeError);
      expect(attempt).toThrow('roomMorphStereo: receiverSpacingM must be finite and in (0, 4]');
    },
  );

  it('refuses a receiverSpacingM that is not a number with a TypeError', () => {
    const attempt = () =>
      roomMorphStereo({
        left: impulse(),
        right: impulse(),
        sampleRate: 48000,
        ...room,
        receiverSpacingM: '0.5' as unknown as number,
      });
    expect(attempt).toThrow(new TypeError('roomMorphStereo: receiverSpacingM must be a number'));
  });

  it('rejects mismatched channel lengths with a RangeError', () => {
    expect(() =>
      roomMorphStereo({
        left: new Float32Array(100),
        right: new Float32Array(99),
        sampleRate: 48000,
        ...room,
      }),
    ).toThrow(RangeError);
  });

  it('refuses a listener too close to a wall with the coded geometry error', () => {
    const attempt = () =>
      roomMorphStereo({
        left: impulse(),
        right: impulse(),
        sampleRate: 48000,
        ...room,
        listener: { x: 0.1, y: 3, z: 1.5 },
        receiverSpacingM: 0.5,
      });
    expect(attempt).toThrow(RangeError);
    // The core's receiver-pair check, not the facade's listener reader.
    expect(attempt).toThrow(
      /^roomMorphStereo: receiverSpacingM must place both receivers inside the room for /,
    );
  });
});

describe('roomMorphStereo receiver spacing fitted to the room', () => {
  const impulse = () => {
    const x = new Float32Array(1000);
    x[0] = 1;
    return x;
  };
  // A listener 0.1 m from a side wall: the 0.5 m pair does not fit there, a narrower one does.
  const narrow = {
    left: impulse(),
    right: impulse(),
    sampleRate: 48000,
    lengthM: 12,
    widthM: 9,
    heightM: 5,
    maxSec: 0.2,
    listener: { x: 0.1, y: 4, z: 1.7 },
  };

  it('renders with an omitted spacing where the default pair would leave the room', () => {
    const out = roomMorphStereo(narrow);
    expect(out.left.length).toBe(out.right.length);
    expect(out.left.every(Number.isFinite)).toBe(true);
    expect(Array.from(out.left)).not.toEqual(Array.from(out.right));
    expect(() => roomMorphStereo({ ...narrow, receiverSpacingM: 0.5 })).toThrow(
      /^roomMorphStereo: receiverSpacingM must place both receivers inside the room for /,
    );
  });

  it('still refuses a given NaN spacing by the domain', () => {
    expect(() => roomMorphStereo({ ...narrow, receiverSpacingM: Number.NaN })).toThrow(
      'roomMorphStereo: receiverSpacingM must be finite and in (0, 4]',
    );
  });
});

describe('default room placement', () => {
  const f = Math.fround;
  // The C ABI's default placement for the 7 x 5 x 3 m room, scaled per axis into another room.
  const scaled = (l: number, w: number, h: number) => {
    const axis = (p: number, extent: number, base: number) => f(f(p) * f(extent / base));
    return {
      source: { x: axis(1, l, 7), y: axis(1, w, 5), z: axis(1.2, h, 3) },
      listener: { x: axis(5, l, 7), y: axis(4, w, 5), z: axis(1.7, h, 3) },
    };
  };
  const small = { lengthM: 4, widthM: 3, heightM: 2.6, absorption: 0.3, maxSec: 0.2 };
  const impulse = () => {
    const x = new Float32Array(1000);
    x[0] = 1;
    return x;
  };
  const rir = (options: Record<string, unknown>) =>
    Array.from(synthesizeRir({ sampleRate: 48000, ...options }).rir);
  // A placement written out in JS rounds apart from the core's by an ulp, so compare to the peak.
  const expectClose = (actual: ArrayLike<number>, expected: ArrayLike<number>) => {
    expect(actual.length).toBe(expected.length);
    const peak = Math.max(...Array.from(expected, Math.abs));
    for (let i = 0; i < actual.length; i++) {
      expect(Math.abs(actual[i] - expected[i])).toBeLessThanOrEqual(peak * 1e-4);
    }
  };

  it('places an omitted source and listener inside a room smaller than the default', () => {
    const omitted = rir(small);
    expect(omitted.some((s) => s !== 0)).toBe(true);
    expectClose(omitted, rir({ ...small, ...scaled(4, 3, 2.6) }));
  });

  it('keeps the default room on its unscaled placement', () => {
    const room = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.3, maxSec: 0.2 };
    const explicit = {
      source: { x: 1, y: 1, z: 1.2 },
      listener: { x: 5, y: 4, z: 1.7 },
    };
    expect(rir(room)).toEqual(rir({ ...room, ...explicit }));
  });

  it('reads a null placement as omitted', () => {
    expect(rir({ ...small, source: null, listener: null })).toEqual(rir(small));
  });

  it('refuses a partly given position by name', () => {
    expect(
      expectRefusalOf(() => rir({ ...small, source: { x: 1, y: 1 } }), TypeError).message,
    ).toMatch(/source\.z must be a number$/);
    expect(
      expectRefusalOf(() => rir({ ...small, listener: { x: 1, y: Number.NaN, z: 1 } })).message,
    ).toMatch(/listener\.y must be finite/);
  });

  it('morphs toward a small room with the placement omitted, in both forms', () => {
    const positional = roomMorph(impulse(), 48000, small);
    const request = roomMorph({ samples: impulse(), sampleRate: 48000, ...small });
    expect(Array.from(request.audio)).toEqual(Array.from(positional.audio));
    const explicit = roomMorph(impulse(), 48000, { ...small, ...scaled(4, 3, 2.6) });
    expectClose(positional.audio, explicit.audio);
  });

  it('morphs a stereo pair toward a small room with the placement omitted', () => {
    const out = roomMorphStereo({ left: impulse(), right: impulse(), sampleRate: 48000, ...small });
    expect(out.left.length).toBe(out.right.length);
    expect(out.left.length).toBeGreaterThan(1000);
  });
});
