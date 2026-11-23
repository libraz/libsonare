import { describe, expect, it } from 'vitest';
import {
  midi2ControlToMidi1,
  midi2ControlToMidi1Control14,
  midi2NoteOnVelocityToMidi1,
  midi2PitchBendToMidi1,
  midi2VelocityToMidi1,
  midiControl14ToMidi2,
  midiControlToMidi2,
  midiNoteOnVelocityToMidi2,
  midiPitchBendToMidi2,
  midiVelocityToMidi2,
} from '../src/index.js';

describe('MIDI 1.0 <-> MIDI 2.0 value scaling', () => {
  it('lands min, center and max', () => {
    expect(midiVelocityToMidi2(0)).toBe(0);
    expect(midiVelocityToMidi2(64)).toBe(0x8000);
    expect(midiVelocityToMidi2(127)).toBe(0xffff);
    expect(midiNoteOnVelocityToMidi2(1)).toBe(0x0200);
    expect(midiControlToMidi2(0)).toBe(0);
    expect(midiControlToMidi2(64)).toBe(0x80000000);
    expect(midiControlToMidi2(127)).toBe(0xffffffff);
    expect(midiControl14ToMidi2(8192)).toBe(0x80000000);
    expect(midiControl14ToMidi2(16383)).toBe(0xffffffff);
    expect(midiPitchBendToMidi2(0)).toBe(0);
    expect(midiPitchBendToMidi2(8192)).toBe(0x80000000);
    expect(midiPitchBendToMidi2(16383)).toBe(0xffffffff);
    expect(midi2VelocityToMidi1(0)).toBe(0);
    expect(midi2NoteOnVelocityToMidi1(0)).toBe(1);
    expect(midi2NoteOnVelocityToMidi1(0x01ff)).toBe(1);
    expect(midi2NoteOnVelocityToMidi1(0xffff)).toBe(127);
  });

  it('returns every input exactly when widening then narrowing', () => {
    for (let v = 0; v < 128; v++) {
      expect(midi2VelocityToMidi1(midiVelocityToMidi2(v))).toBe(v);
      if (v !== 0) expect(midi2NoteOnVelocityToMidi1(midiNoteOnVelocityToMidi2(v))).toBe(v);
      expect(midi2ControlToMidi1(midiControlToMidi2(v))).toBe(v);
    }
    for (let v = 0; v < 16384; v++) {
      expect(midi2ControlToMidi1Control14(midiControl14ToMidi2(v))).toBe(v);
      expect(midi2PitchBendToMidi1(midiPitchBendToMidi2(v))).toBe(v);
    }
  });

  it('is not exact narrowing then widening', () => {
    // 0x8001 narrows to 64, which widens back to the center.
    expect(midiVelocityToMidi2(midi2VelocityToMidi1(0x8001))).toBe(0x8000);
  });

  it('refuses a value outside the MIDI range by its name', () => {
    expect(() => midiVelocityToMidi2(128)).toThrow(
      new RangeError('midiVelocityToMidi2: velocity must be an integer in [0, 127]'),
    );
    expect(() => midiNoteOnVelocityToMidi2(0)).toThrow(
      new RangeError('midiNoteOnVelocityToMidi2: velocity must be an integer in [1, 127]'),
    );
    expect(() => midiControlToMidi2(200)).toThrow(
      new RangeError('midiControlToMidi2: value must be an integer in [0, 127]'),
    );
    expect(() => midiControl14ToMidi2(16384)).toThrow(
      new RangeError('midiControl14ToMidi2: value14 must be an integer in [0, 16383]'),
    );
    expect(() => midiPitchBendToMidi2(0xffff)).toThrow(
      new RangeError('midiPitchBendToMidi2: bend14 must be an integer in [0, 16383]'),
    );
  });

  it.each([
    ['midiVelocityToMidi2', () => midiVelocityToMidi2(300), 'velocity'],
    ['midiControlToMidi2', () => midiControlToMidi2(-1), 'value'],
    ['midi2VelocityToMidi1', () => midi2VelocityToMidi1(65536), 'velocity16'],
    ['midiPitchBendToMidi2', () => midiPitchBendToMidi2(65536 + 8192), 'bend14'],
    ['midi2ControlToMidi1', () => midi2ControlToMidi1(2 ** 32), 'value32'],
    ['midi2PitchBendToMidi1', () => midi2PitchBendToMidi1(1.5), 'bend32'],
  ] as const)(
    '%s refuses a value its argument cannot hold rather than wrapping it',
    (entry, call, name) => {
      expect(call).toThrow(RangeError);
      expect(call).toThrow(new RegExp(`^${entry}: ${name} must be `));
    },
  );

  it('refuses a non-number as a TypeError naming the argument', () => {
    expect(() => midiVelocityToMidi2('64' as never)).toThrow(TypeError);
    expect(() => midiVelocityToMidi2('64' as never)).toThrow(/^midiVelocityToMidi2: velocity /);
    expect(() => midi2PitchBendToMidi1(undefined as never)).toThrow(TypeError);
  });
});
