import { beforeEach, describe, expect, it, vi } from 'vitest';
import { answerAt, back, composerDone, gradeOf, resetComposer } from '../../src/feedback/actions';
import { fb } from '../../src/feedback/store';
import { TREE } from '../../src/feedback/tree';
import { freshState } from './helpers';

vi.mock('../../src/listen/actions', () => ({ loadFeedbackIndex: vi.fn(), reloadSet: vi.fn() }));

beforeEach(() => {
  freshState();
  resetComposer();
});

/** Every answer position that ends the narrowing, with the tags it records and the verdict it earns. */
const LEAVES: [string, number[], string[], string][] = [
  ['fine as it is', [1], ['ok'], 'ok'],
  ['hard onset', [0, 0, 0, 0], ['off', 'acceptable', 'onset', 'onset/hard'], 'acceptable'],
  ['soft onset', [0, 0, 0, 1], ['off', 'acceptable', 'onset', 'onset/soft'], 'acceptable'],
  ['long tail', [0, 0, 1, 0, 0], ['off', 'acceptable', 'body', 'tail', 'tail/long'], 'acceptable'],
  [
    'short tail',
    [0, 0, 1, 0, 1],
    ['off', 'acceptable', 'body', 'tail', 'tail/short'],
    'acceptable',
  ],
  [
    'bright tone',
    [0, 0, 1, 1, 0],
    ['off', 'acceptable', 'body', 'tone', 'tone/bright'],
    'acceptable',
  ],
  ['dark tone', [0, 0, 1, 1, 1], ['off', 'acceptable', 'body', 'tone', 'tone/dark'], 'acceptable'],
  ['barely sounds', [0, 1, 0], ['off', 'wrong-instrument', 'broken'], 'broken'],
  [
    'synthetic',
    [0, 1, 1, 0],
    ['off', 'wrong-instrument', 'wrong-instrument', 'wrong-instrument/synthetic'],
    'wrong-instrument',
  ],
  [
    'other',
    [0, 1, 1, 1],
    ['off', 'wrong-instrument', 'wrong-instrument', 'wrong-instrument/other'],
    'wrong-instrument',
  ],
];

describe('every path to a leaf', () => {
  it.each(LEAVES)('%s', (_name, picks, tags, grade) => {
    expect(fb().node).toBe('off');
    for (const [i, pick] of picks.entries()) {
      expect(composerDone()).toBe(false);
      expect(answerAt(pick)).toBe(true);
      expect(fb().trail).toHaveLength(i + 1);
    }
    expect(fb().node).toBeNull();
    expect(composerDone()).toBe(true);
    expect(fb().trail.map((s) => s.tag)).toEqual(tags);
    expect(fb().trail.every((s) => !s.unsure)).toBe(true);
    expect(gradeOf(fb().trail)).toBe(grade);
  });

  it('covers every leaf the tree has', () => {
    const leaves = Object.values(TREE.nodes).flatMap((n) => n.a.filter((c) => c.leaf));
    expect(leaves).toHaveLength(LEAVES.length);
  });
});

describe('"not sure"', () => {
  it('ends the narrowing at the root with the root unsure tag', () => {
    expect(answerAt(2)).toBe(true);
    expect(fb().trail).toEqual([{ node: 'off', tag: 'off/unsure', unsure: true }]);
    expect(composerDone()).toBe(true);
    expect(gradeOf(fb().trail)).toBe('off/unsure');
  });

  it('is last at every node and records that node unsure tag', () => {
    const walk = (node: string, via: number[]) => {
      const n = TREE.nodes[node];
      if (!n) {
        throw new Error(node);
      }
      resetComposer();
      for (const p of via) {
        answerAt(p);
      }
      expect(fb().node).toBe(node);
      expect(answerAt(n.a.length)).toBe(true);
      expect(fb().trail.at(-1)).toEqual({ node, tag: n.unsure, unsure: true });
      expect(fb().node).toBeNull();
      n.a.forEach((c, i) => {
        if (c.to) {
          walk(c.to, [...via, i]);
        }
      });
    };
    walk(TREE.start, []);
  });

  it('under the verdict question grades as not sure; elsewhere keeps the verdict given', () => {
    answerAt(0);
    answerAt(2);
    expect(gradeOf(fb().trail)).toBe('off/unsure');
    resetComposer();
    for (const p of [0, 0, 1, 2]) {
      answerAt(p);
    }
    expect(gradeOf(fb().trail)).toBe('acceptable');
    expect(fb().trail.at(-1)?.tag).toBe('body/unsure');
  });
});

describe('answerAt', () => {
  it('refuses a position past "not sure", a negative one and a fraction', () => {
    expect(answerAt(3)).toBe(false);
    expect(answerAt(-1)).toBe(false);
    expect(answerAt(0.5)).toBe(false);
    expect(fb().trail).toEqual([]);
    expect(fb().node).toBe('off');
  });

  it('refuses once the narrowing is final', () => {
    answerAt(1);
    expect(answerAt(0)).toBe(false);
    expect(fb().trail).toHaveLength(1);
  });
});

describe('back and start over', () => {
  it('back steps to the question that was answered last', () => {
    answerAt(0);
    answerAt(0);
    expect(fb().node).toBe('when');
    back();
    expect(fb().node).toBe('grade');
    expect(fb().trail.map((s) => s.tag)).toEqual(['off']);
    back();
    expect(fb().node).toBe('off');
    expect(fb().trail).toEqual([]);
    back();
    expect(fb().node).toBe('off');
  });

  it('back from a final answer reopens that question', () => {
    answerAt(1);
    expect(fb().node).toBeNull();
    back();
    expect(fb().node).toBe('off');
    expect(fb().trail).toEqual([]);
  });

  it('start over clears the trail', () => {
    answerAt(0);
    answerAt(0);
    resetComposer();
    expect(fb().trail).toEqual([]);
    expect(fb().node).toBe('off');
  });
});
