/**
 * Scalar arguments of the embind entries that take a bare JS number read
 * through the checked readers, so a non-finite value is refused at the native
 * boundary with the argument's name. Driven on the module itself, below the
 * facades' own validation.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init } from '../src/index';
import { getSonareModule } from '../src/module_state';
import type { SonareModule } from '../src/sonare.js';

let mod: SonareModule;

/** The raw embind Project methods this file drives; the module declaration does not export the class. */
interface RawProject {
  addMidiClip(startPpq: number, lengthPpq: number): { trackId: number };
  addAutomationLane(trackId: number, lane: unknown): number;
  editAutomationLane(trackId: number, laneId: number, lane: unknown): unknown;
  delete(): void;
}

beforeAll(async () => {
  await init();
  mod = getSonareModule();
});

function refusal(fn: () => unknown): Error {
  try {
    fn();
  } catch (error) {
    expect(error).toBeInstanceOf(Error);
    return error as Error;
  }
  throw new Error('expected a refusal');
}

describe('raw scalar arguments refuse non-finite values by name', () => {
  it('streaming retune prepare: sampleRate', () => {
    const retune = mod.createStreamingRetune({});
    try {
      expect(refusal(() => retune.prepare(Number.NaN, 512)).message).toContain('sampleRate');
      expect(() => retune.prepare(48000, 512)).not.toThrow();
    } finally {
      retune.delete();
    }
  });

  it('mixer scheduleFaderAutomation: samplePos', () => {
    const scene = JSON.stringify({
      version: 2,
      strips: [{ id: 'a' }],
      buses: [{ id: 'master', role: 'master' }],
    });
    const mixer = mod.createMixerFromSceneJson(scene, 48000, undefined);
    try {
      expect(refusal(() => mixer.scheduleFaderAutomation(0, Number.NaN, -6, 0)).message).toContain(
        'samplePos',
      );
      expect(() => mixer.scheduleFaderAutomation(0, 0, -6, 0)).not.toThrow();
    } finally {
      mixer.delete();
    }
  });

  it('project addMidiClip: startPpq; editAutomationLane: laneId', () => {
    const project = new (mod as unknown as { Project: new () => RawProject }).Project();
    try {
      expect(refusal(() => project.addMidiClip(Number.NaN, 4)).message).toContain('startPpq');
      const { trackId } = project.addMidiClip(0, 4);
      const lane = {
        targetParamId: 1,
        points: [{ ppq: 0, value: 0.5 }],
      };
      const laneId = project.addAutomationLane(trackId, lane);
      expect(
        refusal(() => project.editAutomationLane(trackId, Number.NaN, { points: [] })).message,
      ).toContain('laneId');
      expect(() => project.editAutomationLane(trackId, laneId, { points: [] })).not.toThrow();
    } finally {
      project.delete();
    }
  });

  it('realtime engine setTempo: bpm', () => {
    const engine = new mod.RealtimeEngine(48000, 128, 256, 256);
    try {
      expect(refusal(() => engine.setTempo(Number.NaN)).message).toContain('bpm');
      expect(() => engine.setTempo(120)).not.toThrow();
    } finally {
      engine.delete();
    }
  });
});

describe('the raw mixer factory reads maxBlockSize as the facade does', () => {
  const scene = JSON.stringify({
    version: 2,
    strips: [],
    buses: [{ id: 'master', role: 'master' }],
  });

  it.each([0, -1, 2 ** 20 + 1])('refuses a given block size of %d with the C domain', (block) => {
    const error = refusal(() => mod.createMixerFromSceneJson(scene, 48000, block));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(
      'createMixerFromSceneJson: maxBlockSize must be an integer in [1, 1048576]',
    );
  });

  it('takes the library default when omitted, read back from the mixer', () => {
    const omitted = mod.createMixerFromSceneJson(scene, 48000, undefined);
    const given = mod.createMixerFromSceneJson(scene, 48000, 256);
    try {
      expect(omitted.maxBlockSize()).toBeGreaterThan(0);
      expect(given.maxBlockSize()).toBe(256);
      expect(omitted.outputLeftView().length).toBe(omitted.maxBlockSize());
    } finally {
      omitted.delete();
      given.delete();
    }
  });
});
