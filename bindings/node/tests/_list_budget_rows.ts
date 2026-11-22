/**
 * The per-method rows of the caller-list budget evidence: each row hands a list past the budget to
 * one public method and names the list in the refusal. The rows are data so the list-taking
 * callable coverage test can join them against the callables the type checker finds.
 */

import type { Mixer, Project, RealtimeEngine } from '../src/index.js';

/** One method refusing one oversized list: a label, the refusal subject, and the call. */
export type Row<H> = readonly [
  name: string,
  subject: string,
  call: (handle: H, list: never) => void,
];

export const ENGINE_ROWS: ReadonlyArray<Row<RealtimeEngine>> = [
  ['setMarkers', 'markers', (e, l) => e.setMarkers(l)],
  ['setAutomationLane', 'points', (e, l) => e.setAutomationLane(1, l)],
  ['setClips', 'clips', (e, l) => e.setClips(l)],
  ['setTrackLanes', 'lanes', (e, l) => e.setTrackLanes(l)],
  ['setTrackLanes sends', 'lanes[0].sends', (e, l) => e.setTrackLanes([{ trackId: 1, sends: l }])],
  ['setTrackSends', 'sends', (e, l) => e.setTrackSends(1, l)],
  ['setTrackBuses', 'buses', (e, l) => e.setTrackBuses(l)],
  ['setTrackBuses sends', 'buses[0].sends', (e, l) => e.setTrackBuses([{ busId: 1, sends: l }])],
  ['setStrip sends', 'strip.sends', (e, l) => e.setStrip(1, { sends: l } as never)],
  ['setStrip inserts', 'strip', (e, l) => e.setStrip(1, { inserts: l } as never)],
  ['setBusStrip', 'bus', (e, l) => e.setBusStrip(1, { inserts: l } as never)],
  ['setStripEqBand', 'band', (e, l) => e.setStripEqBand(1, 0, { gains: l } as never)],
  ['setGraph nodes', 'nodes', (e, l) => e.setGraph({ nodes: l, connections: [] } as never)],
  ['setGraph connections', 'connections', (e, l) => e.setGraph({ connections: l } as never)],
  [
    'setGraph parameterBindings',
    'parameterBindings',
    (e, l) => e.setGraph({ parameterBindings: l } as never),
  ],
  ['renderOffline', 'channels', (e, l) => e.renderOffline({ channels: l } as never)],
  ['setTempoSegments', 'segments', (e, l) => e.setTempoSegments(l)],
  ['setTimeSignatureSegments', 'segments', (e, l) => e.setTimeSignatureSegments(l)],
  ['setSynthInstrument', 'modRoutings', (e, l) => e.setSynthInstrument(0, { modRoutings: l })],
  [
    'setPartRig',
    'inserts',
    (e, l) => e.setPartRig({ destinationId: 0, part: 0, mode: 'chain', inserts: l }),
  ],
  ['setMidiFx', 'config', (e, l) => e.setMidiFx(0, { chordIntervals: l })],
  ['setMidiClips', 'clips', (e, l) => e.setMidiClips(l)],
  ['pushMidiUmp', 'words', (e, l) => e.pushMidiUmp(0, l)],
  ['pushMidiInputUmp', 'words', (e, l) => e.pushMidiInputUmp(l)],
];

export const PROJECT_ROWS: ReadonlyArray<Row<Project>> = [
  ['setClipTakes', 'takes', (p, l) => p.setClipTakes(1, l)],
  ['setClipCompSegments', 'segments', (p, l) => p.setClipCompSegments(1, l)],
  ['setMidiEvents', 'events', (p, l) => p.setMidiEvents(1, l)],
  ['annotateKeys', 'keys', (p, l) => p.annotateKeys(l)],
  ['annotateChords', 'chords', (p, l) => p.annotateChords(l)],
  [
    'setPartRig',
    'inserts',
    (p, l) => p.setPartRig({ destinationId: 0, part: 0, mode: 'chain', inserts: l }),
  ],
  ['setTempoSegments', 'segments', (p, l) => p.setTempoSegments(l)],
  ['setTimeSignatures', 'segments', (p, l) => p.setTimeSignatures(l)],
  ['setWarpMap', 'anchors', (p, l) => p.setWarpMap({ id: 1, anchors: l })],
  [
    'addClip channels',
    'channels',
    (p, l) => p.addClip({ trackId: 1, channels: l, sampleRate: 48000 } as never),
  ],
  ['importExternalStems', 'stems', (p, l) => p.importExternalStems({ stems: l } as never)],
  [
    'bounce builtinInstruments',
    'builtinInstruments',
    (p, l) => void p.bounce({ builtinInstruments: l }),
  ],
  ['bounce instruments', 'instruments', (p, l) => void p.bounce({ instruments: l })],
  [
    'addAutomationLane',
    'points',
    (p, l) => p.addAutomationLane(1, { targetParamId: 1, points: l } as never),
  ],
];

export const MIXER_ROWS: ReadonlyArray<Row<Mixer>> = [
  ['addVcaGroup', 'members', (m, l) => m.addVcaGroup('v', 0, l)],
  ['setVcaGroupMembers', 'members', (m, l) => m.setVcaGroupMembers('v', l)],
];

/** The public API namespace the facade rows call. */
export type Api = typeof import('../src/index.js');

/** One callable refusing one oversized list, with the whole refusal message. */
export interface FacadeBudgetRow {
  /** `functionName` or `Class.method`, the spelling the callable enumerator uses. */
  callable: string;
  message: string;
  run: (api: Api, list: never) => unknown;
}

const OVER = 'exceeds the input length budget';
const X = new Float32Array(4096).fill(0.1);
const F0 = new Float32Array(4);
const SR = 22050;

/** A row for `callable` whose refusal names `subject`, the message prefixed by `prefix` (default: the callable). */
const row = (
  callable: string,
  subject: string,
  run: FacadeBudgetRow['run'],
  prefix = callable,
): FacadeBudgetRow => ({ callable, message: `${prefix}: ${subject} ${OVER}`, run });

const withEngine = (api: Api, call: (engine: InstanceType<Api['RealtimeEngine']>) => unknown) => {
  const engine = new api.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  try {
    return call(engine);
  } finally {
    engine.destroy();
  }
};
const withProject = (api: Api, call: (project: InstanceType<Api['Project']>) => unknown) => {
  const project = new api.Project();
  try {
    return call(project);
  } finally {
    project.destroy();
  }
};
const withMixer = (api: Api, call: (mixer: InstanceType<Api['Mixer']>) => unknown) => {
  const mixer = new api.Mixer({ sampleRate: 48000 });
  try {
    return call(mixer);
  } finally {
    mixer.destroy();
  }
};

/** The vocal-edit request of the smallest session the draft accepts. */
const vocalSession = (api: Api) => {
  const frames = Math.ceil(4096 / 128) + 1;
  return api.createVocalEditSession({
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
};

const noteTrack = { samples: X, sampleRate: SR, f0Hz: F0, frameRate: 100 };

/**
 * Rows for the callables outside the three handle tables. Each hands the call a sparse array
 * whose length is past the budget and whose first entry throws when read.
 */
export const FACADE_ROWS: ReadonlyArray<FacadeBudgetRow> = [
  row('ClipPageProvider.supply', 'channels', (api, l) =>
    new api.ClipPageProvider({ numChannels: 1, numSamples: 100, pageFrames: 10 }).supply(0, l),
  ),
  row('RealtimeEngine.upsertClip', 'channels', (api, l) =>
    withEngine(api, (e) => e.upsertClip({ id: 1, trackId: 1, channels: l } as never)),
  ),
  row('RealtimeEngine.upsertClip', 'warpAnchors', (api, l) =>
    withEngine(api, (e) =>
      e.upsertClip({
        id: 1,
        trackId: 1,
        startPpq: 0,
        channels: [X],
        sampleRate: 48000,
        warpMode: 'repitch',
        warpAnchors: l,
      } as never),
    ),
  ),
  row('RealtimeEngine.process', 'channels', (api, l) => withEngine(api, (e) => e.process(l))),
  row('Mixer.processStereo', 'leftInputs', (api, l) =>
    withMixer(api, (m) => m.processStereo(l, l)),
  ),
  row('Project.setSourceAudio', 'channels', (api, l) =>
    withProject(api, (p) =>
      p.setSourceAudio({ sourceId: 1, channels: l, sampleRate: 48000 } as never),
    ),
  ),
  row('Project.addLoopRecordingTakes', 'audio', (api, l) =>
    withProject(api, (p) => p.addLoopRecordingTakes({ audio: l } as never)),
  ),
  row('Project.editAutomationLane', 'points', (api, l) =>
    withProject(api, (p) => p.editAutomationLane(1, 1, { targetParamId: 1, points: l } as never)),
  ),
  row('Project.bakeMidiFx', 'config', (api, l) =>
    withProject(api, (p) => p.bakeMidiFx({ clipId: 1, config: { chordIntervals: l } } as never)),
  ),
  row('Project.previewMidiFxCount', 'config', (api, l) =>
    withProject(api, (p) =>
      p.previewMidiFxCount({ clipId: 1, config: { chordIntervals: l } } as never),
    ),
  ),
  row('detectKey', 'modes', (api, l) =>
    api.detectKey({ samples: X, sampleRate: SR, modes: l } as never),
  ),
  row('detectKeyCandidates', 'modes', (api, l) =>
    api.detectKeyCandidates({ samples: X, sampleRate: SR, modes: l } as never),
  ),
  row('Audio.detectKey', 'modes', (api, l) =>
    api.Audio.fromBuffer(X, SR).detectKey({ modes: l } as never),
  ),
  row('Audio.detectKeyCandidates', 'modes', (api, l) =>
    api.Audio.fromBuffer(X, SR).detectKeyCandidates({ modes: l } as never),
  ),
  row('analyze', 'meterCandidateNumerators', (api, l) =>
    api.analyze({ samples: X, sampleRate: SR, meterCandidateNumerators: l } as never),
  ),
  row('analyzeAsync', 'meterCandidateNumerators', (api, l) =>
    api.analyzeAsync({ samples: X, sampleRate: SR, meterCandidateNumerators: l } as never),
  ),
  row('Audio.analyze', 'meterCandidateNumerators', (api, l) =>
    api.Audio.fromBuffer(X, SR).analyze({ meterCandidateNumerators: l } as never),
  ),
  row('estimateMeter', 'beats', (api, l) => api.estimateMeter({ beats: l } as never)),
  row('estimateMeter', 'candidateNumerators', (api, l) =>
    api.estimateMeter({ beats: [], candidateNumerators: l } as never),
  ),
  row('synthesizeRir', 'bandAbsorption', (api, l) =>
    api.synthesizeRir({ sampleRate: SR, bandAbsorption: l } as never),
  ),
  row('roomGeometryFromEstimate', 'bandAbsorption', (api, l) =>
    api.roomGeometryFromEstimate({
      estimate: { lengthM: 1, widthM: 1, heightM: 1, bandAbsorption: l },
    } as never),
  ),
  row('roomMorph', 'bandAbsorption', (api, l) =>
    api.roomMorph({ samples: X, sampleRate: SR, bandAbsorption: l } as never),
  ),
  row('roomMorphStereo', 'bandAbsorption', (api, l) =>
    api.roomMorphStereo({ left: X, right: X, sampleRate: SR, bandAbsorption: l } as never),
  ),
  row('chordFunctions', 'chords', (api, l) =>
    api.chordFunctions({ chords: l, key: { root: 0, mode: 'major' } } as never),
  ),
  row('Audio.masterAudio', 'overrides', (api, l) =>
    api.Audio.fromBuffer(X, SR).masterAudio({
      preset: 'x',
      overrides: { eq: { bands: l } },
    } as never),
  ),
  row('downmix', 'channels', (api, l) => api.downmix(l, 'stereo')),
  row('extractNotes', 'voiced', (api, l) => api.extractNotes({ ...noteTrack, voiced: l } as never)),
  row('renderNotes', 'notes', (api, l) =>
    api.renderNotes({ samples: X, sampleRate: SR, notes: l } as never),
  ),
  row('decomposeNotePitch', 'voiced', (api, l) =>
    api.decomposeNotePitch({ f0Hz: F0, voiced: l, frameRate: 100 } as never),
  ),
  row('noteSegments', 'voiced', (api, l) =>
    api.noteSegments({ f0Hz: F0, voiced: l, frameRate: 100 } as never),
  ),
  row('splitNote', 'voiced', (api, l) =>
    api.splitNote({ ...noteTrack, voiced: l, notes: [], index: 0, frame: 1 } as never),
  ),
  row('mergeNotes', 'voiced', (api, l) =>
    api.mergeNotes({ ...noteTrack, voiced: l, notes: [], first: 0, last: 1 } as never),
  ),
  row('pitchCorrectTimevarying', 'voiced', (api, l) =>
    api.pitchCorrectTimevarying({ samples: X, sampleRate: SR, f0Hz: F0, voiced: l } as never),
  ),
  row('assignNoteTargets', 'notes', (api, l) =>
    api.assignNoteTargets({ notes: l, sampleRate: 48000, targets: [] } as never),
  ),
  row('assignNoteTargets', 'targets', (api, l) =>
    api.assignNoteTargets({ notes: [], sampleRate: 48000, targets: l } as never),
  ),
  row('extractPercussiveEvents', 'samples', (api, l) =>
    api.extractPercussiveEvents({ samples: l, sampleRate: SR } as never),
  ),
  row('renderPercussiveEvents', 'events', (api, l) =>
    api.renderPercussiveEvents({ samples: X, sampleRate: SR, events: l } as never),
  ),
  row('spectralEdit', 'ops', (api, l) =>
    api.spectralEdit({ samples: X, sampleRate: SR, ops: l } as never),
  ),
  row('streamingLoudnessGain', 'config', (api, l) =>
    api.streamingLoudnessGain({
      samples: X,
      sampleRate: SR,
      config: { eq: { bands: l } },
    } as never),
  ),
  row('streamingLoudnessGainStereo', 'config', (api, l) =>
    api.streamingLoudnessGainStereo({
      left: X,
      right: X,
      sampleRate: SR,
      config: { eq: { bands: l } },
    } as never),
  ),
  row('masterAudio', 'overrides', (api, l) =>
    api.masterAudio({ samples: X, sampleRate: SR, overrides: { eq: { bands: l } } } as never),
  ),
  row('masterAudioStereo', 'overrides', (api, l) =>
    api.masterAudioStereo({
      left: X,
      right: X,
      sampleRate: SR,
      overrides: { eq: { bands: l } },
    } as never),
  ),
  row('masterAudioAsync', 'overrides', (api, l) =>
    api.masterAudioAsync({ samples: X, sampleRate: SR, overrides: { eq: { bands: l } } } as never),
  ),
  row('masterAudioStereoAsync', 'overrides', (api, l) =>
    api.masterAudioStereoAsync({
      left: X,
      right: X,
      sampleRate: SR,
      overrides: { eq: { bands: l } },
    } as never),
  ),
  row('masteringStreamingPreview', 'platforms', (api, l) =>
    api.masteringStreamingPreview({ samples: X, sampleRate: SR, platforms: l } as never),
  ),
  row('masteringStreamingPreviewStereo', 'platforms', (api, l) =>
    api.masteringStreamingPreviewStereo({
      left: X,
      right: X,
      sampleRate: SR,
      platforms: l,
    } as never),
  ),
  row(
    'StreamingMasteringChain.constructor',
    'config',
    (api, l) =>
      new api.StreamingMasteringChain({
        sampleRate: 48000,
        dynamics: { multibandComp: { bands: l } },
      } as never),
    'StreamingMasteringChain',
  ),
  row('masteringRepairDereverbClassicalLinked', 'channels', (api, l) =>
    api.masteringRepairDereverbClassicalLinked({ channels: l, sampleRate: 48000 } as never),
  ),
  row('masteringRepairDenoiseClassicalLinked', 'channels', (api, l) =>
    api.masteringRepairDenoiseClassicalLinked({ channels: l, sampleRate: 48000 } as never),
  ),
  row('masteringRepairAnalyze', 'channels', (api, l) =>
    api.masteringRepairAnalyze({ channels: l, sampleRate: 48000 } as never),
  ),
  row('masteringRepairApply', 'channels', (api, l) =>
    api.masteringRepairApply({
      channels: l,
      sampleRate: 48000,
      stages: [{ stage: 'declick' }],
    } as never),
  ),
  row('masteringRepairApply', 'stages', (api, l) =>
    api.masteringRepairApply({ channels: [X], sampleRate: 48000, stages: l } as never),
  ),
  row('realtimeVoiceChangerRender', 'channels', (api, l) =>
    api.realtimeVoiceChangerRender({ channels: l, sampleRate: 48000 } as never),
  ),
  {
    callable: 'RealtimeVoiceChanger.processPlanar',
    message: 'RealtimeVoiceChanger.processPlanar: numChannels must be 1 or 2',
    run: (api, l) =>
      new api.RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 2 } as never).processPlanar(l),
  },
  row('segmentSubsegment', 'boundaries', (api, l) =>
    api.segmentSubsegment({
      data: { data: new Float32Array(4), rows: 2, cols: 2 },
      boundaries: l,
    } as never),
  ),
  row('decomposeStemsLinked', 'channels', (api, l) =>
    api.decomposeStemsLinked({ channels: l, sampleRate: SR } as never),
  ),
  row('remix', 'intervals', (api, l) =>
    api.remix({ samples: X, sampleRate: SR, intervals: l } as never),
  ),
  row('remixAlignedIntervals', 'intervals', (api, l) =>
    api.remixAlignedIntervals({ samples: X, sampleRate: SR, intervals: l } as never),
  ),
  row('splitSilenceCommon', 'signals', (api, l) => api.splitSilenceCommon({ signals: l } as never)),
  row('splitSilenceCommonWithReport', 'signals', (api, l) =>
    api.splitSilenceCommonWithReport({ signals: l } as never),
  ),
  row('fixFrames', 'frames', (api, l) => api.fixFrames({ frames: l } as never)),
  row('lufsMultichannel', 'channels', (api, l) =>
    api.lufsMultichannel({ channels: l, sampleRate: 48000 } as never),
  ),
  row('lufsSeriesMultichannel', 'channels', (api, l) =>
    api.lufsSeriesMultichannel({ channels: l, sampleRate: 48000 } as never),
  ),
  row('onsetBacktrack', 'events', (api, l) =>
    api.onsetBacktrack({ events: l, energy: new Float32Array(8) } as never),
  ),
  row('tempogramRatio', 'factors', (api, l) =>
    api.tempogramRatio({
      tempogram: { data: new Float32Array(4), rows: 2, cols: 2 },
      factors: l,
    } as never),
  ),
  row('waveformPeaks', 'channels', (api, l) => api.waveformPeaks({ channels: l } as never)),
  row('waveformPeakPyramid', 'channels', (api, l) =>
    api.waveformPeakPyramid({ channels: l } as never),
  ),
  row('waveformPeakPyramid', 'levels', (api, l) =>
    api.waveformPeakPyramid({ channels: [X], samplesPerBucketLevels: l } as never),
  ),
  row('midiParamToCc', 'bindings', (api, l) =>
    api.midiParamToCc({ bindings: l, paramId: 1, unitValue: 0.5, group: 0 } as never),
  ),
  row('midiCcToBreakpoint', 'bindings', (api, l) =>
    api.midiCcToBreakpoint({ bindings: l, event: {} } as never),
  ),
  row('midiRouteEvents', 'events', (api, l) => api.midiRouteEvents({ events: l } as never)),
  row('midiCcLearn', 'events', (api, l) => api.midiCcLearn({ events: l, paramId: 1 } as never)),
  row('mixingAssistantSuggest', 'tracks', (api, l) =>
    api.mixingAssistantSuggest({ tracks: l, sampleRate: 48000 } as never),
  ),
  row('mixStereo', 'strips', (api, l) => api.mixStereo({ sampleRate: 48000, strips: l } as never)),
  row('renderPlayback', 'config', (api, l) =>
    api.renderPlayback({
      samples: X,
      numChannels: 1,
      sampleRate: 48000,
      config: { input: { channelMap: l } },
    } as never),
  ),
  row(
    'PlaybackRenderer.constructor',
    'config',
    (api, l) =>
      new api.PlaybackRenderer({
        sampleRate: 48000,
        config: { input: { channelMap: l } },
      } as never),
    'PlaybackRenderer',
  ),
  row('PlaybackRenderer.processPlanar', 'planes', (api, l) =>
    new api.PlaybackRenderer({ sampleRate: 48000, config: {} } as never).processPlanar(l),
  ),
  row('PlaybackRenderer.setConfig', 'config', (api, l) =>
    new api.PlaybackRenderer({ sampleRate: 48000, config: {} } as never).setConfig({
      input: { channelMap: l },
    } as never),
  ),
  row('PolyphonicAnalysis.setNoteEdit', 'amplitudeEnvelope', (api, l) =>
    api
      .analyzePolyphonic({
        samples: new Float32Array(22050).map(
          (_, i) => 0.3 * Math.sin((2 * Math.PI * 440 * i) / 22050),
        ),
        sampleRate: 22050,
      } as never)
      .setNoteEdit(0, { amplitudeEnvelope: l } as never),
  ),
  row('VocalEditDraft.apply', 'operations', (api, l) => {
    const session = vocalSession(api);
    try {
      return session.beginEdit().apply({ operations: l } as never);
    } finally {
      session.destroy();
    }
  }),
];

/** The `Class.method` a row label names; a label is the method, then an optional field. */
const callableOf = (cls: string, row: Row<never>): string => `${cls}.${row[0].split(' ')[0]}`;

/**
 * Callables whose refusal is asserted outside the row loops: the Mixer scene document through its
 * constructor, and the request-key walk through `configureCapture`.
 */
export const OUT_OF_TABLE_COVERED: readonly string[] = [
  'Mixer.constructor',
  'RealtimeEngine.configureCapture',
];

/** Every `Class.method` the rows exercise, in the spelling the callable enumerator uses. */
export const LIST_BUDGET_COVERED: readonly string[] = [
  ...new Set([
    ...ENGINE_ROWS.map((row) => callableOf('RealtimeEngine', row as Row<never>)),
    ...PROJECT_ROWS.map((row) => callableOf('Project', row as Row<never>)),
    ...MIXER_ROWS.map((row) => callableOf('Mixer', row as Row<never>)),
    ...OUT_OF_TABLE_COVERED,
    ...FACADE_ROWS.map((row) => row.callable),
  ]),
];
