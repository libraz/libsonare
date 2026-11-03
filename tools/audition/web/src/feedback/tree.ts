/* The narrowing a listener is walked through, two answers at a time with "not
 * sure" always a third, in the words a listener would use rather than the words
 * a parameter is called by.
 *
 * The verdict (`grade`) is kept apart from the finest point reached (`tag`):
 * "recognisably the instrument and I would still change it" and "this is a
 * different instrument" are the same `onset/hard` underneath, and only one of
 * them is a defect. `broken` is a missing mechanism, not a mis-set constant.
 * The tag is what anything mechanical reads; the question text never is.
 * Adding a fork is one entry in `tree.json` and its words in each table.
 */

import { isKey, type TKey, t } from '../i18n';
import data from './tree.json';

/** An answer; its words are the `tree.<node>.<tag>` resource. */
export interface Choice {
  tag: string;
  /** The node it leads to; absent on a leaf. */
  to?: string;
  leaf?: boolean;
}

/** A question; its words are `tree.<node>.q`, and `tree.<node>.qSolo` where `solo` is set. */
export interface TreeNode {
  /** Whether it is worded apart for a page holding the model alone. */
  solo: boolean;
  a: Choice[];
  /** The tag a "not sure" here records. */
  unsure: string;
}

export interface Tree {
  start: string;
  nodes: Record<string, TreeNode>;
}

export const TREE: Tree = data;

function key(text: string): TKey {
  if (!isKey(text)) {
    throw new Error(`the narrowing names ${text}, which no table carries`);
  }
  return text;
}

/** The question a node asks, in the wording for a page with or without a reference. */
export const questionOf = (id: string, solo: boolean): string =>
  t(key(solo && TREE.nodes[id]?.solo ? `tree.${id}.qSolo` : `tree.${id}.q`));

/** What an answer says. */
export const choiceText = (id: string, tag: string): string => t(key(`tree.${id}.${tag}`));
