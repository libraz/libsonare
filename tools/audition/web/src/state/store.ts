/* What every part of the page reads: one store.
 *
 * The listening surface, the scopes, the feedback composer, the bank and the
 * run all answer questions about the same take, so there is no useful boundary
 * to draw between them. Audio objects (the context, decoded buffers, running
 * sources) and the transport clock are not here: they live in `player/`, which
 * changes them sixty times a second without asking React to re-render.
 */

import { useStore } from 'zustand';
import { createStore } from 'zustand/vanilla';
import type { FeedbackIndex, Item, Manifest, QueueStep, Role, SetEntry } from '../api/schemas';

/** A decoded take as the page reasons about it; its buffers are the player's. */
export interface Take {
  id: string;
  keys: string[];
  rms: Record<string, number>;
  rates: Record<string, number>;
  duration: number;
  /** The selection that committed it, which the scope caches key on. */
  epoch: number;
}

/** A blind answer, fixed when it was given. */
export interface Pick {
  take: string;
  candidates: string[];
  comparison_id: string | null;
  set_generation: string | null;
  answered_at: string;
  slot?: number;
  key?: string;
  unseparated?: boolean;
  revealed: boolean;
}

export interface OracleChoice {
  role: Role;
  key: string;
}

export type View = 'run' | 'bank' | 'listen';

export interface AppState {
  sets: SetEntry[];
  feedbackIndex: FeedbackIndex;
  /** The set being listened to. */
  setId: string | null;
  /** URL prefix the set's audio is under. */
  base: string;
  /** False once a set turns out to hold one version per take. */
  compare: boolean;
  manifest: Manifest | null;
  items: Item[];
  itemIndex: number;
  /** An index into `take.keys`, or into `blindOrder` when blind. */
  versionIndex: number;
  /** Slots in the order they are shown, which is what 1..9 count. */
  display: number[];
  /** Version to re-select when the take or the set changes. */
  wantKey: string;
  /** Role -> the slot last chosen in it, for the swap key. */
  lastByRole: Partial<Record<Role, number>>;
  /** The last reference or comparison version selected. */
  lastOracle: OracleChoice | null;
  /** A listener's correction to `lastOracle`. */
  oracleOverride: OracleChoice | null;
  take: Take | null;
  /** The take being decoded, for the caption; null once committed or failed. */
  takeStatus: { state: 'loading' } | { state: 'failed'; msg: string } | null;
  playing: boolean;
  loop: boolean;
  /** The marked passage, in seconds. */
  region: [number, number] | null;
  blind: boolean;
  /** Slot -> real version index during a blind run. */
  blindOrder: number[];
  /** Take id -> the blind answer given on it, for the selected comparison. */
  picks: Record<string, Pick>;
  /** Whether the recorded run's tally may be shown. */
  blindRevealed: boolean;
  /** The manifest comparison being listened to, or null for its default. */
  comparisonId: string | null;
  /** Bumped per set load; an async reply about an older set commits nothing. */
  setEpoch: number;
  /** Comparison id -> the version last chosen in it. */
  wantByComparison: Record<string, string>;
  matchRms: boolean;
  restartOnSwitch: boolean;
  /** Whether a note carries what is sounding. */
  attach: boolean;
  /** Off between a selection starting and its load landing. */
  canSend: boolean;
  /** The page title: the manifest's, or a failure that stays put. */
  title: string;
  failed: boolean;
  view: View;
  helpOpen: boolean;
  optionsOpen: boolean;
  paletteOpen: boolean;
  /** The listening run's queue as the server last reported it. */
  queue: QueueStep[];
}

export const initialState = (): AppState => ({
  sets: [],
  feedbackIndex: {},
  setId: null,
  base: '',
  compare: true,
  manifest: null,
  items: [],
  itemIndex: 0,
  versionIndex: 0,
  display: [],
  wantKey: '',
  lastByRole: {},
  lastOracle: null,
  oracleOverride: null,
  take: null,
  takeStatus: null,
  playing: false,
  loop: false,
  region: null,
  blind: false,
  blindOrder: [],
  picks: {},
  blindRevealed: false,
  comparisonId: null,
  setEpoch: 0,
  wantByComparison: {},
  matchRms: false,
  restartOnSwitch: true,
  attach: true,
  canSend: false,
  title: '',
  failed: false,
  view: 'listen',
  helpOpen: false,
  optionsOpen: false,
  paletteOpen: false,
  queue: [],
});

export const store = createStore<AppState>()(initialState);

export const getState = store.getState;
export const setState = store.setState;

/** Read a slice of the store from a component. */
export function useApp<T>(select: (s: AppState) => T): T {
  return useStore(store, select);
}

/** Put the store back to its first state; for tests. */
export function resetStore(over: Partial<AppState> = {}): void {
  store.setState({ ...initialState(), ...over }, true);
}

export const SET_KEY = 'audition:set';
export const VIEW_KEY = 'audition:view';
