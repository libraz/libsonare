/**
 * Pins the three per-note expression dimensions on both live-MIDI paths: the
 * engine-owned input source (`pushMidiInput*`) and the destination queue
 * (`pushMidi*`).
 *
 * The cases measure the rendered audio rather than asserting that a call did
 * not throw, because every defect this surface can have — a dropped event, a
 * bend truncated onto the wrong value, a pressure applied to the wrong voice —
 * reports success and renders something. The built-in synth is the instrument
 * with a stated answer to compare against: a full-scale bend is 2 semitones,
 * and full pressure doubles the amplitude.
 */

import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, RealtimeEngine } from '../src/index.js';

const SR = 48000;
const BLOCK = 128;
const BLOCKS = 48;
/** A4, so an unbent render reads as 440 Hz. */
const NOTE = 69;
const NOTE_HZ = 440;
/** `kPitchBendRangeSemitones` in the built-in synth. */
const BEND_RANGE_SEMITONES = 2;
const BEND_CENTRE = 8192;
const BEND_MAX = 16383;
/**
 * Pitch ratio of a full bend in either direction. Not quite 2^(2/12): the
 * 14-bit domain is asymmetric about its centre, so the largest upward bend is
 * one step short of the range the downward one reaches.
 */
const bendRatio = (bend14: number): number =>
  2 ** (((bend14 - BEND_CENTRE) / BEND_CENTRE) * (BEND_RANGE_SEMITONES / 12));

/** A held note on the built-in sine synth, rendered under one gesture. */
function renderHeldNote(gesture: (engine: RealtimeEngine) => void): Float32Array {
  const engine = new RealtimeEngine(SR, BLOCK);
  try {
    // A flat envelope, so the measured span is one steady tone and an RMS
    // ratio reads as the pressure gain rather than as a decay.
    engine.setBuiltinInstrument({ waveform: 'sine', attackMs: 1, sustain: 1, releaseMs: 1 }, 0);
    engine.play();
    gesture(engine);
    const out: number[] = [];
    for (let block = 0; block < BLOCKS; block++) {
      out.push(...engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)])[0]);
    }
    return Float32Array.from(out);
  } finally {
    engine.destroy();
  }
}

/** The steady span, past the attack. */
const steady = (data: Float32Array): Float32Array => data.subarray(data.length >> 1);

/**
 * Fundamental from positive-going zero crossings, measured between the first
 * and last so the count is a whole number of periods.
 */
function fundamentalHz(data: Float32Array): number {
  let first = -1;
  let last = -1;
  let count = 0;
  for (let i = 1; i < data.length; i++) {
    if (data[i - 1] <= 0 && data[i] > 0) {
      if (first < 0) {
        first = i;
      }
      last = i;
      count++;
    }
  }
  if (count < 2) {
    return 0;
  }
  return ((count - 1) * SR) / (last - first);
}

const rms = (data: Float32Array): number =>
  Math.sqrt(data.reduce((sum, value) => sum + value * value, 0) / data.length);

/** What the C ABI answers for a bend outside its 14-bit domain. */
function sonareErrorCode(call: () => void): ErrorCode | undefined {
  try {
    call();
  } catch (error) {
    return isSonareError(error) ? error.code : undefined;
  }
  return undefined;
}

describe('a live pitch bend reaches the rendered audio', () => {
  const unbent = renderHeldNote((engine) => {
    engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
  });

  it('renders the unbent note at its own pitch, so the arms cannot agree by silence', () => {
    expect(rms(steady(unbent))).toBeGreaterThan(0);
    expect(fundamentalHz(steady(unbent))).toBeCloseTo(NOTE_HZ, 0);
  });

  it('bends a held note up by the synth range on the destination queue', () => {
    const bent = renderHeldNote((engine) => {
      engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
      engine.pushMidiPitchBend(0, 0, 0, BEND_MAX);
    });
    // The ratio rather than the absolute pitch, so the crossing estimator's own
    // bias is common to both arms and what is measured is the bend.
    expect(fundamentalHz(steady(bent)) / fundamentalHz(steady(unbent))).toBeCloseTo(
      bendRatio(BEND_MAX),
      2,
    );
    expect([...bent]).not.toEqual([...unbent]);
  });

  it('bends a held note through the engine-owned input source', () => {
    const bent = renderHeldNote((engine) => {
      engine.setMidiInputSource(0);
      engine.pushMidiInputNoteOn(0, 0, NOTE, 100, 0);
      engine.pushMidiInputPitchBend(0, 0, BEND_MAX, 0);
    });
    expect(fundamentalHz(steady(bent)) / fundamentalHz(steady(unbent))).toBeCloseTo(
      bendRatio(BEND_MAX),
      2,
    );
  });

  it('bends DOWN at the bottom of the domain, which is where a truncation lands', () => {
    // 16384 masked to 14 bits is this value, so the direction is what separates
    // a refused out-of-domain bend from a silently truncated one.
    const bent = renderHeldNote((engine) => {
      engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
      engine.pushMidiPitchBend(0, 0, 0, 0);
    });
    expect(fundamentalHz(steady(bent))).toBeLessThan(NOTE_HZ);
    expect(fundamentalHz(steady(bent)) / fundamentalHz(steady(unbent))).toBeCloseTo(
      bendRatio(0),
      2,
    );
  });

  it('leaves the pitch where it was when the bend is the centre value', () => {
    const centred = renderHeldNote((engine) => {
      engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
      engine.pushMidiPitchBend(0, 0, 0, BEND_CENTRE);
    });
    expect(fundamentalHz(steady(centred))).toBeCloseTo(NOTE_HZ, 0);
  });
});

describe('a live pressure reaches the rendered audio', () => {
  const unpressed = renderHeldNote((engine) => {
    engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
  });

  it('doubles the amplitude at full channel pressure on the destination queue', () => {
    const pressed = renderHeldNote((engine) => {
      engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
      engine.pushMidiChannelPressure(0, 0, 0, 127);
    });
    expect(rms(steady(pressed)) / rms(steady(unpressed))).toBeCloseTo(2, 1);
  });

  it('doubles the amplitude at full channel pressure through the input source', () => {
    const pressed = renderHeldNote((engine) => {
      engine.setMidiInputSource(0);
      engine.pushMidiInputNoteOn(0, 0, NOTE, 100, 0);
      engine.pushMidiInputChannelPressure(0, 0, 127, 0);
    });
    expect(rms(steady(pressed)) / rms(steady(unpressed))).toBeCloseTo(2, 1);
  });

  it('applies a poly pressure to the key it names and to no other', () => {
    const onTheHeldKey = renderHeldNote((engine) => {
      engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
      engine.pushMidiPolyPressure(0, 0, 0, NOTE, 127);
    });
    const onAnotherKey = renderHeldNote((engine) => {
      engine.pushMidiNoteOn(0, 0, 0, NOTE, 100);
      engine.pushMidiPolyPressure(0, 0, 0, NOTE + 3, 127);
    });
    expect(rms(steady(onTheHeldKey)) / rms(steady(unpressed))).toBeCloseTo(2, 1);
    // A key that is not sounding owns no voice, so the render is the untouched
    // one — which is what separates "reached the named voice" from "reached
    // every voice on the channel".
    expect([...onAnotherKey]).toEqual([...unpressed]);
  });

  it('applies a poly pressure through the input source', () => {
    const pressed = renderHeldNote((engine) => {
      engine.setMidiInputSource(0);
      engine.pushMidiInputNoteOn(0, 0, NOTE, 100, 0);
      engine.pushMidiInputPolyPressure(0, 0, NOTE, 127, 0);
    });
    expect(rms(steady(pressed)) / rms(steady(unpressed))).toBeCloseTo(2, 1);
  });
});

describe('a bend outside its domain is refused, never truncated into it', () => {
  const preparedEngine = (): RealtimeEngine => {
    const engine = new RealtimeEngine(SR, BLOCK);
    engine.setBuiltinInstrument({ waveform: 'sine' }, 0);
    engine.setMidiInputSource(0);
    return engine;
  };

  it('refuses the first value past the 14-bit domain on both paths', () => {
    const engine = preparedEngine();
    // 16384 is 0 masked to 14 bits — a FULL BEND DOWN — so a truncating reader
    // would accept it and bend the wrong way rather than report anything.
    expect(sonareErrorCode(() => engine.pushMidiPitchBend(0, 0, 0, BEND_MAX + 1))).toBe(
      ErrorCode.InvalidParameter,
    );
    expect(sonareErrorCode(() => engine.pushMidiInputPitchBend(0, 0, BEND_MAX + 1, 0))).toBe(
      ErrorCode.InvalidParameter,
    );
    engine.destroy();
  });

  it('refuses a value the uint16 cast would wrap before the C ABI sees it', () => {
    const engine = preparedEngine();
    // 65536 casts to 0, which is inside the bend domain, so the C ABI's own
    // range check would accept it; the addon has to close that.
    expect(() => engine.pushMidiPitchBend(0, 0, 0, 65536)).toThrow(RangeError);
    expect(() => engine.pushMidiInputPitchBend(0, 0, 65536, 0)).toThrow(RangeError);
    expect(() => engine.pushMidiPitchBend(0, 0, 0, -1)).toThrow(RangeError);
    expect(() => engine.pushMidiPitchBend(0, 0, 0, 8192.5)).toThrow(RangeError);
    engine.destroy();
  });

  it('accepts both ends of the domain', () => {
    const engine = preparedEngine();
    expect(() => engine.pushMidiPitchBend(0, 0, 0, 0)).not.toThrow();
    expect(() => engine.pushMidiPitchBend(0, 0, 0, BEND_MAX)).not.toThrow();
    expect(() => engine.pushMidiInputPitchBend(0, 0, 0, 0)).not.toThrow();
    expect(() => engine.pushMidiInputPitchBend(0, 0, BEND_MAX, 0)).not.toThrow();
    engine.destroy();
  });

  it('refuses a pressure byte that would wrap through the uint8 cast', () => {
    const engine = preparedEngine();
    expect(() => engine.pushMidiChannelPressure(0, 0, 0, 256)).toThrow(RangeError);
    expect(() => engine.pushMidiPolyPressure(0, 0, 0, 256, 100)).toThrow(RangeError);
    expect(() => engine.pushMidiInputChannelPressure(0, 0, 256, 0)).toThrow(RangeError);
    expect(() => engine.pushMidiInputPolyPressure(0, 0, 256, 100, 0)).toThrow(RangeError);
    engine.destroy();
  });

  it('reports a pressure past the 7-bit domain rather than masking it', () => {
    const engine = preparedEngine();
    expect(sonareErrorCode(() => engine.pushMidiChannelPressure(0, 0, 0, 128))).toBe(
      ErrorCode.InvalidParameter,
    );
    expect(sonareErrorCode(() => engine.pushMidiInputPolyPressure(0, 0, NOTE, 128, 0))).toBe(
      ErrorCode.InvalidParameter,
    );
    engine.destroy();
  });
});
