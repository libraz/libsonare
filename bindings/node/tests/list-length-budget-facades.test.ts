/**
 * A caller list longer than the input length budget is refused by the facade
 * before the facade walks it, with the text the addon uses for the same
 * refusal. The list is a proxy over an empty array whose `length` is past the
 * budget and whose entries count their reads, so a read count of zero shows the
 * refusal came first, whatever the machine's speed.
 */

import { describe, expect, it } from 'vitest';
import type { MixSceneStripInput } from '../src/index.js';
import * as api from '../src/index.js';
import { Mixer, Project, RealtimeEngine } from '../src/index.js';
import {
  ENGINE_ROWS,
  FACADE_ROWS,
  MIXER_ROWS,
  PROJECT_ROWS,
  type Row,
} from './_list_budget_rows.js';

const OVER_BUDGET = 64 * 1024 * 1024 + 1;

interface HugeList {
  list: never;
  reads: () => number;
}

function hugeList(): HugeList {
  let reads = 0;
  const isIndex = (key: string | symbol) => typeof key === 'string' && /^\d+$/.test(key);
  const list = new Proxy([] as unknown[], {
    get(target, key, receiver) {
      if (key === 'length') {
        return OVER_BUDGET;
      }
      if (isIndex(key)) {
        reads += 1;
      }
      return Reflect.get(target, key, receiver);
    },
    has(target, key) {
      if (isIndex(key)) {
        reads += 1;
      }
      return Reflect.has(target, key);
    },
  });
  return { list: list as never, reads: () => reads };
}

function expectRefused<H>(make: () => H, destroy: (handle: H) => void, row: Row<H>): void {
  const [, subject, call] = row;
  const { list, reads } = hugeList();
  const handle = make();
  try {
    expect(() => call(handle, list)).toThrow(RangeError);
    expect(() => call(handle, list)).toThrow(`${subject} exceeds the input length budget`);
    expect(reads()).toBe(0);
  } finally {
    destroy(handle);
  }
}

describe('facades refuse an oversized caller list before walking it', () => {
  const makeEngine = () => new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  it.each(ENGINE_ROWS)('RealtimeEngine.%s', (...row) => {
    expectRefused(makeEngine, (e) => e.destroy(), row);
  });

  it.each(PROJECT_ROWS)('Project.%s', (...row) => {
    expectRefused(
      () => new Project(),
      (p) => p.destroy(),
      row,
    );
  });

  it.each(MIXER_ROWS)('Mixer.%s', (...row) => {
    expectRefused(
      () => new Mixer({ sampleRate: 48000 }),
      (m) => m.destroy(),
      row,
    );
  });

  it('Mixer scene document', () => {
    const { list, reads } = hugeList();
    expect(
      () => new Mixer({ sampleRate: 48000, scene: { strips: list as MixSceneStripInput[] } }),
    ).toThrow('scene exceeds the input length budget');
    expect(reads()).toBe(0);
  });

  it('the request-key walk refuses a nested list before reading an entry', () => {
    const { list, reads } = hugeList();
    const engine = makeEngine();
    try {
      expect(() => engine.configureCapture({ punch: list } as never)).toThrow(
        'punch exceeds the input length budget',
      );
      expect(reads()).toBe(0);
    } finally {
      engine.destroy();
    }
  });
});

/** A real array whose length is past the budget and whose first entry throws and counts when read. */
function sparseHugeList(): HugeList {
  let reads = 0;
  const list: unknown[] = [];
  list.length = OVER_BUDGET;
  Object.defineProperty(list, 0, {
    enumerable: true,
    configurable: true,
    get() {
      reads += 1;
      throw new Error('entry read');
    },
  });
  return { list: list as never, reads: () => reads };
}

describe('callables outside the handle tables refuse an oversized sparse list before walking it', () => {
  it.each(FACADE_ROWS.map((row) => [row.callable, row.message, row] as const))(
    '%s: %s',
    async (_callable, message, row) => {
      const { list, reads } = sparseHugeList();
      let caught: unknown;
      try {
        await row.run(api, list);
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(RangeError);
      expect((caught as RangeError).message).toBe(message);
      expect(reads()).toBe(0);
    },
  );
});
