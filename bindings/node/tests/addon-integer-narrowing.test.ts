/**
 * A caller's number is refused, not folded into a legal one.
 *
 * `Int32Value()` is ToInt32 and therefore WRAPS, so the value that matters is
 * not the implausible one: `2**31` wraps negative and the core's own guards
 * catch it, while `2**32 + 8` lands on 8, which is a setting a caller could
 * have asked for. Each case opens with a positive control — two legitimate
 * values whose results differ — because without it an accepted out-of-range
 * value cannot be told from a field the entry point never reads.
 *
 * Driven against the addon rather than the TypeScript facade: the facade's own
 * guards would pre-empt the native check, and the addon is the boundary a
 * generated binding or a direct consumer reaches.
 */

import { describe, expect, it } from 'vitest';
import { addon } from '../src/native.js';

const SAMPLE_RATE = 22050;

function tone(): Float32Array {
  const out = new Float32Array(SAMPLE_RATE / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.3 * Math.sin((2 * Math.PI * 220 * i) / SAMPLE_RATE);
  }
  return out;
}

const OUT_OF_INT_RANGE = [
  2 ** 31,
  2 ** 32,
  2 ** 32 + 1,
  2 ** 32 + 2,
  -(2 ** 31) - 1,
  Number.NaN,
  Number.POSITIVE_INFINITY,
];

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

describe('the addon refuses a number it cannot hold', () => {
  it('refuses a wrapped positional count rather than reading it as a smaller one', () => {
    const samples = tone();
    const mel = (n: number) =>
      native.melSpectrogram(samples, SAMPLE_RATE, 512, 256, n, 0, 0, false);
    expect(mel(8).nMels).not.toBe(mel(16).nMels); // positive control
    for (const value of OUT_OF_INT_RANGE) {
      expect(() => mel(value), `nMels ${value}`).toThrow(RangeError);
    }
  });

  it('refuses a wrapped options-bag value', () => {
    const rir = (ismOrder: number) => {
      const result = native.synthesizeRir({
        sampleRate: 48000,
        roomWidth: 5,
        roomDepth: 4,
        roomHeight: 3,
        ismOrder,
        rt60: 0.6,
      });
      let energy = 0;
      for (const x of result.rir) {
        energy += Math.abs(x);
      }
      return energy;
    };
    expect(rir(1)).not.toBe(rir(3)); // positive control
    for (const value of OUT_OF_INT_RANGE) {
      expect(() => rir(value), `ismOrder ${value}`).toThrow(RangeError);
    }
  });

  it('refuses a wrapped struct-field value on an engine object', () => {
    const engine = new native.RealtimeEngine(48000, 256);
    engine.prepare(48000, 256, 1024, 1024, 8);
    const marker = (kind: number) => {
      engine.setMarkers([{ id: 1, kind, keyFifths: 0, keyMinor: false, ppq: 0, name: 'm' }]);
      return engine.markerByIndex(0).kind;
    };
    expect(marker(0)).not.toBe(marker(1)); // positive control
    for (const value of OUT_OF_INT_RANGE) {
      expect(() => marker(value), `kind ${value}`).toThrow(RangeError);
    }
  });

  it('refuses a negative id rather than reading it as the largest one', () => {
    const project = new native.Project();
    const a = project.addTrack(0, 'a');
    const b = project.addTrack(0, 'b');
    project.setTrackGain(a, 0.25);
    project.setTrackGain(b, 0.75);
    expect(project.trackByIndex(0).gain).not.toBe(project.trackByIndex(1).gain);
    for (const value of [-1, -2, 2 ** 32, 2 ** 32 + 2, Number.NaN]) {
      expect(() => project.setTrackGain(value, 0.5), `trackId ${value}`).toThrow(RangeError);
    }
  });

  it('keeps the bit-31 spelling of a raw MIDI word, which is not an out-of-range value', () => {
    const engine = new native.RealtimeEngine(48000, 256);
    engine.prepare(48000, 256, 1024, 1024, 8);
    const push = (word0: number) =>
      engine.setMidiClips([
        {
          id: 1,
          trackId: 0,
          startSample: 0,
          lengthSamples: 480,
          events: [{ renderFrame: 0, word0, wordCount: 1 }],
        },
      ]);
    // `(0x8 << 28) | 0x1234` is how JS spells a word with bit 31 set, and it is
    // a NEGATIVE int32. Refusing it would reject the idiomatic spelling.
    expect(() => push((0x8 << 28) | 0x1234)).not.toThrow();
    expect(() => push(-1)).not.toThrow();
    expect(() => push(0xffffffff)).not.toThrow();
    expect(() => push(2 ** 32)).toThrow(RangeError);
    expect(() => push(-(2 ** 31) - 1)).toThrow(RangeError);
  });

  it('names the property a rejected element lacks instead of one message for every mistake', () => {
    const samples = tone();
    const remix = (intervals: unknown) => native.remix(samples, intervals, SAMPLE_RATE, false);
    // Positive control: the interval end sets the output length, so an
    // assertion about the message cannot pass on a path that never reads it.
    expect(remix([0, 1000]).length).not.toBe(remix([0, 2000]).length);
    // The `$` anchors are what make these three distinguish each other: the
    // range message begins with the fractional one, so an unanchored match
    // would pass against a single merged message for all three.
    expect(() => remix([0, 1000.7])).toThrow(/intervals\[1\] must be an integer$/);
    expect(() => remix([0, 2 ** 32])).toThrow(
      /intervals\[1\] must be an integer in \[-2147483648, 2147483647\]$/,
    );
    expect(() => remix([0, Number.POSITIVE_INFINITY])).toThrow(
      /intervals\[1\] must be a finite integer$/,
    );
    expect(() => remix([0, Number.NaN])).toThrow(/intervals\[1\] must be a finite integer$/);
    // A non-number is the wrong type rather than the wrong domain, and only the
    // class carries that difference.
    expect(() => remix([0, '1000'])).toThrow(TypeError);
    expect(() => remix([0, 1000.7])).toThrow(RangeError);
  });
});
