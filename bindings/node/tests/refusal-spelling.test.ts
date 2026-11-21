/**
 * Every row of the generated spelling table reaches a refusal that names the caller's spelling.
 *
 * The rows are read from `src/addon/field_spelling.h` at run time, so a row the generator adds is
 * swept without a pin: it needs a driver below (or an `UNDRIVEN` entry with a reason) or the sweep
 * fails. Each row's spelling is given values outside any domain until the library refuses one;
 * every refusal on the way must name the spelling and never the C-ABI subject.
 */
import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import {
  Audio,
  alignTakeToReference,
  downmix,
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
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index.js';

const TABLE_PATH = new URL('../src/addon/field_spelling.h', import.meta.url).pathname;

interface Row {
  entry: string;
  subject: string;
  spelling: string;
}

/**
 * The rows of one generated table, `{"<entry>", "<subject>", "<spelling>"}`: `kOverrides` renames
 * a subject, `kLengths` names a count by the array whose length it is.
 */
function parseRows(table: 'kOverrides' | 'kLengths'): Row[] {
  const text = readFileSync(TABLE_PATH, 'utf8');
  const start = text.indexOf(`${table} = `);
  const open = text.indexOf('{{', start);
  const close = text.indexOf('}};', open);
  if (start < 0 || open < 0 || close < 0) {
    throw new Error(`no ${table} block in ${TABLE_PATH}`);
  }
  const block = text.slice(open + 2, close);
  const rows: Row[] = [];
  for (const m of block.matchAll(/\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}/g)) {
    rows.push({ entry: m[1], subject: m[2], spelling: m[3] });
  }
  return rows;
}

const ROWS = [...parseRows('kOverrides'), ...parseRows('kLengths')];

const SR = 22050;
const TONE = Float32Array.from({ length: SR / 2 }, (_, i) =>
  Math.fround(0.5 * Math.sin((2 * Math.PI * 440 * i) / SR)),
);
const MEL = { data: new Float32Array(128 * 4).fill(1), rows: 128, cols: 4 };

/** A well-formed Standard MIDI File header, so only the swept key can be the refused one. */
const SMF = Uint8Array.from([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96]);

const OUTSIDE: unknown[] = [0, -1, -1.5, Number.NaN, 1e30, 'nope'];

/** A `Float32Array` holding a non-finite sample, which a sample buffer refuses. */
const NON_FINITE = Float32Array.of(Number.NaN, 0);

/** Candidates beyond `OUTSIDE` for a spelling that is a byte or sample buffer or a plane set. */
const EXTRA_OUTSIDE: Record<string, unknown[]> = {
  downmix: [[], [new Float32Array(0)], [new Float32Array(4), new Float32Array(5)]],
  engine_drain_external_midi: [1, 2.5],
  engine_process: [[], [new Float32Array(0)], [new Float32Array(1 << 20)]],
  mel_to_audio: [
    { data: new Float32Array(0), rows: 0, cols: 0 },
    { data: MEL.data, rows: 3 },
  ],
  mel_to_stft: [
    { data: new Float32Array(0), rows: 0, cols: 0 },
    { data: MEL.data, rows: 3 },
  ],
  note_targets_from_smf: [new Uint8Array(0), new Uint8Array([1, 2, 3])],
  playback_loudness_meter_push_interleaved: [new Float32Array(0), NON_FINITE],
  playback_renderer_process_interleaved: [new Float32Array(0), NON_FINITE],
  playback_renderer_process_planar: [
    [],
    [new Float32Array(0)],
    [new Float32Array(4), new Float32Array(5)],
  ],
  playback_renderer_render_offline: [new Float32Array(0), NON_FINITE],
  project_apply_vocal_edit: [new Uint8Array(0), new Uint8Array([1, 2, 3])],
  project_import_clip_file: [new Uint8Array(0), new Uint8Array([1, 2, 3])],
  project_import_smf: [new Uint8Array(0), new Uint8Array([1, 2, 3])],
  realtime_voice_changer_render: [{}, []],
  mixer_process_stereo: [[new Float32Array(4)], [new Float32Array(0)], [{}]],
  waveform_peak_pyramid: [[], [new Float32Array(0)], [new Float32Array(4), new Float32Array(5)]],
  waveform_peaks: [[], [new Float32Array(0)], [new Float32Array(4), new Float32Array(5)]],
};

/** The request with every key valid except `spelling`, which carries `value`. */
const given = (base: Record<string, unknown>, spelling: string, value: unknown) => ({
  ...base,
  [spelling]: value,
});

/** Runs `fn` against a handle and releases the handle whether or not the call throws. */
function withHandle<T extends { destroy(): void }>(handle: T, fn: (h: T) => unknown): unknown {
  try {
    return fn(handle);
  } finally {
    handle.destroy();
  }
}

const withEngine = (fn: (engine: RealtimeEngine) => unknown) =>
  withHandle(new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 }), fn);

const withMixer = (fn: (mixer: Mixer) => unknown) =>
  withHandle(new Mixer({ sampleRate: 48000 }), fn);

/** A mixer holding strip `a` with one send (index 0) to bus `bus`. */
const withSendMixer = (fn: (mixer: Mixer) => unknown) =>
  withMixer((mixer) => {
    mixer.addStrip('a');
    mixer.addBus('bus');
    mixer.addSend('a', { id: 's', destinationBusId: 'bus' });
    return fn(mixer);
  });

const withRenderer = (fn: (renderer: PlaybackRenderer) => unknown) =>
  withHandle(new PlaybackRenderer({ sampleRate: 48000, config: {} }), fn);

const withAnalyzer = (fn: (analyzer: StreamAnalyzer) => unknown) =>
  withHandle(new StreamAnalyzer({ sampleRate: SR, nFft: 512, hopLength: 256 }), fn);

const withChanger = (fn: (changer: RealtimeVoiceChanger) => unknown) =>
  withHandle(
    new RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1, maxBlockSize: 256 }),
    fn,
  );

/**
 * A fresh valid call of the facade reaching each C entry, every argument valid except the one
 * the caller spells `spelling`, which gets `value`. Add a row's driver here when the generator adds
 * the row.
 */
const DRIVERS: Record<string, (spelling: string, value: unknown) => unknown> = {
  align_take_to_reference: (s, v) =>
    alignTakeToReference(given({ reference: TONE, take: TONE }, s, v) as never),
  audio_spectrum_frame: (_s, v) =>
    withHandle(Audio.fromBuffer(TONE, SR), (audio) => audio.spectrumFrame(v as number)),
  downmix: (_s, v) => downmix(v as Float32Array[], 'mono'),
  engine_drain_external_midi: (_s, v) =>
    withEngine((engine) => engine.drainExternalMidi(v as number)),
  engine_process: (_s, v) =>
    withHandle(new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 }), (engine) =>
      engine.process(v as Float32Array[], { withMonitor: true }),
    ),
  engine_resolve_master_automation_id: (_s, v) =>
    withEngine((engine) => engine.resolveStripAutomationId('master', v as never)),
  engine_resolve_track_lane_automation_id: (_s, v) =>
    withEngine((engine) => engine.resolveStripAutomationId(1, v as never)),
  engine_set_controller_velocity_meaningful: (_s, v) =>
    withEngine((engine) => engine.setControllerVelocityMeaningful(0, v as boolean)),
  engine_set_external_midi_clock_enabled: (_s, v) =>
    withEngine((engine) => engine.setExternalMidiClockEnabled(v as boolean)),
  engine_set_midi_destination_external: (_s, v) =>
    withEngine((engine) => engine.setMidiDestinationExternal(0, v as boolean)),
  engine_set_track_output_bus: (_s, v) =>
    withEngine((engine) => engine.setTrackOutputBus(1, v as number)),
  mel_to_audio: (s, v) => melToAudio(given({ sampleRate: SR }, s, v) as never),
  mel_to_stft: (s, v) => melToStft(given({ sampleRate: SR }, s, v) as never),
  metering_spectrum_frame: (s, v) =>
    meteringSpectrumFrame(given({ samples: TONE, sampleRate: SR }, s, v) as never),
  mixer_add_strip: (s, v) => withMixer((mixer) => mixer.addStrip('strip', { [s]: v } as never)),
  mixer_process_stereo: (s, v) =>
    withSendMixer((mixer) =>
      s === 'leftInputs'
        ? mixer.processStereo([v] as never, [new Float32Array(4)])
        : mixer.processStereo([new Float32Array(4)], [v] as never),
    ),
  mixing_scene_preset_json: (_s, v) => mixingScenePreset(v as never),
  note_targets_from_smf: (s, v) => noteTargetsFromSmf(given({ data: SMF }, s, v) as never),
  playback_loudness_meter_push_interleaved: (_s, v) =>
    withHandle(new PlaybackLoudnessMeter({ numChannels: 2, sampleRate: 48000 }), (meter) =>
      meter.pushInterleaved(v as Float32Array),
    ),
  playback_renderer_process_interleaved: (_s, v) =>
    withRenderer((renderer) => renderer.processInterleaved(v as Float32Array, 2)),
  playback_renderer_process_planar: (_s, v) =>
    withRenderer((renderer) => renderer.processPlanar(v as Float32Array[])),
  playback_renderer_render_offline: (_s, v) =>
    withRenderer((renderer) => renderer.renderOffline(v as Float32Array, 2)),
  project_apply_vocal_edit: (s, v) =>
    withHandle(new Project(), (project) =>
      project.applyVocalEdit({ renderedMono: new Float32Array(4), [s]: v } as never),
    ),
  project_import_clip_file: (_s, v) =>
    withHandle(new Project(), (project) => project.importClipFile(v as Uint8Array)),
  project_import_smf: (_s, v) =>
    withHandle(new Project(), (project) => project.importSmf(v as Uint8Array)),
  project_move_clip: (s, v) =>
    withHandle(new Project(), (project) => project.moveClip(1, 0, { [s]: v } as never)),
  project_set_marker: (s, v) =>
    withHandle(new Project(), (project) => project.setMarker({ ppq: 0, [s]: v } as never)),
  project_set_overlap_policy: (_s, v) =>
    withHandle(new Project(), (project) => project.setOverlapPolicy(v as never)),
  realtime_voice_changer_process_mono: (_s, v) =>
    withChanger((changer) => changer.processMono(v as Float32Array)),
  stream_analyzer_process: (_s, v) =>
    withAnalyzer((analyzer) => analyzer.process(TONE.subarray(0, 512), v as number)),
  stream_analyzer_reset: (_s, v) => withAnalyzer((analyzer) => analyzer.reset(v as number)),
  strip_remove_send: (_s, v) => withSendMixer((mixer) => mixer.removeSend('a', v as number)),
  strip_set_send_db: (_s, v) => withSendMixer((mixer) => mixer.setSendDb('a', v as number, 0)),
  realtime_voice_changer_render: (s, v) =>
    realtimeVoiceChangerRender(
      given({ channels: [TONE.subarray(0, 512)], sampleRate: 48000 }, s, v) as never,
    ),
  waveform_peak_pyramid: (s, v) => waveformPeakPyramid(given({}, s, v) as never),
  waveform_peaks: (s, v) => waveformPeaks(given({}, s, v) as never),
};

const UNDRIVEN_REASONS = [
  'no value refuses it',
  'needs live state',
  'content refusal carries no subject',
];

/** Rows no value can make refuse as a parameter, keyed `"<entry>.<subject>"`. */
const UNDRIVEN: Record<string, string> = {};

const escapeRegExp = (text: string): string => text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/** Whether `text` names `word` where a refusal puts its subject, not inside its prose. */
function names(text: string, word: string): boolean {
  const path = '(?:[A-Za-z_]+(?:\\[\\d+\\])?\\.)*';
  return new RegExp(`(?:^[A-Za-z_.]+: |^|unknown key |, |\\{)${path}${escapeRegExp(word)}\\b`).test(
    text,
  );
}

function refusals(row: Row): string[] {
  const driver = DRIVERS[row.entry];
  const texts: string[] = [];
  for (const value of [...OUTSIDE, ...(EXTRA_OUTSIDE[row.entry] ?? [])]) {
    try {
      driver(row.spelling, value);
    } catch (error) {
      texts.push(error instanceof Error ? error.message : String(error));
    }
  }
  return texts;
}

const counts = { driven: 0, undriven: 0 };
const key = (row: Row): string => `${row.entry}.${row.subject}`;

describe('refusal spelling sweep', () => {
  it('parses the generated table', () => {
    expect(ROWS.length).toBeGreaterThan(0);
  });

  it.each(ROWS.map((row) => [key(row), row] as const))(
    '%s is refused under the caller spelling',
    (_name, row) => {
      const undrivenReason = UNDRIVEN[key(row)];
      if (undrivenReason !== undefined) {
        expect(UNDRIVEN_REASONS).toContain(undrivenReason);
        expect(DRIVERS[row.entry], `${key(row)} is undriven and must not also have a driver`).toBe(
          undefined,
        );
        counts.undriven += 1;
        return;
      }
      expect(
        DRIVERS[row.entry],
        `table row ${key(row)} -> ${row.spelling} has no driver and is not in UNDRIVEN`,
      ).toBeTypeOf('function');
      const texts = refusals(row);
      const leaked = texts.filter((t) => names(t, row.subject));
      expect(leaked, `${key(row)} refusals name the C subject`).toEqual([]);
      expect(
        texts.some((t) => names(t, row.spelling)),
        `${key(row)}: no refusal names ${row.spelling}; got ${JSON.stringify(texts)}`,
      ).toBe(true);
      counts.driven += 1;
    },
  );

  it('accounts for every row', () => {
    // biome-ignore lint/suspicious/noConsole: the sweep's driven/undriven totals are its report.
    console.log(`refusal spelling sweep: driven=${counts.driven} undriven=${counts.undriven}`);
    const keys = new Set(ROWS.map(key));
    const stale = Object.keys(UNDRIVEN).filter((k) => !keys.has(k));
    expect(stale, 'UNDRIVEN keys that are not rows of the table').toEqual([]);
    const unaccounted = ROWS.filter((r) => !(key(r) in UNDRIVEN) && !(r.entry in DRIVERS));
    expect(unaccounted.map(key)).toEqual([]);
    expect(counts.driven).toBeGreaterThan(0);
    expect(counts.driven + counts.undriven).toBe(ROWS.length);
  });
});
