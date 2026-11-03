/* The listening run: the bank in working order, one voice after another.
 *
 * Everything the listening surface leaves to the listener is decided here, so
 * the ear is the only thing left to supply. The order is the server's
 * (`queue.json`: tier, the group inside it, then number); the take is the
 * musical one; the comparison is the page's own best-standing one; the library
 * side sounds first, looping, and `tab` brings in the other side. A sent note
 * moves the run on, and the server re-renders a page that trails the bank
 * before it is played, so no verdict is spent on a voice that no longer ships.
 */

import { useStore } from 'zustand';
import { createStore } from 'zustand/vanilla';
import { getJSON, postJSON } from '../api/client';
import { Queue, type QueueStep } from '../api/schemas';
import { onNoteSent } from '../feedback/actions';
import { t } from '../i18n';
import {
  loadSet,
  loadSets,
  setBlind,
  setMatchRms,
  setRestartOnSwitch,
  setVersion,
} from '../listen/actions';
import { pause, setLoop, setRegion } from '../player/engine';
import { roleOf, selectedComparison } from '../state/selectors';
import { type AppState, getState, setState } from '../state/store';

export const PASSED_KEY = 'audition:run-passed';
export const POLL_MS = 1500;
/** How many voices past the one being heard are rendered ahead of it. */
export const AHEAD = 2;

export type Wait = 'rendering' | 'failed' | 'done' | null;

export interface RunState {
  on: boolean;
  /** The queue entry being heard, or waited for. */
  step: QueueStep | null;
  /** The queue's length when the run began, for the count. */
  total: number;
  wait: Wait;
  /** Why the run is not playing, where the server said. */
  error: string;
  /** When the wait for the page on screen began, for the elapsed line. */
  waitSince: number;
}

const initial = (): RunState => ({
  on: false,
  step: null,
  total: 0,
  wait: null,
  error: '',
  waitSince: 0,
});

export const runStore = createStore<RunState>()(initial);
export const useRun = <T>(select: (r: RunState) => T): T => useStore(runStore, select);
const run = runStore.getState;
const setRun = runStore.setState;

let timer: ReturnType<typeof setTimeout> | null = null;
/** Bumped on every open, so a poll or a load answering an older step commits nothing. */
let generation = 0;

export function resetRun(): void {
  clearTimer();
  generation += 1;
  runStore.setState(initial(), true);
}

function clearTimer(): void {
  if (timer) {
    clearTimeout(timer);
  }
  timer = null;
}

const passed = (): Set<string> => {
  try {
    return new Set(JSON.parse(sessionStorage.getItem(PASSED_KEY) || '[]') as string[]);
  } catch {
    return new Set();
  }
};

function pass(setId: string): void {
  const done = passed();
  done.add(setId);
  sessionStorage.setItem(PASSED_KEY, JSON.stringify([...done]));
}

/** The voices still owed a verdict this session, in order. */
export function pending(queue: QueueStep[] = getState().queue): QueueStep[] {
  const done = passed();
  return queue.filter((s) => !done.has(s.set));
}

/** The take a verdict is asked on: real music, since "is this the instrument" is answered by hearing it played. */
export const preferredTake = (step: QueueStep): string => (step.kit ? 'groove' : 'music');

/* ------------------------------------------------------------ the server */

async function fetchQueue(): Promise<void> {
  setState({ queue: await getJSON('queue.json', Queue) });
}

async function requestRenders(sets: string[]): Promise<void> {
  if (!sets.length) {
    return;
  }
  setState({ queue: await postJSON('render', { sets }, Queue) });
}

/** The voices just behind this one start rendering while this one is heard. */
function renderAhead(): void {
  const here = run().step?.set;
  const ahead = pending()
    .filter((s) => s.set !== here)
    .slice(0, AHEAD);
  void requestRenders(ahead.filter((s) => s.page === 'render').map((s) => s.set)).catch(() => {});
}

/* ---------------------------------------------------------------- moving */

/** The library's render the chosen comparison judges. */
export function modelKey(s: AppState): string {
  const keys = s.take?.keys ?? [];
  const c = selectedComparison(s);
  return (
    c?.model_sources.find((k) => keys.includes(k)) ??
    keys.find((k) => roleOf(s, k) === 'model') ??
    keys[0] ??
    ''
  );
}

/** The library side first, looping, so the reference is one key away. Every
 * voice is heard under the same conditions, whatever the listening surface had set. */
function sound(autoplay: boolean): void {
  const s = getState();
  if (!s.take) {
    return;
  }
  // Before the version is chosen: a blind index is a draw position, not a take index.
  setBlind(false);
  setMatchRms(false);
  setRestartOnSwitch(true);
  setRegion(null);
  setState({ attach: true });
  setLoop(true);
  setVersion(s.take.keys.indexOf(modelKey(getState())), { play: autoplay });
}

/** Open the step the run is on: wait for its page, then sound it. */
async function openStep(autoplay: boolean): Promise<void> {
  clearTimer();
  const mine = ++generation;
  const step = pending()[0] ?? null;
  if (step?.set !== run().step?.set) {
    setRun({ waitSince: Date.now() });
  }
  setRun({ step });
  if (!step) {
    pause();
    setRun({ wait: 'done', error: '' });
    return;
  }
  let now = step;
  if (step.page === 'render') {
    await requestRenders([step.set]).catch(() => {});
    if (mine !== generation) {
      return;
    }
    now = pending().find((s) => s.set === step.set) ?? step;
    setRun({ step: now });
  }
  if (now.page !== 'ready') {
    const failed = now.page === 'failed';
    setRun({ wait: failed ? 'failed' : 'rendering', error: failed ? now.error : '' });
    if (!failed) {
      timer = setTimeout(() => void refresh(autoplay), POLL_MS);
    }
    return;
  }
  setRun({ wait: null, error: '' });
  // Re-read even when this set is already open: it may have just been re-rendered.
  await loadSets();
  await loadSet(now.set, { set: now.set, take: preferredTake(now), ver: '', cmp: '', t: null });
  if (!run().on || mine !== generation) {
    return;
  }
  sound(autoplay);
  renderAhead();
}

async function refresh(autoplay: boolean): Promise<void> {
  if (!run().on) {
    return;
  }
  try {
    await fetchQueue();
  } catch {
    timer = setTimeout(() => void refresh(autoplay), POLL_MS);
    return;
  }
  await openStep(autoplay);
}

/** Pass the voice on screen and open the next. */
export async function advance(): Promise<void> {
  const step = run().step;
  if (step) {
    pass(step.set);
  }
  pause();
  await refresh(true);
}

let unsubscribe: (() => void) | null = null;

export async function startRun({ autoplay = true }: { autoplay?: boolean } = {}): Promise<void> {
  if (run().on) {
    return;
  }
  setRun({ on: true });
  unsubscribe = onNoteSent(({ set }) => {
    const step = run().step;
    if (run().on && step && set === step.set) {
      void advance();
    }
  });
  try {
    await fetchQueue();
  } catch (err) {
    setState({ queue: [] });
    setRun({
      wait: 'failed',
      error: t('run.noQueue', { msg: (err as Error).message }),
      step: null,
    });
    return;
  }
  setRun({ total: pending().length });
  await openStep(autoplay);
}

export function stopRun(): void {
  unsubscribe?.();
  unsubscribe = null;
  clearTimer();
  generation += 1;
  setRun({ on: false, wait: null, error: '' });
}

export const runOn = (): boolean => run().on;

export interface RunKeyActions {
  answerAt: (index: number) => boolean;
  send: () => Promise<boolean>;
}

/** The run's keys, ahead of the listening surface's: the digits answer, enter sends, `n` skips. True when handled. */
export function runKey(ev: KeyboardEvent, { answerAt, send }: RunKeyActions): boolean {
  if (!run().on || !run().step) {
    return false;
  }
  const k = ev.key;
  if (k >= '1' && k <= '3' && k.length === 1) {
    ev.preventDefault();
    answerAt(Number(k) - 1);
    return true;
  }
  if (k === 'Enter') {
    ev.preventDefault();
    void send();
    return true;
  }
  if (k === 'n' || k === 'N') {
    ev.preventDefault();
    void advance();
    return true;
  }
  return false;
}
