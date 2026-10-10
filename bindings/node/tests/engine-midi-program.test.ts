import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, Project, RealtimeEngine } from '../src/index.js';

const DEST = 7;
const BLOCK = 128;
const BLOCKS = 24;

function errorCode(call: () => void): ErrorCode | undefined {
  try {
    call();
  } catch (error) {
    return isSonareError(error) ? error.code : undefined;
  }
  return undefined;
}

/** Words of a MIDI 2.0 Program Change, as the builder packs them. */
function programWords(
  group: number,
  channel: number,
  program: number,
  bankValid: boolean,
  bankMsb: number,
  bankLsb: number,
): [number, number] {
  const e = Project.midi2Program(0, group, channel, program, bankValid, bankMsb, bankLsb);
  return [e.data0, e.data1 ?? 0];
}

/** Render one note on the no-SoundFont Sf2Player floor after `prepare` ran. */
function renderNote(prepare: (engine: RealtimeEngine) => void, input = false): Float32Array {
  const engine = new RealtimeEngine(48000, BLOCK);
  try {
    engine.setSf2Instrument({}, DEST);
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
    const viaOp = renderNote((e) => e.pushMidiProgram(DEST, 0, 0, 0, true, 8, 0));
    const viaRaw = renderNote((e) => e.pushMidiUmp(DEST, programWords(0, 0, 0, true, 8, 0)));
    const bank0 = renderNote((e) => e.pushMidiProgram(DEST, 0, 0, 0, true, 0, 0));
    const none = renderNote(() => {});
    expect(peak(viaOp)).toBeGreaterThan(0);
    expect(equal(viaOp, viaRaw)).toBe(true);
    expect(equal(viaOp, bank0)).toBe(false);
    expect(equal(viaOp, none)).toBe(false);
  });

  it('input source variant matches its raw UMP push and differs from bank 0', () => {
    const viaOp = renderNote((e) => e.pushMidiInputProgram(0, 0, 0, true, 8, 0), true);
    const viaRaw = renderNote((e) => e.pushMidiInputUmp(programWords(0, 0, 0, true, 8, 0)), true);
    const bank0 = renderNote((e) => e.pushMidiInputProgram(0, 0, 0, true, 0, 0), true);
    expect(peak(viaOp)).toBeGreaterThan(0);
    expect(equal(viaOp, viaRaw)).toBe(true);
    expect(equal(viaOp, bank0)).toBe(false);
  });

  it('refuses a bank with bank-valid false on the pushes and the builder', () => {
    const engine = new RealtimeEngine(48000, BLOCK);
    try {
      // The pushes refuse through the C ABI, so the error is a coded SonareError.
      for (const call of [
        () => engine.pushMidiProgram(DEST, 0, 0, 5, false, 8, 0),
        () => engine.pushMidiProgram(DEST, 0, 0, 5, false, 0, 1),
        () => engine.pushMidiInputProgram(0, 0, 5, false, 8, 0),
      ]) {
        expect(errorCode(call)).toBe(ErrorCode.InvalidParameter);
      }
    } finally {
      engine.destroy();
    }
    expect(() => Project.midi2Program(0, 0, 0, 5, false, 8, 0)).toThrow(
      'Project.midi2Program: bank MSB and LSB must be 0 when bank-valid is false',
    );
    expect(() => Project.midi2Program(0, 0, 0, 5, false, 0, 1)).toThrow(RangeError);
    expect(() => Project.midi2Program(0, 0, 0, 5, true, 8, 1)).not.toThrow();
    expect(() => Project.midi2Program(0, 0, 0, 5)).not.toThrow();
  });

  it('refuses out-of-range and mistyped arguments like the sibling pushes', () => {
    const engine = new RealtimeEngine(48000, BLOCK);
    try {
      expect(errorCode(() => engine.pushMidiProgram(DEST, 16, 0, 0))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(errorCode(() => engine.pushMidiProgram(DEST, 0, 16, 0))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(() => engine.pushMidiProgram(DEST, 300, 0, 0)).toThrow(/group/);
      expect(() => engine.pushMidiProgram(DEST, 0, 300, 0)).toThrow(/channel/);
      expect(errorCode(() => engine.pushMidiProgram(DEST, 0, 0, 128))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(errorCode(() => engine.pushMidiProgram(DEST, 0, 0, 0, true, 128, 0))).toBe(
        ErrorCode.InvalidParameter,
      );
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 300)).toThrow(/program/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, true, 300, 0)).toThrow(/bankMsb/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, true, 0, 300)).toThrow(/bankLsb/);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, 'yes' as unknown as boolean)).toThrow(
        /bankValid/,
      );
      expect(() => engine.pushMidiProgram('x' as unknown as number, 0, 0, 0)).toThrow(TypeError);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 0, false, 0, 0, -1)).toThrow(RangeError);
      expect(() => engine.pushMidiInputProgram(300, 0, 0)).toThrow(/group/);
      expect(() => engine.pushMidiInputProgram(0, 0, 300)).toThrow(/program/);
      expect(() => engine.pushMidiInputProgram(0, 0, 0, true, 0, 300)).toThrow(/bankLsb/);
    } finally {
      engine.destroy();
    }
  });
});
