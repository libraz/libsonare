/**
 * An embedded NUL in any string the WASM binding hands to a NUL-terminated API
 * is refused by name.
 *
 * The C ABI and the internal `const char*` functions end a string at its first
 * NUL, so a name, key or JSON text with a NUL in it used to be cut there and
 * succeed as its prefix. Each case pairs the prefix, which must keep working,
 * with the same text carrying a NUL, which must be a RangeError naming the
 * field. The scan at the foot keeps `wasmCString` the only route from a JS
 * string to a C string.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson, RealtimeEngine } from '../dist/index.js';
import { getSonareModule, setSonareModule } from '../src/module_state.js';
import { synthPatchRoundTripForTest } from '../src/project.js';
import { wasmBindingSources } from './_wasm_binding_sources';

const NUL = '\0';

function refusal(call: () => unknown): Error {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught, 'the call must throw').toBeInstanceOf(Error);
  return caught as Error;
}

function expectNulRefusal(call: () => unknown, field: string): void {
  const error = refusal(call);
  expect(error).toBeInstanceOf(RangeError);
  expect(error.message).toBe(`${field} must not contain NUL`);
}

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine(48000, 256);
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

function withMixer<T>(body: (mixer: Mixer) => T): T {
  const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'));
  try {
    return body(mixer);
  } finally {
    mixer.destroy();
  }
}

/** The first strip id of the scene the mixer cases use; read after `init()`. */
function stripId(): string {
  return JSON.parse(mixingScenePresetJson('drumBusSubgroup')).strips[0].id;
}

const BAND_JSON = '{"type":"Peak","frequencyHz":1000,"gainDb":3,"q":1}';
const SCENE_JSON = JSON.stringify({
  version: 1,
  strips: [],
  buses: [{ id: '1', inserts: [] }],
  connections: [],
});

function graph(inputNode: string) {
  return {
    nodes: [{ id: 'a', numPorts: 1 }],
    connections: [],
    inputNode,
    outputNode: 'a',
    numChannels: 1,
  };
}

function vocalRequest() {
  return {
    samples: new Float32Array(256),
    sampleRate: 16000,
    analysis: {
      frameOriginSample: 0,
      samplesPerFrame: 256,
      frameLengthSamples: 256,
      f0Hz: new Float32Array([220]),
      voiced: new Uint8Array([1]),
      algorithmId: 'host',
      algorithmVersion: 1,
    },
  };
}

describe('embedded NUL refusal, by reader', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  it('stringProperty names the key', () => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ id: 1, ppq: 0, name: 'a', kind: 0 }])).not.toThrow();
      expectNulRefusal(
        () => engine.setMarkers([{ id: 1, ppq: 0, name: `a${NUL}b`, kind: 0 }]),
        'name',
      );
    });
  });

  it('stringPropertyStrict names the key', () => {
    const native = getSonareModule();
    const request = vocalRequest();
    request.analysis.algorithmId = `host${NUL}suffix`;
    expectNulRefusal(
      () => native.vocalEditSessionCreate(request.samples, request.sampleRate, request),
      'algorithmId',
    );
  });

  it('the builtin waveform name names the field', () => {
    withEngine((engine) => {
      expect(() => engine.setBuiltinInstrument({ waveform: 'sine' }, 0)).not.toThrow();
      expectNulRefusal(
        () => engine.setBuiltinInstrument({ waveform: `sine${NUL}x` }, 0),
        'waveform',
      );
    });
  });

  it('wasmCString names the subject of each handoff', () => {
    withMixer((mixer) => {
      expectNulRefusal(() => mixer.busMeter(`a${NUL}b`), 'busId');
      expectNulRefusal(() => mixer.stripById(`${stripId()}${NUL}b`), 'id');
    });
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setBusStripJson(1, SCENE_JSON);
      expectNulRefusal(() => engine.setBusStripEqBandJson(1, 0, `a${NUL}b`), 'bandJson');
    });
  });
});

describe('embedded NUL refusal, by handoff kind', () => {
  beforeAll(async () => {
    await init();
    const createModule = (await import('../dist/sonare.js')).default;
    setSonareModule(await createModule());
  });

  it('a name lookup (mixer bus id)', () => {
    withMixer((mixer) => {
      expect(() => mixer.busMeter('master')).not.toThrow();
      expect(() => mixer.busMeter('masterx')).toThrow();
      expectNulRefusal(() => mixer.busMeter(`master${NUL}x`), 'busId');
    });
  });

  it('a name lookup (mixer strip id)', () => {
    withMixer((mixer) => {
      expect(mixer.stripById(stripId())).toBe(0);
      expect(mixer.stripById(`${stripId()}x`)).toBeNull();
      expectNulRefusal(() => mixer.stripById(`${stripId()}${NUL}x`), 'id');
    });
  });

  it('a JSON key (EQ band)', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setBusStripJson(1, SCENE_JSON);
      expect(() => engine.setBusStripEqBandJson(1, 0, BAND_JSON)).not.toThrow();
      expect(() => engine.setBusStripEqBandJson(1, 0, 'garbage')).toThrow();
      expectNulRefusal(
        () => engine.setBusStripEqBandJson(1, 0, `${BAND_JSON}${NUL}garbage`),
        'bandJson',
      );
    });
  });

  it('a fixed-buffer copy (synth patch preset)', () => {
    expect(synthPatchRoundTripForTest({ preset: 'piano' }).preset).toBe('piano');
    expectNulRefusal(() => synthPatchRoundTripForTest({ preset: `piano${NUL}x` }), 'preset');
    expectNulRefusal(
      () => synthPatchRoundTripForTest(`piano${NUL}x` as unknown as { preset: string }),
      'preset',
    );
  });

  it('an internal const char* (swap_graph node ids)', () => {
    withEngine((engine) => {
      engine.setGraph(graph('a'));
      expect(engine.graphNodeCount()).toBe(1);
      expect(() => engine.setGraph(graph('zz'))).toThrow();
      expectNulRefusal(() => engine.setGraph(graph(`a${NUL}b`)), 'inputNode');
      expect(engine.graphNodeCount()).toBe(1);
    });
  });
});

describe('embedded NUL refusal, scan', () => {
  /**
   * `.c_str()` calls that never carry a JS-supplied string, each with the reason.
   * An entry matches a source line by its text; one that matches nothing is stale.
   */
  const ALLOWED: { file: string; text: string; reason: string }[] = [
    { file: 'common/common.cpp', text: 'return text.c_str();', reason: 'wasmCString itself' },
    {
      file: 'common/common.cpp',
      text: 'budget.c_str()',
      reason: 'error-subject label built from literals',
    },
    {
      file: 'common/common.h',
      text: '(std::string(subject) + "." + key).c_str()',
      reason: 'error-subject label built from a literal subject and key',
    },
    {
      file: 'common/note_val.h',
      text: 'subject.c_str()',
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'effects/notes.cpp',
      text: 'budget.c_str()',
      reason: 'error-subject label built from literals',
    },
    {
      file: 'effects/notes.cpp',
      text: 'list_subject.c_str()',
      reason: 'error-subject label built from literals',
    },
    {
      file: 'effects/notes.cpp',
      text: 'subject.c_str()',
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'effects/notes.cpp',
      text: '".frameStart").c_str()',
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'effects/notes.cpp',
      text: '".frameEnd").c_str()',
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'effects/repair.cpp',
      text: '" input").c_str()',
      reason: 'error-subject label built from a literal entry-point name',
    },
    {
      file: 'features/core.cpp',
      text: 'budget_subject.c_str()',
      reason: 'error-subject label built from a literal function name',
    },
    {
      file: 'analysis/quick.cpp',
      text: '".root").c_str()',
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'analysis/quick.cpp',
      text: '".quality").c_str()',
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'mastering/api.cpp',
      text: 'hasProperty(params_obj, key.c_str())',
      reason: 'property name from a static table of literals',
    },
    {
      file: 'mixing/mixing.cpp',
      text: 'element.c_str()',
      reason: 'error-subject label built from literals and an index',
    },
    {
      file: 'mixing/mixing.cpp',
      text: '("strip" + std::to_string(index)).c_str()',
      reason: 'strip id generated internally',
    },
    {
      file: 'playback/playback.cpp',
      text: 'budget.c_str()',
      reason: 'error-subject label built from literals',
    },
  ];

  /** Every `.c_str()` call outside comments, as file:line plus the source text. */
  function cStrCalls(): { scanned: number; calls: { at: string; file: string; code: string }[] } {
    const sources = wasmBindingSources();
    const calls: { at: string; file: string; code: string }[] = [];
    for (const { file, text } of sources) {
      text.split('\n').forEach((line, index) => {
        const code = line.replace(/\/\/.*$/, '');
        if (/\.c_str\s*\(/.test(code)) {
          calls.push({ at: `${file}:${index + 1}`, file, code });
        }
      });
    }
    return { scanned: sources.length, calls };
  }

  it('hands no JS string to a C string outside wasmCString', () => {
    const { scanned, calls } = cStrCalls();
    expect(scanned, 'the scan must reach the binding sources').toBeGreaterThan(20);
    const unlisted = calls.filter(
      (call) =>
        !ALLOWED.some((entry) => entry.file === call.file && call.code.includes(entry.text)),
    );
    expect(unlisted.map((call) => call.at)).toEqual([]);
  });

  it('keeps every allowlist entry live', () => {
    const { calls } = cStrCalls();
    const stale = ALLOWED.filter(
      (entry) => !calls.some((call) => call.file === entry.file && call.code.includes(entry.text)),
    );
    expect(stale.map((entry) => `${entry.file}: ${entry.text}`)).toEqual([]);
  });

  it('routes the handoffs through the helper', () => {
    const uses = wasmBindingSources().reduce(
      (total, { text }) => total + (text.match(/\bwasmCString\s*\(/g)?.length ?? 0),
      0,
    );
    expect(uses, 'wasmCString call sites').toBeGreaterThan(40);
  });
});
