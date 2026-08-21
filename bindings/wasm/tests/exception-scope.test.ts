import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, noteToHz, RealtimeEngine } from '../dist/index.js';

// emscripten defaults to DISABLE_EXCEPTION_CATCHING=1, which elides landing
// pads while compiling, so a `catch` in a unit built without -fexceptions is
// deleted outright. The loss is silent -- not a compile error, not a link
// error, not a test failure -- and what reaches JS is whatever the throw was,
// rather than the value the catch promised. These cases assert the promise
// from the JS side, one per mechanism, so the flag going missing from a unit
// again shows up as behaviour rather than only as a build-database check.
describe('WASM exception scope', () => {
  beforeAll(async () => {
    await init();
  });

  it('degrades an unparseable note name instead of throwing', () => {
    // src/core/convert.cpp catches std::invalid_argument from the octave parse
    // and answers 0. It lives in sonare_core_objects, a sibling static library,
    // and is exposed straight to embind, so it is the shortest path from a JS
    // caller to a catch outside the module's own target. It also ships in the
    // analysis-only bundle, which had no covered unit at all.
    expect(noteToHz('A4')).toBeCloseTo(440.0, 4);
    expect(noteToHz('Cx')).toBe(0);
    expect(noteToHz('')).toBe(0);
    expect(noteToHz('H9999999999999999999999')).toBe(0);
  });

  it('reports malformed MIDI FX JSON as an error code, not a raw exception', () => {
    // src/c_api/midi_fx_json.h catches json::JsonError and answers
    // SONARE_ERROR_INVALID_FORMAT. It is an inline function, so the catch
    // belongs to every unit that includes it rather than to the header -- the
    // reason a unit can be uncovered while its own text spells no catch at all.
    const engine = new RealtimeEngine(48000, 128);
    try {
      let thrown: unknown;
      try {
        engine.setMidiFx(0, '{bad');
      } catch (error) {
        thrown = error;
      }
      expect(isSonareError(thrown)).toBe(true);
      if (!isSonareError(thrown)) {
        throw new Error('expected SonareError');
      }
      // The point is that a decoded SonareError arrives at all: an elided
      // handler delivers the raw json::JsonError, which carries no code for
      // the decoder to read.
      expect(typeof thrown.code).toBe('number');
      expect(thrown.code).not.toBe(ErrorCode.Unknown);
    } finally {
      engine.delete();
    }
  });
});
