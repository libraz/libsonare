import { describe, expect, it } from 'vitest';
import { estimateRoom, roomMorph, synthesizeRir } from '../src/index.js';

describe('geometric room acoustics', () => {
  it('synthesizes a decaying RIR from geometry', () => {
    const result = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    expect(result.hasError).toBe(false);
    expect(result.sampleRate).toBe(48000);
    expect(result.rir.length).toBeGreaterThan(0);
    expect(Array.from(result.rir).some((s) => Math.abs(s) > 0)).toBe(true);
  });

  it('flags invalid geometry with an empty RIR', () => {
    const result = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, sourceX: 99 });
    expect(result.hasError).toBe(true);
    expect(result.rir.length).toBe(0);
  });

  it('round-trips a known shoebox within tolerance', () => {
    const rir = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    const est = estimateRoom(rir.rir, 48000, {
      aspectHintLw: 7 / 5,
      aspectHintLh: 7 / 3,
      referenceAbsorption: 0.15,
    });
    expect(est.volume).toBeGreaterThan(105 * 0.8);
    expect(est.volume).toBeLessThan(105 * 1.2);
    expect(est.confidence).toBeGreaterThan(0);
    expect(est.rt60Bands.length).toBeGreaterThanOrEqual(4);
    expect(Number.isFinite(est.drrDb)).toBe(true);
  });

  it('treats seed 0 as the library default (1) for cross-surface reproducibility', () => {
    // seed:0 must keep the RirSynthConfig default (1) — matching the C ABI and
    // Python — instead of seeding the PRNG with 0 (which gave a different RIR).
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2, maxSeconds: 0.3 };
    const zero = synthesizeRir({ ...base, seed: 0 });
    const one = synthesizeRir({ ...base, seed: 1 });
    const two = synthesizeRir({ ...base, seed: 2 });
    expect(Array.from(zero.rir)).toEqual(Array.from(one.rir));
    expect(Array.from(zero.rir)).not.toEqual(Array.from(two.rir));
  });

  it('treats crossfadeMs 0 as the library default (5 ms) for the RIR splice', () => {
    // crossfadeMs:0 documents "use the default" — it must keep the RirSynthConfig
    // default (5 ms), matching the C ABI/WASM/Python guard, not force a literal
    // zero-width crossfade (which shifts the splice ~1 sample and clicks).
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2, maxSeconds: 0.3 };
    const omitted = synthesizeRir(base);
    const zero = synthesizeRir({ ...base, crossfadeMs: 0 });
    const wide = synthesizeRir({ ...base, crossfadeMs: 40 });
    expect(Array.from(zero.rir)).toEqual(Array.from(omitted.rir));
    expect(Array.from(zero.rir)).not.toEqual(Array.from(wide.rir));
  });

  it('treats crossfadeMs 0 as the library default (5 ms) for room morph', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const base = { lengthM: 12, widthM: 9, heightM: 5, absorption: 0.08, wet: 0.7 };
    const omitted = roomMorph(samples, 48000, base);
    const zero = roomMorph(samples, 48000, { ...base, crossfadeMs: 0 });
    const wide = roomMorph(samples, 48000, { ...base, crossfadeMs: 40 });
    expect(Array.from(zero)).toEqual(Array.from(omitted));
    expect(Array.from(zero)).not.toEqual(Array.from(wide));
  });

  it('morphs toward a target room and is deterministic', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const opts = { lengthM: 12, widthM: 9, heightM: 5, absorption: 0.08, wet: 0.7 };
    const a = roomMorph(samples, 48000, opts);
    const b = roomMorph(samples, 48000, opts);
    expect(a.length).toBeGreaterThan(samples.length);
    expect(Array.from(a)).toEqual(Array.from(b));
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

  it('rejects the same invalid room/material inputs the C ABI refuses', () => {
    // Over the 64-band material cap.
    expect(() => synthesizeRir({ bandAbsorption: new Array(65).fill(0.2) })).toThrow();
    // Per-band absorption/scattering outside [0, 1] are rejected, matching the
    // C-ABI oracle's `unit` predicate (they used to be silently clamped here).
    expect(() => synthesizeRir({ bandAbsorption: [0.2, 1.5, 0.3] })).toThrow();
    expect(() => synthesizeRir({ bandAbsorption: [0.1, 0.3, 0.5] })).not.toThrow();
    expect(() =>
      synthesizeRir({ bandAbsorption: [0.2, 0.3], bandScattering: [0, -0.5] }),
    ).toThrow();
    // The scalar absorption path is still rejected out of [0, 1] (both surfaces).
    expect(() => synthesizeRir({ absorption: 2 })).toThrow();
    // Non-finite geometry and timing.
    expect(() => synthesizeRir({ lengthM: Number.NaN })).toThrow();
    expect(() => synthesizeRir({ sourceX: Number.POSITIVE_INFINITY })).toThrow();
    expect(() => synthesizeRir({ maxSeconds: 100000 })).toThrow();
    // An in-range absorption of exactly 1.0 is accepted (matches the C ABI's
    // [0, 1] bound, not the old 0.999 clamp).
    expect(() => synthesizeRir({ absorption: 1, maxSeconds: 0.1 })).not.toThrow();
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
        sourceX: 99,
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
        maxSeconds: Number.POSITIVE_INFINITY,
      }),
    ).toThrow();
  });

  it('honors the late-tail model selector', () => {
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.4, maxSeconds: 0.3 };
    const sabine = synthesizeRir({ ...base, preferEyring: false });
    const eyring = synthesizeRir({ ...base, preferEyring: true });
    expect(Array.from(sabine.rir)).not.toEqual(Array.from(eyring.rir));
  });

  it('honors per-band wall scattering', () => {
    const base = {
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      bandAbsorption: [0.2, 0.22, 0.24, 0.26],
      maxSeconds: 0.3,
      seed: 123,
    };
    const mirror = synthesizeRir({ ...base, bandScattering: [0, 0, 0, 0] });
    const diffuse = synthesizeRir({ ...base, bandScattering: [0.8, 0.8, 0.8, 0.8] });
    expect(Array.from(diffuse.rir)).not.toEqual(Array.from(mirror.rir));
  });

  it('emits absorption and rt60 bands at the same length', () => {
    const rir = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    const est = estimateRoom(rir.rir, 48000);
    expect(est.absorptionBands.length).toBe(est.rt60Bands.length);
  });

  it('keeps the full band count when one of the two estimates fails', () => {
    // Absorption (the inverse problem) and RT60 (the decay fit) are independent:
    // white noise has no usable broadband decay, so the absorption solve bails
    // out with no bands at all while the analyzer still reports its per-band
    // vector. Reporting both at the shorter length threw the surviving vector
    // away; the C ABI NaN-fills the failed side instead and Node now matches.
    let state = 7;
    const noise = new Float32Array(48000);
    for (let i = 0; i < noise.length; i++) {
      state = (state * 1103515245 + 12345) & 0x7fffffff;
      noise[i] = (state / 0x7fffffff) * 2 - 1;
    }
    const analyzable = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    const bandCount = estimateRoom(analyzable.rir, 48000).rt60Bands.length;
    expect(bandCount).toBeGreaterThan(0);

    const degraded = estimateRoom(noise, 48000);
    expect(degraded.confidence).toBe(0);
    expect(degraded.rt60Bands.length).toBe(bandCount);
    expect(degraded.absorptionBands.length).toBe(degraded.rt60Bands.length);
  });

  it('reports which diagnostic fired instead of a bare hasError boolean', () => {
    const invalid = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, sourceX: 99 });
    expect(invalid.hasError).toBe(true);
    // The five geometry errors were indistinguishable through hasError alone.
    expect(invalid.errorMessage).toContain('acoustic.source_outside_room');
    expect(invalid.diagnostics.map((d) => d.code)).toContain('acoustic.source_outside_room');
    expect(invalid.diagnostics.every((d) => d.severity === 'error')).toBe(true);

    const clean = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    expect(clean.hasError).toBe(false);
    expect(clean.errorMessage).toBe('');
    expect(clean.diagnostics).toEqual([]);
  });

  it('surfaces a maxSeconds tail clamp as a warning on a successful call', () => {
    // A clamped tail used to be indistinguishable from an untruncated RIR: the
    // convolution just sounded wrong, with hasError still false.
    const clamped = synthesizeRir({
      lengthM: 20,
      widthM: 15,
      heightM: 8,
      absorption: 0.03,
      maxSeconds: 0.2,
    });
    expect(clamped.hasError).toBe(false);
    const warning = clamped.diagnostics.find((d) => d.code === 'acoustic.rir_length_clamped');
    expect(warning).toBeDefined();
    expect(warning?.severity).toBe('warning');
  });

  // Cross-surface parity fixes: these paths previously diverged from the C ABI.
  it('rejects out-of-range band absorption to match the C ABI (does not clamp)', () => {
    // The C ABI rejects any per-band coefficient outside [0, 1]; the Node facade
    // now rejects too instead of silently clamping (which diverged the RIR).
    expect(() =>
      synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, bandAbsorption: [1.5, 1.5, 1.5, 1.5] }),
    ).toThrow();
    expect(() =>
      synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, bandAbsorption: [0.1, -0.2, 0.3] }),
    ).toThrow();
    expect(() =>
      synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, bandAbsorption: [0.1, Number.NaN, 0.3] }),
    ).toThrow();
    // An in-range band table is still accepted and produces a valid RIR.
    const ok = synthesizeRir({
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      bandAbsorption: [0.1, 0.3, 0.5],
    });
    expect(ok.hasError).toBe(false);
    expect(ok.rir.length).toBeGreaterThan(0);
  });

  it('rejects a negative ISM order instead of clamping it to zero', () => {
    // The C ABI rejects a negative ismOrder; Node must too (it used to clamp).
    expect(() =>
      synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15, ismOrder: -1 }),
    ).toThrow();
  });

  it('treats an explicit zero aspect hint as the default, matching the C ABI', () => {
    // aspectHint == 0 means "use the default 1.0" on the C ABI/Python; Node now
    // remaps it the same way instead of letting the core reject a zero hint.
    const rir = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    const est = estimateRoom(rir.rir, 48000, { aspectHintLw: 0, aspectHintLh: 0 });
    expect(est.rt60Bands.length).toBeGreaterThanOrEqual(4);
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
    sourceX: 3,
    sourceY: 3,
    sourceZ: 1.5,
    listenerX: 10,
    listenerY: 8,
    listenerZ: 1.7,
    absorption: 0.2,
    maxSeconds: 0.5,
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
    const off = synthesizeRir(airHall);
    const iso = synthesizeRir({
      ...airHall,
      airAbsorptionEnabled: true,
      airTemperatureC: 20,
      airHumidityPercent: 50,
    });
    expect(off.hasError).toBe(false);
    expect(iso.hasError).toBe(false);
    expect(Array.from(iso.rir)).not.toEqual(Array.from(off.rir));
    // Air absorption can only take energy out of the tail.
    expect(tailEnergy(iso.rir)).toBeLessThan(tailEnergy(off.rir));

    // A zeroed/omitted climate resolves to the ISO reference, matching the
    // seed and crossfadeMs convention these options already follow.
    const implicitIso = synthesizeRir({ ...airHall, airAbsorptionEnabled: true });
    expect(Array.from(implicitIso.rir)).toEqual(Array.from(iso.rir));

    // Both climate values are read individually.
    const warm = synthesizeRir({
      ...airHall,
      airAbsorptionEnabled: true,
      airTemperatureC: 35,
      airHumidityPercent: 50,
    });
    const humid = synthesizeRir({
      ...airHall,
      airAbsorptionEnabled: true,
      airTemperatureC: 20,
      airHumidityPercent: 90,
    });
    expect(Array.from(warm.rir)).not.toEqual(Array.from(iso.rir));
    expect(Array.from(humid.rir)).not.toEqual(Array.from(iso.rir));
    expect(Array.from(warm.rir)).not.toEqual(Array.from(humid.rir));

    // The climate is ignored while the flag is off.
    const ignored = synthesizeRir({ ...airHall, airTemperatureC: 35, airHumidityPercent: 90 });
    expect(Array.from(ignored.rir)).toEqual(Array.from(off.rir));
  });

  it('reports an implausible air climate as a diagnostic, not a throw', () => {
    const bad = synthesizeRir({
      ...airHall,
      maxSeconds: 0.2,
      airAbsorptionEnabled: true,
      airTemperatureC: -500,
    });
    expect(bad.hasError).toBe(true);
    expect(bad.rir.length).toBe(0);
    expect(bad.diagnostics.map((d) => d.code)).toContain('acoustic.invalid_air_absorption');
  });

  it('routes air absorption into the room-morph target room', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const target = { ...airHall, maxSeconds: 0.3, wet: 1.0, sourceTailSuppression: 0.5 };
    const off = roomMorph(samples, 48000, target);
    const on = roomMorph(samples, 48000, {
      ...target,
      airAbsorptionEnabled: true,
      airTemperatureC: 20,
      airHumidityPercent: 50,
    });
    expect(on.length).toBe(off.length);
    expect(Array.from(on)).not.toEqual(Array.from(off));
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
