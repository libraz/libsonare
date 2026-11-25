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
  analyzePolyphonic,
  chordFunctions,
  createVocalEditSession,
  decomposeNotePitch,
  detectKey,
  detectKeyCandidates,
  downmix,
  encodeAudio,
  estimateMeter,
  lufsMultichannel,
  lufsSeriesMultichannel,
  Mixer,
  masteringAbMatchLoudness,
  masteringAbMatchLoudnessStereo,
  masteringAnalyzePair,
  masteringApplyPairProcessor,
  masteringApplyPairProcessorStereo,
  melToAudio,
  melToStft,
  mergeNotes,
  meteringSpectrumFrame,
  midiCc,
  midiCcLearn,
  midiRouteEvents,
  mixingScenePreset,
  noteTargetsFromSmf,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  Project,
  pcen,
  pitchCorrectTimevarying,
  RealtimeEngine,
  RealtimeVoiceChanger,
  realtimeVoiceChangerRender,
  renderNotes,
  renderPercussiveEvents,
  renderPlayback,
  roomMorph,
  roomMorphStereo,
  SampleBank,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  splitNote,
  synthesizeRir,
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
 * a subject, `kLengths` names a count by the array whose length it is, and `kQuotients`
 * (`{..., "<array>", "<divisor>"}`) by that length over another argument.
 */
function parseRows(table: 'kOverrides' | 'kLengths' | 'kQuotients'): Row[] {
  const text = readFileSync(TABLE_PATH, 'utf8');
  const start = text.indexOf(`${table} = `);
  const open = text.indexOf('{{', start);
  const close = text.indexOf('}};', open);
  if (start < 0 || open < 0 || close < 0) {
    throw new Error(`no ${table} block in ${TABLE_PATH}`);
  }
  const block = text.slice(open + 2, close);
  const rows: Row[] = [];
  const row = /\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*(?:,\s*"([^"]+)"\s*)?\}/g;
  for (const m of block.matchAll(row)) {
    const spelling = m[4] === undefined ? m[3] : `${m[3]}.length / ${m[4]}`;
    rows.push({ entry: m[1], subject: m[2], spelling });
  }
  return rows;
}

const ROWS = [...parseRows('kOverrides'), ...parseRows('kLengths'), ...parseRows('kQuotients')];

const SR = 22050;
const TONE = Float32Array.from({ length: SR / 2 }, (_, i) =>
  Math.fround(0.5 * Math.sin((2 * Math.PI * 440 * i) / SR)),
);
const MEL = { data: new Float32Array(128 * 4).fill(1), rows: 128, cols: 4 };

/** A well-formed Standard MIDI File header, so only the swept key can be the refused one. */
const SMF = Uint8Array.from([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96]);

const F0 = new Float32Array(20).fill(220);
const VOICED_PROB = new Float32Array(20).fill(1);
const NOTE = { onsetSample: 0, offsetSample: 5000, frameStart: 0, frameEnd: 10, medianHz: 220 };
const NOTE_TRACK = { samples: TONE, sampleRate: SR, f0Hz: F0, frameRate: 40 };
const EMPTY = new Float32Array(0);
const LONG = new Float32Array(1 << 20);

/** A non-empty valid pair of planes at a sample rate every pair entry accepts. */
const PAIR = { source: TONE, reference: TONE, sampleRate: SR };
const STEREO_PAIR = {
  sourceLeft: TONE,
  sourceRight: TONE,
  referenceLeft: TONE,
  referenceRight: TONE,
  sampleRate: SR,
};

/** The room every acoustic entry accepts: small, with source and listener placed inside it. */
const ROOM = {
  lengthM: 4,
  widthM: 3,
  heightM: 2.5,
  ismOrder: 1,
  maxSec: 0.2,
  source: { x: 1, y: 1, z: 1.2 },
  listener: { x: 3, y: 2, z: 1.2 },
};

const METER_OBSERVATIONS = {
  accent: Array.from({ length: 8 }, (_, i) => (i % 4 ? 0.5 : 1)),
  hasAttack: [] as boolean[],
  lowFrequencyEnergy: [] as number[],
  subdivisionAccent: [] as number[],
  chordChange: [] as number[],
};
const MIDI_CC_EVENTS = [
  midiCc({ ppq: 0, group: 0, channel: 2, controller: 1, value: 64 }),
  midiCc({ ppq: 0.1, group: 0, channel: 2, controller: 33, value: 12 }),
];

const OUTSIDE: unknown[] = [0, -1, -1.5, Number.NaN, 1e30, 'nope'];

/** A `Float32Array` holding a non-finite sample, which a sample buffer refuses. */
const NON_FINITE = Float32Array.of(Number.NaN, 0);

/** Candidates beyond `OUTSIDE` for a spelling that is a byte or sample buffer or a plane set. */
const EXTRA_OUTSIDE: Record<string, unknown[]> = {
  align_take_to_reference: [EMPTY],
  chord_functions: [[]],
  decompose_note_pitch: [EMPTY],
  detect_key: [[]],
  detect_key_candidates: [[]],
  engine_set_bus_strip_json: [{ inserts: [{}] }],
  engine_set_master_strip_json: [{ inserts: [{}] }],
  engine_set_track_strip_json: [{ inserts: [{}] }],
  encode_audio: [[], Array.from({ length: 9 }, () => TONE)],
  engine_push_midi_sysex: [new Uint8Array(0), new Uint8Array(1 << 20)],
  engine_render_offline: [[], Array.from({ length: 65 }, () => new Float32Array(16))],
  engine_set_track_lanes: [Array.from({ length: 33 }, (_, i) => ({ trackId: i + 1 }))],
  eq_magnitude_response: [EMPTY, LONG],
  eq_match: [EMPTY],
  eq_process: [EMPTY, LONG],
  eq_set_sidechain: [EMPTY, LONG],
  estimate_meter_json: [[], Array(17).fill(4)],
  mastering_ab_match_loudness: [EMPTY],
  mastering_ab_match_loudness_stereo: [EMPTY],
  mastering_analyze_pair: [EMPTY],
  mastering_analyze_pair_stereo: [EMPTY, LONG],
  mastering_apply_pair_processor: [EMPTY],
  mastering_apply_pair_processor_stereo: [EMPTY],
  merge_notes: [EMPTY, []],
  playback_render_interleaved: [EMPTY, NON_FINITE],
  polyphonic_set_note_edit: [Float32Array.of(-1)],
  midi_cc_learn: [[]],
  midi_route_events: [[]],
  pitch_correct_timevarying: [EMPTY],
  project_add_automation_lane: [[]],
  project_annotate_chords: [[]],
  project_annotate_keys: [[]],
  project_annotate_sections: [[{}]],
  project_bounce: [[]],
  project_edit_automation_lane: [[]],
  project_import_external_stems: [[]],
  project_set_assist_sidecar: [new Uint8Array(0)],
  project_set_warp_map: [[]],
  render_notes: [EMPTY, []],
  render_percussive_events: [[]],
  room_morph: [EMPTY, new Float32Array(1 << 10)],
  room_morph_stereo: [EMPTY, new Float32Array(1 << 10)],
  sample_bank_add_sample: [EMPTY],
  split_note: [EMPTY, []],
  streaming_mastering_chain_process_mono: [EMPTY, LONG],
  streaming_mastering_chain_process_stereo: [EMPTY, LONG],
  streaming_retune_process_mono: [EMPTY, LONG],
  synthesize_rir: [EMPTY, new Float32Array(1 << 10)],
  vocal_draft_apply: [[]],
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

const withProject = (fn: (project: Project) => unknown) => withHandle(new Project(), fn);

const withEq = (fn: (eq: StreamingEqualizer) => unknown) =>
  withHandle(new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 128 }), fn);

const withChain = (numChannels: 1 | 2, fn: (chain: StreamingMasteringChain) => unknown) =>
  withHandle(
    new StreamingMasteringChain({ sampleRate: 48000, maxBlockSize: 128, numChannels }),
    fn,
  );

/** A vocal-edit draft over the smallest session the draft accepts, released after `fn`. */
function withDraft(
  fn: (draft: ReturnType<ReturnType<typeof createVocalEditSession>['beginEdit']>) => unknown,
) {
  const frames = Math.ceil(4096 / 128) + 1;
  const session = createVocalEditSession({
    samples: new Float32Array(4096).map((_, i) => 0.25 * Math.sin((2 * Math.PI * 440 * i) / 16000)),
    sampleRate: 16000,
    frameLengthSamples: 256,
    hopLengthSamples: 128,
    analysis: {
      frameOriginSample: 0,
      samplesPerFrame: 128,
      frameLengthSamples: 256,
      f0Hz: new Float32Array(frames).fill(440),
      voiced: new Uint8Array(frames).fill(1),
      algorithmId: 'libsonare.pyin',
      algorithmVersion: 1,
    },
  });
  try {
    return fn(session.beginEdit());
  } finally {
    session.destroy();
  }
}

const withTrack = (fn: (project: Project, trackId: number) => unknown) =>
  withProject((project) => fn(project, project.addTrack({ kind: 'audio' })));

const PARAM = { name: 'x', value: 0 };
/** A sidechain source whose `kind` or `id` (named by the spelling) is the swept value. */
const source = (spelling: string, value: unknown) =>
  spelling === 'source.kind' ? { kind: value, id: 2 } : { kind: 'track', id: value };
/** An insert parameter whose `name` is the swept value. */
const param = (_spelling: string, value: unknown) => ({ ...PARAM, name: value }) as never;
const RIG = { destinationId: 0, part: 0, mode: 'chain' } as const;
const SCENE_STRIP = { id: 'a' };

/**
 * A fresh valid call of the facade reaching each C entry, every argument valid except the one
 * the caller spells `spelling`, which gets `value`. Add a row's driver here when the generator adds
 * the row.
 */
const DRIVERS: Record<string, (spelling: string, value: unknown) => unknown> = {
  chord_functions: (_s, v) =>
    chordFunctions({ chords: v, key: { root: 0, mode: 'major' } } as never),
  decompose_note_pitch: (s, v) =>
    decomposeNotePitch(given({ f0Hz: F0, frameRate: 40, medianHz: 220 }, s, v) as never),
  detect_key: (s, v) => detectKey(given({ samples: TONE, sampleRate: SR }, s, v) as never),
  detect_key_candidates: (s, v) =>
    detectKeyCandidates(given({ samples: TONE, sampleRate: SR }, s, v) as never),
  engine_apply_track_strip_insert_param_by_name_now: (s, v) =>
    withEngine((engine) =>
      s === 'ref'
        ? engine.applyStripInsertParamNow(v as never, 0, PARAM)
        : engine.applyStripInsertParamNow(1, 0, param(s, v)),
    ),
  engine_apply_master_strip_insert_param_by_name_now: (s, v) =>
    withEngine((engine) => engine.applyStripInsertParamNow('master', 0, param(s, v))),
  engine_apply_bus_strip_insert_param_by_name_now: (s, v) =>
    withEngine((engine) => engine.applyBusInsertParamNow(1, 0, param(s, v))),
  engine_can_set_bus_sidechain: (s, v) =>
    withEngine((engine) => engine.canSetBusSidechain(1, 0, source(s, v) as never)),
  engine_can_set_lane_sidechain: (s, v) =>
    withEngine((engine) =>
      s === 'ref'
        ? engine.canSetStripSidechain(v as never, 0, { kind: 'track', id: 2 })
        : engine.canSetStripSidechain(1, 0, source(s, v) as never),
    ),
  engine_can_set_master_sidechain: (s, v) =>
    withEngine((engine) => engine.canSetStripSidechain('master', 0, source(s, v) as never)),
  engine_clear_track_insert_parameter_bases: (_s, v) =>
    withEngine((engine) => engine.clearStripInsertParameterBases(v as never)),
  engine_push_midi_sysex: (_s, v) =>
    withEngine((engine) => engine.pushMidiSysex(0, v as Uint8Array)),
  engine_render_offline: (_s, v) =>
    withEngine((engine) => engine.renderOffline({ channels: v } as never)),
  engine_resolve_track_insert_automation_id: (_s, v) =>
    withEngine((engine) => engine.resolveStripInsertAutomationId(v as never, 0, 'x')),
  engine_restore_track_strip_insert_param_by_name: (s, v) =>
    withEngine((engine) =>
      s === 'ref'
        ? engine.restoreStripInsertParam(v as never, 0, PARAM)
        : engine.restoreStripInsertParam(1, 0, param(s, v)),
    ),
  engine_restore_master_strip_insert_param_by_name: (s, v) =>
    withEngine((engine) => engine.restoreStripInsertParam('master', 0, param(s, v))),
  engine_restore_bus_strip_insert_param_by_name: (s, v) =>
    withEngine((engine) => engine.restoreBusInsertParam(1, 0, param(s, v))),
  engine_set_bus_strip_json: (_s, v) =>
    withEngine((engine) => {
      engine.setTrackBuses([{ busId: 1 }]);
      return engine.setBusStrip(1, v as never);
    }),
  engine_set_bus_sidechain: (s, v) =>
    withEngine((engine) => engine.setBusSidechain(1, 0, source(s, v) as never)),
  engine_set_lane_sidechain: (s, v) =>
    withEngine((engine) =>
      s === 'ref'
        ? engine.setStripSidechain(v as never, 0, { kind: 'track', id: 2 })
        : engine.setStripSidechain(1, 0, source(s, v) as never),
    ),
  engine_set_master_sidechain: (s, v) =>
    withEngine((engine) => engine.setStripSidechain('master', 0, source(s, v) as never)),
  engine_set_master_strip_json: (_s, v) =>
    withEngine((engine) => engine.setStrip('master', v as never)),
  engine_set_part_rig: (_s, v) =>
    withEngine((engine) => engine.setPartRig({ ...RIG, inserts: v } as never)),
  engine_set_track_lanes: (_s, v) => withEngine((engine) => engine.setTrackLanes(v as never)),
  engine_set_track_strip_channel_delay_samples: (_s, v) =>
    withEngine((engine) => engine.setStripChannelDelaySamples(v as never, 0)),
  engine_set_track_strip_dual_pan: (_s, v) =>
    withEngine((engine) => engine.setStripDualPan(v as never, 0, 0)),
  engine_set_track_strip_eq_band_json: (_s, v) =>
    withEngine((engine) => engine.setStripEqBand(v as never, 0, {})),
  engine_set_track_strip_insert_bypassed: (_s, v) =>
    withEngine((engine) => engine.setStripInsertBypassed(v as never, 0, { bypassed: true })),
  engine_set_track_strip_insert_param_by_name: (s, v) =>
    withEngine((engine) =>
      s === 'ref'
        ? engine.setStripInsertParam(v as never, 0, PARAM)
        : engine.setStripInsertParam(1, 0, param(s, v)),
    ),
  engine_set_master_strip_insert_param_by_name: (s, v) =>
    withEngine((engine) => engine.setStripInsertParam('master', 0, param(s, v))),
  engine_set_bus_strip_insert_param_by_name: (s, v) =>
    withEngine((engine) => engine.setBusInsertParam(1, 0, param(s, v))),
  engine_set_track_strip_json: (s, v) =>
    withEngine((engine) =>
      s === 'ref' ? engine.setStrip(v as never, SCENE_STRIP) : engine.setStrip(1, v as never),
    ),
  engine_set_track_strip_pan: (_s, v) => withEngine((engine) => engine.setStripPan(v as never, 0)),
  engine_set_track_strip_pan_law: (_s, v) =>
    withEngine((engine) => engine.setStripPanLaw(v as never, 'constantPower' as never)),
  engine_set_track_strip_pan_mode: (_s, v) =>
    withEngine((engine) => engine.setStripPanMode(v as never, 'balance' as never)),
  engine_set_track_strip_surround_pan: (_s, v) =>
    withEngine((engine) => engine.setStripSurroundPan(v as never, {})),
  eq_magnitude_response: (_s, v) => withEq((eq) => eq.magnitudeResponse(v as Float32Array)),
  eq_match: (s, v) =>
    withEq((eq) =>
      s === 'source' ? eq.match(v as Float32Array, TONE) : eq.match(TONE, v as Float32Array),
    ),
  eq_process: (_s, v) => withEq((eq) => eq.processMono(v as Float32Array)),
  eq_set_sidechain: (_s, v) => withEq((eq) => eq.setSidechainMono(v as Float32Array)),
  encode_audio: (s, v) =>
    encodeAudio(given({ channels: [TONE], sampleRate: SR, format: 'wav' }, s, v) as never),
  estimate_meter_json: (s, v) =>
    estimateMeter(given({ observations: METER_OBSERVATIONS }, s, v) as never),
  mastering_ab_match_loudness: (s, v) => masteringAbMatchLoudness(given(PAIR, s, v) as never),
  mastering_ab_match_loudness_stereo: (s, v) =>
    masteringAbMatchLoudnessStereo(given(STEREO_PAIR, s, v) as never),
  mastering_analyze_pair: (s, v) =>
    masteringAnalyzePair(given({ ...PAIR, analysis: 'match.referenceLoudness' }, s, v) as never),
  mastering_analyze_pair_stereo: (s, v) =>
    masteringAnalyzePair(
      given(
        {
          analysis: 'match.referenceLoudness',
          source: [TONE, TONE],
          reference: [TONE, TONE],
          sampleRate: SR,
        },
        s,
        [v, v],
      ) as never,
    ),
  mastering_apply_pair_processor: (s, v) =>
    masteringApplyPairProcessor(given({ ...PAIR, processor: 'match.abCrossfade' }, s, v) as never),
  mastering_apply_pair_processor_stereo: (s, v) =>
    masteringApplyPairProcessorStereo(
      given({ ...STEREO_PAIR, processor: 'match.abCrossfade' }, s, v) as never,
    ),
  merge_notes: (s, v) =>
    mergeNotes(
      given(
        { ...NOTE_TRACK, voicedProb: VOICED_PROB, notes: [NOTE], first: 0, last: 0 },
        s,
        v,
      ) as never,
    ),
  midi_cc_learn: (s, v) =>
    midiCcLearn(given({ events: MIDI_CC_EVENTS, paramId: 77 }, s, v) as never),
  midi_route_events: (s, v) => midiRouteEvents(given({ events: MIDI_CC_EVENTS }, s, v) as never),
  pitch_correct_timevarying: (s, v) =>
    pitchCorrectTimevarying(given({ samples: TONE, sampleRate: SR, f0Hz: F0 }, s, v) as never),
  project_add_automation_lane: (_s, v) =>
    withTrack((project, trackId) =>
      project.addAutomationLane(trackId, { targetParamId: 1, points: v } as never),
    ),
  project_annotate_chords: (_s, v) => withProject((project) => project.annotateChords(v as never)),
  project_annotate_keys: (_s, v) => withProject((project) => project.annotateKeys(v as never)),
  project_annotate_sections: (_s, v) =>
    withProject((project) => project.annotateSections(v as never)),
  project_bounce: (s, v) => withProject((project) => project.bounce({ [s]: v } as never)),
  project_edit_automation_lane: (_s, v) =>
    withTrack((project, trackId) =>
      project.editAutomationLane(trackId, 1, {
        targetKind: 'opaque',
        targetParamId: 1,
        points: v,
      } as never),
    ),
  project_import_external_stems: (_s, v) =>
    withProject((project) => project.importExternalStems({ sampleRate: 48000, stems: v } as never)),
  project_set_assist_sidecar: (_s, v) =>
    withProject((project) => project.setAssistSidecar({ moduleId: 'm', payload: v } as never)),
  project_set_part_rig: (_s, v) =>
    withProject((project) => project.setPartRig({ ...RIG, inserts: v } as never)),
  project_set_warp_map: (_s, v) =>
    withProject((project) => project.setWarpMap({ id: 1, anchors: v } as never)),
  render_notes: (s, v) =>
    renderNotes(
      given({ ...NOTE_TRACK, notes: [{ ...NOTE, edit: { pitchSemitones: 1 } }] }, s, v) as never,
    ),
  render_percussive_events: (s, v) =>
    renderPercussiveEvents(
      given(
        {
          samples: TONE,
          sampleRate: SR,
          events: [{ onsetSample: 0, offsetSample: 200, edit: { muted: true } }],
        },
        s,
        v,
      ) as never,
    ),
  room_morph: (s, v) =>
    roomMorph(given({ samples: TONE, sampleRate: 48000, ...ROOM }, s, v) as never),
  room_morph_stereo: (s, v) =>
    roomMorphStereo(given({ left: TONE, right: TONE, sampleRate: 48000, ...ROOM }, s, v) as never),
  sample_bank_add_sample: (_s, v) => new SampleBank().addSample(v as Float32Array),
  split_note: (s, v) =>
    splitNote(
      given(
        { ...NOTE_TRACK, voicedProb: VOICED_PROB, notes: [NOTE], index: 0, frame: 5 },
        s,
        v,
      ) as never,
    ),
  streaming_mastering_chain_process_mono: (_s, v) =>
    withChain(1, (chain) => chain.processMono(v as Float32Array)),
  streaming_mastering_chain_process_stereo: (_s, v) =>
    withChain(2, (chain) => chain.processStereo(v as Float32Array, new Float32Array(16))),
  streaming_retune_process_mono: (_s, v) =>
    withHandle(new StreamingRetune(), (retune) => {
      retune.prepare(48000, 128);
      return retune.processMono(v as Float32Array);
    }),
  synthesize_rir: (s, v) => synthesizeRir(given({ sampleRate: 48000, ...ROOM }, s, v) as never),
  vocal_draft_apply: (_s, v) => withDraft((draft) => draft.apply({ operations: v } as never)),
  align_take_to_reference: (s, v) =>
    alignTakeToReference(given({ reference: TONE, take: TONE, sampleRate: SR }, s, v) as never),
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
  engine_resolve_track_lane_automation_id: (s, v) =>
    withEngine((engine) =>
      s === 'ref'
        ? engine.resolveStripAutomationId(v as never, 'pan')
        : engine.resolveStripAutomationId(1, v as never),
    ),
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
  pcen: (spelling, value) =>
    pcen({
      values: { data: new Float32Array(0), rows: 1, cols: 1, [spelling.split('.')[1]]: value },
    } as never),
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
  playback_loudness_meter_create: (s, v) =>
    withHandle(
      new PlaybackLoudnessMeter({ [s]: v, sampleRate: 48000 } as never),
      (meter) => meter.integratedLufs,
    ),
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
  engine_set_record_offset_samples: (s, v) =>
    withEngine((engine) => engine.configureCapture(given({ capacityFrames: 1024 }, s, v) as never)),
  lufs_interleaved: (s, v) =>
    lufsMultichannel(given({ channels: [TONE, TONE], sampleRate: SR }, s, v) as never),
  lufs_series_interleaved: (s, v) =>
    lufsSeriesMultichannel(given({ channels: [TONE, TONE], sampleRate: SR }, s, v) as never),
  mixer_strip_at: (_s, v) =>
    withMixer((mixer) => {
      mixer.addStrip('a');
      return mixer.setFaderDb(v as never, 0);
    }),
  playback_render_interleaved: (s, v) =>
    renderPlayback(
      given({ samples: TONE, sampleRate: SR, numChannels: 1, config: {} }, s, v) as never,
    ),
  polyphonic_set_note_edit: (_s, v) =>
    withHandle(analyzePolyphonic({ samples: TONE, sampleRate: SR }), (analysis) =>
      analysis.setNoteEdit(0, { amplitudeEnvelope: v } as never),
    ),
};

const UNDRIVEN_REASONS = [
  'no value refuses it',
  'needs live state',
  'content refusal carries no subject',
];

/** Rows no value can make refuse as a parameter, keyed `"<entry>.<subject>"`. */
const UNDRIVEN: Record<string, string> = {
  // The realtime path records no refusal text, so nothing can name the quotient.
  'playback_renderer_process_interleaved.frames': 'no value refuses it',
  // An empty render is refused by its message, which names no argument.
  'playback_renderer_render_offline.frames': 'content refusal carries no subject',
};

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

  it('keeps the edit root on an indexed envelope refusal', () => {
    expect(() =>
      withHandle(analyzePolyphonic({ samples: TONE, sampleRate: SR }), (analysis) => {
        expect(analysis.noteCount).toBeGreaterThan(0);
        analysis.setNoteEdit(0, { amplitudeEnvelope: Float32Array.of(-1) });
      }),
    ).toThrow(
      new RangeError(
        'PolyphonicAnalysis.setNoteEdit: edit.amplitudeEnvelope[0] must be finite and at least 0',
      ),
    );
  });

  it.each(ROWS.map((row) => [key(row), row] as const))(
    '%s is refused under the caller spelling',
    (_name, row) => {
      const undrivenReason = UNDRIVEN[key(row)];
      if (undrivenReason !== undefined) {
        expect(UNDRIVEN_REASONS).toContain(undrivenReason);
        if (ROWS.filter((r) => r.entry === row.entry).every((r) => key(r) in UNDRIVEN)) {
          expect(DRIVERS[row.entry], `${row.entry} drives no row and must not have a driver`).toBe(
            undefined,
          );
        }
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
