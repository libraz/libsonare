/**
 * Pins the controller-profile facade: how a device gesture is spelled, which
 * expression axis it means, and that the mapping reaches the audio.
 *
 * The name-level cases fix the refusals — an unknown spelling, a per-note
 * gesture on a channel-level axis, a range that is not finite — because each of
 * them would otherwise resolve to an in-domain value that plays and listens to
 * the wrong thing. The render at the end is the only case that separates a
 * working facade from one that reports success and drops the call: it compares
 * a CC2 ramp against the same ramp with nothing bound to it.
 */

import { describe, expect, it } from 'vitest';
import { controllerProfileNames, RealtimeEngine, synthEnumTables } from '../src/index.js';

describe('controller profile catalog', () => {
  it('lists the profile presets the core ships', () => {
    expect(controllerProfileNames()).toEqual(['gm', 'breath', 'breath-aftertouch', 'mpe']);
  });

  it('exposes every controller enumerator the C ABI declares', () => {
    // The spellings are pinned against the facade constants by the enum-table
    // shape check in synth-patch.test.ts; what this adds is the count, so a
    // table truncated on its way through the split reads as a failure rather
    // than as a shorter enum.
    const tables = synthEnumTables();
    expect(tables.controllerInputs).toHaveLength(5);
    expect(tables.controllerAxes).toHaveLength(8);
  });
});

describe('RealtimeEngine controller bindings', () => {
  // The patch-driven synth is the instrument that holds a profile; a reed is
  // also one of the engines whose exciter a live axis can reach.
  const reedEngine = (): RealtimeEngine => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setSynthInstrument('clarinet', 0);
    return engine;
  };

  it('refuses an unknown axis name rather than resolving it to none', () => {
    const engine = reedEngine();
    expect(() =>
      engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitement' as never }),
    ).toThrow(/Unknown controller axis name/);
    engine.destroy();
  });

  it('refuses a poly-pressure binding on a channel-level axis', () => {
    const engine = reedEngine();
    expect(() => engine.bindController(0, { input: 'poly-pressure', axis: 'loudness' })).toThrow();
    engine.destroy();
  });

  it('refuses a non-finite range', () => {
    const engine = reedEngine();
    expect(() =>
      engine.bindController(0, {
        input: 'control-change',
        index: 2,
        axis: 'excitation',
        hi: Number.POSITIVE_INFINITY,
      }),
    ).toThrow(/hi must be a finite number/);
    engine.destroy();
  });

  it('counts a bound gesture and drops every binding on clear', () => {
    const engine = reedEngine();
    const before = engine.controllerBindingCount(0);
    engine.bindController(0, { input: 'channel-pressure', axis: 'excitation' });
    expect(engine.controllerBindingCount(0)).toBe(before + 1);
    engine.clearControllerBindings(0);
    expect(engine.controllerBindingCount(0)).toBe(0);
    engine.destroy();
  });

  it('round-trips the velocity-is-expression flag', () => {
    const engine = reedEngine();
    engine.setControllerVelocityMeaningful(0, false);
    expect(engine.controllerVelocityMeaningful(0)).toBe(false);
    engine.setControllerVelocityMeaningful(0, true);
    expect(engine.controllerVelocityMeaningful(0)).toBe(true);
    engine.destroy();
  });

  it('installs a named preset and refuses an unknown one', () => {
    const engine = reedEngine();
    engine.setControllerProfile(0, 'breath');
    expect(engine.controllerBindingCount(0)).toBeGreaterThan(0);
    // The breath preset states that velocity is not expression, so a preset
    // that failed to install would leave the gm default's `true` here.
    expect(engine.controllerVelocityMeaningful(0)).toBe(false);
    expect(() => engine.setControllerProfile(0, 'no-such-preset')).toThrow();
    engine.destroy();
  });

  it('reports a destination with no instrument rather than answering a count', () => {
    const engine = new RealtimeEngine(48000, 128);
    expect(() => engine.controllerBindingCount(0)).toThrow();
    engine.destroy();
  });
});

describe('RealtimeEngine controller bindings reach the audio', () => {
  // One held note under a CC2 ramp, rendered block by block so the ramp lands
  // between blocks rather than all at once before the first.
  const renderRamp = (bindBreath: boolean): Float32Array => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setSynthInstrument('clarinet', 0);
    // Both arms start from an empty profile, so the only difference between
    // them is the one binding under test rather than the gm default's three.
    engine.clearControllerBindings(0);
    if (bindBreath) {
      engine.bindController(0, { input: 'control-change', index: 2, axis: 'excitation' });
    }
    engine.play();
    engine.pushMidiNoteOn(0, 0, 0, 60, 100);
    const blocks: number[] = [];
    for (let block = 0; block < 24; block++) {
      engine.pushMidiCc(0, 0, 0, 2, Math.min(127, block * 8));
      blocks.push(...engine.process([new Float32Array(128), new Float32Array(128)])[0]);
    }
    engine.destroy();
    return Float32Array.from(blocks);
  };

  const rms = (data: Float32Array): number =>
    Math.sqrt(data.reduce((sum, value) => sum + value * value, 0) / data.length);

  it('changes the rendered audio, against a bit-identical unbound render', () => {
    const unbound = renderRamp(false);
    const unboundAgain = renderRamp(false);
    const bound = renderRamp(true);

    // Without this the two sides could agree by both being silence.
    expect(rms(unbound)).toBeGreaterThan(0);
    expect([...unboundAgain]).toEqual([...unbound]);
    expect([...bound]).not.toEqual([...unbound]);
  });
});
