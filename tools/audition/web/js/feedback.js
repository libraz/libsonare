/* Saying what you heard, on the page, while you are still hearing it.
 *
 * The page used to end at a textarea kept in this browser's local storage and a
 * button that downloaded it, which meant a listening report reached anyone else
 * only if its author remembered to export it, find the file and paste it
 * somewhere. Most of what is heard in a session never made that trip.
 *
 * WHAT IS ASKED IS WHAT A LISTENER HEARS, NOT WHAT A PARAMETER IS CALLED. The
 * first fork is "does this sound like a different instrument", because that is
 * the size of error still in the bank; a form offering "attack", "decay" and
 * "harmonics" asks the listener to translate on the harness's behalf, and a
 * mistranslation arrives as a confident, wrong, machine-readable tag. Two
 * answers at a time, "not sure" always a third, and every node can be sent
 * from — a narrowing that stopped early is worth more than one that guessed.
 */

'use strict';

import { $, el, state, roleClass, roleOf, sourceLabel, oracleSources } from './state.js';
import { t, phrase, tree, currentLang } from './i18n.js';
import { loadFeedbackIndex } from './palette.js';
import {
  activeKey, comparedAgainst, conditions, evaluation, evidenceClaim,
} from './player.js';

const fb = {
  node: null,      // the question on screen, or null once an answer is final
  trail: [],       // [{ node, tag, unsure }] — every answer given, in order; labels are resolved at render
  entries: [],     // what has already been sent about this set
  path: '',        // where the server is writing, for the hint under the panel
  busy: false,
  mine: {},        // set id -> ids of the entries THIS page posted, oldest first
  drafts: {},      // set id -> composer text not yet sent
  draftFor: null,  // the set whose text is in the box now
  stale: null,     // the 409 reason on screen, so a language change can re-say it
};

const nodeOf = (id) => tree().nodes[id];

/// Resolved in the language in force now, so a trail answered before a switch
/// reads, and is sent, in the language the note is stamped with.
function labelOf(step) {
  if (step.unsure) return t('fb.notSure');
  const choice = nodeOf(step.node).a.find((c) => c.tag === step.tag);
  return choice ? phrase(choice) : step.tag;
}

const myIds = (setId) => fb.mine[setId] || [];

export function resetComposer() {
  fb.node = tree().start;
  fb.trail = [];
  renderComposer();
}

function answer(node, choice) {
  fb.trail.push({ node, tag: choice.tag });
  fb.node = choice.leaf ? null : (choice.to || null);
  renderComposer();
  // The panel grows as it is answered, and a question that scrolls off under
  // the console is a question nobody notices they were asked.
  $('fbQuestion').scrollIntoView({ block: 'nearest' });
}

function unsure(node) {
  const n = nodeOf(node);
  fb.trail.push({ node, tag: n.unsure, unsure: true });
  fb.node = null;
  renderComposer();
}

function back() {
  const last = fb.trail.pop();
  fb.node = last ? last.node : tree().start;
  renderComposer();
}

function renderComposer() {
  const trail = $('fbTrail');
  trail.replaceChildren();
  fb.trail.forEach((step, i) => {
    if (i) trail.append(el('span', 'fb-sep', '›'));
    trail.append(el('span', 'fb-step', labelOf(step)));
  });
  $('fbBack').hidden = !fb.trail.length;
  $('fbRestart').hidden = !fb.trail.length;

  const box = $('fbChoices');
  box.replaceChildren();
  const node = fb.node ? nodeOf(fb.node) : null;
  const ask = node && (!state.compare && node.qSolo ? node.qSolo : node.q);
  $('fbQuestion').textContent = ask ? phrase(ask) : '';
  $('fbQuestion').hidden = !node;
  if (!node) return;

  for (const choice of node.a) {
    const b = el('button', 'fb-choice', phrase(choice));
    b.type = 'button';
    b.addEventListener('click', () => answer(fb.node, choice));
    box.append(b);
  }
  const idk = el('button', 'fb-choice fb-unsure', t('fb.notSure'));
  idk.type = 'button';
  idk.addEventListener('click', () => unsure(fb.node));
  box.append(idk);
}

/* --------------------------------------------------------- compared against */

/* Which oracle a note is about: the last reference or comparison selected in
 * this set, which the select beside it can correct. */

export function renderComparedAgainst() {
  const chip = $('cmpChip');
  if (!chip) return;
  const ca = comparedAgainst();
  chip.replaceChildren();
  chip.className = 'cmp-chip';
  if (!ca) {
    // Absent is a state worth flagging rather than a quiet default: a note
    // sent from here carries no oracle, which is the one thing this whole
    // feature exists to stop happening unnoticed.
    chip.classList.add(roleClass(''), 'warn');
    chip.append(el('span', 'dot'), el('span', '', t('cmp.unplayed')));
    chip.title = t('cmp.unplayedTitle');
  } else {
    chip.classList.add(roleClass(ca.role));
    chip.append(
      el('span', 'dot'),
      el('span', 'cmp-name', ca.label),
      el('span', 'cmp-role', `(${t(`role.${ca.role}`)})`),
    );
    chip.title = ca.chosen === 'manual' ? t('cmp.manualTitle') : t('cmp.autoTitle');
  }
  const sel = $('cmpOverride');
  if (sel) sel.value = state.oracleOverride ? state.oracleOverride.key : '';
}

/// Rebuilt per set and per take, so it only offers the oracles the take holds.
function buildComparedAgainstOverride() {
  const sel = $('cmpOverride');
  if (!sel) return;
  sel.replaceChildren();
  const auto = el('option', '', t('cmp.overrideAuto'));
  auto.value = '';
  sel.append(auto);
  for (const src of oracleSources()) {
    const opt = el('option', '', `${t(`role.${src.role}`)}: ${src.label}`);
    opt.value = src.key;
    sel.append(opt);
  }
  // Left at auto; `renderComparedAgainst` applies any override in force.
}

/// A note may only claim an oracle the take on screen holds: the server refuses
/// any other, and a reload cannot fix that.
export function takeChanged() {
  const held = (o) => !o || state.take.keys.includes(o.key);
  if (!held(state.lastOracle)) state.lastOracle = null;
  if (!held(state.oracleOverride)) state.oracleOverride = null;
  buildComparedAgainstOverride();
  renderComparedAgainst();
}

function onOverrideChange(ev) {
  const key = ev.target.value;
  const found = key ? oracleSources().find((s) => s.key === key) : null;
  state.oracleOverride = found ? { role: found.role, key: found.key } : null;
  renderComparedAgainst();
}

/* ------------------------------------------------------------------- send */

const finalTag = () => (fb.trail.length ? fb.trail[fb.trail.length - 1].tag : '');

/* The verdict, separate from the diagnosis.
 *
 * "Recognisably the instrument, and I would still change it" is the state most
 * of the bank is in, and a finer tag buries it: `onset/hard` is the same string
 * whether the voice is shippable or unrecognisable. The grade is read off the
 * verdict node, with `broken` overriding it, so a triage that only wants to
 * know which voices are defects never has to parse the rest.
 */
const GRADES = {
  ok: 'grade.ok',
  acceptable: 'grade.acceptable',
  'wrong-instrument': 'grade.wrongInstrument',
  broken: 'grade.broken',
  off: 'grade.off',
  'off/unsure': 'grade.unsure',
};

function grade() {
  if (fb.trail.some((s) => s.tag === 'broken')) return 'broken';
  const verdict = fb.trail.find((s) => s.node === 'grade');
  if (verdict) return verdict.tag;
  return fb.trail.length ? fb.trail[0].tag : '';
}

const gradeLabel = (name) => (GRADES[name] ? t(GRADES[name]) : name);

/// Teal for a voice that is fine, amber for one that is liveable, red for one
/// that is a defect: the same three weights the bank's own colours carry.
///
/// Written out rather than looked up in a table of class names, because the
/// check that every class the page uses still has a rule reads literals: a
/// modifier that only exists as a value in an object is invisible to it, and
/// the rule for it can be deleted with nothing going red.
function gradeChip(name) {
  const chip = el('span', 'fb-grade', gradeLabel(name));
  if (name === 'ok') chip.classList.add('good');
  else if (name === 'acceptable') chip.classList.add('mid');
  else chip.classList.add('bad');
  return chip;
}

function say(msg, bad) {
  const line = $('fbStatus');
  line.textContent = msg;
  line.classList.toggle('bad', Boolean(bad));
}

/// The last action's reply belongs to the set and comparison it was about, so
/// it comes off when either changes.
export function clearStatus() {
  say('');
  $('fbReload').hidden = true;
  fb.stale = null;
}

/// The composer's text is per set: what was written about one voice is never
/// sent as a note on another. Called with the set about to be shown.
export function switchDraft(setId) {
  if (fb.draftFor === setId) return;
  if (fb.draftFor !== null) fb.drafts[fb.draftFor] = $('fbComment').value;
  $('fbComment').value = fb.drafts[setId] || '';
  fb.draftFor = setId;
}

/// The palette and the bank read their counts from the index, so a note or an
/// undo has to bring it up to date and redraw whatever shows it.
async function refreshIndex() {
  await loadFeedbackIndex();
  document.dispatchEvent(new CustomEvent('audition:feedback-changed'));
}

/// One way to reach the log, so a note, a blind result and an undo report the
/// same way and none of them can be sent twice by an impatient second click.
/// True once the server has written it.
///
/// A 409 means the set was re-rendered after this page read it: nothing was
/// written, the composer keeps its text and the page offers to re-read the set.
/// A reply arriving after another set was opened is not this set's log, so it
/// commits nothing to the panel; it only takes down the "sending" line if that
/// is still what the line says.
///
/// Every entry this page writes is remembered by the id the server gives it, per
/// set, and an undo names one of those ids: it never reaches an entry this page
/// did not post. A 404 means the entry is already gone from the log.
async function post(payload, done, after) {
  if (fb.busy) return false;
  fb.busy = true;
  const setEpoch = state.setEpoch;
  const setId = state.setId;
  const sending = t('fb.sending');
  const superseded = () => {
    if (setEpoch === state.setEpoch) return false;
    if ($('fbStatus').textContent === sending) say('');
    return true;
  };
  say(sending);
  try {
    const res = await fetch('feedback', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (res.status === 409) {
      const why = await res.json().catch(() => ({}));
      if (superseded()) return false;
      fb.stale = why.reason || '409';
      say(t('fb.stale', { reason: fb.stale }), true);
      $('fbReload').hidden = false;
      return false;
    }
    if (res.status === 404 && payload.op === 'undo') {
      fb.mine[setId] = myIds(setId).filter((id) => id !== payload.id);
      const log = await fetch(`feedback.json?set=${encodeURIComponent(setId || '')}`)
        .then((r) => r.json()).catch(() => null);
      if (superseded()) return false;
      if (log) fb.entries = log.entries || [];
      renderRecent();
      say(t('fb.undoGone'), true);
      return false;
    }
    if (!res.ok) throw new Error(String(res.status));
    const got = await res.json();
    if (payload.op === 'undo') {
      fb.mine[setId] = myIds(setId).filter((id) => id !== payload.id);
    } else if (got.entry_id !== undefined && got.entry_id !== null) {
      fb.mine[setId] = [...myIds(setId), got.entry_id];
    }
    refreshIndex();
    if (superseded()) {
      // The text just sent was parked as that set's draft when it was left.
      if (after) fb.drafts[setId] = '';
      return true;
    }
    fb.entries = got.entries || [];
    fb.path = got.path || fb.path;
    if (after) after();
    renderRecent();
    say(t(done));
    return true;
  } catch (err) {
    if (!superseded()) say(t('fb.failed', { msg: err.message }), true);
    return false;
  } finally {
    fb.busy = false;
  }
}

async function send() {
  const text = $('fbComment').value.trim();
  if (!fb.trail.length && !text) { say(t('fb.needSomething'), true); return; }
  const attached = $('fbAttach').checked && Boolean(state.take);
  const judged = evaluation({ attached });
  await post({
    schema_version: 2,
    lang: currentLang(),
    grade: grade(),
    tag: finalTag(),
    answers: fb.trail.map((s) => ({ q: s.node, tag: s.tag, said: labelOf(s) })),
    text,
    // Unattached, a note is about the voice rather than about the moment, so
    // it still has to carry which voice: that is what the log is keyed by.
    // The judged oracle survives un-attaching, unlike the take and playhead.
    conditions: $('fbAttach').checked
      ? conditions()
      : { set: state.setId, compared_against: comparedAgainst() },
    evaluation: judged,
    // A memo about the voice names no recording, and carries no evidence.
    evidence: attached ? evidenceClaim(judged.judged_source) : null,
  }, 'fb.sent', () => {
    $('fbComment').value = '';
    resetComposer();
  });
}

/* A blind run's tally, as a note.
 *
 * It is a result rather than an impression, and it used to leave the page only
 * through a download that had to be remembered, found and pasted somewhere —
 * so in practice it left with whoever closed the tab. It goes in the same log
 * as everything else because it is the same kind of statement: this is what the
 * ear preferred, on this voice, on this day. The picks ride with it per take,
 * since a tally of four to two does not say which four.
 */
/* `unseparated` rides beside the picks rather than being folded into them: a
 * take the ear could not split is a result about the voice — the two versions
 * are that close — and counting it as a pick for whichever one was sounding is
 * how a tally comes to claim a discrimination nobody made. */
/* `blind_answers` are each answer as it was given — take, draw, comparison,
 * generation, time — so the run carries no playhead or take from the moment it
 * happened to be sent. */
export async function recordBlind(summary, picks, unseparated, blindAnswers) {
  const judged = evaluation({ attached: false });
  return post({
    schema_version: 2,
    lang: currentLang(),
    grade: '',
    tag: 'blind',
    answers: [],
    text: summary,
    conditions: {
      set: state.setId,
      comparison_id: judged.comparison_id,
      blind: true,
      picks,
      unseparated: unseparated || [],
    },
    evaluation: judged,
    evidence: evidenceClaim(null),
    blind_answers: blindAnswers || [],
  }, 'fb.sent');
}

/* One version of one take, put forward as the one to keep.
 *
 * A voice with a set of recorded candidates is a question — which of these
 * should the library ship — and the triage tree cannot ask it: the tree asks
 * what is wrong with what is sounding, one version at a time, and a ranking is
 * not a fault. Without this the answer had to be typed out, which means the
 * version it names is whatever the typist remembered rather than what was
 * playing.
 *
 * Sighted, and the log says so: `conditions.blind` rides with every note, so a
 * preference formed while the names were visible can be read for what it is
 * rather than weighed against a blind run's tally.
 */
export async function recordPreference() {
  const text = $('fbComment').value.trim();
  if (!state.take) return;
  const sounding = activeKey();
  await post({
    schema_version: 2,
    lang: currentLang(),
    grade: '',
    tag: 'prefer',
    answers: [],
    text,
    conditions: conditions(),
    // A preference is about the version sounding, whichever side it is on.
    evaluation: { ...evaluation(), judged_source: sounding },
    evidence: evidenceClaim(sounding),
  }, 'fb.sent', () => { $('fbComment').value = ''; });
}

async function undo() {
  const ids = myIds(state.setId);
  if (!ids.length) return;
  await post({ op: 'undo', set: state.setId, id: ids[ids.length - 1] }, 'fb.undone');
}

/* ----------------------------------------------------------------- recent */

/* Shown per voice rather than per take. Two takes of one instrument usually
 * carry the same complaint, and a list scoped to the take in front of you hides
 * exactly the repetition that says a fault is the voice's rather than one
 * phrase's. */

const clock = (iso) => {
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString(currentLang(), {
    month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
  });
};

/// A saved note's oracle: recorded, explicitly none, or unknown (predates the field).
function comparedAgainstChip(c) {
  const chip = el('span', 'fb-cmp');
  if (!('compared_against' in c)) {
    chip.classList.add(roleClass(''));
    chip.append(el('span', 'dot'), el('span', '', t('cmp.unknown')));
    chip.title = t('cmp.unknownTitle');
    return chip;
  }
  const ca = c.compared_against;
  if (!ca) {
    chip.classList.add(roleClass(''));
    chip.append(el('span', 'dot'), el('span', '', t('cmp.unplayed')));
    return chip;
  }
  chip.classList.add(roleClass(ca.role));
  chip.append(el('span', 'dot'), el('span', '', ca.label || ca.version));
  chip.title = t(`role.${ca.role}`);
  return chip;
}

function entryEl(entry) {
  const row = el('div', 'fb-entry');
  const head = el('div', 'fb-entry-head');
  const c = entry.conditions || {};
  if (entry.grade) head.append(gradeChip(entry.grade));
  if (c.version) {
    const chip = el('span', 'fb-who');
    chip.classList.add(roleClass(roleOf(c.version)));
    chip.append(el('span', 'dot'), el('span', '', c.version));
    chip.title = sourceLabel(c.version);
    head.append(chip);
  } else if (c.blind) {
    const chip = el('span', 'fb-who', t('now.hidden'));
    chip.classList.add(roleClass(''));
    head.append(chip);
  }
  head.append(comparedAgainstChip(c));
  if (c.take) head.append(el('span', 'fb-take', c.take));
  head.append(el('span', 'grow'));
  head.append(el('span', 'fb-when', clock(entry.at)));
  row.append(head);

  const said = (entry.answers || []).map((a) => a.said).filter(Boolean);
  if (said.length) row.append(el('p', 'fb-said', said.join('  ›  ')));
  if (entry.text) row.append(el('p', 'fb-text', entry.text));
  if (entry.tag) {
    const tag = el('code', 'fb-tag', entry.tag);
    row.append(tag);
  }
  return row;
}

function renderRecent() {
  const box = $('fbRecent');
  box.replaceChildren();
  if (!fb.entries.length) {
    box.append(el('p', 'hint', t('fb.none')));
  } else {
    for (const entry of [...fb.entries].reverse()) box.append(entryEl(entry));
  }
  $('fbUndo').disabled = !myIds(state.setId).length;
  $('fbWhere').textContent = fb.path ? t('fb.where', { path: fb.path }) : '';
}

/// Reload the log for whichever set is open, and start the questions over.
export async function loadFeedback() {
  const setEpoch = state.setEpoch;
  fb.entries = [];
  fb.path = '';
  clearStatus();
  let got = null;
  try {
    got = await (await fetch(
      `feedback.json?set=${encodeURIComponent(state.setId || '')}`)).json();
  } catch {
    // The page is usable against a directory of renders served by anything at
    // all; a server with no feedback endpoint leaves the composer up and fails
    // on send, which is where the message belongs.
  }
  if (setEpoch !== state.setEpoch) return;
  if (got) {
    fb.entries = got.entries || [];
    fb.path = got.path || '';
  }
  resetComposer();
  renderRecent();
  buildComparedAgainstOverride();
  renderComparedAgainst();
}

/// The composer and the log are re-rendered wholesale on a language change. The
/// status line goes with them: it is the last action's reply and it would
/// otherwise sit there in the language nobody is reading any more.
export function refreshFeedback() {
  // A refused note keeps saying why, in the new language, beside its button.
  if (fb.stale !== null) say(t('fb.stale', { reason: fb.stale }), true);
  else say('');
  renderComposer();
  renderRecent();
  buildComparedAgainstOverride();
  renderComparedAgainst();
}

export function wireFeedback() {
  $('fbSend').addEventListener('click', send);
  $('fbUndo').addEventListener('click', undo);
  $('fbBack').addEventListener('click', back);
  $('fbRestart').addEventListener('click', () => { fb.trail = []; resetComposer(); });
  // Sending is the one thing on this panel worth a shortcut, and it has to be a
  // modified key: the box is where a sentence gets typed.
  $('fbComment').addEventListener('keydown', (ev) => {
    if ((ev.metaKey || ev.ctrlKey) && ev.key === 'Enter') { ev.preventDefault(); send(); }
  });
  $('cmpOverride').addEventListener('change', onOverrideChange);
  // `listen.js` re-reads the set; the composer is left as it is.
  $('fbReload').addEventListener('click', () => {
    clearStatus();
    document.dispatchEvent(new CustomEvent('audition:reload-set'));
  });
}
