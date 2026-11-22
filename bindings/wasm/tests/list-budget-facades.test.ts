/**
 * A caller list past the native list budget is refused by the facades that carry no engine
 * setter row, with the native message and before any entry of the list is read. The list is a
 * proxy over an empty array whose `length` is past the budget and whose entries throw on read.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import * as api from '../dist/index.js';
import { FACADE_ROWS } from './_list_budget_rows';

const OVER_BUDGET = 64 * 1024 * 1024 + 1;

function hugeList(): { list: never; reads: () => number } {
  let reads = 0;
  const isIndex = (key: string | symbol) => typeof key === 'string' && /^\d+$/.test(key);
  const list = new Proxy([] as unknown[], {
    get(target, key, receiver) {
      if (key === 'length') {
        return OVER_BUDGET;
      }
      if (isIndex(key)) {
        reads += 1;
        throw new Error('entry read');
      }
      return Reflect.get(target, key, receiver);
    },
    has(target, key) {
      if (isIndex(key)) {
        reads += 1;
        throw new Error('entry read');
      }
      return Reflect.has(target, key);
    },
  });
  return { list: list as never, reads: () => reads };
}

describe('facades refuse an oversized caller list before walking it', () => {
  beforeAll(async () => {
    await api.init();
  });

  it.each(FACADE_ROWS.map((row) => [row.callable, row.message, row] as const))(
    '%s: %s',
    async (_callable, message, row) => {
      const { list, reads } = hugeList();
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
