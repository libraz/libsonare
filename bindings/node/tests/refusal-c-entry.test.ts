import { describe, expect, it } from 'vitest';
import { midiVelocityToMidi2, normalize } from '../src/index.js';
import { respellRefusal } from '../src/internal/respell.js';

const REFUSAL = Symbol.for('libsonare.refusal');

function cEntryOf(run: () => unknown): unknown {
  try {
    run();
  } catch (error) {
    return ((error as Record<symbol, unknown>)[REFUSAL] as { cEntry?: unknown } | undefined)
      ?.cEntry;
  }
  throw new Error('expected a refusal');
}

describe('a refusal names the C-ABI entry that refused it', () => {
  it('carries the entry key of a C-ABI call', () => {
    expect(cEntryOf(() => midiVelocityToMidi2(128))).toBe('midi_velocity_to_midi2');
  });

  it('keeps the entry when the facade re-renders the refusal under the caller name', () => {
    let error: unknown;
    try {
      midiVelocityToMidi2(128);
    } catch (caught) {
      error = caught;
    }
    const rendered = respellRefusal(error, { velocity: { name: 'speed', scale: 1 } }) as Record<
      symbol,
      { subjects: string[]; cEntry?: unknown }
    >;
    expect(rendered[REFUSAL].subjects).toEqual(['speed']);
    expect(rendered[REFUSAL].cEntry).toBe('midi_velocity_to_midi2');
  });

  it('is null when a direct core call refused', () => {
    expect(cEntryOf(() => normalize(new Float32Array(64), 48000))).toBeNull();
  });
});
