/**
 * The per-setter rows of the caller-list budget evidence: each row hands a list past the budget to
 * one public method and names the refusal the method owes. The rows are data so the
 * list-taking-callable coverage test can join them against the callables the type checker finds.
 */

import type { MixSceneStripInput, RealtimeEngine } from '../dist/index.js';
import type { SonareEngine } from '../dist/worklet.js';
import * as worklet from '../dist/worklet.js';

/** One method refusing one oversized list, with the text after `Class.method: `. */
export interface ListBudgetRow<E> {
  method: string;
  text: string;
  call: (engine: E, list: never) => unknown;
}

/** The refusal text for a list named `subject` that is past the native list budget. */
export const overBudget = (subject: string): string =>
  `${subject} exceeds the WASM Float32 input budget`;

const UMP_WORD_COUNT = 'words must hold 1 to 4 words';

export const REALTIME_ENGINE_ROWS: ReadonlyArray<ListBudgetRow<RealtimeEngine>> = [
  { method: 'setMidiClips', text: overBudget('clips'), call: (e, l) => e.setMidiClips(l) },
  {
    method: 'setAutomationLane',
    text: overBudget('points'),
    call: (e, l) => e.setAutomationLane(1, l),
  },
  {
    method: 'setGraph',
    text: overBudget('nodes'),
    call: (e, l) => e.setGraph({ inputNode: 'in', outputNode: 'out', nodes: l, connections: [] }),
  },
  {
    method: 'setGraph',
    text: overBudget('connections'),
    call: (e, l) => e.setGraph({ inputNode: 'in', outputNode: 'out', nodes: [], connections: l }),
  },
  { method: 'setClips', text: overBudget('clips'), call: (e, l) => e.setClips(l) },
  { method: 'setTrackLanes', text: overBudget('lanes'), call: (e, l) => e.setTrackLanes(l) },
  {
    method: 'setTrackLanes',
    text: overBudget('sends'),
    call: (e, l) => e.setTrackLanes([{ trackId: 1, sends: l }]),
  },
  { method: 'setTrackSends', text: overBudget('sends'), call: (e, l) => e.setTrackSends(1, l) },
  { method: 'setTrackBuses', text: overBudget('buses'), call: (e, l) => e.setTrackBuses(l) },
  {
    method: 'setTrackBuses',
    text: overBudget('sends'),
    call: (e, l) => e.setTrackBuses([{ busId: 1, sends: l }]),
  },
  {
    method: 'setStrip',
    text: overBudget('settings.sends'),
    call: (e, l) => e.setStrip(1, { sends: l } as never),
  },
  {
    method: 'setStrip',
    text: overBudget('settings'),
    call: (e, l) => e.setStrip(1, { inserts: l } as never),
  },
  {
    method: 'setStrip',
    text: overBudget('settings'),
    call: (e, l) => e.setStrip(1, { eq: { bands: l } } as never),
  },
  {
    method: 'setBusStrip',
    text: overBudget('settings'),
    call: (e, l) => e.setBusStrip(1, { inserts: l } as never),
  },
  {
    method: 'upsertClip',
    text: overBudget('channels'),
    call: (e, l) => e.upsertClip({ id: 1, trackId: 1, channels: l } as never),
  },
  { method: 'process', text: overBudget('channels'), call: (e, l) => e.process(l) },
  {
    method: 'setPartRig',
    text: overBudget('inserts'),
    call: (e, l) => e.setPartRig({ destinationId: 1, part: 0, mode: 'chain', inserts: l }),
  },
  {
    method: 'setSynthInstrument',
    text: overBudget('modRoutings'),
    call: (e, l) => e.setSynthInstrument(1, { modRoutings: l } as never),
  },
  {
    method: 'setMidiFx',
    text: overBudget('config'),
    call: (e, l) => e.setMidiFx(1, { chordIntervals: l }),
  },
  {
    method: 'renderOffline',
    text: overBudget('channels'),
    call: (e, l) => e.renderOffline({ channels: l }),
  },
  {
    method: 'setTempoSegments',
    text: overBudget('segments'),
    call: (e, l) => e.setTempoSegments(l),
  },
  {
    method: 'setTimeSignatureSegments',
    text: overBudget('segments'),
    call: (e, l) => e.setTimeSignatureSegments(l),
  },
  { method: 'setMarkers', text: overBudget('markers'), call: (e, l) => e.setMarkers(l) },
  { method: 'pushMidiUmp', text: UMP_WORD_COUNT, call: (e, l) => e.pushMidiUmp(1, l) },
  { method: 'pushMidiInputUmp', text: UMP_WORD_COUNT, call: (e, l) => e.pushMidiInputUmp(l) },
];

export const SONARE_ENGINE_ROWS: ReadonlyArray<ListBudgetRow<SonareEngine>> = [
  {
    method: 'setTempoSegments',
    text: overBudget('segments'),
    call: (e, l) => e.setTempoSegments(l),
  },
  {
    method: 'setTimeSignatureSegments',
    text: overBudget('segments'),
    call: (e, l) => e.setTimeSignatureSegments(l),
  },
  {
    method: 'setAutomationLane',
    text: overBudget('points'),
    call: (e, l) => e.setAutomationLane(1, l),
  },
  { method: 'setTrackLanes', text: overBudget('lanes'), call: (e, l) => e.setTrackLanes(l) },
  {
    method: 'setTrackLanes',
    text: overBudget('sends'),
    call: (e, l) => e.setTrackLanes([{ trackId: 1, sends: l }]),
  },
  { method: 'setTrackSends', text: overBudget('sends'), call: (e, l) => e.setTrackSends(1, l) },
  { method: 'setMidiClips', text: overBudget('clips'), call: (e, l) => e.setMidiClips(l) },
  {
    method: 'setMidiClips',
    text: overBudget('events'),
    call: (e, l) => e.setMidiClips([{ events: l } as never]),
  },
  {
    method: 'setStrip',
    text: overBudget('settings.sends'),
    call: (e, l) => e.setStrip('master', { sends: l } as never),
  },
  {
    method: 'setStrip',
    text: overBudget('settings'),
    call: (e, l) => e.setStrip('master', { inserts: l } as never),
  },
  {
    method: 'setStrip',
    text: overBudget('settings'),
    call: (e, l) => e.setStrip('master', { eq: { bands: l } } as never),
  },
  {
    method: 'setBusStrip',
    text: overBudget('settings'),
    call: (e, l) => e.setBusStrip(1, { inserts: l } as never),
  },
  { method: 'setTrackBuses', text: overBudget('buses'), call: (e, l) => e.setTrackBuses(l) },
  {
    method: 'setTrackBuses',
    text: overBudget('sends'),
    call: (e, l) => e.setTrackBuses([{ busId: 1, sends: l }]),
  },
  {
    method: 'setSynthInstrument',
    text: overBudget('modRoutings'),
    call: (e, l) => e.setSynthInstrument(1, { modRoutings: l } as never),
  },
  {
    method: 'setMidiFx',
    text: overBudget('config'),
    call: (e, l) => e.setMidiFx(1, { chordIntervals: l }),
  },
  {
    method: 'addClip',
    text: overBudget('channels'),
    call: (e, l) => e.addClip(1, { id: 1, channels: l } as never),
  },
  { method: 'setMarkers', text: overBudget('markers'), call: (e, l) => e.setMarkers(l) },
  { method: 'pushMidiUmp', text: UMP_WORD_COUNT, call: (e, l) => e.pushMidiUmp(1, l) },
  { method: 'pushMidiInputUmp', text: UMP_WORD_COUNT, call: (e, l) => e.pushMidiInputUmp(l) },
];

/** The public API namespace the facade rows call. */
export type Api = typeof import('../dist/index.js');

/** One facade callable refusing one oversized list, with the whole refusal message. */
export interface FacadeBudgetRow {
  /** `functionName` or `Class.method`, the spelling the callable enumerator uses. */
  callable: string;
  message: string;
  run: (api: Api, list: never) => unknown;
}

const SAMPLES = new Float32Array(4096).fill(0.1);
const SPEAKERS = { target: { kind: 'speakers', layout: 'stereo' } } as const;
const MATRIX = { data: new Float32Array(4), rows: 2, cols: 2 };
const BAND_ABSORPTION_ESTIMATE = { lengthM: 4, widthM: 3, heightM: 2.5 };

/** Installs a Web MIDI stub for the length of `call`, then restores the previous navigator. */
const withMidiAccess = async (call: () => Promise<unknown>): Promise<unknown> => {
  const previous = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: { requestMIDIAccess: async () => ({}) },
  });
  try {
    return await call();
  } finally {
    if (previous === undefined) {
      Reflect.deleteProperty(globalThis, 'navigator');
    } else {
      Object.defineProperty(globalThis, 'navigator', previous);
    }
  }
};

/** Runs `call` against a playback processor whose port keeps the refusal it was answered with. */
const throughPlaybackPort = (call: (port: { postMessage: (m: unknown) => void }) => void): void => {
  let refusal: string | undefined;
  call({
    postMessage: (message) => {
      const posted = message as { type?: string; message?: string };
      if (posted.type === 'error') {
        refusal = posted.message;
      }
    },
  });
  if (refusal !== undefined) {
    throw new RangeError(refusal);
  }
};

const withMixer = (api: Api, call: (mixer: InstanceType<Api['Mixer']>) => unknown): unknown => {
  const mixer = new api.Mixer({ sampleRate: 48000 });
  try {
    return call(mixer);
  } finally {
    mixer.destroy();
  }
};

const withProject = (
  api: Api,
  call: (project: InstanceType<Api['Project']>) => unknown,
): unknown => {
  const project = new api.Project();
  try {
    return call(project);
  } finally {
    project.destroy();
  }
};

export const FACADE_ROWS: ReadonlyArray<FacadeBudgetRow> = [
  {
    callable: 'Mixer.addVcaGroup',
    message: `Mixer.addVcaGroup: ${overBudget('members')}`,
    run: (api, l) => withMixer(api, (m) => m.addVcaGroup('v', 0, l)),
  },
  {
    callable: 'Mixer.setVcaGroupMembers',
    message: `Mixer.setVcaGroupMembers: ${overBudget('members')}`,
    run: (api, l) => withMixer(api, (m) => m.setVcaGroupMembers('v', l)),
  },
  {
    callable: 'Mixer.processStereo',
    message: `Mixer.processStereo: ${overBudget('leftInputs')}`,
    run: (api, l) => withMixer(api, (m) => m.processStereo(l, l)),
  },
  {
    callable: 'Mixer.processStereoInto',
    message: `Mixer.processStereoInto: ${overBudget('leftInputs')}`,
    run: (api, l) =>
      withMixer(api, (m) => m.processStereoInto(l, l, new Float32Array(4), new Float32Array(4))),
  },
  {
    callable: 'Project.setWarpMap',
    message: `Project.setWarpMap: ${overBudget('anchors')}`,
    run: (api, l) => withProject(api, (p) => p.setWarpMap({ id: 1, anchors: l })),
  },
  {
    callable: 'Project.setPartRig',
    message: `Project.setPartRig: ${overBudget('inserts')}`,
    run: (api, l) =>
      withProject(api, (p) =>
        p.setPartRig({ destinationId: 0, part: 0, mode: 'chain', inserts: l }),
      ),
  },
  {
    callable: 'Project.bakeMidiFx',
    message: `Project.bakeMidiFx: ${overBudget('config')}`,
    run: (api, l) =>
      withProject(api, (p) => p.bakeMidiFx({ clipId: 1, config: { chordIntervals: l } })),
  },
  {
    callable: 'Project.previewMidiFxCount',
    message: `Project.previewMidiFxCount: ${overBudget('config')}`,
    run: (api, l) =>
      withProject(api, (p) => p.previewMidiFxCount({ clipId: 1, config: { chordIntervals: l } })),
  },
  {
    callable: 'Project.setTempoSegments',
    message: `Project.setTempoSegments: ${overBudget('segments')}`,
    run: (api, l) => withProject(api, (p) => p.setTempoSegments(l)),
  },
  {
    callable: 'Project.setTimeSignatures',
    message: `Project.setTimeSignatures: ${overBudget('segments')}`,
    run: (api, l) => withProject(api, (p) => p.setTimeSignatures(l)),
  },
  {
    callable: 'downmix',
    message: `downmix: ${overBudget('channels')}`,
    run: (api, l) => api.downmix(l, 'stereo'),
  },
  {
    callable: 'assignNoteTargets',
    message: `assignNoteTargets: ${overBudget('notes')}`,
    run: (api, l) => api.assignNoteTargets({ notes: l, sampleRate: 48000, targets: [] }),
  },
  {
    callable: 'assignNoteTargets',
    message: `assignNoteTargets: ${overBudget('targets')}`,
    run: (api, l) => api.assignNoteTargets({ notes: [], sampleRate: 48000, targets: l }),
  },
  {
    callable: 'midiCcLearn',
    message: `midiCcLearn: ${overBudget('events')}`,
    run: (api, l) => api.midiCcLearn({ events: l, paramId: 1 }),
  },
  {
    callable: 'midiRouteEvents',
    message: `midiRouteEvents: ${overBudget('events')}`,
    run: (api, l) => api.midiRouteEvents({ events: l }),
  },
  {
    callable: 'waveformPeakPyramid',
    message: `waveformPeakPyramid: ${overBudget('samplesPerBucketLevels')}`,
    run: (api, l) => api.waveformPeakPyramid({ channels: [SAMPLES], samplesPerBucketLevels: l }),
  },
  {
    callable: 'tempogramRatio',
    message: `tempogramRatio: ${overBudget('factors')}`,
    run: (api, l) =>
      api.tempogramRatio({
        tempogram: { data: new Float32Array(4), rows: 2, cols: 2 },
        factors: l,
      }),
  },
  {
    callable: 'analyze',
    message: `analyze: ${overBudget('meterCandidateNumerators')}`,
    run: (api, l) =>
      api.analyze({ samples: SAMPLES, sampleRate: 22050, meterCandidateNumerators: l }),
  },
  {
    callable: 'Audio.analyze',
    message: `Audio.analyze: ${overBudget('meterCandidateNumerators')}`,
    run: (api, l) => api.Audio.fromBuffer(SAMPLES, 22050).analyze({ meterCandidateNumerators: l }),
  },
  {
    callable: 'estimateMeter',
    message: `estimateMeter: ${overBudget('candidateNumerators')}`,
    run: (api, l) => api.estimateMeter({ beats: [], candidateNumerators: l }),
  },
  {
    callable: 'roomMorph',
    message: `roomMorph: ${overBudget('bandAbsorption')}`,
    run: (api, l) => api.roomMorph({ samples: SAMPLES, sampleRate: 22050, bandAbsorption: l }),
  },
  {
    callable: 'roomMorphStereo',
    message: `roomMorphStereo: ${overBudget('bandAbsorption')}`,
    run: (api, l) =>
      api.roomMorphStereo({ left: SAMPLES, right: SAMPLES, sampleRate: 22050, bandAbsorption: l }),
  },
  {
    callable: 'synthesizeRir',
    message: `synthesizeRir: ${overBudget('bandAbsorption')}`,
    run: (api, l) => api.synthesizeRir({ sampleRate: 22050, bandAbsorption: l }),
  },
  {
    callable: 'Mixer.constructor',
    message: `Mixer: ${overBudget('scene')}`,
    run: (api, l) =>
      new api.Mixer({ sampleRate: 48000, scene: { strips: l as MixSceneStripInput[] } }),
  },
  {
    callable: 'Project.rehydrateVocalEdits',
    message: `Project.rehydrateVocalEdits: ${overBudget('originals')}`,
    run: (api, l) => withProject(api, (p) => p.rehydrateVocalEdits(l)),
  },
  {
    callable: 'Project.addClip',
    message: `Project.addClip: ${overBudget('channels')}`,
    run: (api, l) =>
      withProject(api, (p) => p.addClip({ trackId: 1, channels: l, sampleRate: 48000 } as never)),
  },
  {
    callable: 'Project.setSourceAudio',
    message: `Project.setSourceAudio: ${overBudget('channels')}`,
    run: (api, l) =>
      withProject(api, (p) => p.setSourceAudio({ sourceId: 1, channels: l, sampleRate: 48000 })),
  },
  {
    callable: 'Project.importExternalStems',
    message: `Project.importExternalStems: ${overBudget('stems')}`,
    run: (api, l) =>
      withProject(api, (p) => p.importExternalStems({ sampleRate: 48000, stems: l })),
  },
  {
    callable: 'Project.addLoopRecordingTakes',
    message: `Project.addLoopRecordingTakes: ${overBudget('audio')}`,
    run: (api, l) => withProject(api, (p) => p.addLoopRecordingTakes({ audio: l } as never)),
  },
  {
    callable: 'Project.setMidiEvents',
    message: `Project.setMidiEvents: ${overBudget('events')}`,
    run: (api, l) => withProject(api, (p) => p.setMidiEvents(1, l)),
  },
  {
    callable: 'Project.bounce',
    message: `Project.bounce: ${overBudget('builtinInstruments')}`,
    run: (api, l) => withProject(api, (p) => p.bounce({ builtinInstruments: l })),
  },
  {
    callable: 'Project.bounce',
    message: `Project.bounce: ${overBudget('instruments')}`,
    run: (api, l) => withProject(api, (p) => p.bounce({ instruments: l })),
  },
  {
    callable: 'Project.bounce',
    message: `Project.bounce: ${overBudget('synthInstruments')}`,
    run: (api, l) => withProject(api, (p) => p.bounce({ synthInstruments: l })),
  },
  {
    callable: 'Project.bounce',
    message: `Project.bounce: ${overBudget('sf2Instruments')}`,
    run: (api, l) => withProject(api, (p) => p.bounce({ sf2Instruments: l })),
  },
  {
    callable: 'Project.setClipTakes',
    message: `Project.setClipTakes: ${overBudget('takes')}`,
    run: (api, l) => withProject(api, (p) => p.setClipTakes(1, l)),
  },
  {
    callable: 'Project.setClipCompSegments',
    message: `Project.setClipCompSegments: ${overBudget('segments')}`,
    run: (api, l) => withProject(api, (p) => p.setClipCompSegments(1, l)),
  },
  {
    callable: 'Project.addAutomationLane',
    message: `Project.addAutomationLane: ${overBudget('points')}`,
    run: (api, l) =>
      withProject(api, (p) => p.addAutomationLane(1, { targetParamId: 1, points: l } as never)),
  },
  {
    callable: 'Project.editAutomationLane',
    message: `Project.editAutomationLane: ${overBudget('points')}`,
    run: (api, l) =>
      withProject(api, (p) => p.editAutomationLane(1, 1, { targetParamId: 1, points: l } as never)),
  },
  {
    callable: 'Project.annotateKeys',
    message: `Project.annotateKeys: ${overBudget('keys')}`,
    run: (api, l) => withProject(api, (p) => p.annotateKeys(l)),
  },
  {
    callable: 'Project.annotateChords',
    message: `Project.annotateChords: ${overBudget('chords')}`,
    run: (api, l) => withProject(api, (p) => p.annotateChords(l)),
  },
  {
    callable: 'Audio.detectKey',
    message: `Audio.detectKey: ${overBudget('modes')}`,
    run: (api, l) => api.Audio.fromBuffer(SAMPLES, 22050).detectKey({ modes: l }),
  },
  {
    callable: 'Audio.detectKeyCandidates',
    message: `Audio.detectKeyCandidates: ${overBudget('modes')}`,
    run: (api, l) => api.Audio.fromBuffer(SAMPLES, 22050).detectKeyCandidates({ modes: l }),
  },
  {
    callable: 'detectKey',
    message: `detectKey: ${overBudget('modes')}`,
    run: (api, l) => api.detectKey({ samples: SAMPLES, sampleRate: 22050, modes: l }),
  },
  {
    callable: 'detectKeyCandidates',
    message: `detectKeyCandidates: ${overBudget('modes')}`,
    run: (api, l) => api.detectKeyCandidates({ samples: SAMPLES, sampleRate: 22050, modes: l }),
  },
  {
    callable: 'Audio.masterAudio',
    message: `Audio.masterAudio: ${overBudget('overrides')}`,
    run: (api, l) =>
      api.Audio.fromBuffer(SAMPLES, 22050).masterAudio({ overrides: { stages: l } } as never),
  },
  {
    callable: 'masterAudio',
    message: `masterAudio: ${overBudget('overrides')}`,
    run: (api, l) =>
      api.masterAudio({ samples: SAMPLES, sampleRate: 22050, overrides: { stages: l } } as never),
  },
  {
    callable: 'masterAudioStereo',
    message: `masterAudioStereo: ${overBudget('overrides')}`,
    run: (api, l) =>
      api.masterAudioStereo({
        left: SAMPLES,
        right: SAMPLES,
        sampleRate: 22050,
        overrides: { stages: l },
      } as never),
  },
  {
    callable: 'streamingLoudnessGain',
    message: `streamingLoudnessGain: ${overBudget('config')}`,
    run: (api, l) =>
      api.streamingLoudnessGain({
        samples: SAMPLES,
        sampleRate: 22050,
        config: { stages: l },
      } as never),
  },
  {
    callable: 'streamingLoudnessGainStereo',
    message: `streamingLoudnessGainStereo: ${overBudget('config')}`,
    run: (api, l) =>
      api.streamingLoudnessGainStereo({
        left: SAMPLES,
        right: SAMPLES,
        sampleRate: 22050,
        config: { stages: l },
      } as never),
  },
  {
    callable: 'StreamingMasteringChain.constructor',
    message: `StreamingMasteringChain: ${overBudget('config')}`,
    run: (api, l) =>
      new api.StreamingMasteringChain({ sampleRate: 48000, numChannels: 1, stages: l } as never),
  },
  {
    callable: 'decomposeNotePitch',
    message: `decomposeNotePitch: ${overBudget('voiced')}`,
    run: (api, l) =>
      api.decomposeNotePitch({
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voiced: l,
        medianHz: 220,
      }),
  },
  {
    callable: 'extractNotes',
    message: `extractNotes: ${overBudget('voiced')}`,
    run: (api, l) =>
      api.extractNotes({
        samples: SAMPLES,
        sampleRate: 22050,
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voiced: l,
      }),
  },
  {
    callable: 'noteSegments',
    message: `noteSegments: ${overBudget('voiced')}`,
    run: (api, l) => api.noteSegments({ f0Hz: new Float32Array(4), frameRate: 100, voiced: l }),
  },
  {
    callable: 'pitchCorrectTimevarying',
    message: `pitchCorrectTimevarying: ${overBudget('voiced')}`,
    run: (api, l) =>
      api.pitchCorrectTimevarying({
        samples: SAMPLES,
        sampleRate: 22050,
        f0Hz: new Float32Array(4),
        voiced: l,
      } as never),
  },
  {
    callable: 'mergeNotes',
    message: `mergeNotes: ${overBudget('voiced')}`,
    run: (api, l) =>
      api.mergeNotes({
        samples: SAMPLES,
        sampleRate: 22050,
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voiced: l,
        notes: [],
        first: 0,
        last: 1,
      }),
  },
  {
    callable: 'splitNote',
    message: `splitNote: ${overBudget('voiced')}`,
    run: (api, l) =>
      api.splitNote({
        samples: SAMPLES,
        sampleRate: 22050,
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voiced: l,
        notes: [],
        index: 0,
        frame: 1,
      }),
  },
  {
    callable: 'renderNotes',
    message: `renderNotes: ${overBudget('voiced')}`,
    run: (api, l) =>
      api.renderNotes({
        samples: SAMPLES,
        sampleRate: 22050,
        notes: [],
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voiced: l,
      }),
  },
  {
    callable: 'mergeNotes',
    message: `mergeNotes: ${overBudget('notes')}`,
    run: (api, l) =>
      api.mergeNotes({
        samples: SAMPLES,
        sampleRate: 22050,
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voicedProb: new Float32Array(4),
        notes: l,
        first: 0,
        last: 1,
      }),
  },
  {
    callable: 'splitNote',
    message: `splitNote: ${overBudget('notes')}`,
    run: (api, l) =>
      api.splitNote({
        samples: SAMPLES,
        sampleRate: 22050,
        f0Hz: new Float32Array(4),
        frameRate: 100,
        voicedProb: new Float32Array(4),
        notes: l,
        index: 0,
        frame: 1,
      }),
  },
  {
    callable: 'renderNotes',
    message: `renderNotes: ${overBudget('notes')}`,
    run: (api, l) => api.renderNotes({ samples: SAMPLES, sampleRate: 22050, notes: l }),
  },
  {
    callable: 'extractPercussiveEvents',
    message: `extractPercussiveEvents: ${overBudget('samples')}`,
    run: (api, l) => api.extractPercussiveEvents({ samples: l, sampleRate: 22050 }),
  },
  {
    callable: 'extractPercussiveEvents',
    message: `extractPercussiveEvents: ${overBudget('samples')}`,
    run: (api, l) => api.extractPercussiveEvents(l, 22050),
  },
  {
    callable: 'renderPercussiveEvents',
    message: `renderPercussiveEvents: ${overBudget('samples')}`,
    run: (api, l) => api.renderPercussiveEvents({ samples: l, sampleRate: 22050, events: [] }),
  },
  {
    callable: 'renderPercussiveEvents',
    message: `renderPercussiveEvents: ${overBudget('samples')}`,
    run: (api, l) => api.renderPercussiveEvents(l, 22050, { events: [] }),
  },
  {
    callable: 'renderPercussiveEvents',
    message: `renderPercussiveEvents: ${overBudget('events')}`,
    run: (api, l) => api.renderPercussiveEvents({ samples: SAMPLES, sampleRate: 22050, events: l }),
  },
  {
    callable: 'masteringRepairAnalyze',
    message: `masteringRepairAnalyze: ${overBudget('channels')}`,
    run: (api, l) => api.masteringRepairAnalyze({ channels: l, sampleRate: 48000 }),
  },
  {
    callable: 'masteringRepairApply',
    message: `masteringRepairApply: ${overBudget('channels')}`,
    run: (api, l) => api.masteringRepairApply({ channels: l, sampleRate: 48000, stages: [] }),
  },
  {
    callable: 'masteringRepairDenoiseClassicalLinked',
    message: `masteringRepairDenoiseClassicalLinked: ${overBudget('channels')}`,
    run: (api, l) => api.masteringRepairDenoiseClassicalLinked({ channels: l, sampleRate: 48000 }),
  },
  {
    callable: 'masteringRepairDereverbClassicalLinked',
    message: `masteringRepairDereverbClassicalLinked: ${overBudget('channels')}`,
    run: (api, l) => api.masteringRepairDereverbClassicalLinked({ channels: l, sampleRate: 48000 }),
  },
  {
    callable: 'decomposeStemsLinked',
    message: `decomposeStemsLinked: ${overBudget('channels')}`,
    run: (api, l) => api.decomposeStemsLinked({ channels: l, sampleRate: 48000 }),
  },
  {
    callable: 'lufsMultichannel',
    message: `lufsMultichannel: ${overBudget('channels')}`,
    run: (api, l) => api.lufsMultichannel({ channels: l, sampleRate: 48000 }),
  },
  {
    callable: 'lufsSeriesMultichannel',
    message: `lufsSeriesMultichannel: ${overBudget('channels')}`,
    run: (api, l) => api.lufsSeriesMultichannel({ channels: l, sampleRate: 48000 }),
  },
  {
    callable: 'realtimeVoiceChangerRender',
    message: `realtimeVoiceChangerRender: ${overBudget('channels')}`,
    run: (api, l) => api.realtimeVoiceChangerRender({ channels: l, sampleRate: 48000 } as never),
  },
  {
    callable: 'waveformPeaks',
    message: `waveformPeaks: ${overBudget('channels')}`,
    run: (api, l) => api.waveformPeaks({ channels: l }),
  },
  {
    callable: 'waveformPeakPyramid',
    message: `waveformPeakPyramid: ${overBudget('channels')}`,
    run: (api, l) => api.waveformPeakPyramid({ channels: l }),
  },
  {
    callable: 'masteringStreamingPreview',
    message: `masteringStreamingPreview: ${overBudget('platforms')}`,
    run: (api, l) =>
      api.masteringStreamingPreview({ samples: SAMPLES, sampleRate: 22050, platforms: l }),
  },
  {
    callable: 'masteringStreamingPreviewStereo',
    message: `masteringStreamingPreviewStereo: ${overBudget('platforms')}`,
    run: (api, l) =>
      api.masteringStreamingPreviewStereo({
        left: SAMPLES,
        right: SAMPLES,
        sampleRate: 22050,
        platforms: l,
      }),
  },
  {
    callable: 'mixStereo',
    message: `mixStereo: ${overBudget('strips')}`,
    run: (api, l) => api.mixStereo({ sampleRate: 48000, strips: l }),
  },
  {
    callable: 'spectralEdit',
    message: `spectralEdit: ${overBudget('ops')}`,
    run: (api, l) => api.spectralEdit({ samples: SAMPLES, sampleRate: 22050, ops: l }),
  },
  {
    callable: 'fixFrames',
    message: `fixFrames: ${overBudget('frames')}`,
    run: (api, l) => api.fixFrames({ frames: l }),
  },
  {
    callable: 'onsetBacktrack',
    message: `onsetBacktrack: ${overBudget('events')}`,
    run: (api, l) => api.onsetBacktrack({ events: l, energy: new Float32Array(4) }),
  },
  {
    callable: 'remix',
    message: `remix: ${overBudget('intervals')}`,
    run: (api, l) => api.remix({ samples: SAMPLES, sampleRate: 22050, intervals: l }),
  },
  {
    callable: 'remixAlignedIntervals',
    message: `remixAlignedIntervals: ${overBudget('intervals')}`,
    run: (api, l) =>
      api.remixAlignedIntervals({ samples: SAMPLES, sampleRate: 22050, intervals: l }),
  },
  {
    callable: 'segmentSubsegment',
    message: `segmentSubsegment: ${overBudget('boundaries')}`,
    run: (api, l) => api.segmentSubsegment({ data: MATRIX, boundaries: l }),
  },
  {
    callable: 'splitSilenceCommon',
    message: `splitSilenceCommon: ${overBudget('signals')}`,
    run: (api, l) => api.splitSilenceCommon({ signals: l }),
  },
  {
    callable: 'splitSilenceCommonWithReport',
    message: `splitSilenceCommonWithReport: ${overBudget('signals')}`,
    run: (api, l) => api.splitSilenceCommonWithReport({ signals: l }),
  },
  {
    callable: 'midiCcToBreakpoint',
    message: `midiCcToBreakpoint: ${overBudget('bindings')}`,
    run: (api, l) => api.midiCcToBreakpoint({ bindings: l, event: {} } as never),
  },
  {
    callable: 'midiParamToCc',
    message: `midiParamToCc: ${overBudget('bindings')}`,
    run: (api, l) => api.midiParamToCc({ bindings: l, paramId: 1, unitValue: 0, group: 0 }),
  },
  {
    callable: 'mixingAssistantSuggest',
    message: `mixingAssistantSuggest: ${overBudget('tracks')}`,
    run: (api, l) => api.mixingAssistantSuggest({ tracks: l, sampleRate: 48000 }),
  },
  {
    callable: 'chordFunctions',
    message: `chordFunctions: ${overBudget('chords')}`,
    run: (api, l) => api.chordFunctions({ chords: l, key: { root: 0, mode: 'major' } }),
  },
  {
    callable: 'chordFunctions',
    message: `chordFunctions: ${overBudget('chords')}`,
    run: (api, l) => api.chordFunctions({ chords: { chords: l }, key: { root: 0, mode: 'major' } }),
  },
  {
    callable: 'estimateMeter',
    message: `estimateMeter: ${overBudget('beats')}`,
    run: (api, l) => api.estimateMeter({ beats: l }),
  },
  {
    callable: 'roomGeometryFromEstimate',
    message: `roomGeometryFromEstimate: ${overBudget('estimate.bandAbsorption')}`,
    run: (api, l) =>
      api.roomGeometryFromEstimate({
        estimate: { ...BAND_ABSORPTION_ESTIMATE, bandAbsorption: l },
      } as never),
  },
  {
    callable: 'renderPlayback',
    message: `renderPlayback: ${overBudget('config')}`,
    run: (api, l) =>
      api.renderPlayback({
        samples: SAMPLES,
        numChannels: 1,
        sampleRate: 48000,
        config: { layout: l },
      } as never),
  },
  {
    callable: 'PlaybackRenderer.constructor',
    message: `PlaybackRenderer: ${overBudget('config')}`,
    run: (api, l) =>
      new api.PlaybackRenderer({ sampleRate: 48000, config: { layout: l } } as never),
  },
  {
    callable: 'PlaybackRenderer.setConfig',
    message: `PlaybackRenderer.setConfig: ${overBudget('config')}`,
    run: (api, l) => {
      const renderer = new api.PlaybackRenderer({ sampleRate: 48000, config: SPEAKERS });
      try {
        renderer.setConfig({ layout: l } as never);
      } finally {
        renderer.destroy();
      }
    },
  },
  {
    callable: 'SonarePlaybackWorkletProcessor.constructor',
    message: `PlaybackRenderer: ${overBudget('config')}`,
    run: (_api, l) =>
      new worklet.SonarePlaybackWorkletProcessor({
        sampleRate: 48000,
        config: { layout: l },
      } as never),
  },
  {
    callable: 'SonarePlaybackWorkletProcessor.receiveMessage',
    message: `PlaybackRenderer.setConfig: ${overBudget('config')}`,
    run: (_api, l) =>
      throughPlaybackPort((port) => {
        const processor = new worklet.SonarePlaybackWorkletProcessor({
          sampleRate: 48000,
          config: SPEAKERS,
          port,
        });
        try {
          processor.receiveMessage({ type: 'config', config: { layout: l } } as never);
        } finally {
          processor.destroy();
        }
      }),
  },
  {
    callable: 'SonarePlaybackNode.create',
    message: `SonarePlaybackNode.create: ${overBudget('config')}`,
    run: (_api, l) =>
      worklet.SonarePlaybackNode.create(
        { sampleRate: 48000 } as never,
        {
          config: { layout: l },
        } as never,
      ),
  },
  {
    callable: 'RealtimeVoiceChanger.processPlanar',
    message: `RealtimeVoiceChanger.processPlanar: ${overBudget('channels')}`,
    run: (api, l) => {
      const changer = new api.RealtimeVoiceChanger({ sampleRate: 48000, numChannels: 1 });
      try {
        changer.processPlanar(l);
      } finally {
        changer.destroy();
      }
    },
  },
  {
    callable: 'writeOpfsClip',
    message: `writeOpfsClip: ${overBudget('channels')}`,
    run: (api, l) => api.writeOpfsClip('clips/a.f32', l),
  },
  {
    callable: 'VocalEditDraft.apply',
    message: `VocalEditDraft.apply: ${overBudget('operations')}`,
    run: (api, l) => {
      const session = api.createVocalEditSession({ samples: SAMPLES, sampleRate: 16000 });
      try {
        session.beginEdit().apply({ operations: l });
      } finally {
        session.destroy();
      }
    },
  },
  {
    callable: 'bindWebMidi',
    message: `bindWebMidi: ${overBudget('inputIds')}`,
    run: (api, l) => withMidiAccess(() => api.bindWebMidi({} as never, { inputIds: l })),
  },
  {
    callable: 'bindWebMidi',
    message: `bindWebMidi: ${overBudget('ccBindings')}`,
    run: (api, l) => withMidiAccess(() => api.bindWebMidi({} as never, { ccBindings: l })),
  },
];

/** Every `Class.method` the rows exercise, in the spelling the callable enumerator uses. */
export const LIST_BUDGET_COVERED: readonly string[] = [
  ...new Set([
    ...REALTIME_ENGINE_ROWS.map((row) => `RealtimeEngine.${row.method}`),
    ...SONARE_ENGINE_ROWS.map((row) => `SonareEngine.${row.method}`),
    ...FACADE_ROWS.map((row) => row.callable),
  ]),
];
