/**
 * An embedded NUL in any string the addon takes from JS is refused by name.
 *
 * The C ABI takes NUL-terminated strings, so a name, key, path or JSON text with
 * a NUL in it used to be cut at the NUL and succeed as its prefix. Each case
 * pairs the prefix, which must keep working, with the same text carrying a NUL,
 * which must be a RangeError naming the field. The scan at the foot keeps the
 * shared string readers the only route from a JS string to a C++ one.
 */

import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  Mixer,
  masterAudio,
  masterAudioStereo,
  masteringChainConfigFromJson,
  masteringInsertParamNames,
  masteringPresetParams,
  mixingScenePreset,
  noteToHz,
  Project,
  RealtimeEngine,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  realtimeVoiceChangerValidatePresetJson,
  StreamingMasteringChain,
} from '../src/index.js';
import { addon } from '../src/native.js';
import { addonSources } from './_addon_sources.js';
import { sine, writePcm16Wav } from './_helpers.js';

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

function expectUnknownPresetRefusal(call: () => unknown): void {
  const error = refusal(call);
  expect(error).toBeInstanceOf(RangeError);
  expect(error.message).toMatch(/must be one of/);
}

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 256 });
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

/**
 * The facade serialises a band or bus object itself, so a NUL can no longer
 * reach the native JSON-string readers through it; these two cases drive the
 * addon handle directly to keep exercising the readers.
 */
interface NativeJsonEngine {
  setTrackBuses(buses: Array<{ busId: number }>): void;
  setBusStripJson(busId: number, json: string): void;
  setBusStripEqBandJson(busId: number, bandIndex: number, json: string): void;
  destroy(): void;
}

function withNativeEngine<T>(body: (engine: NativeJsonEngine) => T): T {
  const engine = new addon.RealtimeEngine(48000, 256) as NativeJsonEngine;
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

const BAND_JSON = '{"type":"peak","frequencyHz":1000,"gainDb":3,"q":1}';
const SCENE_JSON = JSON.stringify({
  version: 1,
  strips: [],
  buses: [{ id: '1', inserts: [] }],
  connections: [],
});

describe('embedded NUL refusal, mastering chain documents', () => {
  it('a chain configuration document is refused by name', () => {
    const document = '{"version":2,"params":{}}';
    expect(() => masteringChainConfigFromJson(document)).not.toThrow();
    expectNulRefusal(() => masteringChainConfigFromJson(`${document}${NUL}x`), 'document');
  });
});

describe('embedded NUL refusal, mastering preset names', () => {
  const samples = new Float32Array(4096).fill(0.1);
  const preset = `pop${NUL}x` as never;

  it('every spelling of a preset name is refused by name', () => {
    expect(() => masteringPresetParams('pop')).not.toThrow();
    expectNulRefusal(() => masteringPresetParams(preset), 'preset');
    expectNulRefusal(() => masterAudio({ samples, sampleRate: 44100, preset }), 'presetName');
    expectNulRefusal(() => masterAudio(samples, 44100, { preset }), 'presetName');
    expectNulRefusal(
      () => masterAudioStereo({ left: samples, right: samples, sampleRate: 44100, preset }),
      'presetName',
    );
  });
});

describe('embedded NUL refusal, by reader', () => {
  it('node_narrow_string (positional) names the argument', () => {
    expect(masteringInsertParamNames('utility.gain').length).toBeGreaterThan(0);
    expectNulRefusal(() => addon.masteringInsertParamNames(`utility.gain${NUL}x`), 'name');
  });

  it('OptionalStringArg names the argument', () => {
    withNativeEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      engine.setBusStripJson(1, SCENE_JSON);
      expect(() => engine.setBusStripEqBandJson(1, 0, BAND_JSON)).not.toThrow();
      expectNulRefusal(
        () => engine.setBusStripEqBandJson(1, 0, `${BAND_JSON}${NUL}garbage`),
        'bandJson',
      );
    });
  });

  it('RequiredStringValue names the label', () => {
    const chain = new StreamingMasteringChain({ sampleRate: 48000, numChannels: 1 });
    try {
      expectNulRefusal(() => chain.setParameter(`a${NUL}b`, 1), 'key');
    } finally {
      chain.destroy();
    }
  });

  it('RequiredStringProperty names the key', () => {
    withEngine((engine) => {
      const graph = (id: string) => ({
        nodes: [{ id }],
        connections: [],
        inputNode: id,
        outputNode: id,
      });
      expectNulRefusal(() => engine.setGraph(graph(`a${NUL}b`)), 'id');
    });
  });

  it('StringProperty names the key', () => {
    withEngine((engine) => {
      expect(() => engine.setMarkers([{ id: 1, ppq: 0, name: 'a', kind: 'marker' }])).not.toThrow();
      expectNulRefusal(
        () => engine.setMarkers([{ id: 1, ppq: 0, name: `a${NUL}b`, kind: 'marker' }]),
        'name',
      );
    });
  });

  it('ReadBuiltinWaveform names the field', () => {
    withEngine((engine) => {
      expect(() => engine.setBuiltinInstrument(0, { waveform: 'sine' })).not.toThrow();
      expectNulRefusal(
        // @ts-expect-error deliberately invalid waveform name for runtime refusal.
        () => engine.setBuiltinInstrument(0, { waveform: `sine${NUL}x` }),
        'waveform',
      );
    });
  });
});

describe('embedded NUL refusal, by handoff kind', () => {
  it('a name lookup (mixer bus id)', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('drumBusSubgroup')), {
      sampleRate: 48000,
    });
    try {
      expect(() => mixer.busMeter('master')).not.toThrow();
      expect(() => mixer.busMeter('masterx')).toThrow();
      expectNulRefusal(() => mixer.busMeter(`master${NUL}x`), 'busId');
    } finally {
      mixer.destroy();
    }
  });

  it('a name lookup (mastering processor)', () => {
    expect(masteringInsertParamNames('utility.gain')).toEqual(
      masteringInsertParamNames('utility.gain'),
    );
    expectNulRefusal(() => addon.masteringInsertParamNames(`utility.gain${NUL}x`), 'name');
    // The facade resolves the closed token set first, so a NUL-bearing name is an unknown name.
    expect(() => masteringInsertParamNames(`utility.gain${NUL}x` as never)).toThrow(RangeError);
  });

  it('a JSON key (EQ band)', () => {
    withNativeEngine((engine) => {
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
    expect(addon._synthPatchRoundTrip({ preset: 'piano' }).preset).toBe('piano');
    expectNulRefusal(() => addon._synthPatchRoundTrip({ preset: `piano${NUL}x` }), 'preset');
    expectNulRefusal(() => addon._synthPatchRoundTrip(`piano${NUL}x`), 'preset');
  });

  it('a fixed-buffer copy (marker name)', () => {
    withEngine((engine) => {
      expectNulRefusal(
        () => engine.setMarkers([{ id: 1, ppq: 0, name: `verse${NUL}x`, kind: 'marker' }]),
        'name',
      );
    });
  });

  it('a path', () => {
    const dir = mkdtempSync(join(tmpdir(), 'sonare-nul-path-'));
    try {
      const wavPath = join(dir, 'tone.wav');
      writePcm16Wav(wavPath, [sine(440, 0.05)], 44100);
      expect(() => Audio.fromFile(wavPath)).not.toThrow();
      expectNulRefusal(() => Audio.fromFile(`${wavPath}${NUL}x`), 'path');
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });

  it('a string handed to C++ as std::string (voice changer preset id)', () => {
    const [id] = realtimeVoiceChangerPresetNames();
    expect(realtimeVoiceChangerPresetJson(id).length).toBeGreaterThan(0);
    // The facade resolves the id against the closed token set, so a NUL-bearing id is an unknown id.
    expectUnknownPresetRefusal(() => realtimeVoiceChangerPresetJson(`${id}${NUL}x` as typeof id));
  });
});

describe('embedded NUL refusal, text handed to the core as std::string', () => {
  it('a note name, scene preset, voice preset and JSON text are refused', () => {
    const [id] = realtimeVoiceChangerPresetNames();
    expectNulRefusal(() => noteToHz(`A4${NUL}junk`), 'note');
    expectNulRefusal(() => addon.mixingScenePresetJson(`vocalReverbSend${NUL}junk`), 'preset');
    expect(() => mixingScenePreset(`vocalReverbSend${NUL}junk` as never)).toThrow(RangeError);
    expectUnknownPresetRefusal(() =>
      realtimeVoiceChangerPresetConfig(`${id}${NUL}junk` as typeof id),
    );
    expectNulRefusal(() => realtimeVoiceChangerValidatePresetJson(`{}${NUL}junk`), 'presetJson');
    const json = new Project().toJson();
    expectNulRefusal(() => Project.fromJson(`${json}${NUL}junk`), 'json');
  });
});

describe('embedded NUL refusal, scan', () => {
  const READER_FILE = 'sonare_wrap_options.h';

  /** Every raw `Utf8Value()` read outside the shared reader header, as file:line. */
  function rawUtf8Reads(): { scanned: number; matches: string[] } {
    const sources = addonSources();
    const matches: string[] = [];
    let scanned = 0;
    for (const { file, text } of sources.filter((source) => source.file !== READER_FILE)) {
      scanned += 1;
      text.split('\n').forEach((line, index) => {
        if (/\bUtf8Value\s*\(/.test(line.replace(/\/\/.*$/, ''))) {
          matches.push(`${file}:${index + 1}`);
        }
      });
    }
    return { scanned, matches };
  }

  it('reads no JS string outside the shared readers', () => {
    const { scanned, matches } = rawUtf8Reads();
    expect(scanned, 'the scan must reach the addon sources').toBeGreaterThan(20);
    expect(matches).toEqual([]);
  });

  it('keeps the shared readers as the one place a string is decoded', () => {
    const header = addonSources().find((source) => source.file === READER_FILE);
    expect(header, `${READER_FILE} must be among the scanned sources`).toBeDefined();
    expect(header?.text).toMatch(/must not contain NUL|node_nul_message/);
  });
});
