/* The feedback composer's actions as the rest of the page calls them.
 *
 * The listening surface tells the composer when the set, the take or the
 * comparison moved; the blind run and "keep this one" post through it; the
 * listening run answers and sends through it. Everything else about the
 * composer is its own business (`feedback/`).
 *
 * A note is written permanently to the listener's log, so every body is
 * checked against its strict schema before it leaves and every reply is parsed
 * where it arrives.
 */

import { getJSON, HttpError, postJSON } from '../api/client';
import { type BlindAnswer, FeedbackLog, NotePost, StaleRefusal, UndoPost } from '../api/schemas';
import { currentLang, type TKey, t } from '../i18n';
import { loadFeedbackIndex } from '../listen/actions';
import { comparedAgainst, conditions, evaluation, evidenceClaim } from '../listen/note';
import { playhead } from '../player/engine';
import { activeKey, oracleSources } from '../state/selectors';
import { getState, type OracleChoice, setState } from '../state/store';
import { fb, type Step, setFb } from './store';
import { choiceText, TREE } from './tree';

type Sent = { set: string | null; grade: string };
const sentListeners = new Set<(s: Sent) => void>();

/** Called after a note the composer sent was written; the run moves on from it. */
export function onNoteSent(fn: (s: Sent) => void): () => void {
  sentListeners.add(fn);
  return () => sentListeners.delete(fn);
}

export function announceSent(s: Sent): void {
  for (const fn of sentListeners) {
    fn(s);
  }
}

const keyOf = (setId: string | null): string => setId ?? '';
const myIds = (setId: string | null): string[] => fb().mine[keyOf(setId)] ?? [];

/** The words of one step, in the language in force now. */
export function labelOf(step: Step): string {
  if (step.unsure) {
    return t('fb.notSure');
  }
  return choiceText(step.node, step.tag);
}

/* ------------------------------------------------------------- narrowing */

/** Start the questions over. */
export function resetComposer(): void {
  setFb({ node: TREE.start, trail: [] });
}

function answer(nodeId: string, choiceIndex: number): void {
  const choice = TREE.nodes[nodeId]?.a[choiceIndex];
  if (!choice) {
    return;
  }
  setFb({
    trail: [...fb().trail, { node: nodeId, tag: choice.tag }],
    node: choice.leaf ? null : (choice.to ?? null),
  });
}

function unsure(nodeId: string): void {
  const node = TREE.nodes[nodeId];
  if (!node) {
    return;
  }
  setFb({ trail: [...fb().trail, { node: nodeId, tag: node.unsure, unsure: true }], node: null });
}

/** Take back the last answer. */
export function back(): void {
  const trail = fb().trail.slice(0, -1);
  const last = fb().trail[fb().trail.length - 1];
  setFb({ trail, node: last ? last.node : TREE.start });
}

/** Answer the question on screen by position, `not sure` being the last: the listening run puts the answers on the digit keys. */
export function answerAt(index: number): boolean {
  const nodeId = fb().node;
  const node = nodeId ? TREE.nodes[nodeId] : undefined;
  if (!nodeId || !node || !Number.isInteger(index) || index < 0) {
    return false;
  }
  if (index < node.a.length) {
    answer(nodeId, index);
  } else if (index === node.a.length) {
    unsure(nodeId);
  } else {
    return false;
  }
  return true;
}

/** True once the narrowing has reached an answer it cannot refine further. */
export const composerDone = (s = fb()): boolean => s.trail.length > 0 && s.node === null;

/* The verdict is read off the verdict node, `broken` overriding it, so a triage that wants the defects never parses the rest. */
export function gradeOf(trail: Step[]): string {
  if (trail.some((s) => s.tag === 'broken')) {
    return 'broken';
  }
  const verdict = trail.find((s) => s.node === 'grade');
  if (verdict) {
    return verdict.tag;
  }
  return trail[0]?.tag ?? '';
}

/* ---------------------------------------------------------------- status */

function say(key: TKey, bad = false, vars?: Record<string, string | number>): void {
  setFb({ status: { key, bad, ...(vars ? { vars } : {}) } });
}

/** Drop the last action's reply line: it belongs to the set and comparison it was about. */
export function clearStatus(): void {
  setFb({ status: null, stale: null });
}

/* ---------------------------------------------------------------- drafts */

/** The composer's text is per set: what was written about one voice is never sent as a note on another. */
export function switchDraft(setId: string): void {
  const f = fb();
  if (f.draftFor === setId) {
    return;
  }
  const drafts = f.draftFor === null ? f.drafts : { ...f.drafts, [f.draftFor]: f.comment };
  setFb({ drafts, comment: drafts[setId] ?? '', draftFor: setId });
}

/** Put text in the composer's box if the listener has not written anything there yet. */
export function offerText(text: string): void {
  if (!fb().comment.trim()) {
    setFb({ comment: text });
  }
}

export function setComment(text: string): void {
  setFb({ comment: text });
}

/* ------------------------------------------------------- comparison chip */

/** A note may only claim an oracle the take on screen holds: the server refuses any other, and a reload cannot fix that. */
export function takeChanged(): void {
  const s = getState();
  const held = (o: OracleChoice | null) => !o || !s.take || s.take.keys.includes(o.key);
  if (!held(s.lastOracle)) {
    setState({ lastOracle: null });
  }
  if (!held(s.oracleOverride)) {
    setState({ oracleOverride: null });
  }
}

/** The override select: name the oracle a note is about, or '' to follow the last one selected. */
export function chooseOracle(key: string): void {
  const found = key ? oracleSources(getState()).find((o) => o.key === key) : undefined;
  setState({ oracleOverride: found ? { role: found.role, key: found.key } : null });
}

/* -------------------------------------------------------------------- post */

/* One way to reach the log, so a note, a blind result and an undo report the same way and none can be sent twice
 * by an impatient second click. True once the server has written it.
 * A 409 means the set was re-rendered after this page read it: nothing was written, the composer keeps its text.
 * A reply arriving after another set was opened commits nothing to the panel.
 * Every entry this page writes is remembered by id per set, and an undo names one of those ids. */
async function post(
  payload: NotePost | UndoPost,
  done: TKey,
  after?: () => void,
): Promise<boolean> {
  if (fb().busy) {
    return false;
  }
  setFb({ busy: true });
  const setEpoch = getState().setEpoch;
  const setId = getState().setId;
  const sk = keyOf(setId);
  const superseded = (): boolean => {
    if (setEpoch === getState().setEpoch) {
      return false;
    }
    if (fb().status?.key === 'fb.sending') {
      setFb({ status: null });
    }
    return true;
  };
  const forget = (id: string) =>
    setFb({ mine: { ...fb().mine, [sk]: myIds(setId).filter((x) => x !== id) } });
  const undoId = 'op' in payload ? payload.id : null;
  say('fb.sending');
  try {
    const body = undoId === null ? NotePost.parse(payload) : UndoPost.parse(payload);
    let got: FeedbackLog;
    try {
      got = await postJSON('feedback', body, FeedbackLog);
    } catch (err) {
      if (!(err instanceof HttpError)) {
        throw err;
      }
      if (err.status === 409) {
        if (superseded()) {
          return false;
        }
        const why = StaleRefusal.safeParse(err.body);
        const reason = why.success ? why.data.reason : '409';
        setFb({ stale: reason });
        say('fb.stale', true, { reason });
        return false;
      }
      if (err.status === 404 && undoId !== null) {
        forget(undoId);
        const log = await getJSON(
          `feedback.json?set=${encodeURIComponent(setId ?? '')}`,
          FeedbackLog,
        ).catch(() => null);
        if (superseded()) {
          return false;
        }
        if (log) {
          setFb({ entries: log.entries });
        }
        say('fb.undoGone', true);
        return false;
      }
      throw err;
    }
    if (undoId !== null) {
      forget(undoId);
    } else if (got.entry_id !== undefined) {
      setFb({ mine: { ...fb().mine, [sk]: [...myIds(setId), got.entry_id] } });
    }
    void loadFeedbackIndex();
    if (superseded()) {
      // The text just sent was parked as that set's draft when it was left.
      if (after) {
        setFb({ drafts: { ...fb().drafts, [sk]: '' } });
      }
      return true;
    }
    setFb({ entries: got.entries, path: got.path || fb().path, stale: null });
    after?.();
    say(done);
    return true;
  } catch (err) {
    if (!superseded()) {
      say('fb.failed', true, { msg: err instanceof Error ? err.message : String(err) });
    }
    return false;
  } finally {
    setFb({ busy: false });
  }
}

/* ------------------------------------------------------------------ reads */

/** Reload the log for the set now open, and start the questions over. */
export async function loadFeedback(): Promise<void> {
  const setEpoch = getState().setEpoch;
  setFb({ entries: [], path: '' });
  clearStatus();
  let got: FeedbackLog | null = null;
  try {
    got = await getJSON(
      `feedback.json?set=${encodeURIComponent(getState().setId ?? '')}`,
      FeedbackLog,
    );
  } catch {
    // A server with no feedback endpoint leaves the composer up and fails on send, where the message belongs.
  }
  if (setEpoch !== getState().setEpoch) {
    return;
  }
  if (got) {
    setFb({ entries: got.entries, path: got.path });
  }
  resetComposer();
}

/* ------------------------------------------------------------------ sends */

/** Send what the composer holds; true once written. Sent notes are announced with their grade. */
export async function send(): Promise<boolean> {
  const f = fb();
  const text = f.comment.trim();
  if (!f.trail.length && !text) {
    say('fb.needSomething', true);
    return false;
  }
  const s = getState();
  if (!s.canSend) {
    return false;
  }
  const attached = s.attach && Boolean(s.take);
  const judged = evaluation(s, { attached });
  const verdict = gradeOf(f.trail);
  const ok = await post(
    {
      schema_version: 2,
      lang: currentLang(),
      grade: verdict,
      tag: f.trail[f.trail.length - 1]?.tag ?? '',
      answers: f.trail.map((step) => ({ q: step.node, tag: step.tag, said: labelOf(step) })),
      text,
      // Unattached, a note still names the voice and the judged oracle, which survive un-attaching.
      conditions: s.attach
        ? conditions(s, playhead())
        : { set: s.setId, compared_against: comparedAgainst(s) },
      evaluation: judged,
      // A memo about the voice names no recording, and carries no evidence.
      evidence: attached ? evidenceClaim(s, judged.judged_source) : null,
    },
    'fb.sent',
    () => {
      setFb({ comment: '' });
      resetComposer();
    },
  );
  if (ok) {
    announceSent({ set: s.setId, grade: verdict });
  }
  return ok;
}

/* A blind run's tally, as a note. `unseparated` rides beside the picks rather than inside them: a take the ear
 * could not split is a result about the voice, and counting it as a pick claims a discrimination nobody made.
 * `blind_answers` are each answer as given, so the run carries no playhead or take from the moment it was sent. */
export async function recordBlind(
  summary: string,
  picks: Record<string, string>,
  unseparated: string[],
  answers: BlindAnswer[],
): Promise<boolean> {
  const s = getState();
  const judged = evaluation(s, { attached: false });
  return post(
    {
      schema_version: 2,
      lang: currentLang(),
      grade: '',
      tag: 'blind',
      answers: [],
      text: summary,
      conditions: {
        set: s.setId,
        comparison_id: judged.comparison_id,
        blind: true,
        picks,
        unseparated: unseparated || [],
      },
      evaluation: judged,
      evidence: evidenceClaim(s, null),
      blind_answers: answers || [],
    },
    'fb.sent',
  );
}

/* One version of one take, put forward as the one to keep. Sighted, and the log says so through `conditions.blind`. */
export async function recordPreference(): Promise<void> {
  const s = getState();
  if (!s.take) {
    return;
  }
  const sounding = activeKey(s);
  await post(
    {
      schema_version: 2,
      lang: currentLang(),
      grade: '',
      tag: 'prefer',
      answers: [],
      text: fb().comment.trim(),
      conditions: conditions(s, playhead()),
      // A preference is about the version sounding, whichever side it is on.
      evaluation: { ...evaluation(s), judged_source: sounding },
      evidence: evidenceClaim(s, sounding),
    },
    'fb.sent',
    () => setFb({ comment: '' }),
  );
}

/** Take back this page's last note; it never reaches one this page did not post. */
export async function undo(): Promise<void> {
  const setId = getState().setId;
  const ids = myIds(setId);
  const last = ids[ids.length - 1];
  if (setId === null || last === undefined) {
    return;
  }
  await post({ op: 'undo', set: setId, id: last }, 'fb.undone');
}
