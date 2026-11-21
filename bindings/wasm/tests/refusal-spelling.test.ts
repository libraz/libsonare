/**
 * Every row of the generated spelling table reaches a refusal that names the caller's
 * spelling and never the C-ABI subject.
 *
 * The rows are parsed from `src/wasm/field_spelling.h` (`kOverrides` and `kLengths`) at run time, so a row the
 * generator adds is swept without a pin: it needs a driver below or an `UNDRIVEN` entry, and
 * the sweep fails until it has one. Each driver makes a fresh valid call of the facade that
 * reaches the C entry, with the one argument the caller spells `spelling` set to a value.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  Audio,
  alignTakeToReference,
  downmix,
  init,
  Mixer,
  melToAudio,
  melToStft,
  meteringSpectrumFrame,
  mixingScenePreset,
  noteTargetsFromSmf,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  Project,
  RealtimeEngine,
  RealtimeVoiceChanger,
  realtimeVoiceChangerRender,
  StreamAnalyzer,
  stft,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index';

const SOURCE = new URL('../../../src/wasm/field_spelling.h', import.meta.url).pathname;

interface Row {
  entry: string;
  subject: string;
  spelling: string;
}

/**
 * The rows of one generated table in table order: `kOverrides` renames a subject, `kLengths`
 * names a count by the array whose length it is.
 */
function parseRows(table: 'kOverrides' | 'kLengths'): Row[] {
  const text = readFileSync(SOURCE, 'utf8');
  const start = text.indexOf(`${table} = `);
  const end = text.indexOf('}};', start);
  if (start < 0 || end < 0) {
    throw new Error(`no ${table} block found in ${SOURCE}`);
  }
  const rows: Row[] = [];
  const row = /\{"([A-Za-z0-9_.]+)",\s*"([A-Za-z0-9_.]+)",\s*"([A-Za-z0-9_.]+)"\}/g;
  for (const m of text.slice(start, end).matchAll(row)) {
    rows.push({ entry: m[1], subject: m[2], spelling: m[3] });
  }
  return rows;
}

const RATE = 22050;
const TONE = new Float32Array(RATE / 2).map(
  (_, i) => 0.5 * Math.sin((2 * Math.PI * 440 * i) / RATE),
);
const BLOCK = TONE.slice(0, 128);
const MEL = { data: new Float32Array(128 * 4).fill(1), rows: 128, cols: 4 };

/** Releases a handle once `fn` has run, whether or not it threw. */
function using<T extends { destroy(): void }, R>(handle: T, fn: (handle: T) => R): R {
  try {
    return fn(handle);
  } finally {
    handle.destroy();
  }
}

const withEngine = (fn: (engine: RealtimeEngine) => unknown) =>
  using(new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 }), fn);

const withMixer = (fn: (mixer: Mixer) => unknown) => using(new Mixer({ sampleRate: 48000 }), fn);

/** A mixer holding strip `a` with one send (index 0) to bus `bus`. */
const withSendMixer = (fn: (mixer: Mixer) => unknown) =>
  withMixer((mixer) => {
    mixer.addStrip('a');
    mixer.addBus('bus');
    mixer.addSend('a', { id: 's', destinationBusId: 'bus' });
    return fn(mixer);
  });

const withRenderer = (fn: (renderer: PlaybackRenderer) => unknown) =>
  using(
    new PlaybackRenderer({
      sampleRate: 48000,
      config: { input: { layout: 'stereo' }, target: { kind: 'speakers', layout: 'stereo' } },
    }),
    fn,
  );

const withAnalyzer = (fn: (analyzer: StreamAnalyzer) => unknown) =>
  using(new StreamAnalyzer({ sampleRate: RATE, nFft: 512, hopLength: 256 }), fn);

const withChanger = (fn: (changer: RealtimeVoiceChanger) => unknown) =>
  using(new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, maxBlockSize: 256 }), fn);

/** A vocal-edit request valid in every key but `spelling`, which carries `value`. */
const vocalEditRequest = (spelling: string, value: unknown) => ({
  clipId: 3,
  expectedSourceId: 4,
  expectedSourceSampleRate: 16000,
  expectedSourceSampleCount: 4,
  expectedSourceSha256: 'ab'.repeat(32),
  expectedClipLengthPpq: 4,
  expectedSourceOffsetPpq: 0,
  renderedMono: new Float32Array([0.1, -0.2, 0.3, -0.4]),
  renderedSampleRate: 16000,
  renderToken: {
    sessionEpoch: '17',
    revision: '9',
    draftId: '0',
    generation: '0',
    requestId: '0',
    profileId: 1,
  },
  [spelling]: value,
});

/** A fresh valid call of the facade reaching a C entry, with `spelling` set to `value`. */
type Driver = (spelling: string, value: unknown) => unknown;

/** Request-object drivers: every key but `spelling` is valid, and `spelling` carries `value`. */
const DRIVERS: Record<string, Driver> = {
  align_take_to_reference: (spelling, value) =>
    alignTakeToReference({
      reference: TONE,
      take: TONE,
      sampleRate: RATE,
      [spelling]: value,
    } as never),
  audio_spectrum_frame: (_spelling, value) =>
    using(Audio.fromBuffer(TONE, RATE), (audio) => audio.spectrumFrame(value as number)),
  downmix: (_spelling, value) => downmix(value as Float32Array[], 'mono'),
  engine_drain_external_midi: (_spelling, value) =>
    withEngine((engine) => engine.drainExternalMidi(value as number)),
  engine_process: (_spelling, value) =>
    using(new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 }), (engine) =>
      engine.processWithMonitor(value as Float32Array[]),
    ),
  engine_resolve_master_automation_id: (_spelling, value) =>
    withEngine((engine) => engine.resolveStripAutomationId('master', value as never)),
  engine_resolve_track_lane_automation_id: (_spelling, value) =>
    withEngine((engine) => engine.resolveStripAutomationId(1, value as never)),
  engine_set_controller_velocity_meaningful: (_spelling, value) =>
    withEngine((engine) => engine.setControllerVelocityMeaningful(0, value as boolean)),
  engine_set_external_midi_clock_enabled: (_spelling, value) =>
    withEngine((engine) => engine.setExternalMidiClockEnabled(value as boolean)),
  engine_set_midi_destination_external: (_spelling, value) =>
    withEngine((engine) => engine.setMidiDestinationExternal(0, value as boolean)),
  engine_set_track_output_bus: (_spelling, value) =>
    withEngine((engine) => engine.setTrackOutputBus(1, value as number)),
  mel_to_audio: (spelling, value) => melToAudio({ power: MEL, [spelling]: value } as never),
  mel_to_stft: (spelling, value) => melToStft({ power: MEL, [spelling]: value } as never),
  metering_spectrum_frame: (spelling, value) =>
    meteringSpectrumFrame({ samples: TONE, sampleRate: RATE, [spelling]: value } as never),
  mixer_add_strip: (spelling, value) =>
    withMixer((mixer) => mixer.addStrip('strip', { [spelling]: value } as never)),
  mixer_process_stereo: (spelling, value) =>
    withSendMixer((mixer) =>
      spelling === 'leftInputs'
        ? mixer.processStereo([value] as never, [new Float32Array(4)])
        : mixer.processStereo([new Float32Array(4)], [value] as never),
    ),
  mixing_scene_preset_json: (_spelling, value) => mixingScenePreset(value as never),
  note_targets_from_smf: (spelling, value) => noteTargetsFromSmf({ [spelling]: value } as never),
  playback_loudness_meter_push_interleaved: (_spelling, value) =>
    using(new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 }), (meter) =>
      meter.pushInterleaved(value as Float32Array),
    ),
  playback_renderer_process_interleaved: (_spelling, value) =>
    withRenderer((renderer) => renderer.processInterleaved(value as Float32Array, 2)),
  playback_renderer_process_planar: (_spelling, value) =>
    withRenderer((renderer) => renderer.processPlanar(value as Float32Array[])),
  playback_renderer_render_offline: (_spelling, value) =>
    withRenderer((renderer) => renderer.renderOffline(value as Float32Array, 2)),
  project_apply_vocal_edit: (spelling, value) =>
    using(new Project(), (project) =>
      project.applyVocalEdit(vocalEditRequest(spelling, value) as never),
    ),
  project_import_clip_file: (_spelling, value) =>
    using(new Project(), (project) => project.importClipFile(value as Uint8Array)),
  project_import_smf: (_spelling, value) =>
    using(new Project(), (project) => project.importSmf(value as Uint8Array)),
  project_move_clip: (spelling, value) =>
    using(new Project(), (project) => project.moveClip(1, 0, { [spelling]: value } as never)),
  project_set_marker: (spelling, value) =>
    using(new Project(), (project) => project.setMarker({ ppq: 0, [spelling]: value } as never)),
  project_set_overlap_policy: (_spelling, value) =>
    using(new Project(), (project) => project.setOverlapPolicy(value as never)),
  realtime_voice_changer_process_mono: (_spelling, value) =>
    withChanger((changer) => changer.processMono(value as Float32Array)),
  stream_analyzer_process: (_spelling, value) =>
    withAnalyzer((analyzer) => analyzer.process(BLOCK, value as number)),
  stft: (spelling, value) => stft({ samples: TONE, sampleRate: RATE, [spelling]: value } as never),
  stream_analyzer_reset: (_spelling, value) =>
    withAnalyzer((analyzer) => analyzer.reset(value as number)),
  strip_remove_send: (_spelling, value) =>
    withSendMixer((mixer) => mixer.removeSend('a', value as number)),
  strip_set_send_db: (_spelling, value) =>
    withSendMixer((mixer) => mixer.setSendDb('a', value as number, 0)),
  realtime_voice_changer_render: (spelling, value) =>
    realtimeVoiceChangerRender({
      channels: [BLOCK],
      sampleRate: 48000,
      [spelling]: value,
    } as never),
  waveform_peak_pyramid: (spelling, value) => waveformPeakPyramid({ [spelling]: value } as never),
  waveform_peaks: (spelling, value) => waveformPeaks({ [spelling]: value } as never),
};

/** Values outside any domain: numbers, a string, empty and malformed buffers, wrong-shaped planes. */
const OUTSIDE: unknown[] = [
  0,
  -1,
  -1.5,
  Number.NaN,
  1e30,
  'nope',
  null,
  {},
  [],
  new Uint8Array([1, 2, 3]),
  new Uint8Array(0),
  new Float32Array(0),
  [new Float32Array(0)],
  { data: new Float32Array(0), rows: 0, cols: 0 },
  Array.from({ length: 65 }, () => new Float32Array(128)),
];

/**
 * Rows no value can make refuse as a parameter, or that need live state the harness cannot
 * build. Keyed `"entry.subject"`; the reason is one of the three fixed phrases.
 */
const UNDRIVEN: Record<string, string> = {};

const REASONS = ['no value refuses it', 'needs live state', 'content refusal carries no subject'];

/** Whether `text` names `word` where a refusal puts its subject, not inside its prose. */
function names(text: string, word: string): boolean {
  const escaped = word.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return new RegExp(`(^[A-Za-z_.]+: |unknown key |, |\\{)${escaped}\\b`).test(text);
}

/** Every message the driver throws across the outside values. */
function refusals(driver: Driver, spelling: string): string[] {
  const texts: string[] = [];
  for (const value of OUTSIDE) {
    try {
      driver(spelling, value);
    } catch (error) {
      if (error instanceof Error) {
        texts.push(error.message);
      }
    }
  }
  return texts;
}

describe('refusal spelling', () => {
  const rows = [...parseRows('kOverrides'), ...parseRows('kLengths')];
  let driven = 0;
  let undriven = 0;

  beforeAll(async () => {
    await init();
  });

  it('parses the spelling table', () => {
    expect(rows.length).toBeGreaterThan(0);
  });

  it.each(rows.map((r) => [`${r.entry}.${r.subject} -> ${r.spelling}`, r] as const))(
    'refuses %s under the caller spelling',
    (_label, { entry, subject, spelling }) => {
      const key = `${entry}.${subject}`;
      if (key in UNDRIVEN) {
        undriven += 1;
        return;
      }
      const driver = DRIVERS[entry];
      if (driver === undefined) {
        throw new Error(`row ${key} -> ${spelling} has no driver and is not in UNDRIVEN`);
      }
      driven += 1;
      const texts = refusals(driver, spelling);
      expect(
        texts.filter((t) => names(t, subject)),
        `${key} leaked the C subject`,
      ).toEqual([]);
      expect(
        texts.some((t) => names(t, spelling)),
        `${key} never named ${spelling}: ${JSON.stringify(texts)}`,
      ).toBe(true);
    },
  );

  it('accounts for every row', () => {
    const keys = new Set(rows.map((r) => `${r.entry}.${r.subject}`));
    const stale = Object.keys(UNDRIVEN).filter((k) => !keys.has(k));
    const badReason = Object.values(UNDRIVEN).filter((r) => !REASONS.includes(r));
    const unaccounted = rows
      .filter((r) => !(`${r.entry}.${r.subject}` in UNDRIVEN) && !(r.entry in DRIVERS))
      .map((r) => `${r.entry}.${r.subject}`);
    // eslint-disable-next-line no-console
    console.log(`refusal spelling sweep: driven=${driven} undriven=${undriven}`);
    expect(stale).toEqual([]);
    expect(badReason).toEqual([]);
    expect(unaccounted).toEqual([]);
    expect(driven).toBeGreaterThan(0);
  });
});
