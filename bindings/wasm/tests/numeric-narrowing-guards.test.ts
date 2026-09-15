/**
 * What a JS number is allowed to become on its way into the C++ facade.
 *
 * `val::as<T>()` changes a value silently in three directions and each one
 * reaches a different wrong result: it saturates an `int` at `INT_MAX` (a
 * plausible in-domain number every positivity guard waves through), it wraps a
 * `uint8_t` modulo 256 (so `511` reads as the any-channel wildcard), and it
 * truncates a fractional value onto a neighbouring id, channel or enum member.
 * A non-finite value is not refused either — `NaN` reads as `0`, which is a
 * legal destination, a legal channel and a legal ordinal.
 *
 * Every case below pairs the refusal with the legal value next to it, because a
 * field the facade never reads refuses nothing and accepts nothing in exactly
 * the same way, and a test written only on the refusals cannot tell them apart.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  fixFrames,
  init,
  isSonareError,
  masteringRepairDereverbClassical,
  meteringSpectrumFrame,
  Project,
  RealtimeEngine,
  type SonareError,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

function expectInvalidParameter(fn: () => unknown): void {
  let caught: unknown;
  try {
    fn();
  } catch (e) {
    caught = e;
  }
  expect(caught, 'expected a SonareError, got no throw').toBeDefined();
  expect(isSonareError(caught)).toBe(true);
  expect((caught as SonareError).code).toBe(ErrorCode.InvalidParameter);
}

/** Saturating, wrapping and truncating inputs, in the spellings they arrive in. */
const SATURATING = [2 ** 31, 2 ** 40, 3e9, 4294967295, 2 ** 53 + 1];
const NON_FINITE = [Number.NaN, Number.POSITIVE_INFINITY];

describe('int fields refuse a saturating, fractional or non-finite value', () => {
  it('pins a time signature only for a whole numerator and denominator', () => {
    const legal = (numerator: number, denominator: number): [number, number] => {
      const project = new Project();
      try {
        project.setTimeSignatures([{ startPpq: 0, numerator, denominator }]);
        const segment = project.timeSignatureByIndex(0);
        return [segment.numerator, segment.denominator];
      } finally {
        project.destroy();
      }
    };
    // The control: two legal signatures reach the timeline as themselves.
    expect(legal(3, 4)).toEqual([3, 4]);
    expect(legal(7, 8)).toEqual([7, 8]);

    for (const value of [...SATURATING, ...NON_FINITE, 4.5]) {
      expectInvalidParameter(() => legal(value, 4));
      expectInvalidParameter(() => legal(4, value));
    }
  });

  it('imports external stems only at a whole sample rate', () => {
    const importAt = (sampleRate: number): number => {
      const project = new Project();
      try {
        project.setSampleRate(48000);
        const result = project.importExternalStems({
          sampleRate,
          stems: [{ name: 's', layout: 1, planarSamples: [new Float32Array(4800)] }],
        });
        return result.trackIds.length;
      } finally {
        project.destroy();
      }
    };
    expect(importAt(48000)).toBe(1);
    // A rate the project does not carry is refused, which is what makes the
    // acceptance above a measurement of the field rather than of its absence.
    expectInvalidParameter(() => importAt(44100));
    expectInvalidParameter(() => importAt(48000.5));
  });

  it('keeps the -1 routing sentinel while refusing what a cast turned into a filter', () => {
    const events = [Project.midiNoteOn(0, 0, 0, 60, 100), Project.midiNoteOn(1, 0, 3, 62, 100)];
    const routed = (config: Record<string, number>): number =>
      Project.midiRouteEvents(events, config).events.length;

    // The control: two legal channels select different events.
    expect(routed({ filterChannel: 0 })).toBe(1);
    expect(routed({ filterChannel: 3 })).toBe(1);
    expect(Project.midiRouteEvents(events, { filterChannel: 0 }).events[0]?.ppq).toBe(0);
    expect(Project.midiRouteEvents(events, { filterChannel: 3 }).events[0]?.ppq).toBe(1);
    // -1 is "any group / any channel / no remap" and must survive the check.
    expect(routed({ filterChannel: -1 })).toBe(2);
    expect(routed({ filterGroup: -1 })).toBe(2);
    expect(routed({ remapChannel: -1 })).toBe(2);

    for (const key of ['filterGroup', 'filterChannel', 'remapChannel']) {
      for (const value of [...SATURATING, ...NON_FINITE, 1.5]) {
        expectInvalidParameter(() => routed({ [key]: value }));
      }
    }
  });
});

describe('clip fields refuse a handle or an ordinal that is not whole', () => {
  const source = new Float32Array(128);
  for (let i = 0; i < source.length; i++) {
    source[i] = Math.sin(i * 0.3);
  }

  const render = (extra: Record<string, unknown>): number => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.setClips([{ id: 1, channels: [source], startPpq: 0, ...extra }]);
      engine.play();
      const out = engine.process([new Float32Array(128)]);
      let sum = 0;
      for (const value of out[0]) {
        sum += value * value;
      }
      return Math.sqrt(sum / out[0].length);
    } finally {
      engine.destroy();
    }
  };

  it('refuses a fractional page-provider handle instead of binding a real provider', () => {
    const withProvider = (pageProvider: number): number => {
      const engine = new RealtimeEngine(48000, 128);
      try {
        const first = engine.createClipPageProvider(1, 128, 128);
        const second = engine.createClipPageProvider(1, 128, 128);
        engine.supplyClipPage(first.id, 0, [new Float32Array(128).fill(0.25)]);
        engine.supplyClipPage(second.id, 0, [new Float32Array(128).fill(-0.5)]);
        engine.setClips([{ id: 1, startPpq: 0, pageProvider, lengthSamples: 128 }]);
        engine.play();
        return engine.process([new Float32Array(128)])[0][0];
      } finally {
        engine.destroy();
      }
    };
    // The control: the two handles carry different audio.
    expect(withProvider(1)).toBeCloseTo(0.25, 4);
    expect(withProvider(2)).toBeCloseTo(-0.5, 4);
    // 1.5 and 2.9 used to resolve onto those same two providers.
    expectInvalidParameter(() => withProvider(1.5));
    expectInvalidParameter(() => withProvider(2.9));
    for (const value of [...NON_FINITE, ...SATURATING, 0, -1, 3]) {
      expectInvalidParameter(() => withProvider(value));
    }
  });

  it('refuses a fractional or non-finite warp-mode ordinal', () => {
    // The control: two legal ordinals render differently.
    expect(render({ lengthSamples: 128, warpMode: 0 })).not.toBeCloseTo(
      render({ lengthSamples: 128, warpMode: 3 }),
      6,
    );
    for (const value of [...SATURATING, ...NON_FINITE, 1.5]) {
      expectInvalidParameter(() => render({ lengthSamples: 128, warpMode: value }));
    }
  });
});

describe('synth patch enum ordinals refuse a fractional or non-finite value', () => {
  const render = (patch: Record<string, unknown>): number => {
    const engine = new RealtimeEngine(48000, 512);
    try {
      engine.setSynthInstrument(patch);
      engine.pushMidiNoteOn(0, 0, 0, 60, 100);
      engine.play();
      const out = engine.process([new Float32Array(512), new Float32Array(512)]);
      let sum = 0;
      for (const value of out[0]) {
        sum += value * value;
      }
      return Math.sqrt(sum / out[0].length);
    } finally {
      engine.destroy();
    }
  };

  it('renders two legal engine modes differently and refuses the values between them', () => {
    expect(render({ engineMode: 1 })).not.toBeCloseTo(render({ engineMode: 2 }), 8);
    for (const key of ['engineMode', 'waveform', 'filterModel', 'body']) {
      for (const value of [...NON_FINITE, 1.5]) {
        expectInvalidParameter(() => render({ [key]: value }));
      }
    }
  });
});

describe('MIDI byte fields refuse a value that would wrap into the byte domain', () => {
  type CcBinding = Parameters<typeof Project.midiParamToCc>[0][number];
  const binding = (overrides: Record<string, number>): CcBinding =>
    ({
      ccNumber: 7,
      channel: 0,
      kind: 0,
      paramId: 1,
      minValue: 0,
      maxValue: 1,
      ...overrides,
      // The overrides are the out-of-domain values under test, which is what
      // the declared type exists to forbid; the C++ reader is the subject here.
    }) as CcBinding;
  const encode = (overrides: Record<string, number>): number => {
    const event = Project.midiParamToCc([binding(overrides)], 1, 0.5, 0, 0);
    if (event === null) {
      throw new Error('the binding did not match the requested parameter');
    }
    return event.data0 >>> 0;
  };

  it('places two legal controller numbers in the emitted word', () => {
    expect((encode({ ccNumber: 7 }) >>> 8) & 0xff).toBe(7);
    expect((encode({ ccNumber: 64 }) >>> 8) & 0xff).toBe(64);
  });

  it('refuses a controller number past the byte instead of wrapping it', () => {
    // 256 used to emit controller 0, 300 controller 44, and 512 controller 0.
    for (const value of [256, 300, 512, -1, 1.5, ...NON_FINITE, ...SATURATING]) {
      expectInvalidParameter(() => encode({ ccNumber: value }));
    }
  });

  it('keeps the 255 any-channel wildcard and refuses the 511 that wrapped onto it', () => {
    expect((encode({ channel: 255 }) >>> 16) & 0xf).toBe(0);
    expect((encode({ channel: 7 }) >>> 16) & 0xf).toBe(7);
    for (const value of [256, 257, 511, 512, -1, 1.5, ...NON_FINITE]) {
      expectInvalidParameter(() => encode({ channel: value }));
    }
  });

  it('still accepts a UMP word written as a signed bit expression', () => {
    // `(0x4 << 28) | …` is negative once bit 31 is set, and the whole 32-bit
    // range is legal for a word, so the sign is reinterpreted rather than
    // refused -- the one narrowing here that is not a range check.
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.setBuiltinInstrument({ gain: 0.5 }, 0);
      const noteOn = (0x2 << 28) | (0x9 << 20) | (60 << 8) | 100;
      expect(() =>
        engine.setMidiClips([
          {
            id: 1,
            trackId: 0,
            destinationId: 0,
            lengthSamples: 8192,
            events: [{ renderFrame: 0, word0: noteOn | (1 << 31), wordCount: 1 }],
          },
        ]),
      ).not.toThrow();
    } finally {
      engine.destroy();
    }
  });
});

describe('a positional parameter id refuses what the positional path used to wrap', () => {
  // The object-field and the positional spelling of the same uint32 disagreed:
  // embind converts a declared `uint32_t` parameter by ToUint32, which WRAPS, so
  // 2**32 + 5 asked for a parameter that does not exist and was answered with
  // the binding whose id is 5. A wrapped id is in the legal domain by
  // construction, so no downstream range check could see it.
  type CcBinding = Parameters<typeof Project.midiParamToCc>[0][number];
  const bindings = [
    { ccNumber: 7, channel: 0, kind: 0, paramId: 5, minValue: 0, maxValue: 1 },
    { ccNumber: 64, channel: 0, kind: 0, paramId: 9, minValue: 0, maxValue: 1 },
  ] as unknown as CcBinding[];

  /** The controller number the requested id selected, or null for no match. */
  const selectedCc = (paramId: number): number | null => {
    const event = Project.midiParamToCc(bindings, paramId, 0.5, 0, 0);
    return event === null ? null : (event.data0 >>> 8) & 0xff;
  };

  it('selects a different controller for each of two legal ids', () => {
    // The control. Without it every refusal below would also hold for a call
    // that ignored the id entirely.
    expect(selectedCc(5)).toBe(7);
    expect(selectedCc(9)).toBe(64);
  });

  it('refuses an id that used to fold onto a legal neighbour', () => {
    // 2**32 + 5 wrapped onto 5 and 5.5 truncated onto it, so both returned
    // controller 7 for a parameter the caller never named. The saturating
    // constants shared by this file are deliberately NOT reused here: most of
    // them are legal uint32 ids, so refusing them would be the wrong answer.
    for (const value of [2 ** 32 + 5, 5.5, -1, 2 ** 40, 2 ** 53 + 1, ...NON_FINITE]) {
      expectInvalidParameter(() => selectedCc(value));
    }
  });

  it('answers a legal id that matches nothing with no event rather than a refusal', () => {
    // The other half of the boundary: 4294967295 is a representable id, so the
    // reader passes it and the map reports no match. Without this the refusals
    // above could not be told from a call that had started refusing the whole
    // top of the range.
    expect(selectedCc(4294967295)).toBeNull();
  });

  it('names the field the object-field spelling names', () => {
    // Both spellings now read through the same checked conversion, so the
    // refusal a caller sees does not depend on which door the id came through.
    let caught: unknown;
    try {
      selectedCc(5.5);
    } catch (e) {
      caught = e;
    }
    expect((caught as SonareError).message).toContain('paramId');
  });
});

describe('realtime-engine options-bag readers refuse a silently coerced value', () => {
  const midi1Word = (status: number, channel: number, data1: number, data2: number): number =>
    ((0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data1 << 8) | data2) >>> 0;
  const heldNote = [
    { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
    { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
  ];

  const sound = (clip: Record<string, unknown>, boundAt = 0): number => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      engine.setBuiltinInstrument({ gain: 0.5 }, boundAt);
      engine.setMidiClips([
        { id: 1, trackId: 6, destinationId: 0, lengthSamples: 8192, events: heldNote, ...clip },
      ]);
      engine.play();
      const out = engine.process([new Float32Array(128), new Float32Array(128)]);
      let sum = 0;
      for (const value of out[0]) {
        sum += value * value;
      }
      return Math.sqrt(sum / out[0].length);
    } finally {
      engine.destroy();
    }
  };

  it('routes a clip to the destination it names and refuses one that is not an id', () => {
    // The control, both ways round: the destination decides whether it sounds.
    expect(sound({ destinationId: 0 })).toBeGreaterThan(0);
    expect(sound({ destinationId: 6 })).toBe(0);
    expect(sound({ destinationId: 6 }, 6)).toBeGreaterThan(0);
    // -1 and NaN both used to clamp onto destination 0 and sound there.
    for (const value of [-1, 1.5, ...NON_FINITE, 2 ** 32, 2 ** 53 + 1]) {
      expectInvalidParameter(() => sound({ destinationId: value }));
    }
  });

  it('refuses a render frame that is not a whole sample position', () => {
    const at = (renderFrame: number): number =>
      sound({ events: [{ renderFrame, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 }] });
    // The control: the event's frame moves how much of it lands in the block.
    expect(at(0)).toBeGreaterThan(at(64));
    expect(at(64)).toBeGreaterThan(0);
    for (const value of [1.5, ...NON_FINITE]) {
      expectInvalidParameter(() => at(value));
    }
  });

  it('refuses a bounce length that is not a whole frame count', () => {
    const bounce = (totalFrames: number): number => {
      const engine = new RealtimeEngine(48000, 128);
      try {
        engine.setClips([
          { id: 1, startPpq: 0, channels: [new Float32Array(256).fill(0.25)], lengthSamples: 256 },
        ]);
        return engine.bounceOffline({ totalFrames, blockSize: 128, numChannels: 2 }).frames;
      } finally {
        engine.destroy();
      }
    };
    expect(bounce(128)).toBe(128);
    expect(bounce(256)).toBe(256);
    for (const value of [1.5, ...NON_FINITE, -1, 0]) {
      expectInvalidParameter(() => bounce(value));
    }
  });
});

describe('annotation ordinals refuse a value that clamps or truncates onto a member', () => {
  const annotate = (entry: Record<string, number>): number => {
    const project = new Project();
    try {
      project.annotateKeys([{ startPpq: 0, endPpq: 480, tonicPc: 0, mode: 0, ...entry }]);
      return JSON.parse(project.toJson()).annotation.keys[0].tonic_pc;
    } finally {
      project.destroy();
    }
  };

  it('keeps two legal pitch classes and the 255 unknown sentinel', () => {
    expect(annotate({ tonicPc: 0 })).toBe(0);
    expect(annotate({ tonicPc: 5 })).toBe(5);
    expect(annotate({ tonicPc: 255 })).toBe(255);
  });

  it('refuses a negative, fractional or non-finite pitch class', () => {
    for (const key of ['tonicPc', 'mode']) {
      for (const value of [-1, -2, 1.5, ...NON_FINITE, 2 ** 32, 2 ** 53 + 1]) {
        expectInvalidParameter(() => annotate({ [key]: value }));
      }
    }
  });
});

describe('float options refuse a value the float type cannot hold', () => {
  const noisy = new Float32Array(12000);
  let seed = 12345;
  for (let i = 0; i < noisy.length; i++) {
    seed = (seed * 1103515245 + 12345) >>> 0;
    noisy[i] = 0.4 * Math.sin((2 * Math.PI * 440 * i) / 48000) + 0.05 * ((seed / 2 ** 32) * 2 - 1);
  }
  const dereverb = (t60Sec: number): number => {
    const out = masteringRepairDereverbClassical(noisy, 48000, { t60Sec });
    let sum = 0;
    for (const value of out) {
      sum += value * value;
    }
    return Math.sqrt(sum / out.length);
  };

  it('refuses an overflowing decay time instead of accepting it as infinity', () => {
    // The control: two legal decay times leave different residuals.
    expect(dereverb(0.2)).not.toBeCloseTo(dereverb(1.0), 6);
    // 3.5e38, 1e39, 1e300 and Infinity all became +inf and reached one result.
    for (const value of [3.5e38, 1e39, 1e300, Number.POSITIVE_INFINITY]) {
      expectInvalidParameter(() => dereverb(value));
    }
  });
});

describe('a positional offset refuses what the positional path used to wrap', () => {
  // The sibling of the parameter-id case above, and the one where the core's own
  // clamp is not the backstop it looks like: it only catches an offset PAST the
  // buffer, and a wrapped offset lands inside it by construction. Declared
  // `size_t`, 2**32 + 100 returned the window at 100 and NaN the window at 0,
  // each a perfectly plausible spectrum of audio nobody asked about.
  const sampleRate = 22050;
  const length = 8192;
  // Two halves with different content, so the window a request landed on is
  // readable from the result rather than inferred.
  const samples = new Float32Array(length).map((_, i) =>
    Math.sin((2 * Math.PI * (i < length / 2 ? 440 : 1760) * i) / sampleRate),
  );

  /** The frequency of the loudest bin of the window starting at `frameOffset`. */
  const peakHz = (frameOffset: number): number => {
    const result = meteringSpectrumFrame(samples, sampleRate, frameOffset, { nFft: 1024 });
    let best = 0;
    let bin = 0;
    for (let i = 0; i < result.magnitude.length; i += 1) {
      if (result.magnitude[i] > best) {
        best = result.magnitude[i];
        bin = i;
      }
    }
    return result.frequencies[bin];
  };

  it('reads a different window for each of two legal offsets', () => {
    // The control, and it has to be content rather than a bare success: an
    // offset that is ignored returns a result too.
    expect(peakHz(0)).toBeCloseTo(430.7, 0);
    expect(peakHz(5000)).toBeCloseTo(1765.7, 0);
  });

  it('refuses an offset that used to fold onto a real window', () => {
    for (const value of [2 ** 32 + 100, 100.5, -1, 2 ** 40, ...NON_FINITE]) {
      expectInvalidParameter(() => peakHz(value));
    }
  });

  it('still accepts an offset past the end, which the core zero-pads', () => {
    // The boundary the guard must not eat: past-the-end is a legal request with
    // a documented answer, and refusing it would be a different regression.
    expect(() => peakHz(length)).not.toThrow();
    expect(peakHz(length)).toBe(0);
  });
});

describe('the double reader refuses a non-finite number at the read', () => {
  // Every one of this reader's fields is also checked by the site that reads it,
  // so what moved is WHERE the refusal happens, not whether it happens. That is
  // the point: the check belongs to the reader so the next field added to one of
  // these bags inherits it, and the assertions below are written on the message
  // because that is the only thing a caller can see change.
  const refusalFor = (fn: () => unknown): SonareError => {
    let caught: unknown;
    try {
      fn();
    } catch (e) {
      caught = e;
    }
    expect(isSonareError(caught)).toBe(true);
    return caught as SonareError;
  };

  const withEngine = <T>(fn: (engine: RealtimeEngine) => T): T => {
    const engine = new RealtimeEngine(48000, 128);
    try {
      return fn(engine);
    } finally {
      engine.destroy();
    }
  };

  it('accepts a legal tempo map and a legal marker', () => {
    // The control. Without it a reader that refused every double would satisfy
    // both assertions below.
    withEngine((engine) => {
      expect(() => engine.setTempoSegments([{ startPpq: 0, bpm: 120, endBpm: 140 }])).not.toThrow();
      expect(() => engine.setMarkers([{ ppq: 0, id: 3 }])).not.toThrow();
    });
  });

  it('names the field rather than the entry point when a tempo field is not finite', () => {
    for (const value of [...NON_FINITE, Number.NEGATIVE_INFINITY]) {
      const error = withEngine((engine) =>
        refusalFor(() => engine.setTempoSegments([{ startPpq: 0, bpm: value }])),
      );
      expect(error.code).toBe(ErrorCode.InvalidParameter);
      expect(error.message).toContain('bpm must be a finite number');
      // The site's own composite check is the fallback, not the first line of
      // defence; seeing its wording here would mean the reader let the value by.
      expect(error.message).not.toContain('setTempoSegments:');
    }
  });

  it('refuses a non-finite marker id, which no later lookup could match', () => {
    const error = withEngine((engine) =>
      refusalFor(() => engine.setMarkers([{ ppq: 0, id: Number.NaN }])),
    );
    expect(error.code).toBe(ErrorCode.InvalidParameter);
    expect(error.message).toContain('id must be a finite number');
  });
});

describe('fixFrames reads each frame element through the scalar guard, not a raw cast', () => {
  // A real Int32Array cannot itself carry an out-of-range element -- writing
  // to one wraps at the JS boundary before the C++ side ever sees it. A Proxy
  // over a real Int32Array passes `instanceof Int32Array` (so the facade
  // forwards it unmodified) while lying about one element's value, which is
  // the only way to drive the native per-element read with a value the
  // TypedArray itself refuses to hold.
  function lyingElement(real: number[], index: number, lie: number): Int32Array {
    const backing = new Int32Array(real);
    return new Proxy(backing, {
      // Forwarded through `target` rather than `receiver`: a TypedArray's
      // `.length` is an accessor on the prototype that brand-checks `this`,
      // so calling it with the Proxy itself as `receiver` throws before the
      // lied-about index is ever read.
      get(target, property) {
        if (property === String(index)) {
          return lie;
        }
        return Reflect.get(target, property);
      },
    });
  }

  it('keeps two legal single-frame arrays as themselves', () => {
    expect(Array.from(fixFrames(new Int32Array([5]), -1, -1, false))).toEqual([5]);
    expect(Array.from(fixFrames(new Int32Array([6]), -1, -1, false))).toEqual([6]);
  });

  it('refuses an element that used to wrap or truncate onto a legal frame', () => {
    // 2**32 + 5 used to wrap onto 5 -- identical to the legal [5] above -- and
    // 1.5 used to truncate onto 1.
    for (const value of [2 ** 32 + 5, 1.5, ...SATURATING, ...NON_FINITE]) {
      expectInvalidParameter(() => fixFrames(lyingElement([0], 0, value), -1, -1, false));
    }
  });
});

describe('array-like `.length` reads refuse a wrapped, negative or fractional count', () => {
  // Before this guard, an array-like's raw `.length` narrowed straight into a
  // C++ integer or size_t: 2**32 + 5 wrapped onto 5 (a plausible small count
  // indistinguishable from a caller who really asked for 5), and a negative
  // length sailed past a `> 0` check into a size_t-sized allocation. Each case
  // below pairs the refusal with two legal counts that select a genuinely
  // different outcome, so a reader that refused everything could not pass it
  // by accident.
  const WRAPPING_LENGTHS = [2 ** 32 + 5, -1, 1.5, 2 ** 40, ...NON_FINITE];

  it('routes midiRouteEvents from the real event count, not a lying `.length`', () => {
    const oneEvent = [Project.midiNoteOn(0, 0, 0, 60, 100)];
    const twoEvents = [Project.midiNoteOn(0, 0, 0, 60, 100), Project.midiNoteOn(1, 0, 0, 61, 100)];
    // The control: two legal event arrays route that many events.
    expect(Project.midiRouteEvents(oneEvent, {}).events.length).toBe(1);
    expect(Project.midiRouteEvents(twoEvents, {}).events.length).toBe(2);
    for (const value of WRAPPING_LENGTHS) {
      expectInvalidParameter(() =>
        Project.midiRouteEvents({ length: value } as unknown as typeof oneEvent, {}),
      );
    }
  });

  it('reads midiCcLearn and midiParamToCc bindings from the real array length', () => {
    type CcBinding = Parameters<typeof Project.midiParamToCc>[0][number];
    const binding = (ccNumber: number): CcBinding =>
      ({ ccNumber, channel: 0, kind: 0, paramId: 1, minValue: 0, maxValue: 1 }) as CcBinding;
    // The control: an empty binding list matches nothing; one real, matching
    // binding does -- so the guard is reading how many bindings are really
    // there, not a value baked in ahead of time.
    expect(Project.midiParamToCc([], 1, 0.5, 0, 0)).toBeNull();
    expect(Project.midiParamToCc([binding(7)], 1, 0.5, 0, 0)).not.toBeNull();
    for (const value of WRAPPING_LENGTHS) {
      expectInvalidParameter(() =>
        Project.midiParamToCc({ length: value } as unknown as CcBinding[], 1, 0.5, 0, 0),
      );
      expectInvalidParameter(() =>
        Project.midiCcLearn(
          { length: value } as unknown as ReturnType<typeof Project.midiNoteOn>[],
          1,
        ),
      );
    }
  });
});
