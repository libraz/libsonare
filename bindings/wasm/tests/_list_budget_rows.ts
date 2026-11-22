/**
 * The per-setter rows of the caller-list budget evidence: each row hands a list past the budget to
 * one public method and names the refusal the method owes. The rows are data so the
 * list-taking-callable coverage test can join them against the callables the type checker finds.
 */

import type { RealtimeEngine } from '../dist/index.js';
import type { SonareEngine } from '../dist/worklet.js';

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
    text: overBudget('strip.sends'),
    call: (e, l) => e.setStrip(1, { sends: l } as never),
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
    text: overBudget('strip.sends'),
    call: (e, l) => e.setStrip('master', { sends: l } as never),
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
];

/** Every `Class.method` the rows exercise, in the spelling the callable enumerator uses. */
export const LIST_BUDGET_COVERED: readonly string[] = [
  ...new Set([
    ...REALTIME_ENGINE_ROWS.map((row) => `RealtimeEngine.${row.method}`),
    ...SONARE_ENGINE_ROWS.map((row) => `SonareEngine.${row.method}`),
    ...FACADE_ROWS.map((row) => row.callable),
  ]),
];
