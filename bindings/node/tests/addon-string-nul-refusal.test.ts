/**
 * An embedded NUL in any string the addon takes from JS is refused by name.
 *
 * The C ABI takes NUL-terminated strings, so a name, key, path or JSON text with
 * a NUL in it used to be cut at the NUL and succeed as its prefix. Each case
 * pairs the prefix, which must keep working, with the same text carrying a NUL,
 * which must be a RangeError naming the field. The scan at the foot keeps the
 * shared string readers the only route from a JS string to a C++ one.
 */

import { describe, expect, it } from 'vitest';
import {
  Audio,
  Mixer,
  masteringInsertParamNames,
  mixingScenePresetJson,
  RealtimeEngine,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  StreamingMasteringChain,
} from '../src/index.js';
import { addon } from '../src/native.js';
import { addonSources } from './_addon_sources.js';

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

const BAND_JSON = '{"type":"Peak","frequencyHz":1000,"gainDb":3,"q":1}';
const SCENE_JSON = JSON.stringify({
  version: 1,
  strips: [],
  buses: [{ id: '1', inserts: [] }],
  connections: [],
});
const WAV_PATH = new URL('../resampled.wav', import.meta.url).pathname;

describe('embedded NUL refusal, by reader', () => {
  it('node_narrow_string (positional) names the argument', () => {
    expect(masteringInsertParamNames('utility.gain').length).toBeGreaterThan(0);
    expectNulRefusal(() => masteringInsertParamNames(`utility.gain${NUL}x`), 'name');
  });

  it('OptionalStringArg names the argument', () => {
    withEngine((engine) => {
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
    const chain = new StreamingMasteringChain();
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
      expect(() => engine.setMarkers([{ id: 1, ppq: 0, name: 'a', kind: 0 }])).not.toThrow();
      expectNulRefusal(
        () => engine.setMarkers([{ id: 1, ppq: 0, name: `a${NUL}b`, kind: 0 }]),
        'name',
      );
    });
  });

  it('ReadBuiltinWaveform names the field', () => {
    withEngine((engine) => {
      expect(() => engine.setBuiltinInstrument({ waveform: 'sine' }, 0)).not.toThrow();
      expectNulRefusal(
        // @ts-expect-error deliberately invalid waveform name for runtime refusal.
        () => engine.setBuiltinInstrument({ waveform: `sine${NUL}x` }, 0),
        'waveform',
      );
    });
  });
});

describe('embedded NUL refusal, by handoff kind', () => {
  it('a name lookup (mixer bus id)', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'));
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
    expectNulRefusal(() => masteringInsertParamNames(`utility.gain${NUL}x`), 'name');
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
    expect(addon._synthPatchRoundTrip({ preset: 'piano' }).preset).toBe('piano');
    expectNulRefusal(() => addon._synthPatchRoundTrip({ preset: `piano${NUL}x` }), 'preset');
    expectNulRefusal(() => addon._synthPatchRoundTrip(`piano${NUL}x`), 'preset');
  });

  it('a fixed-buffer copy (marker name)', () => {
    withEngine((engine) => {
      expectNulRefusal(
        () => engine.setMarkers([{ id: 1, ppq: 0, name: `verse${NUL}x`, kind: 0 }]),
        'name',
      );
    });
  });

  it('a path', () => {
    expect(() => Audio.fromFile(WAV_PATH)).not.toThrow();
    expectNulRefusal(() => Audio.fromFile(`${WAV_PATH}${NUL}x`), 'path');
  });

  it('a string handed to C++ as std::string (voice changer preset id)', () => {
    const [id] = realtimeVoiceChangerPresetNames();
    expect(realtimeVoiceChangerPresetJson(id).length).toBeGreaterThan(0);
    expectNulRefusal(() => realtimeVoiceChangerPresetJson(`${id}${NUL}x` as typeof id), 'presetId');
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
