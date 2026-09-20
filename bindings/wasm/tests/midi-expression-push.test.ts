/**
 * The three per-note expression dimensions on the two live push families: the
 * engine-owned MIDI input source and the destination command path. Both are
 * measured rather than smoke-tested, because a facade that returned success and
 * dropped the call would leave the baseline render untouched and pass.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';
import { setSonareModule } from '../src/module_state.js';

const DESTINATION = 0;
const BLOCK = 128;
const BLOCKS = 128;
/** Full positive bend; the built-in synth spans +/-2 semitones, a ratio of 1.1225. */
const MAX_BEND = 16383;
const CENTRE_BEND = 8192;

type Family = 'input-source' | 'destination';

interface Expression {
  bend14?: number;
  channelPressure?: number;
  polyPressure?: number;
}

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine(48000, BLOCK);
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

/**
 * Holds a note, lets it settle, then sends the expression and renders. Sending
 * after the note is sounding is both how a live gesture arrives and what keeps
 * the measurement independent of how two events at one timestamp are ordered.
 */
function render(family: Family, expression: Expression): Float32Array {
  return withEngine((engine) => {
    engine.setBuiltinInstrument({}, DESTINATION);
    const live = family === 'input-source';
    if (live) {
      engine.setMidiInputSource(DESTINATION);
      engine.pushMidiInputNoteOn(0, 0, 60, 100);
    } else {
      engine.pushMidiNoteOn(DESTINATION, 0, 0, 60, 100);
    }
    for (let block = 0; block < 16; block++) {
      engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
    }

    const bend14 = expression.bend14 ?? CENTRE_BEND;
    const channelPressure = expression.channelPressure ?? 0;
    const polyPressure = expression.polyPressure ?? 0;
    if (live) {
      engine.pushMidiInputPitchBend(0, 0, bend14);
      engine.pushMidiInputChannelPressure(0, 0, channelPressure);
      engine.pushMidiInputPolyPressure(0, 0, 60, polyPressure);
    } else {
      engine.pushMidiPitchBend(DESTINATION, 0, 0, bend14);
      engine.pushMidiChannelPressure(DESTINATION, 0, 0, channelPressure);
      engine.pushMidiPolyPressure(DESTINATION, 0, 0, 60, polyPressure);
    }

    const out = new Float32Array(BLOCKS * BLOCK);
    for (let block = 0; block < BLOCKS; block++) {
      const rendered = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      out.set(rendered[0], block * BLOCK);
    }
    return out;
  });
}

/** Sign changes, which rise with the sounding pitch and with nothing else here. */
function zeroCrossings(buf: Float32Array): number {
  let count = 0;
  for (let i = 1; i < buf.length; i++) {
    if (buf[i - 1] < 0 !== buf[i] < 0) {
      count++;
    }
  }
  return count;
}

function peak(buf: Float32Array): number {
  let max = 0;
  for (let i = 0; i < buf.length; i++) {
    max = Math.max(max, Math.abs(buf[i]));
  }
  return max;
}

describe('Sonare WASM live per-note expression', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  for (const family of ['input-source', 'destination'] as const) {
    it(`carries bend and both pressures through the ${family} path`, () => {
      const plain = render(family, {});
      const bent = render(family, { bend14: MAX_BEND });
      const pressed = render(family, { channelPressure: 127 });
      const keyed = render(family, { polyPressure: 127 });

      // The note sounds without any of the three, so no check below can pass on
      // a silent render.
      const plainPeak = peak(plain);
      expect(plainPeak).toBeGreaterThan(0.01);

      // A whole tone up is a frequency ratio of 1.1225, bracketed so that "no
      // change at all" fails on the lower bound.
      const ratio = zeroCrossings(bent) / zeroCrossings(plain);
      expect(ratio).toBeGreaterThan(1.08);
      expect(ratio).toBeLessThan(1.17);

      // Full pressure doubles the level on this synth.
      expect(peak(pressed)).toBeGreaterThan(plainPeak * 1.5);
      expect(peak(keyed)).toBeGreaterThan(plainPeak * 1.5);
    });
  }

  it('refuses a value outside its dimension rather than narrowing it', () => {
    withEngine((engine) => {
      engine.setBuiltinInstrument({}, DESTINATION);
      engine.setMidiInputSource(DESTINATION);
      // A bend is 14-bit, so 16384 is the first value a 7-bit-shaped check would
      // let through and the one that has to be refused.
      expect(() => engine.pushMidiInputPitchBend(0, 0, 16384)).toThrow();
      expect(() => engine.pushMidiPitchBend(DESTINATION, 0, 0, 16384)).toThrow();
      expect(() => engine.pushMidiInputChannelPressure(0, 16, 64)).toThrow();
      expect(() => engine.pushMidiPolyPressure(DESTINATION, 0, 0, 128, 64)).toThrow();
    });
  });

  it('refuses an input-source push before the source is enabled', () => {
    withEngine((engine) => {
      engine.setBuiltinInstrument({}, DESTINATION);
      expect(() => engine.pushMidiInputPitchBend(0, 0, CENTRE_BEND)).toThrow();
      expect(() => engine.pushMidiInputChannelPressure(0, 0, 64)).toThrow();
      expect(() => engine.pushMidiInputPolyPressure(0, 0, 60, 64)).toThrow();
    });
  });
});
