/**
 * Pins the articulation facade: how a mode is spelled, the refusals it has to
 * keep apart, and that a mode set through JS reaches the sound.
 *
 * The last one is the point. A slurred pair of notes and a retriggered pair are
 * the same two pitches, so every call here can return normally with the mode
 * dropped on the floor and a round-trip would still agree with itself. One case
 * renders both and requires them to differ; another takes the engine that
 * declines to be carried and requires the fallback counter to move, which is
 * the only thing separating a refusal from a mode that was never set.
 */

import { describe, expect, it } from 'vitest';
import { ARTICULATIONS, RealtimeEngine, synthEnumTables } from '../src/index.js';
import type { Articulation, SynthEngineMode } from '../src/types.js';

/** An engine with one NativeSynth engine mode bound to destination 3. */
const synthEngine = (engineMode: SynthEngineMode): RealtimeEngine => {
  const engine = new RealtimeEngine(48000, 128);
  engine.setSynthInstrument({ engineMode }, 3);
  return engine;
};

/**
 * The `codeName` of the SonareError a call throws. The message can be a
 * thread-local detail string, so the code is what tells two refusals apart.
 */
const codeNameOf = (call: () => unknown): string => {
  try {
    call();
  } catch (error) {
    return (error as { codeName?: string }).codeName ?? 'not a SonareError';
  }
  return 'did not throw';
};

/**
 * C4 held, then G4 on top of it, then the LATE note-off of C4 — the order a
 * player's slur actually sends, and the one a mode that ignores the overlap
 * cannot be told apart from by the first half alone.
 */
const renderSlur = (
  engineMode: SynthEngineMode,
  articulation: Articulation,
): { audio: Float32Array; fallbacks: number } => {
  const engine = synthEngine(engineMode);
  engine.setArticulation(3, 0, articulation);
  engine.pushMidiNoteOn(3, 0, 0, 60, 100);
  const samples: number[] = [];
  const renderBlocks = (blocks: number): void => {
    for (let block = 0; block < blocks; block++) {
      samples.push(...engine.process([new Float32Array(128), new Float32Array(128)])[0]);
    }
  };
  renderBlocks(96);
  engine.pushMidiNoteOn(3, 0, 0, 67, 100);
  engine.pushMidiNoteOff(3, 0, 0, 60, 0);
  renderBlocks(128);
  const fallbacks = engine.legatoFallbackCount(3);
  engine.destroy();
  return { audio: Float32Array.from(samples), fallbacks };
};

const peak = (data: Float32Array): number =>
  data.reduce((worst, value) => Math.max(worst, Math.abs(value)), 0);

describe('articulation catalog', () => {
  it('spells the modes the C name table does', () => {
    // The facade constant is pinned against the C table by the enum-table shape
    // check in synth-patch.test.ts; what this adds is the count, so a table
    // truncated on its way through the split reads as a failure rather than as
    // a shorter enum.
    expect(synthEnumTables().articulations).toHaveLength(3);
    expect([...ARTICULATIONS]).toEqual(['poly', 'mono-retrigger', 'mono-legato']);
  });
});

describe('RealtimeEngine articulation', () => {
  it('round-trips per channel', () => {
    const engine = synthEngine('reed');
    expect(engine.articulation(3, 0)).toBe('poly');

    engine.setArticulation(3, 0, 'mono-legato');
    expect(engine.articulation(3, 0)).toBe('mono-legato');

    // A second channel is untouched by the first, so the mode is per channel
    // rather than per instrument — a host slurring one part must not slur the
    // rest of the rack.
    expect(engine.articulation(3, 1)).toBe('poly');
    engine.setArticulation(3, 1, 'mono-retrigger');
    expect(engine.articulation(3, 1)).toBe('mono-retrigger');
    expect(engine.articulation(3, 0)).toBe('mono-legato');

    // Nothing has been asked for and declined yet, so the counter starts where
    // a later case can see it move.
    expect(engine.legatoFallbackCount(3)).toBe(0);
    engine.destroy();
  });

  it('accepts the C ordinal as well as the name', () => {
    const engine = synthEngine('reed');
    engine.setArticulation(3, 0, 2);
    expect(engine.articulation(3, 0)).toBe('mono-legato');
    engine.destroy();
  });

  it('refuses an unknown mode rather than resolving it to poly', () => {
    const engine = synthEngine('reed');
    // poly substituted for a misspelled mono-legato plays every note and slurs
    // none of them, which the caller cannot tell from a request that took.
    expect(() => engine.setArticulation(3, 0, 'legato' as never)).toThrow(
      /Unknown articulation name/,
    );
    expect(() => engine.setArticulation(3, 0, 3)).toThrow(/ordinal in \[0, 3\)/);
    expect(() => engine.setArticulation(3, 0, -1)).toThrow(/ordinal in \[0, 3\)/);
    expect(() => engine.setArticulation(3, 0, undefined as never)).toThrow();
    // None of the above took.
    expect(engine.articulation(3, 0)).toBe('poly');
    engine.destroy();
  });

  it('refuses a channel past 15 rather than clamping it', () => {
    const engine = synthEngine('reed');
    expect(codeNameOf(() => engine.setArticulation(3, 16, 'poly'))).toBe('InvalidParameter');
    expect(codeNameOf(() => engine.articulation(3, 16))).toBe('InvalidParameter');
    engine.destroy();
  });

  it('keeps an unbound destination apart from an instrument with no articulation', () => {
    const engine = synthEngine('reed');
    // Nothing is bound to destination 5 yet. Both answers would otherwise read
    // as "the call worked" to a caller that only checks for a return, and both
    // read as one answer to a caller that only checks that something threw.
    expect(codeNameOf(() => engine.setArticulation(5, 0, 'mono-legato'))).toBe('InvalidParameter');
    expect(codeNameOf(() => engine.articulation(5, 0))).toBe('InvalidParameter');
    expect(codeNameOf(() => engine.legatoFallbackCount(5))).toBe('InvalidParameter');

    // The built-in oscillator synth has no articulation of its own, which is a
    // different answer from having no instrument at all.
    engine.setBuiltinInstrument({}, 5);
    expect(codeNameOf(() => engine.setArticulation(5, 0, 'mono-legato'))).toBe('NotSupported');
    expect(codeNameOf(() => engine.articulation(5, 0))).toBe('NotSupported');
    // The counter refuses on the same terms rather than answering zero. All
    // three are asked together by a host, and a zero here would read as "every
    // slur took" on an instrument that never had a slur to take.
    expect(codeNameOf(() => engine.legatoFallbackCount(5))).toBe('NotSupported');
    engine.destroy();
  });
});

describe('an articulation set through the Node facade reaches the sound', () => {
  it('renders a slur differently from a retrigger, against a bit-identical repeat', () => {
    const slurred = renderSlur('reed', 'mono-legato');
    const retriggered = renderSlur('reed', 'mono-retrigger');

    // The renders carry energy, so "they differ" is not two kinds of silence.
    expect(peak(retriggered.audio)).toBeGreaterThan(0);
    expect(slurred.audio.length).toBe(retriggered.audio.length);
    expect([...slurred.audio]).not.toEqual([...retriggered.audio]);
    // A reed accepts the carry, so nothing was refused on either side.
    expect(slurred.fallbacks).toBe(0);
    expect(retriggered.fallbacks).toBe(0);

    // And the same call twice is bit-identical, so the difference above is the
    // articulation rather than anything free-running in the render.
    expect([...renderSlur('reed', 'mono-retrigger').audio]).toEqual([...retriggered.audio]);
  });

  it('counts an engine that declines to be carried, and counts nothing under poly', () => {
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
