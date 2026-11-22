/**
 * Every public callable whose parameters reach a caller-built list is either exercised by a
 * budget row, exempt with a reason, or named as a known gap. The callables come from the package's
 * own type program, so a new list-taking method cannot slip past the budget evidence unseen.
 */

import { describe, expect, it } from 'vitest';
import { LIST_BUDGET_COVERED } from './_list_budget_rows';
import { enumerateCallables } from './_list_taking_callables';

const ENTRIES = ['index.ts', 'worklet.ts'];

/** Callables with no row, and why none is owed. */
const EXEMPT: Record<string, string> = {
  bindMicrophoneInput:
    'options.audio.advanced and deviceId are MediaTrackConstraints lists handed to getUserMedia unread',
  'PlaybackRenderer.processPlanar':
    'planes go to the native processPlanar as given; no facade walk',
  'ClipPageProvider.supply': 'channels go to the native supply as given; no facade walk',
  'PolyphonicAnalysis.setNoteEdit':
    'edit is spread one level and amplitudeEnvelope reaches the native setter as given',
  'OfflineWorkerClient.analyze': 'request is structured-cloned to the worker; no facade walk',
  'OfflineWorkerClient.detectKey': 'request is structured-cloned to the worker; no facade walk',
  'OfflineWorkerClient.masterAudio': 'request is structured-cloned to the worker; no facade walk',
  'OfflineWorkerClient.masterAudioStereo':
    'request is structured-cloned to the worker; no facade walk',
  'VocalEditWorkerDraft.apply': 'operations are structured-cloned to the worker; no facade walk',
  'SonareEngine.create':
    'initialSyncMessages and initialCommands ride processorOptions to the node; no facade walk',
  'SonareRealtimeEngineNode.create':
    'initialSyncMessages and initialCommands ride processorOptions to the processor; no walk here',
  'SonareRealtimeEngineNode.postSync':
    'message is spread one level and posted over the port; no facade walk',
  'SonareRealtimeEngineWorkletProcessor.constructor':
    'options are stored; the registration wrapper replays the initial messages',
  'SonareRealtimeEngineWorkletProcessor.receiveSync':
    'messages are built by the SonareEngine setters after their budget checks',
  'SonareRealtimeEngineWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
  'SonareWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
  'SonarePlaybackNode.setConfig':
    'config is posted over the port; the stringify is in the renderer',
  'SonarePlaybackNode.create': 'config is posted over the port; the stringify is in the renderer',
  'SonarePlaybackWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
  'SonareStreamAnalyzerWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
  'SonareRealtimeVoiceChangerWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
};

const UNGUARDED = 'facade walks the list unguarded; fix pending';

/** Callables that walk a caller list in the facade with no budget check, by the walking site. */
const KNOWN_UNGUARDED: Record<string, string> = {};
function unguarded(site: string, names: readonly string[]): void {
  for (const name of names) {
    KNOWN_UNGUARDED[name] = `${UNGUARDED} (${site})`;
  }
}
unguarded('mixer.ts constructor, JSON.stringify of scene', ['Mixer.constructor']);
unguarded('vocal_project.ts copyOriginalSources', ['Project.rehydrateVocalEdits']);
unguarded('project_internal.ts interleaveChannels', ['Project.addClip', 'Project.setSourceAudio']);
unguarded('project_class.ts importExternalStems', ['Project.importExternalStems']);
unguarded('project_class.ts interleaveLoopRecording', ['Project.addLoopRecordingTakes']);
unguarded('project_internal.ts assertProjectMidiEvents', ['Project.setMidiEvents']);
unguarded('project_class.ts bounce instrument lists', ['Project.bounce']);
unguarded('project_internal.ts takesToNative', ['Project.setClipTakes']);
unguarded('project_internal.ts compSegmentsToNative', ['Project.setClipCompSegments']);
unguarded('project_internal.ts projectAutomationLaneValue', [
  'Project.addAutomationLane',
  'Project.editAutomationLane',
]);
unguarded('project_internal.ts projectKeySegmentsToNative', ['Project.annotateKeys']);
unguarded('project_internal.ts projectChordSymbolsToNative', ['Project.annotateChords']);
unguarded('analysis_helpers.ts keyModeValues', [
  'Audio.detectKey',
  'Audio.detectKeyCandidates',
  'detectKey',
  'detectKeyCandidates',
]);
unguarded('internal/chain_config.ts chainConfigJson, JSON.stringify of the config', [
  'Audio.masterAudio',
  'masterAudio',
  'masterAudioStereo',
  'streamingLoudnessGain',
  'streamingLoudnessGainStereo',
  'StreamingMasteringChain.constructor',
]);
unguarded('internal/effects_common.ts toVoicedFloat32 and resolveEntryTimes', [
  'decomposeNotePitch',
  'extractNotes',
  'mergeNotes',
  'noteSegments',
  'pitchCorrectTimevarying',
  'renderNotes',
  'splitNote',
]);
unguarded('effects_percussive.ts asFloat32 and percussiveRequest, Float32Array.from', [
  'extractPercussiveEvents',
  'renderPercussiveEvents',
]);
unguarded('validation.ts assertAudioChannels', [
  'masteringRepairAnalyze',
  'masteringRepairApply',
  'masteringRepairDenoiseClassicalLinked',
  'masteringRepairDereverbClassicalLinked',
  'decomposeStemsLinked',
  'lufsMultichannel',
  'lufsSeriesMultichannel',
  'realtimeVoiceChangerRender',
]);
unguarded('internal/metering_native.ts interleaveMeterChannels', [
  'waveformPeaks',
  'waveformPeakPyramid',
]);
unguarded('mastering_core.ts previewPlatforms', [
  'masteringStreamingPreview',
  'masteringStreamingPreviewStereo',
]);
unguarded('mixing_oneshot.ts mixStereo strips.forEach', ['mixStereo']);
unguarded('effects_spectral.ts spectralEdit ops.map', ['spectralEdit']);
unguarded('validation.ts toInt32Array', [
  'fixFrames',
  'onsetBacktrack',
  'remix',
  'remixAlignedIntervals',
  'segmentSubsegment',
]);
unguarded('feature_core.ts assertCommonSignals', [
  'splitSilenceCommon',
  'splitSilenceCommonWithReport',
]);
unguarded('internal/midi_cc_binding.ts assertMidiCcBindingListBytes', [
  'midiCcToBreakpoint',
  'midiParamToCc',
]);
unguarded('mixing_assistant.ts planarTracks', ['mixingAssistantSuggest']);
unguarded('quick_analysis.ts chordFunctions entries.map', ['chordFunctions']);
unguarded('quick_analysis.ts estimateMeter beats.forEach', ['estimateMeter']);
unguarded('quick_analysis.ts roomGeometryFromEstimate Float32Array.from', [
  'roomGeometryFromEstimate',
]);
unguarded('playback_renderer.ts configJsonText, JSON.stringify of the config', [
  'renderPlayback',
  'PlaybackRenderer.constructor',
  'PlaybackRenderer.setConfig',
  'SonarePlaybackWorkletProcessor.constructor',
  'SonarePlaybackWorkletProcessor.receiveMessage',
]);
unguarded(
  'internal/mix_scene.ts and engine strip setters, JSON.stringify of inserts and eq.bands',
  [
    'RealtimeEngine.setStrip',
    'RealtimeEngine.setBusStrip',
    'SonareEngine.setStrip',
    'SonareEngine.setBusStrip',
  ],
);
unguarded('realtime_voice_changer.ts assertPlanes', ['RealtimeVoiceChanger.processPlanar']);
unguarded('opfs_clip_pages.ts validateOpfsClipChannels', ['writeOpfsClip']);
unguarded('vocal_edit.ts resolveOperationTimes', ['VocalEditDraft.apply']);
unguarded('web_midi.ts bindWebMidi, Set(inputIds) and for-of ccBindings', ['bindWebMidi']);

const MIN_LIST_TAKING = 116;
const MIN_CALLABLES = 855;

describe('list-taking public callables are covered by the caller-list budget evidence', () => {
  const callables = enumerateCallables(ENTRIES);
  const listTaking = [...callables.values()].filter((c) => c.listTaking);
  const covered = new Set(LIST_BUDGET_COVERED);

  it('finds enough of the surface for the walk to be trusted', () => {
    expect(callables.size).toBeGreaterThanOrEqual(MIN_CALLABLES);
    expect(listTaking.length).toBeGreaterThanOrEqual(MIN_LIST_TAKING);
  });

  it('leaves no list-taking callable without a row, an exemption or a recorded gap', () => {
    const missing = listTaking
      .filter((c) => !covered.has(c.name) && !(c.name in EXEMPT) && !(c.name in KNOWN_UNGUARDED))
      .map((c) => `${c.name} (${c.where}): ${c.listParams.join(', ')}`);
    expect(missing, 'add a row to _list_budget_rows.ts, or an EXEMPT reason').toEqual([]);
  });

  it('holds no stale row, exemption or recorded gap', () => {
    // A row may name a callable whose declared types carry no list (the facade accepts one the
    // types do not name); it must still exist. An exemption or a recorded gap must be list-taking.
    const live = new Set(listTaking.map((c) => c.name));
    const stale = [
      ...[...covered].filter((n) => !callables.has(n)).map((n) => `row ${n}`),
      ...Object.keys(EXEMPT)
        .filter((n) => !live.has(n))
        .map((n) => `EXEMPT ${n}`),
      ...Object.keys(KNOWN_UNGUARDED)
        .filter((n) => !live.has(n))
        .map((n) => `KNOWN_UNGUARDED ${n}`),
    ];
    expect(stale).toEqual([]);
  });

  it('keeps an exemption apart from a row and from a recorded gap', () => {
    const overlap = Object.keys(EXEMPT).filter((n) => covered.has(n) || n in KNOWN_UNGUARDED);
    expect(overlap).toEqual([]);
  });

  it('gives every exemption a reason', () => {
    for (const [name, reason] of Object.entries(EXEMPT)) {
      expect(reason.length, name).toBeGreaterThan(20);
    }
  });
});
