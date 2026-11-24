/**
 * A Program Change with no bank leaves `bank` out; `null` is not a spelling of
 * "no bank" and is refused by name.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../dist/index.js';

const DEST = 7;
const TEXT = 'bank must be a { msb, lsb } object';

describe('Program Change bank absence', () => {
  it('refuses a null bank and accepts an omitted one', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.setMidiInputSource(DEST);
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 5, null as never)).toThrow(
        new TypeError(`RealtimeEngine.pushMidiProgram: ${TEXT}`),
      );
      expect(() => engine.pushMidiInputProgram(0, 0, 5, null as never)).toThrow(
        new TypeError(`RealtimeEngine.pushMidiInputProgram: ${TEXT}`),
      );
      expect(() =>
        engine.pushMidiInputProgram(0, 0, 5, { msb: 0, lsb: 0, extra: 1 } as never),
      ).toThrow(
        new TypeError(
          'RealtimeEngine.pushMidiInputProgram: unknown key bank.extra (expected lsb, msb)',
        ),
      );
      expect(() =>
        engine.pushMidiProgram(DEST, 0, 0, 5, { msb: 0, lsb: 0, extra: 1 } as never),
      ).toThrow(
        new TypeError('RealtimeEngine.pushMidiProgram: unknown key bank.extra (expected lsb, msb)'),
      );
      expect(() => engine.pushMidiProgram(DEST, 0, 0, 5, undefined, 0)).not.toThrow();
      expect(() => engine.pushMidiInputProgram(0, 0, 5)).not.toThrow();
    } finally {
      engine.destroy();
    }
  });
});
