/**
 * A caller-supplied list longer than the input length budget is refused by
 * name before the addon sizes or walks anything from it.
 *
 * Driven against the addon rather than the TypeScript facade, whose own guard
 * would pre-empt the native one. The lists are sparse: they claim one element
 * past the budget while holding none, so the refusal is the only way the call
 * can end without allocating per claimed element.
 */

import { describe, expect, it } from 'vitest';
import { addon } from '../src/native.js';

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

const BUDGET = 64 * 1024 * 1024;

function sparse(): unknown[] {
  const list: unknown[] = [];
  list.length = BUDGET + 1;
  return list;
}

function engine() {
  const e = new native.RealtimeEngine(48000, 256);
  e.prepare(48000, 256, 1024, 1024, 8);
  return e;
}

function mixer() {
  return new native.Mixer(native.mixingScenePresetJson('commentaryDucking'), 48000, 8);
}

// Each row names one reader family and the subject its refusal carries.
const CASES: Array<[string, string, () => unknown]> = [
  ['RealtimeEngine.setMarkers', 'markers', () => engine().setMarkers(sparse())],
  ['RealtimeEngine.setClips', 'clips', () => engine().setClips(sparse())],
  ['RealtimeEngine.setTrackLanes', 'track lanes', () => engine().setTrackLanes(sparse())],
  ['RealtimeEngine.setTrackBuses', 'track buses', () => engine().setTrackBuses(sparse())],
  ['RealtimeEngine.setTempoSegments', 'segments', () => engine().setTempoSegments(sparse())],
  [
    'RealtimeEngine.setGraph',
    'spec.nodes',
    () => engine().setGraph({ nodes: sparse(), connections: [] }),
  ],
  ['Project.setTempoSegments', 'segments', () => new native.Project().setTempoSegments(sparse())],
  ['Project.annotateKeys', 'keys', () => new native.Project().annotateKeys(sparse())],
  ['Project.annotateChords', 'chords', () => new native.Project().annotateChords(sparse())],
  ['Mixer.addVcaGroup', 'members', () => mixer().addVcaGroup('vca', 0, sparse())],
  ['Mixer.processStereo', 'leftInputs', () => mixer().processStereo(sparse(), [])],
  [
    'tempogramRatio',
    'factors',
    () => native.tempogramRatio(new Float32Array(8), 2, 22050, 512, sparse()),
  ],
];

describe('addon list-length budget', () => {
  it.each(CASES)('%s refuses a list past the budget by name', (_entry, subject, call) => {
    expect(call).toThrow(RangeError);
    expect(call).toThrow(`${subject} exceeds the input length budget`);
  });

  it('leaves a list inside the budget to the per-kind limit and its precise refusal', () => {
    const lanes = Array.from({ length: 33 }, (_, i) => ({ trackId: i + 1 }));
    expect(() => engine().setTrackLanes(lanes)).toThrow(RangeError);
    expect(() => engine().setTrackLanes(lanes)).toThrow(
      'lanes.length must be an integer in [0, 32]',
    );
  });
});
