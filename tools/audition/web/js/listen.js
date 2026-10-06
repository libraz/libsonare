/* The listening surface: one voice, its takes, and every version of the take
 * that is open.
 *
 * This module loads a set and moves between its takes. The version switch is
 * `versions.js`, blind mode `blind.js`, the address `address.js`, the voice
 * palette `palette.js`, and the line saying what the page is about
 * `subject.js`.
 */

'use strict';

import { $, el, state, roleOf, notesKey, picksKey, SET_KEY } from './state.js';
import { t, applyStatic } from './i18n.js';
import { takeText } from './take-text.js';
import {
  activeKey, loadTake, markPlay, markRegion, pause, renderLevels, startAt, stopSources,
} from './player.js';
import { loadFeedback, switchDraft, takeChanged } from './feedback.js';
import { writeRoute } from './address.js';
import { buildSetPicker, paletteOpen, renderPickLabel } from './palette.js';
import {
  buildVersionButtons, renderCaptions, selectComparison, selectVersionByKey,
} from './versions.js';
import { applyBlindGate, renderScore, resetBlindReveal, reshuffleBlind } from './blind.js';
import { renderHeadStage, renderSubject } from './subject.js';

/* ------------------------------------------------------------- selection */

/* Counts every selection of a set or a take. A load that finishes after a later
 * selection began belongs to a choice nobody is looking at, so it commits
 * nothing: otherwise the slower of two quick switches wins and plays under the
 * other one's name. The committed take is stamped with it, which is what the
 * pictures key their caches on; `state.setEpoch` is the per-set half that the
 * feedback replies check. */
let selectEpoch = 0;

/* True from the moment a set load starts until its first take is being chosen.
 * The take list on screen is still the previous set's until then, and a click
 * on it would play one set's take under the other's name. */
let setLoading = false;

/* Sending is off from the moment a selection starts until its own load lands,
 * because what the note would be attached to is not settled in between. The
 * button is the visible half; the shortcut calls the send path directly, so it
 * is stopped before it gets there. */
const setSending = (on) => { $('fbSend').disabled = !on; };

document.addEventListener('keydown', (ev) => {
  if ($('fbSend').disabled && (ev.metaKey || ev.ctrlKey) && ev.key === 'Enter'
      && ev.target === $('fbComment')) {
    ev.preventDefault();
    ev.stopImmediatePropagation();
  }
}, true);

/* A note refused because the set was re-rendered asks for this: the same set,
 * take, version and comparison, re-read. The composer's text is left alone. */
document.addEventListener('audition:reload-set', () => {
  const item = state.items[state.itemIndex];
  loadSet(state.setId, {
    set: state.setId, take: item ? item.id : '', ver: state.wantKey, cmp: state.comparisonId,
  });
});

/* ----------------------------------------------------------------- route */

export async function applyRoute(r) {
  if (r.set && r.set !== state.setId && state.sets.some((s) => s.id === r.set)) {
    await loadSet(r.set, r);
    return;
  }
  // A set this server does not hold: the address goes back to the page shown.
  if (r.set && r.set !== state.setId) { writeRoute(); return; }
  // Comparison, then take, then version: each narrows what the next may name.
  if (r.cmp) selectComparison(r.cmp);
  if (r.ver) state.wantKey = r.ver;
  const i = r.take ? state.items.findIndex((it) => it.id === r.take) : -1;
  if (i >= 0 && i !== state.itemIndex) await selectTake(i);
  if (r.ver) selectVersionByKey(r.ver);
}

/* ------------------------------------------------------------------- set */

export async function loadSet(id, want) {
  const entry = state.sets.find((s) => s.id === id) || state.sets[0];
  if (!entry) return;
  const epoch = ++selectEpoch;
  setLoading = true;
  state.setEpoch += 1;
  setSending(false);
  stopSources();
  state.playing = false;
  markPlay(false);
  // Nothing of the previous set may be acted on while this one loads.
  state.take = null;
  $('versions').replaceChildren();

  // The set becomes the current one, and is remembered, only once its manifest
  // has been read: a set that fails to load leaves the page on the previous one.
  let m;
  try {
    const res = await fetch(`s/${entry.id}/manifest.json`);
    if (!res.ok) throw new Error(String(res.status));
    m = await res.json();
  } catch (err) {
    if (epoch !== selectEpoch) return;
    setLoading = false;
    $('title').textContent = t('set.loadFailed', { msg: err.message });
    if (state.items.length) await selectTake(state.itemIndex);
    else setSending(Boolean(state.setId));
    return;
  }
  if (epoch !== selectEpoch) return;
  state.setId = entry.id;
  state.base = `s/${entry.id}/`;
  localStorage.setItem(SET_KEY, entry.id);
  switchDraft(entry.id);
  resetBlindReveal();
  state.manifest = m;
  state.items = m.items || [];
  // A set whose takes hold one version each has nothing to switch between, so
  // the comparison controls come off rather than sitting there doing nothing.
  // That is the ordinary case for anyone without the reference plugin.
  state.compare = state.items.some((it) => Object.keys(it.tracks || {}).length > 1);
  document.body.classList.toggle('no-compare', !state.compare);
  if (!state.compare) {
    state.blind = false;
    $('blind').checked = false;
    document.body.classList.remove('blind-on');
  }
  $('title').textContent = m.title || '';
  $('notes').textContent = m.notes || '';
  $('sharedNote').textContent = m.sources_note || '';
  // Before the picks, which are keyed by the comparison a link names.
  const wanted = want || {};
  const named = wanted.cmp && (m.comparisons || []).some((c) => c && c.id === wanted.cmp);
  state.comparisonId = named ? wanted.cmp : null;
  state.wantByComparison = {};
  // After the compare check and the comparison a link names, so a set or a
  // comparison that cannot be blind-listened to also ends a run left from before.
  applyBlindGate();
  state.picks = JSON.parse(localStorage.getItem(picksKey()) || '{}');
  // A note taken before the feedback log existed is still somebody's listening
  // note, so it is offered back once rather than silently dropped.
  carryOldNotes();
  renderPickLabel();
  renderHeadStage();
  renderSubject();

  if (wanted.ver) state.wantKey = wanted.ver;
  const i = wanted.take ? state.items.findIndex((it) => it.id === wanted.take) : -1;
  state.itemIndex = i >= 0 ? i : 0;
  state.versionIndex = 0;
  state.lastByRole = {};
  // Per-set: the previous voice's last oracle says nothing about this one.
  state.lastOracle = null;
  state.oracleOverride = null;
  buildTakeList();
  await loadFeedback();
  if (epoch !== selectEpoch) return;
  setLoading = false;
  if (state.items.length) {
    await selectTake(state.itemIndex);
  } else { $('versions').replaceChildren(); setSending(true); writeRoute(); }
}

/* Notes taken in this browser before the page could send anything are still
 * worth reading, so they are shown in the composer's box the first time their
 * take is opened and the local copy is dropped once they have been. Keeping
 * both a local note and a sent one would be two places to write the same thing,
 * which is the shape this panel replaced. */
function carryOldNotes() {
  const held = JSON.parse(localStorage.getItem(notesKey()) || '{}');
  state.oldNotes = held;
}

function takeOldNote(id) {
  const held = state.oldNotes || {};
  if (!held[id]) return '';
  const text = held[id];
  delete held[id];
  localStorage.setItem(notesKey(), JSON.stringify(held));
  return text;
}

/* ----------------------------------------------------------------- takes */

function buildTakeList() {
  const nav = $('takes');
  nav.replaceChildren();
  let group = null;
  state.items.forEach((item, i) => {
    if (item.group && item.group !== group) {
      group = item.group;
      nav.appendChild(el('div', 'group', takeText(group)));
    }
    const b = el('button');
    b.type = 'button';
    b.append(el('span', 'take-name', takeText(item.label) || item.id));
    // The id is what the URL carries, so it is shown rather than left to be
    // guessed from a prose label that does not have to resemble it. The id
    // stays as it is in both languages; the sentence after it is translated.
    b.append(el('span', 'sub', item.sub ? `${item.id} — ${takeText(item.sub)}` : item.id));
    // A take that holds a reference is the one worth opening first, and on a
    // voice where only some takes were captured there is otherwise no way to
    // see which from the list.
    if (Object.keys(item.tracks || {}).some((k) => roleOf(k) === 'reference')) {
      const mark = el('span', 'take-ref');
      mark.title = t('takes.hasReference');
      b.append(mark);
    }
    b.addEventListener('click', () => selectTake(i));
    nav.appendChild(b);
  });
}

function markTakeList() {
  [...$('takes').querySelectorAll('button')].forEach((b, i) => {
    if (i === state.itemIndex) {
      b.setAttribute('aria-current', 'true');
      b.scrollIntoView({ block: 'nearest' });
    } else {
      b.removeAttribute('aria-current');
    }
  });
}

export async function selectTake(i) {
  if (setLoading || i < 0 || i >= state.items.length) return;
  const wasPlaying = state.playing;
  const epoch = ++selectEpoch;
  pause();
  setSending(false);
  // The previous take's buffer must not stand for this selection while it loads.
  state.take = null;
  $('versions').replaceChildren();
  state.itemIndex = i;
  state.startOffset = 0;
  // A passage is marked on one take's picture and means nothing on the next
  // one's, so it comes off with the take — and the chip that says so has to go
  // with it, or the transport claims to be playing a passage nothing holds.
  state.region = null;
  markRegion();
  markTakeList();
  // On a kit the take decides which instruments are in play, so the subject
  // line moves with it rather than with the set.
  renderSubject();
  const item = state.items[i];
  const cap = $('specCaption');
  cap.textContent = t('spec.decoding');
  cap.className = 'loading';
  let take;
  try {
    take = await loadTake(item);
  } catch (err) {
    if (epoch !== selectEpoch) return;
    cap.textContent = t('spec.failed', { msg: err.message });
    return;
  }
  if (epoch !== selectEpoch) return;
  take.epoch = epoch;
  state.take = take;
  takeChanged();
  setSending(true);
  cap.className = '';
  reshuffleBlind();
  // The version carries across takes by name rather than by position, so
  // stepping down the take list keeps auditioning the same candidate.
  const want = state.take.keys.indexOf(state.wantKey);
  state.versionIndex = state.blind
    ? Math.min(state.versionIndex, state.blindOrder.length - 1)
    : (want >= 0 ? want : Math.min(state.versionIndex, state.take.keys.length - 1));
  buildVersionButtons();
  if (!state.blind) {
    state.wantKey = activeKey();
    selectVersionByKey(state.wantKey);
  }
  const carried = takeOldNote(item.id);
  if (carried && !$('fbComment').value) $('fbComment').value = carried;
  renderLevels();
  renderScore();
  renderCaptions();
  writeRoute();
  if (wasPlaying) startAt(0);
}

/* Redrawn wholesale when the language changes: every readout above is built in
 * script, so none of them is reached by the static pass over the markup. */
export function refreshListen() {
  applyStatic();
  applyBlindGate();
  renderPickLabel();
  if (paletteOpen()) buildSetPicker();
  // The take list holds translated prose now, so it is rebuilt with everything
  // else rather than keeping the language it was first drawn in.
  if (state.items.length) { buildTakeList(); markTakeList(); }
  if (state.take) buildVersionButtons();
  renderHeadStage();
  renderSubject();
  renderCaptions();
  renderLevels();
  renderScore();
  markPlay(state.playing);
}
