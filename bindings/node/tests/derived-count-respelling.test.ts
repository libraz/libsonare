/**
 * A count the native entry derives from an array's length was never written by the caller, so a
 * refusal of it names that array's length, under the predicate the C ABI states.
 */

import { describe, expect, it } from 'vitest';
import {
  mergeNotes,
  Project,
  pitchCorrectTimevarying,
  RealtimeEngine,
  renderNotes,
  StreamingRetune,
  splitNote,
} from '../src/index.js';

const REFUSAL = Symbol.for('libsonare.refusal');
const SR = 22050;
const samples = new Float32Array(11025);
const f0Hz = new Float32Array(20).fill(220);
const voicedProb = new Float32Array(20).fill(1);
const note = { onsetSample: 0, offsetSample: 5000, frameStart: 0, frameEnd: 10, medianHz: 220 };

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

describe('derived counts are named by the array they come from', () => {
  it.each([
    [
      'nFrames',
      () => pitchCorrectTimevarying({ samples, sampleRate: SR, f0Hz: new Float32Array() }),
      'pitchCorrectTimevarying: f0Hz.length must be an integer of at least 1',
      ['f0Hz.length'],
    ],
    [
      'nFrames',
      () =>
        renderNotes({
          samples,
          sampleRate: SR,
          notes: [{ ...note, frameEnd: 40, edit: { pitchSemitones: 1 } }],
          f0Hz,
          frameRate: 40,
        }),
      'renderNotes: notes[0].frameEnd must not exceed f0Hz.length',
      ['notes[0].frameEnd', 'f0Hz.length'],
    ],
    [
      'noteCount',
      () =>
        splitNote({
          samples,
          sampleRate: SR,
          f0Hz,
          voicedProb,
          frameRate: 40,
          notes: [note],
          index: 3,
          frame: 5,
        }),
      'splitNote: index must be less than notes.length',
      ['index', 'notes.length'],
    ],
    [
      'noteCount',
      () =>
        mergeNotes({
          samples,
          sampleRate: SR,
          f0Hz,
          voicedProb,
          frameRate: 40,
          notes: [note],
          first: 0,
          last: 4,
        }),
      'mergeNotes: last must be less than notes.length',
      ['last', 'notes.length'],
    ],
    [
      'anchorCount',
      () => {
        using project = new Project();
        project.setWarpMap({ id: 1, anchors: [{ sourceSample: 0, warpSample: 0 }] });
      },
      'Project.setWarpMap: anchors.length must be an integer in [2, 500000000]',
      ['anchors.length'],
    ],
    [
      'laneCount',
      () => {
        using engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
        engine.setTrackLanes(Array.from({ length: 33 }, (_, i) => ({ trackId: i + 1 })));
      },
      'RealtimeEngine.setTrackLanes: lanes.length must be an integer in [0, 32]',
      ['lanes.length'],
    ],
    [
      'numChannels',
      () => {
        using engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
        engine.renderOffline({
          channels: Array.from({ length: 65 }, () => new Float32Array(16)),
        });
      },
      'RealtimeEngine.renderOffline: channels.length must be an integer in [1, 64]',
      ['channels.length'],
    ],
    [
      'numSamples',
      () => {
        using retune = new StreamingRetune();
        retune.prepare(48000, 128);
        retune.processMono(new Float32Array(1000));
      },
      'StreamingRetune.processMono: samples.length must be an integer in [0, 128]',
      ['samples.length'],
    ],
  ] as const)('%s', (_count, run, text, subjects) => {
    const error = caught(run);
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(text);
    expect((error as never as Record<symbol, { subjects: string[] }>)[REFUSAL].subjects).toEqual(
      subjects,
    );
  });
});
