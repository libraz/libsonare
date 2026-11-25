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

it('control: the raw mixing assistant refuses a tiny nonzero tempo before float conversion', () => {
  const error = refusal(() =>
    mod.mixingAssistantSuggest([new Float32Array(128)], null, ['bass'], null, null, 48000, {
      tempoBpm: Number.MIN_VALUE,
    }),
  );
  expect(error).toBeInstanceOf(RangeError);
  expect(error.message).toContain('tempoBpm must be 0 or a tempo between 20 and 400 BPM');
});

it('raw mixing assistant checks request shapes and channel lengths before rate domains', () => {
  const call = (left: unknown, right: unknown, rates: unknown, params: unknown): Error =>
    refusal(() =>
      mod.mixingAssistantSuggest(
        left as Float32Array[],
        right as (Float32Array | null)[] | null,
        ['bass'],
        null,
        rates as (number | null)[] | null,
        7999,
        params as Record<string, number | boolean>,
      ),
    );

  const badParams = call([new Float32Array(4)], null, [7999], {
    suggestionStrength: 'strong',
    tempoBpm: 0,
  } as never);
  expect(badParams).toBeInstanceOf(TypeError);
  expect(badParams.message).toContain("mastering override 'suggestionStrength'");

  const badPlane = call(['not a plane'], null, [7999], {});
  expect(badPlane).toBeInstanceOf(TypeError);
  expect(badPlane.message).toContain('leftChannels[0]');

  const unequalPlanes = call([new Float32Array(4)], [new Float32Array(3)], [7999], { tempoBpm: 0 });
  expect(unequalPlanes).toBeInstanceOf(RangeError);
  expect(unequalPlanes.message).toBe(
    'mixingAssistantSuggest: tracks[0].left and tracks[0].right must have the same length',
  );
});

it('the raw mixer accepts only the exact negative pan-mode sentinel', () => {
  const scene = JSON.stringify({
    version: 2,
    strips: [{ id: 'a' }],
    buses: [{ id: 'master', role: 'master' }],
  });
  const mixer = mod.createMixerFromSceneJson(scene, 48000, undefined);
  try {
    expect(() => mixer.setPan(0, 0.25, -1)).not.toThrow();
    const error = refusal(() => mixer.setPan(0, 0.25, -2));
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toContain('panMode must be an integer in [0, 2]');
  } finally {
    mixer.delete();
  }
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

describe('GM and MIDI lookups refuse an index outside its domain with the domain text', () => {
  // biome-ignore lint/suspicious/noExplicitAny: the lookups are not on the module declaration.
  const lookups = (m: any): Array<[string, string, number, (value: unknown) => unknown]> => [
    ['midiGmInstrumentName', 'program', 127, (v) => m.midiGmInstrumentName(v)],
    ['midiGmFamilyName', 'family', 15, (v) => m.midiGmFamilyName(v)],
    ['midiGmFamilyFirstProgram', 'family', 15, (v) => m.midiGmFamilyFirstProgram(v)],
    ['midiGm2InstrumentName', 'bankLsb', 127, (v) => m.midiGm2InstrumentName(v, 0)],
    ['midiGm2InstrumentName', 'program', 127, (v) => m.midiGm2InstrumentName(0, v)],
    ['midiGmDrumName', 'note', 127, (v) => m.midiGmDrumName(v)],
    ['midiGm2DrumSetName', 'bankLsb', 127, (v) => m.midiGm2DrumSetName(v)],
    ['midiGm2DrumName', 'bankLsb', 127, (v) => m.midiGm2DrumName(v, 35)],
    ['midiGm2DrumName', 'note', 127, (v) => m.midiGm2DrumName(0, v)],
    ['midiCcName', 'controller', 127, (v) => m.midiCcName(v)],
    ['midiPerNoteControllerName', 'index', 255, (v) => m.midiPerNoteControllerName(v)],
  ];

  it('every lookup refuses -1 and one past its maximum, and answers the maximum', () => {
    for (const [name, argument, max, lookup] of lookups(mod)) {
      for (const bad of [-1, max + 1]) {
        expect(
          refusal(() => lookup(bad)),
          `${name} ${argument}=${bad}`,
        ).toEqual(new RangeError(`${name}: ${argument} must be an integer in [0, ${max}]`));
      }
      expect(() => lookup(max), name).not.toThrow();
    }
  });
});
