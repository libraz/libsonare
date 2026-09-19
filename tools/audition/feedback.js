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

import { $, el, state, roleClass, roleOf, sourceLabel } from './state.js';
import { t, phrase, tree, currentLang } from './i18n.js';
import { conditions } from './player.js';

const fb = {
  node: null,      // the question on screen, or null once an answer is final
  trail: [],       // [{ node, tag, label }] — every answer given, in order
  entries: [],     // what has already been sent about this set
  path: '',        // where the server is writing, for the hint under the panel
  busy: false,
};

const nodeOf = (id) => tree().nodes[id];

export function resetComposer() {
  fb.node = tree().start;
  fb.trail = [];
  renderComposer();
}

function answer(node, choice) {
  fb.trail.push({ node, tag: choice.tag, label: phrase(choice) });
  fb.node = choice.leaf ? null : (choice.to || null);
  renderComposer();
  // The panel grows as it is answered, and a question that scrolls off under
  // the console is a question nobody notices they were asked.
  $('fbQuestion').scrollIntoView({ block: 'nearest' });
}

function unsure(node) {
  const n = nodeOf(node);
  fb.trail.push({ node, tag: n.unsure, label: t('fb.notSure') });
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
    trail.append(el('span', 'fb-step', step.label));
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

/// One way to reach the log, so a note, a blind result and an undo report the
/// same way and none of them can be sent twice by an impatient second click.
async function post(payload, done, after) {
  if (fb.busy) return;
  fb.busy = true;
  say(t('fb.sending'));
  try {
    const res = await fetch('feedback', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!res.ok) throw new Error(String(res.status));
    const got = await res.json();
    fb.entries = got.entries || [];
    fb.path = got.path || fb.path;
    if (after) after();
    renderRecent();
    say(t(done));
  } catch (err) {
    say(t('fb.failed', { msg: err.message }), true);
  } finally {
    fb.busy = false;
  }
}

async function send() {
  const text = $('fbComment').value.trim();
  if (!fb.trail.length && !text) { say(t('fb.needSomething'), true); return; }
  await post({
    lang: currentLang(),
    grade: grade(),
    tag: finalTag(),
    answers: fb.trail.map((s) => ({ q: s.node, tag: s.tag, said: s.label })),
    text,
    // Unattached, a note is about the voice rather than about the moment, so
    // it still has to carry which voice: that is what the log is keyed by.
    conditions: $('fbAttach').checked ? conditions() : { set: state.setId },
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
export async function recordBlind(summary, picks) {
  await post({
    lang: currentLang(),
    grade: '',
    tag: 'blind',
    answers: [],
    text: summary,
    conditions: { ...conditions(), picks },
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
  await post({
    lang: currentLang(),
    grade: '',
    tag: 'prefer',
    answers: [],
    text,
    conditions: conditions(),
  }, 'fb.sent', () => { $('fbComment').value = ''; });
}

async function undo() {
  if (!fb.entries.length) return;
  await post({ op: 'undo', set: state.setId }, 'fb.undone');
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
  $('fbUndo').disabled = !fb.entries.length;
  $('fbWhere').textContent = fb.path ? t('fb.where', { path: fb.path }) : '';
}

/// Reload the log for whichever set is open, and start the questions over.
export async function loadFeedback() {
  fb.entries = [];
  fb.path = '';
  try {
    const got = await (await fetch(
      `feedback.json?set=${encodeURIComponent(state.setId || '')}`)).json();
    fb.entries = got.entries || [];
    fb.path = got.path || '';
  } catch {
    // The page is usable against a directory of renders served by anything at
    // all; a server with no feedback endpoint leaves the composer up and fails
    // on send, which is where the message belongs.
  }
  resetComposer();
  renderRecent();
}

/// The composer and the log are re-rendered wholesale on a language change. The
/// status line goes with them: it is the last action's reply and it would
/// otherwise sit there in the language nobody is reading any more.
export function refreshFeedback() {
  say('');
  renderComposer();
  renderRecent();
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
}
