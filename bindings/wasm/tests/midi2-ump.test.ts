import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  midi2AssignableController,
  midi2AssignablePerNoteController,
  midi2Cc,
  midi2ChannelPressure,
  midi2NoteOff,
  midi2NoteOn,
  midi2PerNoteManagement,
  midi2PerNotePitchBend,
  midi2PitchBend,
  midi2PolyPressure,
  midi2Program,
  midi2RegisteredController,
  midi2RegisteredPerNoteController,
  midi2RelativeAssignableController,
  midi2RelativeRegisteredController,
  Project,
  RealtimeEngine,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

const G = 1;
const CH = 2;

/** Words a builder produced, as unsigned hex for readable failures. */
function words(event: { word0: number; word1?: number }): [number, number] {
  return [event.word0, event.word1 ?? 0];
}

describe('Project MIDI 2.0 builders', () => {
  it('packs note-on with velocity and attribute', () => {
    const e = midi2NoteOn({
      ppq: 1,
      group: G,
      channel: CH,
      note: 0x3c,
      velocity16: 0xc000,
      attributeType: 3,
      attributeData: 0x1234,
    });
    expect(e.ppq).toBe(1);
    expect(words(e)).toEqual([0x41923c03, 0xc0001234]);
  });

  it('packs note-off with the 16-bit release velocity', () => {
    expect(
      words(midi2NoteOff({ ppq: 0, group: G, channel: CH, note: 0x3c, velocity16: 0x8000 })),
    ).toEqual([0x41823c00, 0x80000000]);
    expect(words(midi2NoteOff({ ppq: 0, group: G, channel: CH, note: 0x3c }))).toEqual([
      0x41823c00, 0,
    ]);
  });

  it('packs the 32-bit channel-voice values', () => {
    expect(
      words(midi2Cc({ ppq: 0, group: G, channel: CH, controller: 7, value32: 0xdeadbeef })),
    ).toEqual([0x41b20700, 0xdeadbeef]);
    expect(
      words(
        midi2PolyPressure({ ppq: 0, group: G, channel: CH, note: 0x40, pressure32: 0x12345678 }),
      ),
    ).toEqual([0x41a24000, 0x12345678]);
    expect(
      words(midi2ChannelPressure({ ppq: 0, group: G, channel: CH, pressure32: 0xffffffff })),
    ).toEqual([0x41d20000, 0xffffffff]);
    expect(words(midi2PitchBend({ ppq: 0, group: G, channel: CH, bend32: 0x80000000 }))).toEqual([
      0x41e20000, 0x80000000,
    ]);
  });

  it('packs the maximum group and channel', () => {
    expect(words(midi2Cc({ ppq: 0, group: 15, channel: 15, controller: 127, value32: 1 }))).toEqual(
      [0x4fbf7f00, 1],
    );
  });

  it('packs program change with and without a bank', () => {
    expect(
      words(
        midi2Program({ ppq: 0, group: G, channel: CH, program: 5, bank: { msb: 0x01, lsb: 0x02 } }),
      ),
    ).toEqual([0x41c20001, 0x05000102]);
    expect(words(midi2Program({ ppq: 0, group: G, channel: CH, program: 5 }))).toEqual([
      0x41c20000, 0x05000000,
    ]);
  });

  it('packs registered and assignable controllers by bank and index', () => {
    expect(
      words(
        midi2RegisteredController({
          ppq: 0,
          group: G,
          channel: CH,
          bank: 0x10,
          index: 0x20,
          value32: 0x11223344,
        }),
      ),
    ).toEqual([0x41221020, 0x11223344]);
    expect(
      words(
        midi2AssignableController({
          ppq: 0,
          group: G,
          channel: CH,
          bank: 0x10,
          index: 0x20,
          value32: 0x11223344,
        }),
      ),
    ).toEqual([0x41321020, 0x11223344]);
  });

  it('packs relative controllers with a two-complement delta', () => {
    expect(
      words(
        midi2RelativeRegisteredController({
          ppq: 0,
          group: G,
          channel: CH,
          bank: 0x10,
          index: 0x20,
          delta32: -1,
        }),
      ),
    ).toEqual([0x41421020, 0xffffffff]);
    expect(
      words(
        midi2RelativeAssignableController({
          ppq: 0,
          group: G,
          channel: CH,
          bank: 0x10,
          index: 0x20,
          delta32: 0x7fffffff,
        }),
      ),
    ).toEqual([0x41521020, 0x7fffffff]);
    expect(
      words(
        midi2RelativeAssignableController({
          ppq: 0,
          group: G,
          channel: CH,
          bank: 0x10,
          index: 0x20,
          delta32: -0x80000000,
        }),
      ),
    ).toEqual([0x41521020, 0x80000000]);
  });

  it('packs per-note controllers with a full 8-bit index', () => {
    expect(
      words(
        midi2RegisteredPerNoteController({
          ppq: 0,
          group: G,
          channel: CH,
          note: 0x3c,
          index: 0xff,
          value32: 9,
        }),
      ),
    ).toEqual([0x41023cff, 9]);
    expect(
      words(
        midi2AssignablePerNoteController({
          ppq: 0,
          group: G,
          channel: CH,
          note: 0x3c,
          index: 0xff,
          value32: 9,
        }),
      ),
    ).toEqual([0x41123cff, 9]);
  });

  it('packs per-note pitch bend and management flags', () => {
    expect(
      words(
        midi2PerNotePitchBend({ ppq: 0, group: G, channel: CH, note: 0x3c, bend32: 0x80000000 }),
      ),
    ).toEqual([0x41623c00, 0x80000000]);
    expect(words(midi2PerNoteManagement({ ppq: 0, group: G, channel: CH, note: 0x3c }))).toEqual([
      0x41f23c00, 0,
    ]);
    expect(
      words(midi2PerNoteManagement({ ppq: 0, group: G, channel: CH, note: 0x3c, detach: true })),
    ).toEqual([0x41f23c02, 0]);
    expect(
      words(midi2PerNoteManagement({ ppq: 0, group: G, channel: CH, note: 0x3c, reset: true })),
    ).toEqual([0x41f23c01, 0]);
    expect(
      words(
        midi2PerNoteManagement({
          ppq: 0,
          group: G,
          channel: CH,
          note: 0x3c,
          detach: true,
          reset: true,
        }),
      ),
    ).toEqual([0x41f23c03, 0]);
  });

  it('refuses out-of-range fields', () => {
    expect(() => midi2NoteOn({ ppq: 0, group: 16, channel: CH, note: 60, velocity16: 0 })).toThrow(
      RangeError,
    );
    expect(() => midi2NoteOn({ ppq: 0, group: G, channel: 16, note: 60, velocity16: 0 })).toThrow(
      RangeError,
    );
    expect(() => midi2NoteOn({ ppq: 0, group: G, channel: CH, note: 128, velocity16: 0 })).toThrow(
      RangeError,
    );
    expect(() =>
      midi2NoteOn({ ppq: 0, group: G, channel: CH, note: 60, velocity16: 0x10000 }),
    ).toThrow(RangeError);
    expect(() => midi2NoteOn({ ppq: -1, group: G, channel: CH, note: 60, velocity16: 0 })).toThrow(
      RangeError,
    );
    expect(() =>
      midi2Cc({ ppq: 0, group: G, channel: CH, controller: 7, value32: 2 ** 32 }),
    ).toThrow(RangeError);
    expect(() => midi2Cc({ ppq: 0, group: G, channel: CH, controller: 7, value32: -1 })).toThrow(
      RangeError,
    );
    expect(() => midi2Cc({ ppq: 0, group: G, channel: CH, controller: 7, value32: 0.5 })).toThrow(
      RangeError,
    );
    expect(() =>
      midi2RegisteredController({ ppq: 0, group: G, channel: CH, bank: 128, index: 0, value32: 0 }),
    ).toThrow(RangeError);
    expect(() =>
      midi2RelativeRegisteredController({
        ppq: 0,
        group: G,
        channel: CH,
        bank: 0,
        index: 0,
        delta32: 2 ** 31,
      }),
    ).toThrow(RangeError);
    expect(() =>
      midi2RegisteredPerNoteController({
        ppq: 0,
        group: G,
        channel: CH,
        note: 60,
        index: 256,
        value32: 0,
      }),
    ).toThrow(RangeError);
    expect(() => midi2Program({ ppq: 0, group: G, channel: CH, program: 128 })).toThrow(RangeError);
  });

  it('produces events the project accepts', () => {
    const project = new Project();
    try {
      const { clipId } = project.addMidiClip(0, 4);
      expect(() =>
        project.setMidiEvents(clipId, [
          midi2NoteOn({ ppq: 0, group: 0, channel: 0, note: 60, velocity16: 0xc000 }),
          midi2NoteOff({ ppq: 1, group: 0, channel: 0, note: 60, velocity16: 0 }),
        ]),
      ).not.toThrow();
    } finally {
      project.destroy();
    }
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
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      body(engine);
    } finally {
      engine.destroy();
    }
  }

  it('pushMidiUmp accepts one- and two-word messages as typed or plain arrays', () => {
    withEngine((engine) => {
      expect(() => engine.pushMidiUmp(0, new Uint32Array(NOTE_ON_2W))).not.toThrow();
      expect(() => engine.pushMidiUmp(0, NOTE_ON_2W)).not.toThrow();
      expect(() => engine.pushMidiUmp(0, [0x00000000])).not.toThrow();
      expect(() => engine.pushMidiUmp(0, [0x20903c64])).not.toThrow();
      // Bit 31 written as the signed spelling of a shift expression.
      expect(() => engine.pushMidiUmp(0, [(0x4 << 28) | 0x00930000, 0])).not.toThrow();
    });
  });

  it('pushMidiUmp refuses a bare number instead of reading it as a one-word message', () => {
    withEngine((engine) => {
      expect(() => engine.pushMidiUmp(0, 0x20903c64 as unknown as number[])).toThrow(TypeError);
    });
  });

  it('pushMidiUmp refuses data messages and mismatched word counts', () => {
    withEngine((engine) => {
      const shape = new RangeError('pushMidiUmp: words do not form a pushable UMP message');
      expect(() => engine.pushMidiUmp(0, [0x30160000, 0])).toThrow(shape);
      expect(() => engine.pushMidiUmp(0, [0x50000000, 0, 0, 0])).toThrow(shape);
      expect(() => engine.pushMidiUmp(0, [0x41923c00])).toThrow(shape);
      expect(() => engine.pushMidiUmp(0, [0x41923c00, 0, 0])).toThrow(shape);
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
      expect(() => engine.pushMidiUmp(0, new Float32Array(2) as unknown as number[])).toThrow(
        TypeError,
      );
    });
  });

  it('pushMidiInputUmp needs an enabled input source and mirrors the destination rules', () => {
    withEngine((engine) => {
      expect(errorCode(() => engine.pushMidiInputUmp(NOTE_ON_2W))).toBe(ErrorCode.InvalidState);
      engine.setMidiInputSource(0);
      expect(() => engine.pushMidiInputUmp(new Uint32Array(NOTE_ON_2W))).not.toThrow();
      expect(() => engine.pushMidiInputUmp(NOTE_ON_2W, 128)).not.toThrow();
      const shape = new RangeError('pushMidiInputUmp: words do not form a pushable UMP message');
      expect(() => engine.pushMidiInputUmp([0x30160000, 0])).toThrow(shape);
      expect(() => engine.pushMidiInputUmp([0x50000000, 0, 0, 0])).toThrow(shape);
      expect(() => engine.pushMidiInputUmp([0x41923c00])).toThrow(shape);
      expect(() => engine.pushMidiInputUmp([])).toThrow(RangeError);
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
