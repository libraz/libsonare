/* The listening surface's moves: open a set, pick a take, pick a version, swap
 * sides, pick a comparison, run blind.
 *
 * A selection that finishes after a later one began belongs to a choice nobody
 * is looking at, so it commits nothing: otherwise the slower of two quick
 * switches wins and plays under the other one's name. `selectEpoch` counts
 * every selection; `setEpoch` in the store is the per-set half the feedback
 * replies check.
 */

import { getJSON } from '../api/client';
import { FeedbackIndex, Manifest, SetIndex } from '../api/schemas';
import {
  clearStatus,
  loadFeedback,
  offerText,
  recordBlind,
  switchDraft,
  takeChanged,
} from '../feedback/actions';
import { t } from '../i18n';
import {
  applyGains,
  commitBuffers,
  decodeTake,
  pause,
  playhead,
  setOffset,
  span,
  startAt,
  stopSources,
} from '../player/engine';
import {
  activeKey,
  comparisonKeys,
  comparisons,
  currentItem,
  earlierPicksKeys,
  notesKey,
  picksKey,
  roleOf,
  selectedComparison,
  setGeneration,
  sourceLabel,
} from '../state/selectors';
import { type AppState, getState, type Pick, SET_KEY, setState } from '../state/store';
import { blindBlocked, blindDraw, displayOrder, slotsInRole } from './display';
import { EMPTY_ROUTE, type Route, writeRoute } from './route';

let selectEpoch = 0;
/* True from a set load starting until its first take is being chosen: the take
 * list on screen is still the previous set's, and a click on it would play one
 * set's take under the other's name. */
let setLoading = false;
/** Notes taken in this browser before the page could send, offered back once per take. */
let oldNotes: Record<string, string> = {};

const readJSON = <T>(key: string, fallback: T): T => {
  try {
    const raw = localStorage.getItem(key);
    return raw === null ? fallback : (JSON.parse(raw) as T);
  } catch {
    return fallback;
  }
};

/* ------------------------------------------------------------- the index */

export async function loadSets(): Promise<void> {
  setState({ sets: await getJSON('sets.json', SetIndex) });
}

/** The palette and the bank read their counts from here, so a note or an undo reloads it. */
export async function loadFeedbackIndex(): Promise<void> {
  try {
    setState({ feedbackIndex: await getJSON('feedback-index.json', FeedbackIndex) });
  } catch {
    // A server without the endpoint leaves the counts off.
  }
}

/* --------------------------------------------------------------- display */

/** Recompute what the switch shows; a version the comparison does not show cannot stay selected. */
function redisplay(): void {
  const s = getState();
  if (!s.take) {
    setState({ display: [] });
    return;
  }
  const display = displayOrder(s);
  setState({ display });
  const now = getState();
  if (!now.blind && display.length && !display.includes(now.versionIndex)) {
    setState({ versionIndex: display[0] as number });
    setState({ wantKey: activeKey(getState()) });
  }
}

/** Redraw the switch without touching the audio: blind on or off, a reveal and a reshuffle change only what it says. */
export function rebuildVersions(): void {
  if (!getState().take) {
    return;
  }
  redisplay();
  if (getState().playing) {
    applyGains(false);
  }
  writeRoute(getState());
}

/* ------------------------------------------------------------------- set */

export async function loadSet(id: string, want: Route = EMPTY_ROUTE): Promise<void> {
  const before = getState();
  const entry = before.sets.find((x) => x.id === id) ?? before.sets[0];
  if (!entry) {
    return;
  }
  const epoch = ++selectEpoch;
  setLoading = true;
  stopSources();
  // Nothing of the previous set may be acted on while this one loads.
  setState({
    setEpoch: before.setEpoch + 1,
    canSend: false,
    playing: false,
    take: null,
    display: [],
  });

  // The set becomes current, and is remembered, only once its manifest is read.
  let m: Manifest;
  try {
    m = await getJSON(`s/${entry.id}/manifest.json`, Manifest);
  } catch (err) {
    if (epoch !== selectEpoch) {
      return;
    }
    setLoading = false;
    setState({ title: t('set.loadFailed', { msg: (err as Error).message }) });
    const s = getState();
    if (s.items.length) {
      await selectTake(s.itemIndex);
    } else {
      setState({ canSend: Boolean(s.setId) });
    }
    return;
  }
  if (epoch !== selectEpoch) {
    return;
  }
  localStorage.setItem(SET_KEY, entry.id);
  switchDraft(entry.id);
  const items = m.items;
  // A set whose takes hold one version each has nothing to switch between.
  const compare = items.some((it) => Object.keys(it.tracks).length > 1);
  const named = Boolean(want.cmp) && m.comparisons.some((c) => c.id === want.cmp);
  const found = want.take ? items.findIndex((it) => it.id === want.take) : -1;
  setState({
    setId: entry.id,
    base: `s/${entry.id}/`,
    manifest: m,
    items,
    compare,
    blind: compare && getState().blind,
    blindRevealed: false,
    title: m.title || '',
    failed: false,
    comparisonId: named ? want.cmp : null,
    wantByComparison: {},
    wantKey: want.ver || getState().wantKey,
    itemIndex: Math.max(found, 0),
    versionIndex: 0,
    lastByRole: {},
    // Per set: the previous voice's last oracle says nothing about this one.
    lastOracle: null,
    oracleOverride: null,
    region: null,
  });
  // After the comparison a link names, so a set that cannot be blind-listened to ends a run left from before.
  applyBlindGate();
  setState({ picks: readJSON(picksKey(getState()), {}) });
  oldNotes = readJSON(notesKey(getState()), {});
  await loadFeedback();
  if (epoch !== selectEpoch) {
    return;
  }
  setLoading = false;
  if (items.length) {
    await selectTake(getState().itemIndex);
  } else {
    setState({ canSend: true });
    writeRoute(getState());
  }
}

/** A note refused because the set was re-rendered asks for this: the same set, take, version and comparison, re-read. */
export async function reloadSet(): Promise<void> {
  const s = getState();
  if (!s.setId) {
    return;
  }
  await loadSet(s.setId, {
    set: s.setId,
    take: currentItem(s)?.id ?? '',
    ver: s.wantKey,
    cmp: s.comparisonId ?? '',
    t: null,
  });
}

function takeOldNote(id: string): string {
  const text = oldNotes[id];
  if (!text) {
    return '';
  }
  delete oldNotes[id];
  localStorage.setItem(notesKey(getState()), JSON.stringify(oldNotes));
  return text;
}

/* ----------------------------------------------------------------- takes */

export async function selectTake(i: number): Promise<void> {
  const s = getState();
  if (setLoading || i < 0 || i >= s.items.length) {
    return;
  }
  const wasPlaying = s.playing;
  const epoch = ++selectEpoch;
  pause();
  // The previous take's buffer must not stand for this selection while it loads; a passage means nothing on the next take.
  setState({
    canSend: false,
    take: null,
    display: [],
    itemIndex: i,
    region: null,
    takeStatus: { state: 'loading' },
  });
  setOffset(0);
  const item = s.items[i];
  if (!item) {
    return;
  }
  let decoded: Awaited<ReturnType<typeof decodeTake>>;
  try {
    decoded = await decodeTake(item, getState().base);
  } catch (err) {
    if (epoch !== selectEpoch) {
      return;
    }
    setState({ takeStatus: { state: 'failed', msg: (err as Error).message } });
    return;
  }
  if (epoch !== selectEpoch) {
    return;
  }
  commitBuffers(decoded.buffers);
  setState({ take: { ...decoded.take, epoch }, takeStatus: null, canSend: true });
  takeChanged();
  setState({ blindOrder: blindDraw(getState()) });
  // The version carries across takes by name, so stepping down the list keeps auditioning the same candidate.
  const now = getState();
  const keys = now.take?.keys ?? [];
  const want = keys.indexOf(now.wantKey);
  let versionIndex = Math.min(now.versionIndex, keys.length - 1);
  if (now.blind) {
    versionIndex = Math.min(now.versionIndex, now.blindOrder.length - 1);
  } else if (want >= 0) {
    versionIndex = want;
  }
  setState({ versionIndex: Math.max(0, versionIndex) });
  redisplay();
  if (!getState().blind) {
    setState({ wantKey: activeKey(getState()) });
    selectVersionByKey(getState().wantKey);
  }
  const carried = takeOldNote(item.id);
  if (carried) {
    offerText(carried);
  }
  writeRoute(getState());
  if (wasPlaying) {
    startAt(0);
  }
}

/* ----------------------------------------------------------------- route */

export async function applyRoute(r: Route): Promise<void> {
  const s = getState();
  if (r.set && r.set !== s.setId && s.sets.some((x) => x.id === r.set)) {
    await loadSet(r.set, r);
    return;
  }
  // A set this server does not hold: the address goes back to the page shown.
  if (r.set && r.set !== s.setId) {
    writeRoute(s);
    return;
  }
  // Comparison, then take, then version: each narrows what the next may name.
  if (r.cmp) {
    selectComparison(r.cmp);
  }
  if (r.ver) {
    setState({ wantKey: r.ver });
  }
  const i = r.take ? getState().items.findIndex((it) => it.id === r.take) : -1;
  if (i >= 0 && i !== getState().itemIndex) {
    await selectTake(i);
  }
  if (r.ver) {
    selectVersionByKey(r.ver);
  }
}

/* -------------------------------------------------------------- versions */

/* Choose a version — and, unless restoring an address, hear it. Restarting
 * gives back the attack; crossfading in place keeps the comparison
 * sample-aligned, the only way to hear a difference in a sustain or decay. */
export function setVersion(slot: number, { play = false }: { play?: boolean } = {}): void {
  const s = getState();
  if (!s.take) {
    return;
  }
  const n = s.blind ? s.blindOrder.length : s.take.keys.length;
  if (!Number.isInteger(slot) || slot < 0 || slot >= n) {
    return;
  }
  setState({ versionIndex: slot });
  if (!getState().blind) {
    const key = activeKey(getState());
    const role = roleOf(getState(), key);
    const patch: Partial<AppState> = { wantKey: key };
    if (role) {
      patch.lastByRole = { ...getState().lastByRole, [role]: slot };
    }
    // Only reference and comparison versions move the judged oracle.
    if (role === 'reference' || role === 'comparison') {
      patch.lastOracle = { role, key };
    }
    setState(patch);
  }
  const now = getState();
  if (now.restartOnSwitch) {
    const from = span()[0];
    if (now.playing || play) {
      startAt(from);
    } else {
      setOffset(from);
    }
  } else if (now.playing) {
    applyGains(false);
  } else if (play) {
    startAt(playhead());
  }
  writeRoute(getState());
}

export function selectVersionByKey(key: string): void {
  const s = getState();
  if (!s.take || s.blind) {
    return;
  }
  const slot = s.take.keys.indexOf(key);
  if (slot >= 0) {
    setVersion(slot);
  }
}

/** Step through the versions in the order they are shown, not manifest order. */
export function stepVersion(delta: number): void {
  const { display, versionIndex } = getState();
  const n = display.length;
  if (!n) {
    return;
  }
  const at = Math.max(0, display.indexOf(versionIndex));
  setVersion(display[(at + delta + n) % n] as number, { play: true });
}

/** The digit keys count the switch as shown. */
export function pickShown(position: number): void {
  const slot = getState().display[position];
  if (slot !== undefined) {
    setVersion(slot, { play: true });
  }
}

/* One key for the comparison the page exists for. Whichever version of a role
 * was last chosen is the one it comes back to, so flipping between a candidate
 * and the reference does not drop you on the baseline each time. */
export function swapRole(): void {
  const s = getState();
  if (!s.take || s.blind) {
    return;
  }
  const here = roleOf(s, activeKey(s));
  const other = here === 'reference' ? 'model' : 'reference';
  const slots = slotsInRole(s, other);
  if (!slots.length) {
    return;
  }
  const want = s.lastByRole[other];
  setVersion(want !== undefined && slots.includes(want) ? want : (slots[0] as number), {
    play: true,
  });
}

/* Move to another comparison. The version chosen in the one being left is kept
 * for the way back; picks, the blind gate and the draw are the new one's own,
 * and the judged oracle starts empty. */
export function selectComparison(id: string): void {
  const s = getState();
  const from = selectedComparison(s);
  if (!s.take || !from || from.id === id || !comparisons(s).some((c) => c.id === id)) {
    return;
  }
  const wantByComparison = s.blind
    ? s.wantByComparison
    : { ...s.wantByComparison, [from.id]: activeKey(s) };
  setState({ comparisonId: id, wantByComparison, lastOracle: null, oracleOverride: null });
  setState({ picks: readJSON(picksKey(getState()), {}) });
  clearStatus();
  setState({ blindRevealed: false });
  applyBlindGate();
  setState({ blindOrder: blindDraw(getState()) });
  const now = getState();
  const c = selectedComparison(now);
  const keys = c ? comparisonKeys(c) : new Set<string>();
  const take = now.take;
  const want = now.wantByComparison[id];
  let slot = want && keys.has(want) && take ? take.keys.indexOf(want) : -1;
  if (slot < 0 && take && c) {
    slot = take.keys.findIndex((k) => c.model_sources.includes(k));
  }
  if (slot < 0 && take) {
    slot = take.keys.findIndex((k) => keys.has(k));
  }
  setState({ versionIndex: now.blind || slot < 0 ? 0 : slot });
  rebuildVersions();
  if (!getState().blind) {
    setVersion(getState().versionIndex);
  }
}

/* --------------------------------------------------------------- options */

export function setMatchRms(on: boolean): void {
  setState({ matchRms: on });
  if (getState().take) {
    applyGains(false);
  }
}

export function setRestartOnSwitch(on: boolean): void {
  setState({ restartOnSwitch: on });
}

/* ----------------------------------------------------------------- blind */

/** Brings blind mode in line with the set and comparison now open. */
export function applyBlindGate(): void {
  if (blindBlocked(getState()) || !getState().compare) {
    setState({ blind: false });
  }
}

/** Why blind mode is refused here, or '' where it is allowed. */
export function blindRefusal(s: AppState): string {
  if (!blindBlocked(s)) {
    return '';
  }
  const c = selectedComparison(s);
  if (c) {
    return t('blind.unmatched', { status: statusWord(c.status) });
  }
  return t('blind.refDi');
}

const STATUS_KEYS = {
  matched: 'compare.status.matched',
  context_only: 'compare.status.context_only',
  unverified: 'compare.status.unverified',
  unavailable: 'compare.status.unavailable',
} as const;

/** A comparison's standing in words; an unknown status reads as unavailable. */
export const statusWord = (status: string): string =>
  t(STATUS_KEYS[status as keyof typeof STATUS_KEYS] ?? STATUS_KEYS.unavailable);

/* Entering blind mode starts a run, and a run starts with its own result
 * withheld. `versionIndex` is a position in the draw when blind and an index
 * into the take otherwise, so the sounding version is read first and re-found after. */
export function setBlind(on: boolean): void {
  const s = getState();
  if (on && (blindBlocked(s) || !s.compare)) {
    return;
  }
  const sounding = s.take ? activeKey(s) : null;
  setState({ blind: on, blindRevealed: false });
  setState({ blindOrder: blindDraw(getState()) });
  const now = getState();
  if (now.take && sounding !== null) {
    const at = now.take.keys.indexOf(sounding);
    setState({ versionIndex: Math.max(0, now.blind ? now.blindOrder.indexOf(at) : at) });
  }
  rebuildVersions();
}

export function reshuffleBlind(): void {
  setState({ blindOrder: blindDraw(getState()) });
  rebuildVersions();
}

function answered(id: string): Omit<Pick, 'revealed'> {
  const s = getState();
  const c = selectedComparison(s);
  return {
    take: id,
    candidates: s.blindOrder.map((i) => s.take?.keys[i] ?? ''),
    comparison_id: c?.id ?? null,
    set_generation: setGeneration(s),
    answered_at: new Date().toISOString(),
  };
}

function writePicks(picks: Record<string, Pick>): void {
  setState({ picks, blindRevealed: false });
  localStorage.setItem(picksKey(getState()), JSON.stringify(picks));
  rebuildVersions();
}

/* CHOOSING IS NOT SWITCHING, and not choosing is a result: moving between
 * versions never votes. "Could not tell" is recorded rather than left absent,
 * because it and "not listened to" are different results. */
export function chooseBlind(): void {
  const s = getState();
  const item = currentItem(s);
  if (!s.blind || !s.take || !item) {
    return;
  }
  const real = s.blindOrder[s.versionIndex];
  writePicks({
    ...s.picks,
    [item.id]: {
      ...answered(item.id),
      slot: s.versionIndex,
      key: real !== undefined ? s.take.keys[real] : undefined,
      revealed: false,
    },
  });
}

export function abstainBlind(): void {
  const s = getState();
  const item = currentItem(s);
  if (!s.blind || !s.take || !item) {
    return;
  }
  writePicks({
    ...s.picks,
    [item.id]: { ...answered(item.id), unseparated: true, revealed: false },
  });
}

/** `R`: reveal this take's pick, or draw again where there is nothing to reveal. */
export function revealOrReshuffle(): void {
  const s = getState();
  const item = currentItem(s);
  const pick = item ? s.picks[item.id] : undefined;
  if (s.blind && item && pick && !pick.revealed) {
    const picks = { ...s.picks, [item.id]: { ...pick, revealed: true } };
    setState({ picks });
    localStorage.setItem(picksKey(getState()), JSON.stringify(picks));
    rebuildVersions();
    return;
  }
  reshuffleBlind();
}

export interface Tally {
  picks: Record<string, string>;
  unsure: string[];
  n: number;
  parts: string[];
}

/** What was chosen per take, what could not be separated, and how it adds up. */
export function blindTally(s: AppState): Tally {
  const picks: Record<string, string> = {};
  const unsure: string[] = [];
  for (const [id, p] of Object.entries(s.picks)) {
    if (p?.unseparated) {
      unsure.push(id);
    } else if (p?.key) {
      picks[id] = p.key;
    }
  }
  const tally: Record<string, number> = {};
  for (const key of Object.values(picks)) {
    tally[key] = (tally[key] ?? 0) + 1;
  }
  const parts = Object.entries(tally)
    .sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${sourceLabel(s, k)} ${v}`);
  if (unsure.length) {
    parts.push(`${t('blind.unsure')} ${unsure.length}`);
  }
  return { picks, unsure, n: Object.keys(picks).length + unsure.length, parts };
}

/** Takes answered under an earlier generation or key: shown, never tallied. */
export function earlierCount(s: AppState): number {
  let n = 0;
  for (const key of earlierPicksKeys(s)) {
    const held = readJSON<Record<string, Partial<Pick> | null>>(key, {});
    n += Object.values(held).filter((p) => p && (p.key || p.unseparated)).length;
  }
  return n;
}

/** A blind run is a result, so it goes into the same log as everything else. */
export async function recordBlindResult(): Promise<void> {
  const s = getState();
  const { picks, unsure, n, parts } = blindTally(s);
  if (!n) {
    return;
  }
  const answers = Object.entries(s.picks)
    .filter(([, p]) => p && (p.key || p.unseparated))
    .map(([id, p]) => ({
      take: p.take ?? id,
      candidates: p.candidates ?? null,
      picked: p.key || null,
      abstained: Boolean(p.unseparated),
      comparison_id: p.comparison_id ?? null,
      set_generation: p.set_generation ?? null,
      answered_at: p.answered_at ?? null,
    }));
  const sent = await recordBlind(
    t('blind.result', { n, tally: parts.join(', ') }),
    picks,
    unsure,
    answers,
  );
  if (sent) {
    setState({ blindRevealed: true });
  }
}
