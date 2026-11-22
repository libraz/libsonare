import { beforeAll, describe, expect, it } from 'vitest';
import { init, noteToHz, RealtimeEngine } from '../dist/index.js';
import { expectArgumentRefusal } from './_helpers';

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

  it('reports an unparseable note name as a coded error instead of throwing raw', () => {
    // src/core/convert.cpp catches std::invalid_argument from the octave parse
    // and rethrows it as a SonareException. It lives in sonare_core_objects, a
    // sibling static library, and is exposed straight to embind, so it is the
    // shortest path from a JS caller to a catch outside the module's own target.
    // It also ships in the analysis-only bundle, which had no covered unit at all.
    expect(noteToHz('A4')).toBeCloseTo(440.0, 4);
    for (const name of ['Cx', '', 'H9999999999999999999999']) {
      let thrown: unknown;
      try {
        noteToHz(name);
      } catch (error) {
        thrown = error;
      }
      const error = expectArgumentRefusal(thrown, RangeError, name);
      expect(error.message, name).toMatch(/^noteToHz: /);
    }
  });

  it('reports a refused MIDI FX config as a translated argument refusal, not a raw exception', () => {
    // src/c_api/midi_fx_json.h refuses a value it cannot accept. It is an inline function, so the
    // catch around its JSON parse belongs to every unit that includes it rather than to the
    // header -- the reason a unit can be uncovered while its own text spells no catch at all.
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      let thrown: unknown;
      try {
        engine.setMidiFx(0, { quantizePpq: 0 });
      } catch (error) {
        thrown = error;
      }
      // An elided handler delivers the raw json::JsonError instead of a translated RangeError.
      const error = expectArgumentRefusal(thrown, RangeError, 'setMidiFx');
      expect(error.message).toBe(
        'RealtimeEngine.setMidiFx: config.quantizePpq must be a positive PPQ length in range',
      );
    } finally {
      engine.destroy();
    }
  });
});
