/**
 * Controller-profile WASM binding tests: controllerProfileNames, the two
 * controller enum tables, and the realtime-engine bindController family.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  CONTROLLER_AXES,
  CONTROLLER_INPUTS,
  controllerProfileNames,
  init,
  MPE_DIMENSIONS,
  NOTE_TRACKINGS,
  RealtimeEngine,
  synthEnumTables,
} from '../dist/index.js';
import { setSonareModule } from '../src/module_state.js';

/** A reed voice — one of the engines whose exciter reads the excitation axes. */
const REED_PRESET = 'clarinet';

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine(48000, 128);
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

/**
 * Renders a held note while a CC2 ramp runs, with the destination's bindings
 * cleared first and `bindings` applied on top. The cleared baseline is what
 * makes the comparison mean something: a facade that returned success and
 * dropped the call would render the baseline both times.
 */
function renderCcRamp(bindCc2ToExcitation: boolean): Float32Array {
  return withEngine((engine) => {
    engine.setSynthInstrument(REED_PRESET, 0);
    engine.clearControllerBindings(0);
    if (bindCc2ToExcitation) {
      engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitation' });
    }
    engine.pushMidiNoteOn(0, 0, 0, 60, 100);
    const out = new Float32Array(32 * 128);
    for (let block = 0; block < 32; block++) {
      engine.pushMidiCc(0, 0, 0, 2, Math.min(127, block * 4));
      const rendered = engine.process([new Float32Array(128), new Float32Array(128)]);
      out.set(rendered[0], block * 128);
    }
    return out;
  });
}

describe('Sonare WASM controller profile', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  it('lists the controller-profile preset catalog', () => {
    const names = controllerProfileNames();
    for (const expected of ['gm', 'breath', 'breath-aftertouch', 'mpe']) {
      expect(names).toContain(expected);
    }
  });

  it('exposes the controller enum tables', () => {
    const tables = synthEnumTables();
    expect(tables.controllerInputs).toEqual([...CONTROLLER_INPUTS]);
    expect(tables.controllerAxes).toEqual([...CONTROLLER_AXES]);
    expect(tables.controllerInputs).toHaveLength(5);
    expect(tables.controllerAxes).toHaveLength(8);
    // The two the note-attribution rule is spelled with, which the library
    // supplies so a host does not hardcode either list.
    expect(tables.mpeDimensions).toEqual([...MPE_DIMENSIONS]);
    expect(tables.noteTrackings).toEqual([...NOTE_TRACKINGS]);
  });

  it('installs a named preset and refuses an unknown one', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(REED_PRESET, 0);
      engine.setControllerProfile(0, 'breath');
      expect(engine.controllerBindingCount(0)).toBeGreaterThan(0);
      expect(engine.controllerVelocityMeaningful(0)).toBe(false);
      expect(() => engine.setControllerProfile(0, 'no-such-profile')).toThrow();
      // A destination nothing is bound to is an error, not an empty profile.
      expect(() => engine.setControllerProfile(3, 'gm')).toThrow();
      expect(() => engine.controllerBindingCount(3)).toThrow();
    });
  });

  it('counts a bind and drops everything on clear', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(REED_PRESET, 0);
      engine.clearControllerBindings(0);
      expect(engine.controllerBindingCount(0)).toBe(0);
      engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitation' });
      expect(engine.controllerBindingCount(0)).toBe(1);
      engine.bindController(0, { input: 'channel-pressure', axis: 'brightness' });
      expect(engine.controllerBindingCount(0)).toBe(2);
      engine.clearControllerBindings(0);
      expect(engine.controllerBindingCount(0)).toBe(0);
    });
  });

  it('round-trips the velocity-meaningful flag', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(REED_PRESET, 0);
      engine.setControllerVelocityMeaningful(0, false);
      expect(engine.controllerVelocityMeaningful(0)).toBe(false);
      engine.setControllerVelocityMeaningful(0, true);
      expect(engine.controllerVelocityMeaningful(0)).toBe(true);
    });
  });

  it('round-trips a note-tracking rule per dimension', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(REED_PRESET, 0);
      // Per dimension, so setting one leaves the other two on their default.
      engine.setControllerNoteTracking(0, 'pressure', 'highest');
      expect(engine.controllerNoteTracking(0, 'pressure')).toBe('highest');
      expect(engine.controllerNoteTracking(0, 'bend')).toBe('last');
      expect(engine.controllerNoteTracking(0, 'timbre')).toBe('last');
      engine.setControllerNoteTracking(0, 'bend', 'all');
      expect(engine.controllerNoteTracking(0, 'bend')).toBe('all');
      expect(engine.controllerNoteTracking(0, 'pressure')).toBe('highest');
      // A misspelling is refused rather than resolved to a default, which would
      // configure a dimension the caller never named.
      // @ts-expect-error unknown dimension name is rejected at runtime
      expect(() => engine.setControllerNoteTracking(0, 'no-such-dimension', 'last')).toThrow();
      // @ts-expect-error unknown tracking name is rejected at runtime
      expect(() => engine.setControllerNoteTracking(0, 'bend', 'no-such-rule')).toThrow();
    });
  });

  it('refuses a binding rather than clamping or substituting it', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(REED_PRESET, 0);
      const base = { input: 'control-change', index: 2, axis: 'excitation' } as const;
      expect(() =>
        // @ts-expect-error unknown axis name is rejected at runtime
        engine.bindController(0, { ...base, axis: 'resonance' }),
      ).toThrow();
      expect(() =>
        // @ts-expect-error unknown input name is rejected at runtime
        engine.bindController(0, { ...base, input: 'breath' }),
      ).toThrow();
      // Loudness is channel-level state, so a per-note input cannot reach it.
      expect(() =>
        engine.bindController(0, { input: 'poly-pressure', axis: 'loudness' }),
      ).toThrow();
      // An axis that means nothing is a caller mistake, not an empty slot.
      expect(() => engine.bindController(0, { ...base, axis: 'none' })).toThrow();
      expect(() => engine.bindController(0, { ...base, hi: Number.POSITIVE_INFINITY })).toThrow();
      expect(() => engine.bindController(0, { ...base, lo: Number.NaN })).toThrow();
      expect(() => engine.bindController(0, { ...base, curve: 0 })).toThrow();
      expect(() => engine.bindController(0, { ...base, curve: Number.NaN })).toThrow();
      expect(() => engine.bindController(0, { ...base, index: 128 })).toThrow();
      // The struct fields are bytes, so a value that would truncate INTO the
      // domain has to be refused on this side: 256 is CC 0 and input 0 once
      // narrowed, and 257 is a valid axis. embind does not narrow for us.
      expect(() => engine.bindController(0, { ...base, index: 256 })).toThrow(/out of range/);
      expect(() => engine.bindController(0, { ...base, input: 256 })).toThrow(/out of range/);
      expect(() => engine.bindController(0, { ...base, axis: 257 })).toThrow(/out of range/);
      // input and axis have no default: ordinal 0 is a working value on both.
      // @ts-expect-error input is required
      expect(() => engine.bindController(0, { axis: 'excitation' })).toThrow();
      // @ts-expect-error axis is required
      expect(() => engine.bindController(0, { input: 'control-change' })).toThrow();
    });
  });

  it('refuses every entry on an instrument that holds no profile', () => {
    // Only NativeSynth answers a controller profile; the base MidiInstrument
    // returns none, so these two report it rather than succeeding quietly.
    for (const bind of [
      (engine: RealtimeEngine) => engine.setBuiltinInstrument({}, 0),
      (engine: RealtimeEngine) => engine.setSf2Instrument({}, 0),
    ]) {
      withEngine((engine) => {
        bind(engine);
        expect(() => engine.controllerBindingCount(0)).toThrow();
        expect(() => engine.setControllerProfile(0, 'gm')).toThrow();
        expect(() =>
          engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitation' }),
        ).toThrow();
        expect(() => engine.clearControllerBindings(0)).toThrow();
        expect(() => engine.setControllerVelocityMeaningful(0, true)).toThrow();
        expect(() => engine.controllerVelocityMeaningful(0)).toThrow();
      });
    }
  });

  it('names the cause of every refusal', () => {
    withEngine((engine) => {
      engine.setSynthInstrument(REED_PRESET, 0);
      const message = (fn: () => void): string => {
        try {
          fn();
        } catch (error) {
          return error instanceof Error ? error.message : String(error);
        }
        throw new Error('expected a throw');
      };
      // INVALID_PARAMETER covers both "nothing bound there" and "bad binding
      // field", so neither may be reported from the code alone.
      expect(message(() => engine.setControllerProfile(3, 'gm'))).not.toBe('');
      expect(
        message(() =>
          engine.bindController(0, { input: 'control-change', index: 256, axis: 'excitation' }),
        ),
      ).not.toBe('');
      expect(message(() => engine.setControllerProfile(0, 'no-such-profile'))).toContain(
        'no-such-profile',
      );
    });
  });

  it('renders a bound CC into the audio and nothing once cleared', () => {
    const bound = renderCcRamp(true);
    const cleared = renderCcRamp(false);
    expect(bound.length).toBe(cleared.length);
    let maxDelta = 0;
    for (let i = 0; i < bound.length; i++) {
      maxDelta = Math.max(maxDelta, Math.abs(bound[i] - cleared[i]));
    }
    expect(maxDelta).toBeGreaterThan(1e-4);
    // The baseline is deterministic, so the difference above is the binding and
    // not run-to-run noise.
    expect(renderCcRamp(false)).toEqual(cleared);
  });
});
