import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, Project, RealtimeEngine } from '../src/index.js';

const G = 1;
const CH = 2;

/** Words a builder produced, as unsigned hex for readable failures. */
function words(event: { data0: number; data1?: number }): [number, number] {
  return [event.data0, event.data1 ?? 0];
}

describe('Project MIDI 2.0 builders', () => {
  it('packs note-on with velocity and attribute', () => {
    const e = Project.midi2NoteOn(1, G, CH, 0x3c, 0xc000, 3, 0x1234);
    expect(e.ppq).toBe(1);
    expect(words(e)).toEqual([0x41923c03, 0xc0001234]);
  });

  it('packs note-off with the 16-bit release velocity', () => {
    expect(words(Project.midi2NoteOff(0, G, CH, 0x3c, 0x8000))).toEqual([0x41823c00, 0x80000000]);
    expect(words(Project.midi2NoteOff(0, G, CH, 0x3c))).toEqual([0x41823c00, 0]);
  });

  it('packs the 32-bit channel-voice values', () => {
    expect(words(Project.midi2Cc(0, G, CH, 7, 0xdeadbeef))).toEqual([0x41b20700, 0xdeadbeef]);
    expect(words(Project.midi2PolyPressure(0, G, CH, 0x40, 0x12345678))).toEqual([
      0x41a24000, 0x12345678,
    ]);
    expect(words(Project.midi2ChannelPressure(0, G, CH, 0xffffffff))).toEqual([
      0x41d20000, 0xffffffff,
    ]);
    expect(words(Project.midi2PitchBend(0, G, CH, 0x80000000))).toEqual([0x41e20000, 0x80000000]);
  });

  it('packs the maximum group and channel', () => {
    expect(words(Project.midi2Cc(0, 15, 15, 127, 1))).toEqual([0x4fbf7f00, 1]);
  });

  it('packs program change with and without a bank', () => {
    expect(words(Project.midi2Program(0, G, CH, 5, true, 0x01, 0x02))).toEqual([
      0x41c20001, 0x05000102,
    ]);
    expect(words(Project.midi2Program(0, G, CH, 5))).toEqual([0x41c20000, 0x05000000]);
  });

  it('packs registered and assignable controllers by bank and index', () => {
    expect(words(Project.midi2RegisteredController(0, G, CH, 0x10, 0x20, 0x11223344))).toEqual([
      0x41221020, 0x11223344,
    ]);
    expect(words(Project.midi2AssignableController(0, G, CH, 0x10, 0x20, 0x11223344))).toEqual([
      0x41321020, 0x11223344,
    ]);
  });

  it('packs relative controllers with a two-complement delta', () => {
    expect(words(Project.midi2RelativeRegisteredController(0, G, CH, 0x10, 0x20, -1))).toEqual([
      0x41421020, 0xffffffff,
    ]);
    expect(
      words(Project.midi2RelativeAssignableController(0, G, CH, 0x10, 0x20, 0x7fffffff)),
    ).toEqual([0x41521020, 0x7fffffff]);
    expect(
      words(Project.midi2RelativeAssignableController(0, G, CH, 0x10, 0x20, -0x80000000)),
    ).toEqual([0x41521020, 0x80000000]);
  });

  it('packs per-note controllers with a full 8-bit index', () => {
    expect(words(Project.midi2RegisteredPerNoteController(0, G, CH, 0x3c, 0xff, 9))).toEqual([
      0x41023cff, 9,
    ]);
    expect(words(Project.midi2AssignablePerNoteController(0, G, CH, 0x3c, 0xff, 9))).toEqual([
      0x41123cff, 9,
    ]);
  });

  it('packs per-note pitch bend and management flags', () => {
    expect(words(Project.midi2PerNotePitchBend(0, G, CH, 0x3c, 0x80000000))).toEqual([
      0x41623c00, 0x80000000,
    ]);
    expect(words(Project.midi2PerNoteManagement(0, G, CH, 0x3c))).toEqual([0x41f23c00, 0]);
    expect(words(Project.midi2PerNoteManagement(0, G, CH, 0x3c, true))).toEqual([0x41f23c02, 0]);
    expect(words(Project.midi2PerNoteManagement(0, G, CH, 0x3c, false, true))).toEqual([
      0x41f23c01, 0,
    ]);
    expect(words(Project.midi2PerNoteManagement(0, G, CH, 0x3c, true, true))).toEqual([
      0x41f23c03, 0,
    ]);
  });

  it('refuses out-of-range fields', () => {
    expect(() => Project.midi2NoteOn(0, 16, CH, 60, 0)).toThrow(RangeError);
    expect(() => Project.midi2NoteOn(0, G, 16, 60, 0)).toThrow(RangeError);
    expect(() => Project.midi2NoteOn(0, G, CH, 128, 0)).toThrow(RangeError);
    expect(() => Project.midi2NoteOn(0, G, CH, 60, 0x10000)).toThrow(RangeError);
    expect(() => Project.midi2NoteOn(-1, G, CH, 60, 0)).toThrow(RangeError);
    expect(() => Project.midi2Cc(0, G, CH, 7, 2 ** 32)).toThrow(RangeError);
    expect(() => Project.midi2Cc(0, G, CH, 7, -1)).toThrow(RangeError);
    expect(() => Project.midi2Cc(0, G, CH, 7, 0.5)).toThrow(RangeError);
    expect(() => Project.midi2RegisteredController(0, G, CH, 128, 0, 0)).toThrow(RangeError);
    expect(() => Project.midi2RelativeRegisteredController(0, G, CH, 0, 0, 2 ** 31)).toThrow(
      RangeError,
    );
    expect(() => Project.midi2RegisteredPerNoteController(0, G, CH, 60, 256, 0)).toThrow(
      RangeError,
    );
    expect(() => Project.midi2Program(0, G, CH, 128)).toThrow(RangeError);
  });

  it('produces events the project accepts', () => {
    const project = Project.create();
    const { clipId } = project.addMidiClip(0, 4);
    expect(() =>
      project.setMidiEvents(clipId, [
        Project.midi2NoteOn(0, 0, 0, 60, 0xc000),
        Project.midi2NoteOff(1, 0, 0, 60, 0),
      ]),
    ).not.toThrow();
  });
});

function errorCode(call: () => void): ErrorCode | undefined {
  try {
    call();
  } catch (error) {
    return isSonareError(error) ? error.code : undefined;
  }
  return undefined;
}

describe('RealtimeEngine raw UMP push', () => {
  const NOTE_ON_2W = [0x41923c00, 0xc0000000];

  function withEngine(body: (engine: RealtimeEngine) => void): void {
    const engine = new RealtimeEngine(48000, 128);
    try {
      body(engine);
    } finally {
      engine.destroy();
    }
  }

  it('pushMidiUmp accepts one- and two-word messages as typed or plain arrays', () => {
    withEngine((engine) => {
      expect(() => engine.pushMidiUmp(0, new Uint32Array(NOTE_ON_2W))).not.toThrow();
      expect(() => engine.pushMidiUmp(0, NOTE_ON_2W, -1)).not.toThrow();
      expect(() => engine.pushMidiUmp(0, [0x00000000])).not.toThrow();
      // Bit 31 written as the signed spelling of a shift expression.
      expect(() => engine.pushMidiUmp(0, [(0x4 << 28) | 0x00930000, 0])).not.toThrow();
    });
  });

  it('pushMidiUmp refuses data messages and mismatched word counts', () => {
    withEngine((engine) => {
      expect(errorCode(() => engine.pushMidiUmp(0, [0x30160000, 0]))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(errorCode(() => engine.pushMidiUmp(0, [0x50000000, 0, 0, 0]))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(errorCode(() => engine.pushMidiUmp(0, [0x41923c00]))).toBe(ErrorCode.InvalidParameter);
      expect(errorCode(() => engine.pushMidiUmp(0, [0x41923c00, 0, 0]))).toBe(
        ErrorCode.InvalidParameter,
      );
    });
  });

  it('pushMidiUmp refuses an empty or oversized array and wrong types by name', () => {
    withEngine((engine) => {
      expect(() => engine.pushMidiUmp(0, [])).toThrow(RangeError);
      expect(() => engine.pushMidiUmp(0, [0, 0, 0, 0, 0])).toThrow(RangeError);
      expect(() => engine.pushMidiUmp(0, [2 ** 32])).toThrow(RangeError);
      expect(() => engine.pushMidiUmp(0, [0x41923c00, 'x' as unknown as number])).toThrow(
        /words\[1\]/,
      );
      expect(() => engine.pushMidiUmp(0, 'abc' as unknown as number[])).toThrow(TypeError);
      expect(() => engine.pushMidiUmp(0, new Float32Array(2))).toThrow(TypeError);
      expect(() => engine.pushMidiUmp('x' as unknown as number, NOTE_ON_2W)).toThrow(TypeError);
      expect(() => engine.pushMidiUmp(0, NOTE_ON_2W, 'now' as unknown as number)).toThrow(
        TypeError,
      );
    });
  });

  it('pushMidiInputUmp needs an enabled input source and mirrors the destination rules', () => {
    withEngine((engine) => {
      expect(errorCode(() => engine.pushMidiInputUmp(NOTE_ON_2W))).toBe(ErrorCode.InvalidParameter);
      engine.setMidiInputSource(0);
      expect(() => engine.pushMidiInputUmp(new Uint32Array(NOTE_ON_2W))).not.toThrow();
      expect(() => engine.pushMidiInputUmp(NOTE_ON_2W, 128)).not.toThrow();
      expect(errorCode(() => engine.pushMidiInputUmp([0x30160000, 0]))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(errorCode(() => engine.pushMidiInputUmp([0x50000000, 0, 0, 0]))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(errorCode(() => engine.pushMidiInputUmp([0x41923c00]))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(() => engine.pushMidiInputUmp([])).toThrow(RangeError);
      expect(() => engine.pushMidiInputUmp(NOTE_ON_2W, 'now' as unknown as number)).toThrow(
        TypeError,
      );
    });
  });

  it('reports a full queue as OutOfMemory', () => {
    withEngine((engine) => {
      let code: ErrorCode | undefined;
      for (let i = 0; i < 100000 && code === undefined; i++) {
        code = errorCode(() => engine.pushMidiUmp(0, NOTE_ON_2W));
      }
      expect(code).toBe(ErrorCode.OutOfMemory);
    });
  });
});
