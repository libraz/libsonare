/**
 * List arguments are refused with the same class and text on Node and WASM.
 *
 * The cases come from tests/conformance/list_reader_refusals.json, which both suites read: a
 * list of audio planes is "<arg> must be an array of Float32Array" with "<arg>[i] must be a
 * Float32Array" for a wrong plane, any other list is "<arg> must be an array", and an object
 * that only carries a length is not a list.
 */

import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  ClipPageProvider,
  downmix,
  init,
  Mixer,
  mixingScenePreset,
  RealtimeEngine,
} from '../dist/index.js';

interface ListCase {
  call: string;
  arg: string;
}

const table = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/list_reader_refusals.json', import.meta.url),
    'utf8',
  ),
) as { plane_lists: ListCase[]; other_lists: ListCase[] };

const BLOCK = 128;
const plane = () => new Float32Array(BLOCK);

const WRONG: Record<string, () => unknown> = {
  object: () => ({}),
  undefined: () => undefined,
  array_like: () => ({ length: 1, 0: plane() }),
  string: () => 'ab',
};

beforeAll(async () => {
  await init();
});

/** Calls `call` with `value` in the position of the list argument the table names. */
function invoke(call: string, value: unknown): void {
  const list = value as never;
  switch (call) {
    case 'Mixer.processStereo': {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
        sampleRate: 48000,
        maxBlockSize: BLOCK,
      });
      try {
        mixer.processStereo(list, [plane()]);
      } finally {
        mixer.destroy();
      }
      return;
    }
    case 'ClipPageProvider.supply': {
      const provider = new ClipPageProvider({ numChannels: 1, numSamples: 8, pageFrames: 4 });
      try {
        provider.supply(0, list);
      } finally {
        provider.destroy();
      }
      return;
    }
    case 'downmix':
      downmix(list, 'mono');
      return;
  }
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: BLOCK });
  try {
    switch (call) {
      case 'RealtimeEngine.process':
        engine.process(list);
        return;
      case 'RealtimeEngine.setTempoSegments':
        engine.setTempoSegments(list);
        return;
      case 'RealtimeEngine.setTimeSignatureSegments':
        engine.setTimeSignatureSegments(list);
        return;
      case 'RealtimeEngine.setMarkers':
        engine.setMarkers(list);
        return;
      case 'RealtimeEngine.setAutomationLane':
        engine.setAutomationLane(1, list);
        return;
    }
  } finally {
    engine.destroy();
  }
  throw new Error(`no runner for ${call}`);
}

function refusal(run: () => void): Error {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
}

describe('list argument refusals (shared table)', () => {
  it('covers both families', () => {
    expect(table.plane_lists.length).toBeGreaterThan(0);
    expect(table.other_lists.length).toBeGreaterThan(0);
  });

  for (const { call, arg } of table.plane_lists) {
    for (const [kind, value] of Object.entries(WRONG)) {
      it(`${call} refuses a ${kind} ${arg} as a plane list`, () => {
        const error = refusal(() => invoke(call, value()));
        expect(error).toBeInstanceOf(TypeError);
        expect(error.message).toBe(`${call}: ${arg} must be an array of Float32Array`);
      });
    }
    it(`${call} names the wrong plane of ${arg} by index`, () => {
      const error = refusal(() => invoke(call, [{}]));
      expect(error).toBeInstanceOf(TypeError);
      expect(error.message).toBe(`${call}: ${arg}[0] must be a Float32Array`);
    });
  }

  for (const { call, arg } of table.other_lists) {
    for (const [kind, value] of Object.entries(WRONG)) {
      it(`${call} refuses a ${kind} ${arg} as a list`, () => {
        const error = refusal(() => invoke(call, value()));
        expect(error).toBeInstanceOf(TypeError);
        expect(error.message).toBe(`${call}: ${arg} must be an array`);
      });
    }
  }
});
