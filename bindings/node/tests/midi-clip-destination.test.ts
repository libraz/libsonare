/**
 * Pins how a MIDI clip's `destinationId` marshals across the facade.
 *
 * The field falls back to `trackId` only when it is *absent*. An explicit 0 is
 * a real destination and must be forwarded unchanged — a facade that treats 0
 * as "unset" reroutes the clip to its track id, which is audible as silence at
 * the requested destination and as an unexpected voice at the track's own. The
 * three cases below fix both directions of that mapping, and Python pins the
 * same behaviour in bindings/python/tests/test_engine.py so the surfaces
 * cannot drift apart.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

describe('RealtimeEngine MIDI clip destination routing', () => {
  const rms = (data: Float32Array): number => {
    let sum = 0;
    for (const value of data) {
      sum += value * value;
    }
    return Math.sqrt(sum / data.length);
  };

  const midi1Word = (status: number, channel: number, data0: number, data1: number): number =>
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | (data0 << 8) | data1;

  // One held note, on at frame 0 and off well past the rendered block.
  const heldNote = [
    { renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 },
    { renderFrame: 4096, word0: midi1Word(0x8, 0, 60, 0), wordCount: 1 },
  ];

  const renderOneBlock = (engine: RealtimeEngine): number => {
    engine.play();
    const out = engine.process([new Float32Array(128), new Float32Array(128)]);
    return Math.max(rms(out[0]), rms(out[1]));
  };

  it('forwards an explicit destinationId of 0 rather than rewriting it to the track id', () => {
    const engine = new RealtimeEngine(48000, 128);
    // Only destination 0 carries an instrument, and the track id differs from
    // it, so a rewrite to trackId would leave the note unheard.
    engine.setBuiltinInstrument({ gain: 0.5 }, 0);
    engine.setMidiClips([
      { id: 1, trackId: 6, destinationId: 0, lengthSamples: 8192, events: heldNote },
    ]);
    expect(renderOneBlock(engine)).toBeGreaterThan(0);
    engine.destroy();
  });

  it('does not sound the track-id instrument when destination 0 is explicit', () => {
    const engine = new RealtimeEngine(48000, 128);
    // The mirror image: the instrument sits at the track id and destination 0
    // is empty, so the same clip must render silence. A rewrite of 0 to trackId
    // is audible here, which is what the previous case alone cannot catch.
    engine.setBuiltinInstrument({ gain: 0.5 }, 6);
    engine.setMidiClips([
      { id: 1, trackId: 6, destinationId: 0, lengthSamples: 8192, events: heldNote },
    ]);
    expect(renderOneBlock(engine)).toBe(0);
    engine.destroy();
  });

  it('falls back to the track id when destinationId is omitted', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setBuiltinInstrument({ gain: 0.5 }, 6);
    engine.setMidiClips([{ id: 1, trackId: 6, lengthSamples: 8192, events: heldNote }]);
    expect(renderOneBlock(engine)).toBeGreaterThan(0);
    engine.destroy();
  });
});
