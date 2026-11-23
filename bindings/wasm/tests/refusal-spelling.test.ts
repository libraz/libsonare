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
  chordFunctions,
  createVocalEditSession,
  decomposeNotePitch,
  detectKey,
  detectKeyCandidates,
  downmix,
  estimateMeter,
  init,
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
  midiCcLearn,
  midiRouteEvents,
  mixingScenePreset,
  noteTargetsFromSmf,
  PlaybackLoudnessMeter,
  PlaybackRenderer,
  Project,
  pitchCorrectTimevarying,
  RealtimeEngine,
  RealtimeVoiceChanger,
  realtimeVoiceChangerRender,
  renderNotes,
  renderPercussiveEvents,
  roomMorph,
  roomMorphStereo,
  SampleBank,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
  splitNote,
  stft,
  synthesizeRir,
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

const withEqualizer = (fn: (eq: StreamingEqualizer) => unknown) =>
  using(new StreamingEqualizer({ sampleRate: 48000 }), fn);

const withProject = (fn: (project: Project) => unknown) => using(new Project(), fn);

const F0 = new Float32Array(44).fill(220);
const ROOM = { lengthM: 7, widthM: 5, heightM: 3, maxSec: 0.2 };
const NOTES = [
  { onsetSample: 0, offsetSample: 2000, frameStart: 0, frameEnd: 4, medianHz: 220 },
  { onsetSample: 2000, offsetSample: 5000, frameStart: 4, frameEnd: 10, medianHz: 220 },
];
const NOTE_TRACK = { samples: TONE, sampleRate: RATE, f0Hz: F0, frameRate: 40 };
const PART_RIG = { destinationId: 3, part: 0, mode: 'chain' } as const;
const PARAM = { name: 'thresholdDb', value: 0 };
/** A sidechain source whose `kind` or `id` (named by the spelling) is the swept value. */
const source = (spelling: string, value: unknown) =>
  spelling === 'source.kind' ? { kind: value, id: 2 } : { kind: 'track', id: value };
/** An insert parameter whose `name` is the swept value. */
const param = (_spelling: string, value: unknown) => ({ ...PARAM, name: value }) as never;
const PAIR = { sampleRate: RATE, source: TONE, reference: TONE };
const STEREO_PAIR = {
  sampleRate: RATE,
  sourceLeft: TONE,
  sourceRight: TONE,
  referenceLeft: TONE,
  referenceRight: TONE,
};

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
      engine.process(value as Float32Array[], { withMonitor: true }),
    ),
  engine_resolve_master_automation_id: (_spelling, value) =>
    withEngine((engine) => engine.resolveStripAutomationId('master', value as never)),
  engine_resolve_track_lane_automation_id: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.resolveStripAutomationId(value as never, 'faderDb')
        : engine.resolveStripAutomationId(1, value as never),
    ),
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
  chord_functions: (_spelling, value) =>
    chordFunctions({ chords: value, key: { root: 7, mode: 'major' } } as never),
  decompose_note_pitch: (spelling, value) =>
    decomposeNotePitch({
      f0Hz: new Float32Array(4),
      frameRate: 100,
      medianHz: 220,
      [spelling]: value,
    } as never),
  detect_key: (spelling, value) =>
    detectKey({ samples: TONE, sampleRate: RATE, [spelling]: value } as never),
  detect_key_candidates: (spelling, value) =>
    detectKeyCandidates({ samples: TONE, sampleRate: RATE, [spelling]: value } as never),
  engine_apply_track_strip_insert_param_by_name_now: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.applyStripInsertParamNow(value as never, 0, PARAM)
        : engine.applyStripInsertParamNow(1, 0, param(spelling, value)),
    ),
  engine_apply_master_strip_insert_param_by_name_now: (spelling, value) =>
    withEngine((engine) => engine.applyStripInsertParamNow('master', 0, param(spelling, value))),
  engine_apply_bus_strip_insert_param_by_name_now: (spelling, value) =>
    withEngine((engine) => engine.applyBusInsertParamNow(1, 0, param(spelling, value))),
  engine_can_set_bus_sidechain: (spelling, value) =>
    withEngine((engine) => engine.canSetBusSidechain(1, 0, source(spelling, value) as never)),
  engine_can_set_lane_sidechain: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.canSetStripSidechain(value as never, 0, { kind: 'track', id: 2 } as never)
        : engine.canSetStripSidechain(1, 0, source(spelling, value) as never),
    ),
  engine_can_set_master_sidechain: (spelling, value) =>
    withEngine((engine) =>
      engine.canSetStripSidechain('master', 0, source(spelling, value) as never),
    ),
  engine_clear_track_insert_parameter_bases: (_spelling, value) =>
    withEngine((engine) => engine.clearStripInsertParameterBases(value as never)),
  engine_push_midi_sysex: (_spelling, value) =>
    withEngine((engine) => engine.pushMidiSysex(0, value as Uint8Array)),
  engine_render_offline: (_spelling, value) =>
    withEngine((engine) => engine.renderOffline({ channels: value } as never)),
  engine_resolve_track_insert_automation_id: (_spelling, value) =>
    withEngine((engine) => engine.resolveStripInsertAutomationId(value as never, 0, 'x')),
  engine_restore_track_strip_insert_param_by_name: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.restoreStripInsertParam(value as never, 0, PARAM)
        : engine.restoreStripInsertParam(1, 0, param(spelling, value)),
    ),
  engine_restore_master_strip_insert_param_by_name: (spelling, value) =>
    withEngine((engine) => engine.restoreStripInsertParam('master', 0, param(spelling, value))),
  engine_restore_bus_strip_insert_param_by_name: (spelling, value) =>
    withEngine((engine) => engine.restoreBusInsertParam(1, 0, param(spelling, value))),
  engine_set_bus_strip_json: (_spelling, value) =>
    withEngine((engine) => engine.setBusStrip(1, value as never)),
  engine_set_bus_sidechain: (spelling, value) =>
    withEngine((engine) => engine.setBusSidechain(1, 0, source(spelling, value) as never)),
  engine_set_lane_sidechain: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.setStripSidechain(value as never, 0, null)
        : engine.setStripSidechain(1, 0, source(spelling, value) as never),
    ),
  engine_set_master_sidechain: (spelling, value) =>
    withEngine((engine) => engine.setStripSidechain('master', 0, source(spelling, value) as never)),
  engine_set_master_strip_json: (_spelling, value) =>
    withEngine((engine) => engine.setStrip('master', value as never)),
  engine_set_part_rig: (_spelling, value) =>
    withEngine((engine) => engine.setPartRig({ ...PART_RIG, inserts: value } as never)),
  engine_set_track_lanes: (_spelling, value) =>
    withEngine((engine) => engine.setTrackLanes(value as never)),
  engine_set_track_strip_channel_delay_samples: (_spelling, value) =>
    withEngine((engine) => engine.setStripChannelDelaySamples(value as never, 0)),
  engine_set_track_strip_dual_pan: (_spelling, value) =>
    withEngine((engine) => engine.setStripDualPan(value as never, 0, 0)),
  engine_set_track_strip_eq_band_json: (_spelling, value) =>
    withEngine((engine) => engine.setStripEqBand(value as never, 0, {} as never)),
  engine_set_track_strip_insert_bypassed: (_spelling, value) =>
    withEngine((engine) => engine.setStripInsertBypassed(value as never, 0, { bypassed: true })),
  engine_set_track_strip_insert_param_by_name: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.setStripInsertParam(value as never, 0, PARAM)
        : engine.setStripInsertParam(1, 0, param(spelling, value)),
    ),
  engine_set_master_strip_insert_param_by_name: (spelling, value) =>
    withEngine((engine) => engine.setStripInsertParam('master', 0, param(spelling, value))),
  engine_set_bus_strip_insert_param_by_name: (spelling, value) =>
    withEngine((engine) => engine.setBusInsertParam(1, 0, param(spelling, value))),
  engine_set_track_strip_json: (spelling, value) =>
    withEngine((engine) =>
      spelling === 'ref'
        ? engine.setStrip(value as never, {} as never)
        : engine.setStrip(1, value as never),
    ),
  engine_set_track_strip_pan: (_spelling, value) =>
    withEngine((engine) => engine.setStripPan(value as never, 0)),
  engine_set_track_strip_pan_law: (_spelling, value) =>
    withEngine((engine) => engine.setStripPanLaw(value as never, 'constantPower' as never)),
  engine_set_track_strip_pan_mode: (_spelling, value) =>
    withEngine((engine) => engine.setStripPanMode(value as never, 'balance' as never)),
  engine_set_track_strip_surround_pan: (_spelling, value) =>
    withEngine((engine) => engine.setStripSurroundPan(value as never, {})),
  eq_magnitude_response: (_spelling, value) =>
    withEqualizer((eq) => eq.magnitudeResponse(value as Float32Array)),
  eq_match: (spelling, value) =>
    withEqualizer((eq) =>
      spelling === 'source'
        ? eq.match(value as Float32Array, TONE)
        : eq.match(TONE, value as Float32Array),
    ),
  eq_process: (_spelling, value) => withEqualizer((eq) => eq.processMono(value as Float32Array)),
  eq_set_sidechain: (_spelling, value) =>
    withEqualizer((eq) => eq.setSidechainMono(value as Float32Array)),
  estimate_meter_json: (spelling, value) =>
    estimateMeter({
      beats: Array.from({ length: 8 }, (_, i) => ({ timeSec: i * 0.5, strength: 1 })),
      [spelling]: value,
    } as never),
  mastering_ab_match_loudness: (spelling, value) =>
    masteringAbMatchLoudness({ ...PAIR, [spelling]: value } as never),
  mastering_ab_match_loudness_stereo: (spelling, value) =>
    masteringAbMatchLoudnessStereo({ ...STEREO_PAIR, [spelling]: value } as never),
  mastering_analyze_pair: (spelling, value) =>
    masteringAnalyzePair({
      ...PAIR,
      analysis: 'match.referenceLoudness',
      [spelling]: value,
    } as never),
  mastering_apply_pair_processor: (spelling, value) =>
    masteringApplyPairProcessor({
      ...PAIR,
      processor: 'match.eq',
      [spelling]: value,
    } as never),
  mastering_apply_pair_processor_stereo: (spelling, value) =>
    masteringApplyPairProcessorStereo({
      ...STEREO_PAIR,
      processor: 'match.abCrossfade',
      [spelling]: value,
    } as never),
  merge_notes: (spelling, value) =>
    mergeNotes({
      ...NOTE_TRACK,
      voiced: new Int32Array(44).fill(1),
      notes: NOTES,
      first: 0,
      last: 1,
      [spelling]: value,
    } as never),
  midi_cc_learn: (spelling, value) =>
    midiCcLearn({ paramId: 3, events: [], [spelling]: value } as never),
  midi_route_events: (spelling, value) =>
    midiRouteEvents({ events: [], [spelling]: value } as never),
  pitch_correct_timevarying: (spelling, value) =>
    pitchCorrectTimevarying({
      samples: TONE,
      sampleRate: RATE,
      f0Hz: F0,
      targetMidi: 57,
      [spelling]: value,
    } as never),
  project_add_automation_lane: (_spelling, value) =>
    withProject((project) =>
      project.addAutomationLane(project.addTrack({ kind: 'audio' }), {
        targetParamId: 1,
        points: value,
      } as never),
    ),
  project_annotate_chords: (_spelling, value) =>
    withProject((project) => project.annotateChords(value as never)),
  project_annotate_keys: (_spelling, value) =>
    withProject((project) => project.annotateKeys(value as never)),
  project_bounce: (spelling, value) =>
    withProject((project) => project.bounce({ [spelling]: value } as never)),
  project_edit_automation_lane: (_spelling, value) =>
    withProject((project) => {
      const track = project.addTrack({ kind: 'audio' });
      project.addAutomationLane(track, { targetParamId: 1, points: [{ ppq: 0, value: 0 }] });
      project.editAutomationLane(track, 1, { targetParamId: 1, points: value } as never);
    }),
  project_import_external_stems: (_spelling, value) =>
    withProject((project) =>
      project.importExternalStems({ sampleRate: RATE, stems: value } as never),
    ),
  project_set_assist_sidecar: (_spelling, value) =>
    withProject((project) =>
      project.setAssistSidecar({ moduleId: 'module', payload: value } as never),
    ),
  project_set_part_rig: (_spelling, value) =>
    withProject((project) => project.setPartRig({ ...PART_RIG, inserts: value } as never)),
  project_set_warp_map: (_spelling, value) =>
    withProject((project) => project.setWarpMap({ id: 1, anchors: value } as never)),
  render_notes: (spelling, value) =>
    renderNotes({ samples: TONE, sampleRate: RATE, notes: [], [spelling]: value } as never),
  render_percussive_events: (_spelling, value) =>
    renderPercussiveEvents({ samples: TONE, sampleRate: RATE, events: value } as never),
  room_morph: (spelling, value) =>
    roomMorph({ samples: TONE, sampleRate: 48000, ...ROOM, [spelling]: value } as never),
  room_morph_stereo: (spelling, value) =>
    roomMorphStereo({
      left: TONE,
      right: TONE,
      sampleRate: 48000,
      ...ROOM,
      [spelling]: value,
    } as never),
  sample_bank_add_sample: (_spelling, value) =>
    using(new SampleBank(), (bank) => bank.addSample(value as Float32Array)),
  split_note: (spelling, value) =>
    splitNote({
      ...NOTE_TRACK,
      voiced: new Int32Array(44).fill(1),
      notes: NOTES,
      index: 1,
      frame: 7,
      [spelling]: value,
    } as never),
  streaming_mastering_chain_process_mono: (_spelling, value) =>
    using(new StreamingMasteringChain({ sampleRate: 48000, numChannels: 1 }), (chain) =>
      chain.processMono(value as Float32Array),
    ),
  streaming_mastering_chain_process_stereo: (_spelling, value) =>
    using(new StreamingMasteringChain({ sampleRate: 48000, numChannels: 2 }), (chain) =>
      chain.processStereo(value as Float32Array, BLOCK),
    ),
  streaming_retune_process_mono: (_spelling, value) =>
    using(new StreamingRetune(), (retune) => retune.processMono(value as Float32Array)),
  synthesize_rir: (spelling, value) =>
    synthesizeRir({ sampleRate: 48000, ...ROOM, [spelling]: value } as never),
  vocal_draft_apply: (_spelling, value) =>
    using(createVocalEditSession({ samples: TONE, sampleRate: RATE }), (session) =>
      using(session.beginEdit(), (draft) => draft.apply({ operations: value } as never)),
    ),
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

/** Extra values for an entry whose count is refused only by a list longer than the budget. */
const EXTRA_OUTSIDE: Record<string, unknown[]> = {
  chord_functions: [new Array(64 * 1024 * 1024 + 1)],
};

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
function refusals(driver: Driver, spelling: string, extra: unknown[]): string[] {
  const texts: string[] = [];
  for (const value of [...OUTSIDE, ...extra]) {
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
      const texts = refusals(driver, spelling, EXTRA_OUTSIDE[entry] ?? []);
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
    // biome-ignore lint/suspicious/noConsole: the sweep's driven/undriven totals are its report.
    console.log(`refusal spelling sweep: driven=${driven} undriven=${undriven}`);
    expect(stale).toEqual([]);
    expect(badReason).toEqual([]);
    expect(unaccounted).toEqual([]);
    expect(driven).toBeGreaterThan(0);
  });
});
