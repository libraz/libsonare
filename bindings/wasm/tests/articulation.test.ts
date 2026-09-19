/**
 * Articulation WASM binding tests: the per-channel round trip, the refusals it
 * has to keep apart, and that a mode set through the binding reaches the sound.
 *
 * The last one is the point. A slurred pair of notes and a retriggered pair are
 * the same two pitches, so every call here can return normally with the mode
 * dropped on the floor and a round-trip test would still pass. One case renders
 * both and requires them to differ; another takes the engine that declines to
 * be carried and requires the fallback counter to move, which is the only thing
 * separating a refusal from a mode that was never set.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ARTICULATIONS,
  type Articulation,
  init,
  RealtimeEngine,
  type SynthEngineMode,
  synthEnumTables,
} from '../dist/index.js';
import { setSonareModule } from '../src/module_state.js';

const BLOCK = 128;

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine(48000, BLOCK);
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

function renderBlocks(engine: RealtimeEngine, blocks: number): Float32Array {
  const out = new Float32Array(blocks * BLOCK);
  for (let block = 0; block < blocks; block++) {
    const rendered = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
    out.set(rendered[0], block * BLOCK);
  }
  return out;
}

/**
 * C4 held, then G4 on top of it, then the LATE note-off of C4 — the order a
 * player's slur actually sends, and the one a mode that ignores the overlap
 * cannot be told apart from by the first half alone.
 */
function renderSlur(
  engineMode: SynthEngineMode,
  articulation: Articulation,
): { audio: Float32Array; fallbacks: number } {
  return withEngine((engine) => {
    engine.setSynthInstrument({ engineMode });
    engine.setArticulation(0, 0, articulation);
    engine.pushMidiNoteOn(0, 0, 0, 60, 100);
    const head = renderBlocks(engine, 96);
    engine.pushMidiNoteOn(0, 0, 0, 67, 100);
    engine.pushMidiNoteOff(0, 0, 0, 60, 0);
    const tail = renderBlocks(engine, 128);
    const audio = new Float32Array(head.length + tail.length);
    audio.set(head, 0);
    audio.set(tail, head.length);
    return { audio, fallbacks: engine.legatoFallbackCount(0) };
  });
}

function peak(samples: Float32Array): number {
  let highest = 0;
  for (const sample of samples) {
    if (!Number.isFinite(sample)) {
      return Number.NaN;
    }
    highest = Math.max(highest, Math.abs(sample));
  }
  return highest;
}

describe('Sonare WASM articulation', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  it('exposes the articulation enum table', () => {
    const tables = synthEnumTables();
    expect(tables.articulations).toEqual([...ARTICULATIONS]);
    expect(tables.articulations).toEqual(['poly', 'mono-retrigger', 'mono-legato']);
  });

  it('round-trips per channel', () => {
    withEngine((engine) => {
      engine.setSynthInstrument({ engineMode: 'reed' });
      expect(engine.articulation(0, 0)).toBe('poly');

      engine.setArticulation(0, 0, 'mono-legato');
      expect(engine.articulation(0, 0)).toBe('mono-legato');

      // A second channel is untouched by the first, so the mode is per channel
      // rather than per instrument — a host slurring one part must not slur the
      // rest of the rack.
      expect(engine.articulation(0, 1)).toBe('poly');
      engine.setArticulation(0, 1, 'mono-retrigger');
      expect(engine.articulation(0, 1)).toBe('mono-retrigger');
      expect(engine.articulation(0, 0)).toBe('mono-legato');

      // The C ordinal is accepted wherever the name is, the way every other
      // enum on this surface reads.
      engine.setArticulation(0, 2, 2);
      expect(engine.articulation(0, 2)).toBe('mono-legato');

      // Nothing has been asked for and declined yet, so the counter starts
      // where a later case can see it move.
      expect(engine.legatoFallbackCount(0)).toBe(0);
    });
  });

  it('refuses a mode rather than clamping or substituting it', () => {
    withEngine((engine) => {
      engine.setSynthInstrument({ engineMode: 'reed' });
      // An ordinal past the enum is refused rather than clamped: poly
      // substituted for a misspelled mono-legato plays every note and slurs
      // none of them, which the caller cannot tell from a request that took.
      expect(() => engine.setArticulation(0, 0, ARTICULATIONS.length)).toThrow(/out of range/);
      expect(() => engine.setArticulation(0, 0, -1)).toThrow(/out of range/);
      // @ts-expect-error an unknown articulation name is rejected at runtime
      expect(() => engine.setArticulation(0, 0, 'legato')).toThrow(/legato/);
      // Checked before the byte it narrows into: 256 is channel 0 once
      // narrowed, and would slur a part the caller never named.
      expect(() => engine.setArticulation(0, 16, 'poly')).toThrow(/out of range/);
      expect(() => engine.setArticulation(0, 256, 'poly')).toThrow(/out of range/);
      expect(() => engine.articulation(0, 16)).toThrow(/out of range/);
      // None of the above took: the mode is still what the instrument started at.
      expect(engine.articulation(0, 0)).toBe('poly');
    });
  });

  it('keeps an unbound destination apart from an instrument without articulation', () => {
    // All three entries throw for both, so "it threw" cannot tell them apart;
    // the message is what a host has to read, and each entry has to name the
    // same cause as its two siblings.
    const entries: Array<[string, (engine: RealtimeEngine) => unknown]> = [
      ['setArticulation', (engine) => engine.setArticulation(5, 0, 'mono-legato')],
      ['articulation', (engine) => engine.articulation(5, 0)],
      ['legatoFallbackCount', (engine) => engine.legatoFallbackCount(5)],
    ];
    withEngine((engine) => {
      engine.setSynthInstrument({ engineMode: 'reed' });
      // Nothing is bound to destination 5 — an error, not a default mode.
      for (const [name, call] of entries) {
        expect(() => call(engine), name).toThrow(/no MIDI instrument is bound/);
      }

      // The built-in oscillator synth holds no articulation of its own, and
      // that is a different answer. The counter is included rather than
      // reporting 0, because a 0 from an instrument that was never asked reads
      // as "every slur took" — the reading it exists to prevent.
      engine.setBuiltinInstrument({}, 5);
      for (const [name, call] of entries) {
        expect(() => call(engine), name).toThrow(/no articulation of its own/);
      }
    });
  });

  it('renders a slur differently from a retrigger', () => {
    const slurred = renderSlur('reed', 'mono-legato');
    const retriggered = renderSlur('reed', 'mono-retrigger');

    // The renders carry energy, so "they differ" is not two kinds of silence.
    expect(peak(retriggered.audio)).toBeGreaterThan(0);
    expect(peak(slurred.audio)).toBeGreaterThan(0);
    expect(slurred.audio.length).toBe(retriggered.audio.length);
    let maxDelta = 0;
    for (let i = 0; i < slurred.audio.length; i++) {
      maxDelta = Math.max(maxDelta, Math.abs(slurred.audio[i] - retriggered.audio[i]));
    }
    expect(maxDelta).toBeGreaterThan(1e-4);
    // A reed accepts the carry, so nothing was refused on either side.
    expect(slurred.fallbacks).toBe(0);
    expect(retriggered.fallbacks).toBe(0);

    // And the same call twice is bit-identical, so the difference above is the
    // articulation rather than anything free-running in the render.
    expect(renderSlur('reed', 'mono-retrigger').audio).toEqual(retriggered.audio);
  });

  it('counts an engine that declines to be carried', () => {
    // A struck string cannot be slurred: its exciter is spent before the second
    // sample. The note still sounds, so the counter is the only evidence.
    const declined = renderSlur('piano', 'mono-legato');
    expect(declined.fallbacks).toBeGreaterThanOrEqual(1);
    expect(peak(declined.audio)).toBeGreaterThan(0);

    // The same phrase under the default mode refuses nothing, so the count
    // above is the request being declined rather than a counter that only ever
    // rises.
    expect(renderSlur('piano', 'poly').fallbacks).toBe(0);
  });
});
