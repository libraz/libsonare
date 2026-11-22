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
import {
  type BuiltinSynthWaveform,
  ErrorCode,
  init,
  type MasteringPreset,
  Mixer,
  masterAudio,
  masterAudioStereo,
  masteringApplyProcessor,
  masteringAssistantSuggest,
  masteringChainConfigFromJson,
  masteringPresetParams,
  mixingScenePreset,
  noteToHz,
  Project,
  RealtimeEngine,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  realtimeVoiceChangerValidatePresetJson,
} from '../dist/index.js';
import { getSonareModule } from '../src/module_state.js';
import { synthPatchRoundTripForTest } from '../src/project.js';
import { expectRefusalOf } from './_helpers';
import { shareBuiltModuleWithSource } from './_module_realm.js';
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

function expectNulRefusal(call: () => unknown, entry: string, field: string): void {
  const error = refusal(call);
  expect(error).toBeInstanceOf(RangeError);
  expect(error.message).toBe(`${entry}: ${field} must not contain NUL`);
}

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

function withMixer<T>(body: (mixer: Mixer) => T): T {
  const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('drumBusSubgroup')), {
    sampleRate: 48000,
  });
  try {
    return body(mixer);
  } finally {
    mixer.destroy();
  }
}

/** The first strip id of the scene the mixer cases use; read after `init()`. */
function stripId(): string {
  return mixingScenePreset('drumBusSubgroup').strips[0].id;
}

/**
 * The embind object behind the facade. The facade serializes an object with
 * `JSON.stringify`, which escapes a NUL, so a raw NUL can only reach the C
 * reader through the native JSON entry points.
 */
function nativeOf(engine: RealtimeEngine): {
  setBusStripEqBandJson(busId: number, bandIndex: number, bandJson: string): void;
} {
  return (engine as unknown as { native: ReturnType<typeof nativeOf> }).native;
}

const BAND_JSON = '{"type":"peak","frequencyHz":1000,"gainDb":3,"q":1}';
const SCENE_BUS = { id: '1', inserts: [] } as never;

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
    shareBuiltModuleWithSource();
  });

  it('stringProperty names the key', () => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ id: 1, ppq: 0, name: 'a', kind: 'marker' }])).not.toThrow();
      expectNulRefusal(
        () => engine.setMarkers([{ id: 1, ppq: 0, name: `a${NUL}b`, kind: 'marker' }]),
        'RealtimeEngine.setMarkers',
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
      'vocalEditSessionCreate',
      'algorithmId',
    );
  });

  it('the builtin waveform name names the field', () => {
    withEngine((engine) => {
      expect(() => engine.setBuiltinInstrument(0, { waveform: 'sine' })).not.toThrow();
      expectNulRefusal(
        () => engine.setBuiltinInstrument(0, { waveform: `sine${NUL}x` as BuiltinSynthWaveform }),
        'RealtimeEngine.setBuiltinInstrument',
        'waveform',
      );
    });
  });

  it('wasmCString names the subject of each handoff', () => {
    withMixer((mixer) => {
      expectNulRefusal(() => mixer.busMeter(`a${NUL}b`), 'Mixer.busMeter', 'busId');
      expectNulRefusal(() => mixer.stripById(`${stripId()}${NUL}b`), 'Mixer.stripById', 'id');
    });
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setBusStrip(1, SCENE_BUS);
      expectNulRefusal(
        () => nativeOf(engine).setBusStripEqBandJson(1, 0, `a${NUL}b`),
        'RealtimeEngine.setBusStripEqBandJson',
        'bandJson',
      );
    });
  });
});

describe('embedded NUL refusal, by handoff kind', () => {
  beforeAll(async () => {
    await init();
    shareBuiltModuleWithSource();
  });

  it('a name lookup (mixer bus id)', () => {
    withMixer((mixer) => {
      expect(() => mixer.busMeter('master')).not.toThrow();
      expect(expectRefusalOf(() => mixer.busMeter('masterx'), RangeError).message).toBe(
        'Mixer.busMeter: busId must be the id of a bus in this mixer',
      );
      expectNulRefusal(() => mixer.busMeter(`master${NUL}x`), 'Mixer.busMeter', 'busId');
    });
  });

  it('a name lookup (mixer strip id)', () => {
    withMixer((mixer) => {
      expect(mixer.stripById(stripId())).toBe(0);
      expect(() => mixer.stripById(`${stripId()}x`)).toThrow(RangeError);
      expectNulRefusal(() => mixer.stripById(`${stripId()}${NUL}x`), 'Mixer.stripById', 'id');
    });
  });

  it('a JSON key (EQ band)', () => {
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setBusStrip(1, SCENE_BUS);
      const native = nativeOf(engine);
      expect(() => native.setBusStripEqBandJson(1, 0, BAND_JSON)).not.toThrow();
      expect(() => native.setBusStripEqBandJson(1, 0, 'garbage')).toThrow(
        expect.objectContaining({
          code: ErrorCode.InvalidFormat,
          message: expect.stringMatching(
            /^RealtimeEngine\.setBusStripEqBandJson: unexpected JSON character at byte 0$/,
          ),
        }),
      );
      expectNulRefusal(
        () => native.setBusStripEqBandJson(1, 0, `${BAND_JSON}${NUL}garbage`),
        'RealtimeEngine.setBusStripEqBandJson',
        'bandJson',
      );
    });
  });

  it('a fixed-buffer copy (synth patch preset)', () => {
    expect(synthPatchRoundTripForTest({ preset: 'piano' }).preset).toBe('piano');
    expectNulRefusal(
      () => synthPatchRoundTripForTest({ preset: `piano${NUL}x` }),
      'synthPatchRoundTripForTest',
      'preset',
    );
    expectNulRefusal(
      () => synthPatchRoundTripForTest(`piano${NUL}x` as unknown as { preset: string }),
      'synthPatchRoundTripForTest',
      'preset',
    );
  });

  it('an internal const char* (swap_graph node ids)', () => {
    withEngine((engine) => {
      engine.setGraph(graph('a'));
      expect(engine.graphNodeCount).toBe(1);
      expect(expectRefusalOf(() => engine.setGraph(graph('zz')), RangeError).message).toBe(
        'RealtimeEngine.setGraph: failed to swap graph',
      );
      expectNulRefusal(
        () => engine.setGraph(graph(`a${NUL}b`)),
        'RealtimeEngine.setGraph',
        'inputNode',
      );
      expect(engine.graphNodeCount).toBe(1);
    });
  });
});

describe('embedded NUL refusal, mastering names', () => {
  const samples = new Float32Array(4096).fill(0.1);
  const base = { samples, sampleRate: 44100 };

  beforeAll(async () => {
    await init();
    shareBuiltModuleWithSource();
  });

  const expectRangeRefusal = (call: () => unknown) => {
    expect(refusal(call)).toBeInstanceOf(RangeError);
  };

  it('a chain configuration document is refused by name', () => {
    const document = '{"version":2,"params":{}}';
    expect(() => masteringChainConfigFromJson(document)).not.toThrow();
    expectNulRefusal(
      () => masteringChainConfigFromJson(`${document}${NUL}x`),
      'masteringChainConfigFromJson',
      'document',
    );
  });

  it('a preset name is refused in every spelling', () => {
    expect(() => masteringPresetParams('pop')).not.toThrow();
    expectNulRefusal(
      () => masteringPresetParams(`pop${NUL}x` as MasteringPreset),
      'masteringPresetParams',
      'preset',
    );
    expectRangeRefusal(() => masterAudio({ ...base, preset: `pop${NUL}x` as MasteringPreset }));
    expectRangeRefusal(() =>
      masterAudio(samples, 44100, { preset: `pop${NUL}x` as MasteringPreset }),
    );
    expectRangeRefusal(() =>
      masterAudioStereo({
        left: samples,
        right: samples,
        sampleRate: 44100,
        preset: `pop${NUL}x` as MasteringPreset,
      }),
    );
    expectRangeRefusal(() => masteringAssistantSuggest({ ...base, preset: `pop${NUL}x` as never }));
  });

  it('a processor, analysis or platform name is refused', () => {
    expectRangeRefusal(() =>
      masteringApplyProcessor({ ...base, processor: `dynamics.compressor${NUL}x` as never }),
    );
    expectRangeRefusal(() =>
      masteringAssistantSuggest({ ...base, targetPlatform: `spotify${NUL}x` as never }),
    );
  });
});

describe('embedded NUL refusal, std::string entry points', () => {
  beforeAll(async () => {
    await init();
    shareBuiltModuleWithSource();
  });

  it('a preset name or note name is refused by name, not looked up truncated', () => {
    const [id] = realtimeVoiceChangerPresetNames();
    expect(noteToHz('A4')).toBeCloseTo(440, 3);
    expectNulRefusal(() => noteToHz(`A4${NUL}junk`), 'noteToHz', 'note');
    // The facade resolves the closed preset set first; the native reader is driven directly.
    const presetJson = (name: string): string => getSonareModule().mixingScenePresetJson(name);
    expect(presetJson('vocalReverbSend').length).toBeGreaterThan(0);
    expectNulRefusal(
      () => presetJson(`vocalReverbSend${NUL}junk`),
      'mixingScenePresetJson',
      'preset',
    );
    expect(refusal(() => mixingScenePreset(`vocalReverbSend${NUL}junk` as never))).toBeInstanceOf(
      RangeError,
    );
    expect(realtimeVoiceChangerPresetJson(id).length).toBeGreaterThan(0);
    // A preset id is a closed token, so the facade refuses the NUL form as an unknown id.
    expect(
      refusal(() => realtimeVoiceChangerPresetJson(`${id}${NUL}junk` as typeof id)),
    ).toBeInstanceOf(RangeError);
    expect(
      refusal(() => realtimeVoiceChangerPresetConfig(`${id}${NUL}junk` as typeof id)),
    ).toBeInstanceOf(RangeError);
  });

  it('a JSON text is refused by name, not reported as trailing data', () => {
    const json = new Project().toJson();
    expect(() => Project.fromJson(json)).not.toThrow();
    expectNulRefusal(() => Project.fromJson(`${json}${NUL}junk`), 'Project.fromJson', 'json');
    expectNulRefusal(
      () => Project.fromJsonWithDiagnostics(`${json}${NUL}junk`),
      'Project.fromJsonWithDiagnostics',
      'json',
    );
    expectNulRefusal(
      () => realtimeVoiceChangerValidatePresetJson(`{}${NUL}junk`),
      'realtimeVoiceChangerValidatePresetJson',
      'json',
    );
  });

  it('an unknown voice preset name is a RangeError', () => {
    const error = refusal(() => realtimeVoiceChangerPresetConfig('no-such-preset' as never));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toContain('preset must be one of neutralMonitor');
  });
});

describe('embedded NUL refusal, scan', () => {
  /** `std::string` entry points whose body must refuse a NUL before the core sees the text. */
  const REFUSING: { file: string; head: string }[] = [
    { file: 'mixing/mixing.cpp', head: 'std::string js_mixing_scene_preset_json(' },
    { file: 'features/core.cpp', head: 'float js_note_to_hz(' },
    { file: 'project/project.cpp', head: 'val ProjectWasm::fromJson(' },
    {
      file: 'effects/realtime_voice_changer.cpp',
      head: 'std::string realtimeVoiceChangerPresetJson(',
    },
    {
      file: 'effects/realtime_voice_changer.cpp',
      head: 'val validateRealtimeVoiceChangerPresetJson(',
    },
  ];

  it('refuses a NUL at the head of each std::string entry point', () => {
    const sources = wasmBindingSources();
    const missing = REFUSING.filter(({ file, head }) => {
      const text = sources.find((source) => source.file === file)?.text ?? '';
      const at = text.indexOf(head);
      return at < 0 || !text.slice(at, at + 400).includes('wasmRefuseEmbeddedNul(');
    }).map(({ file, head }) => `${file}: ${head}`);
    expect(missing).toEqual([]);
  });

  /**
   * `.c_str()` calls that never carry a JS-supplied string, each with the reason.
   * An entry matches source lines by their text and names how many it matches, so
   * a new line that happens to share the text is not excused by an old entry.
   */
  const ALLOWED: { file: string; text: string; count: number; reason: string }[] = [
    {
      file: 'mastering/api.cpp',
      text: 'subject.c_str()',
      count: 2,
      reason: 'error-subject label built from a platform index',
    },
    {
      file: 'mastering/api.cpp',
      text: '(subject + ".name").c_str()',
      count: 1,
      reason: 'error-subject label built from a platform index',
    },
    {
      file: 'mastering/api.cpp',
      text: '(subject + "." + key).c_str()',
      count: 1,
      reason: 'error-subject label built from a platform index and a literal key',
    },
    {
      file: 'common/common.cpp',
      text: 'return text.c_str();',
      count: 1,
      reason: 'wasmCString itself',
    },
    {
      file: 'common/common.h',
      text: 'sonare::util::json::schema::child_path(subject, key).c_str()',
      count: 2,
      reason:
        'Diagnostic labels for requireInt64Property and requireIntProperty: all keys are literal or macro-stringized names, and subjects are code-owned labels (including note-edit labels composed from fixed entry-point names). checkedInt64FromVal/checkedIntFromVal consume the temporary synchronously for errors; no JS string crosses this boundary.',
    },
    {
      file: 'effects/notes.cpp',
      text: 'list_subject.c_str()',
      count: 2,
      reason: 'error-subject label built from literals',
    },
    {
      file: 'effects/notes.cpp',
      text: 'subject.c_str()',
      count: 7,
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'effects/notes.cpp',
      text: '".frameStart").c_str()',
      count: 1,
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'effects/notes.cpp',
      text: '".frameEnd").c_str()',
      count: 1,
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'analysis/quick.cpp',
      text: '".root").c_str()',
      count: 1,
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'analysis/quick.cpp',
      text: '".quality").c_str()',
      count: 1,
      reason: 'error-subject label built from an element index',
    },
    {
      file: 'common/synth_patch_val.h',
      text: 'subject.c_str()',
      count: 1,
      reason: 'error-subject label built from an engineParams key already refused for NUL',
    },
    {
      file: 'mixing/mixing_assistant.cpp',
      text: 'subject.c_str()',
      count: 1,
      reason: 'error-subject label built from a literal name and an element index',
    },
    {
      file: 'realtime/processing.cpp',
      text: 'ports_subject.c_str()',
      count: 1,
      reason: 'error-subject label built from a graph node index and a literal member name',
    },
    {
      file: 'realtime/clips.cpp',
      text: 'subject.c_str()',
      count: 3,
      reason: 'error-subject label built from a clip index and a literal member name',
    },
    {
      file: 'realtime/midi.cpp',
      text: 'subject.c_str()',
      count: 4,
      reason: 'error-subject label built from a clip index and a literal member name',
    },
    {
      file: 'metering/metering.cpp',
      text: 'subject.c_str()',
      count: 1,
      reason: 'error-subject label built from a literal name and an element index',
    },
    {
      file: 'mixing/mixing.cpp',
      text: 'element.c_str()',
      count: 1,
      reason: 'error-subject label built from literals and an index',
    },
    {
      file: 'mixing/mixing_processing.cpp',
      text: 'left_subject.c_str()',
      count: 1,
      reason: 'error-subject label built from a literal name and a strip index',
    },
    {
      file: 'mixing/mixing_processing.cpp',
      text: 'right_subject.c_str()',
      count: 1,
      reason: 'error-subject label built from a literal name and a strip index',
    },
    {
      file: 'mixing/mixing.cpp',
      text: '("strip" + std::to_string(index)).c_str()',
      count: 1,
      reason: 'strip id generated internally',
    },
    {
      file: 'analysis/room_acoustics.cpp',
      text: 'label.c_str()',
      count: 1,
      reason: 'error-subject label built from a literal key and an axis name',
    },
    {
      file: 'common/common.cpp',
      text: 'channel_name.c_str()',
      count: 3,
      reason: 'refusal name built from the caller-supplied literal name and a channel index',
    },
    {
      file: 'effects/realtime_voice_changer.cpp',
      text: '(path + "[" + std::to_string(i) + "]").c_str()',
      count: 1,
      reason: 'error-subject label built from a literal path and an index',
    },
    {
      file: 'effects/spectral_edit.cpp',
      text: 'opPath(i, ',
      count: 4,
      reason: 'error-subject label built from the op index and a literal member name',
    },
    {
      file: 'common/note_val.h',
      text: '(std::string(subject) + ".timeStretchRatio").c_str()',
      count: 1,
      reason: 'error-subject label built from a code-owned edit label and a literal key',
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

  it('matches every allowlist entry exactly as many times as it declares', () => {
    const { calls } = cStrCalls();
    const drifted = ALLOWED.map((entry) => ({
      entry,
      found: calls.filter((call) => call.file === entry.file && call.code.includes(entry.text))
        .length,
    }))
      .filter(({ entry, found }) => found !== entry.count)
      .map(
        ({ entry, found }) =>
          `${entry.file}: ${entry.text} (declares ${entry.count}, found ${found})`,
      );
    expect(drifted).toEqual([]);
  });

  it('routes the handoffs through the helper', () => {
    const uses = wasmBindingSources().reduce(
      (total, { text }) => total + (text.match(/\bwasmCString\s*\(/g)?.length ?? 0),
      0,
    );
    expect(uses, 'wasmCString call sites').toBeGreaterThan(40);
  });
});
