import { describe, expect, it } from 'vitest';
import { midi2Program, RealtimeEngine } from '../src/index.js';

const DEST = 7;
const BLOCK = 128;
const BLOCKS = 24;

/** Words of a MIDI 2.0 Program Change, as the builder packs them. */
function programWords(
  group: number,
  channel: number,
  program: number,
  bankValid: boolean,
  bankMsb: number,
  bankLsb: number,
): [number, number] {
  const e = midi2Program({
    ppq: 0,
    group,
    channel,
    program,
    ...(bankValid ? { bank: { msb: bankMsb, lsb: bankLsb } } : {}),
  });
  return [e.word0, e.word1 ?? 0];
}

/** Render one note on the no-SoundFont Sf2Player floor after `prepare` ran. */
function renderNote(prepare: (engine: RealtimeEngine) => void, input = false): Float32Array {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
  try {
    engine.setSf2Instrument(DEST, {});
    if (input) {
      engine.setMidiInputSource(DEST);
    }
    prepare(engine);
    if (input) {
      engine.pushMidiInputNoteOn(0, 0, 60, 100);
    } else {
      engine.pushMidiNoteOn(DEST, 0, 0, 60, 100);
    }
    const out = new Float32Array(BLOCK * BLOCKS);
    for (let block = 0; block < BLOCKS; block++) {
      const [left] = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      out.set(left, block * BLOCK);
    }
    return out;
  } finally {
    engine.destroy();
  }
}

function equal(a: Float32Array, b: Float32Array): boolean {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

function peak(a: Float32Array): number {
  return a.reduce((p, v) => Math.max(p, Math.abs(v)), 0);
}

describe('RealtimeEngine live program change', () => {
  it('renders bank 8 program 0 as the raw Program Change words do, apart from bank 0 and none', () => {
    const viaOp = renderNote((e) => e.pushMidiProgram(DEST, 0, 0, 0, { msb: 8, lsb: 0 }));
    const viaRaw = renderNote((e) => e.pushMidiUmp(DEST, programWords(0, 0, 0, true, 8, 0)));
    const bank0 = renderNote((e) => e.pushMidiProgram(DEST, 0, 0, 0, { msb: 0, lsb: 0 }));
    const none = renderNote(() => {});
    expect(peak(viaOp)).toBeGreaterThan(0);
    expect(equal(viaOp, viaRaw)).toBe(true);
    expect(equal(viaOp, bank0)).toBe(false);
    expect(equal(viaOp, none)).toBe(false);
  });

  it('input source variant matches its raw UMP push and differs from bank 0', () => {
    const viaOp = renderNote((e) => e.pushMidiInputProgram(0, 0, 0, { msb: 8, lsb: 0 }), true);
    const viaRaw = renderNote((e) => e.pushMidiInputUmp(programWords(0, 0, 0, true, 8, 0)), true);
    const bank0 = renderNote((e) => e.pushMidiInputProgram(0, 0, 0, { msb: 0, lsb: 0 }), true);
    expect(peak(viaOp)).toBeGreaterThan(0);
    expect(equal(viaOp, viaRaw)).toBe(true);
    expect(equal(viaOp, bank0)).toBe(false);
  });

  it('refuses a malformed bank on the pushes', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 5, { msb: 8 } as never)).toThrow(TypeError);
      expect(() =>
        engine.pushMidiProgram(DEST, 0, 0, 5, { msb: 8, lsb: 0, extra: 1 } as never),
      ).toThrow(/unknown key extra \(expected /);
      expect(() => engine.pushMidiInputProgram(0, 0, 5, 3 as never)).toThrow(TypeError);
    } finally {
      engine.destroy();
    }
    expect(() =>
      midi2Program({ ppq: 0, group: 0, channel: 0, program: 5, bank: { msb: 128, lsb: 0 } }),
    ).toThrow(new RangeError('midi2Program: bank.msb must be in [0, 127]'));
    expect(() =>
      midi2Program({ ppq: 0, group: 0, channel: 0, program: 5, bank: { msb: 0, lsb: 1.5 } }),
    ).toThrow(TypeError);
    expect(() =>
      midi2Program({ ppq: 0, group: 0, channel: 0, program: 5, bank: { msb: 8, lsb: 1 } }),
    ).not.toThrow();
    expect(() => midi2Program({ ppq: 0, group: 0, channel: 0, program: 5 })).not.toThrow();
  });

  it('refuses out-of-range and mistyped arguments like the sibling pushes', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
    try {
      expect(() => engine.pushMidiProgram(DEST, 16, 0, 0)).toThrow(
        new RangeError('RealtimeEngine.pushMidiProgram: group must be in [0, 15]'),
      );
      expect(() => engine.pushMidiProgram(DEST, 0, 16, 0)).toThrow(
        new RangeError('RealtimeEngine.pushMidiProgram: channel must be in [0, 15]'),
      );
      expect(() => engine.pushMidiProgram(DEST, 300, 0, 0)).toThrow(/group/);
      expect(() => engine.pushMidiProgram(DEST, 0, 300, 0)).toThrow(/channel/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 128)).toThrow(
        new RangeError('RealtimeEngine.pushMidiProgram: program must be in [0, 127]'),
      );
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 128, lsb: 0 })).toThrow(
        new RangeError('RealtimeEngine.pushMidiProgram: bank_msb must be in [0, 127]'),
      );
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 300)).toThrow(/program/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 300, lsb: 0 })).toThrow(/bankMsb/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, { msb: 0, lsb: 300 })).toThrow(/bankLsb/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, 'yes' as never)).toThrow(TypeError);
      expect(() => engine.pushMidiProgram('x' as unknown as number, 0, 0, 0)).toThrow(TypeError);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, null, -1)).toThrow(RangeError);
      expect(() => engine.pushMidiInputProgram(300, 0, 0)).toThrow(/group/);
      expect(() => engine.pushMidiInputProgram(0, 0, 300)).toThrow(/program/);
      expect(() => engine.pushMidiInputProgram(0, 0, 0, { msb: 0, lsb: 300 })).toThrow(/bankLsb/);
    } finally {
      engine.destroy();
    }
  });
});
