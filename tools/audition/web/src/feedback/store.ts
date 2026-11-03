/* What the composer holds: the narrowing, the text, the log and the last reply.
 *
 * Kept apart from the app store because nothing else reads it; the actions in
 * `actions.ts` are its only writers.
 */

import { useStore } from 'zustand';
import { createStore } from 'zustand/vanilla';
import type { FeedbackEntry } from '../api/schemas';
import type { TKey, Vars } from '../i18n';

/** One answer given, in order; its label is resolved when read, in the language then in force. */
export interface Step {
  node: string;
  tag: string;
  unsure?: boolean;
}

/** The last action's reply, held as a key so a language change re-says it. */
export interface Status {
  key: TKey;
  vars?: Vars;
  bad: boolean;
}

export interface FeedbackState {
  /** The question on screen, or null once an answer is final. */
  node: string | null;
  trail: Step[];
  /** What has already been sent about this set. */
  entries: FeedbackEntry[];
  /** Where the server is writing. */
  path: string;
  busy: boolean;
  /** Set id -> ids of the entries this page posted, oldest first. */
  mine: Record<string, string[]>;
  /** Set id -> composer text not yet sent. */
  drafts: Record<string, string>;
  /** The set whose text is in the box now. */
  draftFor: string | null;
  /** The 409 reason on screen. */
  stale: string | null;
  status: Status | null;
  comment: string;
}

export const initialFeedback = (): FeedbackState => ({
  node: null,
  trail: [],
  entries: [],
  path: '',
  busy: false,
  mine: {},
  drafts: {},
  draftFor: null,
  stale: null,
  status: null,
  comment: '',
});

export const fbStore = createStore<FeedbackState>()(initialFeedback);

export const fb = fbStore.getState;
export const setFb = fbStore.setState;

export function useFb<T>(select: (s: FeedbackState) => T): T {
  return useStore(fbStore, select);
}

/** Put the composer back to its first state; for tests. */
export function resetFeedback(): void {
  fbStore.setState(initialFeedback(), true);
}
