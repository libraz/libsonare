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
  'SonarePlaybackWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
  'SonareStreamAnalyzerWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
  'SonareRealtimeVoiceChangerWorkletProcessor.process':
    'inputs and outputs are the port arrays the audio runtime hands each render quantum',
};

/** Callables that walk a caller list in the facade with no budget check, by the walking site. */
const KNOWN_UNGUARDED: Record<string, string> = {};

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
