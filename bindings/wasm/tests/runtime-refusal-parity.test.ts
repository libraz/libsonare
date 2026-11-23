/**
 * Runtime inputs whose acceptance or refusal class is shared with the Node binding: the same call
 * is accepted, or refused with the same class and text, on both surfaces.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePreset, Project, RealtimeEngine } from '../dist/index.js';
import { type ArgumentRefusalClass, expectRefusalOf } from './_helpers';

const SIZE_T_TEXT =
  'must be a finite non-negative integer no greater than Number.MAX_SAFE_INTEGER or the native size_t maximum';

function expectRefusal(run: () => unknown, kind: ArgumentRefusalClass, message: RegExp): void {
  expect(expectRefusalOf(run, kind).message).toMatch(message);
}

function exactly(text: string): RegExp {
  return new RegExp(`^${text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`);
}

describe('runtime refusal parity (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses empty MIDI file bytes as an empty value, not a missing one', () => {
    const project = new Project();
    try {
      expectRefusal(
        () => project.importSmf(new Uint8Array(0)),
        RangeError,
        exactly('Project.importSmf: data must not be empty'),
      );
      expectRefusal(
        () => project.importClipFile(new Uint8Array(0)),
        RangeError,
        exactly('Project.importClipFile: data must not be empty'),
      );
    } finally {
      project.destroy();
    }
  });

  it('takes an undo depth of 0 and refuses a fractional one with the size_t text', () => {
    const project = new Project();
    try {
      expect(() => project.setMaxUndoDepth(0)).not.toThrow();
      expectRefusal(
        () => project.setMaxUndoDepth(1.5),
        RangeError,
        exactly(`Project.setMaxUndoDepth: depth ${SIZE_T_TEXT}`),
      );
    } finally {
      project.destroy();
    }
  });

  it('drains nothing for maxRecords 0 and refuses a negative one with the size_t text', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expect(engine.drainExternalMidi(0)).toEqual([]);
      expectRefusal(
        () => engine.drainExternalMidi(-1),
        RangeError,
        exactly(`RealtimeEngine.drainExternalMidi: maxRecords ${SIZE_T_TEXT}`),
      );
    } finally {
      engine.destroy();
    }
  });

  it('refuses a fractional UMP word', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      expectRefusal(
        () => engine.pushMidiUmp(1, [1.5]),
        RangeError,
        exactly(
          'RealtimeEngine.pushMidiUmp: words[0] must be an integer in [-2147483648, 4294967295]',
        ),
      );
    } finally {
      engine.destroy();
    }
  });

  it('refuses a mixer input plane that is not a Float32Array', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
      sampleRate: 48000,
      maxBlockSize: 128,
    });
    try {
      const plane = [0, 0, 0, 0] as unknown as Float32Array;
      expectRefusal(
        () => mixer.processStereo([plane], [new Float32Array(4)]),
        TypeError,
        exactly('Mixer.processStereo: leftInputs[0] must be a Float32Array'),
      );
    } finally {
      mixer.destroy();
    }
  });
});
