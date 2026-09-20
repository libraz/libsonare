/* The listening surface: one voice, its takes, and every version of the take
 * that is open.
 *
 * WHICH SIDE OF THE COMPARISON IS SOUNDING IS THE PAGE'S FIRST JOB. A version
 * used to be a key on a button and an accent colour that meant "selected", so
 * the page said which button was pressed and never said whether the sound
 * coming out was the library's or the thing it is being measured against. Now a
 * role owns a colour and a sentence: the banner names it, the row is labelled
 * with it, the button carries its dot, and the two pictures below are drawn in
 * it. One key swaps between the two sides, which is the comparison the page
 * exists for and previously required knowing which digit was which.
 *
 * WHAT IS SOUNDING IS ADDRESSABLE. The fragment is `#<set>/<take>/<version>`
 * and it is rewritten on every move, so the page can be pointed at one exact
 * render and the address bar always names the one being heard. With several
 * sets of the same instrument open — a reference, an unmodified build, a few
 * candidate settings — being sure is not something anyone does from memory.
 */

'use strict';

import {
  $, el, state, ROLE_ORDER, roleClass, roleOf, pathOf, blockOf, sourceOf,
  sourceLabel, notesKey, picksKey, SET_KEY,
} from './state.js';
import { t, applyStatic, phrase } from './i18n.js';
import { takeText } from './take-text.js';
import {
  activeKey, applyGains, loadTake, pause, playhead, renderLevels,
  startAt, stopSources, hitAt, conditions,
} from './player.js';
import { dotEl, heardChip, signoffChip, stageBar } from './bank.js';
import { loadFeedback, recordBlind, recordPreference } from './feedback.js';

/* ----------------------------------------------------------------- route */

export function readRoute() {
  const q = new URLSearchParams(location.search);
  // The playhead rides in the query rather than in the fragment. It is a
  // starting position rather than live state, and writing it back the way the
  // fragment is written back would put a number in the address bar that changes
  // sixty times a second.
  const at = Number.parseFloat(q.get('t'));
  const tt = Number.isFinite(at) ? at : null;
  const frag = location.hash.replace(/^#\/?/, '');
  if (frag) {
    const [set, take, ver] = frag.split('/');
    return {
      set: decodeURIComponent(set || ''),
      take: decodeURIComponent(take || ''),
      ver: decodeURIComponent(ver || ''),
      t: tt,
    };
  }
  return { set: q.get('set') || '', take: q.get('take') || '', ver: q.get('v') || '', t: tt };
}

export function routeHash() {
  const parts = [state.setId];
  const item = state.items[state.itemIndex];
  if (item) parts.push(item.id);
  // Left out in blind mode along with the path and the label: an address bar is
  // visible, and hiding the name is the whole point of that mode.
  if (item && state.take && !state.blind) parts.push(activeKey());
  return '#' + parts.filter(Boolean).map(encodeURIComponent).join('/');
}

/// Rewritten rather than pushed: every arrow key is a move, and a hundred of
/// them in the back stack makes the browser's own back button useless.
function writeRoute() {
  const hash = routeHash();
  if (location.hash !== hash) history.replaceState(null, '', hash);
  renderIdent();
}

export async function applyRoute(r) {
  if (r.set && r.set !== state.setId && state.sets.some((s) => s.id === r.set)) {
    await loadSet(r.set, r);
    return;
  }
  if (r.ver) state.wantKey = r.ver;
  const i = r.take ? state.items.findIndex((it) => it.id === r.take) : -1;
  if (i >= 0 && i !== state.itemIndex) { await selectTake(i); return; }
  if (r.ver) selectVersionByKey(r.ver);
}

/* ------------------------------------------------------------------- set */

export async function loadSet(id, want) {
  const entry = state.sets.find((s) => s.id === id) || state.sets[0];
  if (!entry) return;
  stopSources();
  state.playing = false;
  $('playBtn').textContent = t('transport.play');
  state.setId = entry.id;
  state.base = `s/${entry.id}/`;
  localStorage.setItem(SET_KEY, entry.id);

  const m = await (await fetch(`${state.base}manifest.json`)).json();
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
  }

  $('title').textContent = m.title || '';
  $('notes').textContent = m.notes || '';
  $('sharedNote').textContent = m.sources_note || '';
  state.picks = JSON.parse(localStorage.getItem(picksKey()) || '{}');
  // A note taken before the feedback log existed is still somebody's listening
  // note, so it is offered back once rather than silently dropped.
  carryOldNotes();
  renderPickLabel();
  renderHeadStage();
  renderSubject();

  const wanted = want || {};
  if (wanted.ver) state.wantKey = wanted.ver;
  const i = wanted.take ? state.items.findIndex((it) => it.id === wanted.take) : -1;
  state.itemIndex = i >= 0 ? i : 0;
  state.versionIndex = 0;
  state.lastByRole = {};
  buildTakeList();
  await loadFeedback();
  if (state.items.length) await selectTake(state.itemIndex);
  else { $('versions').replaceChildren(); writeRoute(); }
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

/* The voice palette.
 *
 * A native select over 180 entries shows one at a time and says nothing but the
 * name, so choosing what to listen to next meant already knowing the answer.
 * Every voice here carries the three facts that decide whether it is worth
 * opening — how far its calibration has got, whether anybody has listened to
 * it, and the worst thing they said — which are the same readouts the bank
 * view carries, rendered by the same functions rather than a second set that
 * can disagree with them.
 */

/// The status row for a set, where the bank has one. A set served from outside
/// this repository has none, and the row is then just its name.
const voiceOf = (id) => state.bank.voices.find((v) => v.slug === id);

export function buildSetPicker() {
  const rows = $('voiceRows');
  rows.replaceChildren();
  const find = $('voiceFind').value.trim().toLowerCase();
  let shown = 0;
  let group = null;
  for (const s of state.sets) {
    const v = voiceOf(s.id);
    // Searched over everything on the row rather than the name alone: a voice
    // is looked for by program number and by engine at least as often.
    const hay = [s.id, s.title, s.group, v && v.name, v && v.patch, v && v.engine]
      .filter(Boolean).join(' ').toLowerCase();
    if (find && !hay.includes(find)) continue;
    shown += 1;
    if ((s.group || '') !== group) {
      group = s.group || '';
      if (group) rows.append(el('div', 'palette-group', group));
    }
    rows.append(voiceRow(s, v));
  }
  $('voiceCount').textContent = shown === state.sets.length
    ? String(state.sets.length) : `${shown} / ${state.sets.length}`;
  if (!shown) rows.append(el('p', 'bank-empty', t('bank.nothingMatches')));
  renderPickLabel();
}

function voiceRow(s, v) {
  const row = el('button', 'palette-row');
  row.type = 'button';
  row.setAttribute('role', 'option');
  row.setAttribute('aria-selected', String(s.id === state.setId));
  row.addEventListener('click', () => { closePalette(); loadSet(s.id); });

  row.append(el('span', 'addr', v ? addressOf(v) : ''));
  const who = el('span', 'who', v ? v.name : s.title || s.id);
  if (v && v.patch) who.append(el('span', 'patch', `  ${v.patch}`));
  row.append(who);
  row.append(v ? dotEl(v.engine) : el('span', 'dot'));

  if (v) {
    const stage = el('span', 'stage');
    stage.append(stageBar(v), el('span', '', v.stage.toFixed(1)));
    stage.title = `${t(`stage.${Math.round(v.stage * 5)}`)} — ${v.next}`;
    row.append(stage);
  } else {
    row.append(el('span', 'stage'));
  }

  row.append(heardChip(s.id), signoffChip(v));
  // A voice with no reference plays rather than compares, and that decides what
  // a listening session on it can even be asked. Set faintly rather than as a
  // badge: sixty of the variations are in this state, and sixty chips bury the
  // handful of rows that carry something worth finding.
  row.append(el('span', 'solo', s.compare ? '' : t('pick.playOnly')));
  return row;
}

const addressOf = (v) =>
  (v.kit ? `kit ${v.program}` : `${v.program}${v.bank ? `:${v.bank}` : ''}`);

function renderPickLabel() {
  const v = voiceOf(state.setId);
  const btn = $('voicePick');
  btn.replaceChildren();
  if (v) btn.append(dotEl(v.engine));
  btn.append(el('span', 'vp-name', v ? v.name : state.setId || ''));
  // No stage bar here: `headStage` carries one two elements along the header,
  // and the same reading twice is the reading nobody trusts.
  btn.append(el('span', 'vp-id', state.setId || ''));
}

export function openPalette() {
  $('voicePanel').hidden = false;
  $('voicePick').setAttribute('aria-expanded', 'true');
  $('voiceFind').value = '';
  buildSetPicker();
  $('voiceFind').focus();
  const here = $('voiceRows').querySelector('[aria-selected="true"]');
  if (here) here.scrollIntoView({ block: 'center' });
}

export function closePalette() {
  $('voicePanel').hidden = true;
  $('voicePick').setAttribute('aria-expanded', 'false');
}

export const paletteOpen = () => !$('voicePanel').hidden;

/// Down from the find box and through the rows, so the whole list is reachable
/// without leaving the keyboard the filter is being typed on.
export function paletteKey(ev) {
  const rows = [...$('voiceRows').querySelectorAll('.palette-row')];
  if (!rows.length) return;
  const at = rows.indexOf(document.activeElement);
  if (ev.key === 'ArrowDown') {
    ev.preventDefault();
    rows[Math.min(at + 1, rows.length - 1)].focus();
  } else if (ev.key === 'ArrowUp') {
    ev.preventDefault();
    if (at <= 0) $('voiceFind').focus();
    else rows[at - 1].focus();
  } else if (ev.key === 'Enter' && at < 0) {
    ev.preventDefault();
    rows[0].click();
  }
}

/// Fetched with the set index rather than per voice: the list needs all of it
/// at once, and 180 requests to fill one column is not a thing a list does.
export async function loadFeedbackIndex() {
  try {
    state.fbIndex = await (await fetch('feedback-index.json')).json();
  } catch {
    state.fbIndex = {};
  }
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
  if (i < 0 || i >= state.items.length) return;
  const wasPlaying = state.playing;
  pause();
  state.itemIndex = i;
  state.startOffset = 0;
  state.region = null;
  markTakeList();
  // On a kit the take decides which instruments are in play, so the subject
  // line moves with it rather than with the set.
  renderSubject();
  const item = state.items[i];
  const cap = $('specCaption');
  cap.textContent = t('spec.decoding');
  cap.className = 'loading';
  try {
    state.take = await loadTake(item);
  } catch (err) {
    cap.textContent = t('spec.failed', { msg: err.message });
    state.take = null;
    return;
  }
  cap.className = '';
  reshuffleBlind();
  // The version carries across takes by name rather than by position, so
  // stepping down the take list keeps auditioning the same candidate.
  const want = state.take.keys.indexOf(state.wantKey);
  state.versionIndex = state.blind
    ? Math.min(state.versionIndex, state.blindOrder.length - 1)
    : (want >= 0 ? want : Math.min(state.versionIndex, state.take.keys.length - 1));
  buildVersionButtons();
  if (!state.blind) state.wantKey = activeKey();
  const carried = takeOldNote(item.id);
  if (carried && !$('fbComment').value) $('fbComment').value = carried;
  renderLevels();
  renderScore();
  renderCaptions();
  writeRoute();
  if (wasPlaying) startAt(0);
}

/* The draw for a blind run: which versions are in it, in an order that says
 * nothing. Only the shipped path is drawn — see `displayOrder` — so a slot in
 * blind mode is a position in this list rather than an index into the take. */
export function reshuffleBlind() {
  const order = state.take
    ? state.take.keys.map((_, i) => i).filter((i) => !pathOf(state.take.keys[i]))
    : [];
  for (let i = order.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [order[i], order[j]] = [order[j], order[i]];
  }
  state.blindOrder = order;
}

/* -------------------------------------------------------------- versions */

/* Slots in the order they are shown, which is the order `1`…`9` count in.
 *
 * A blind run is one question, so it is run on the path that ships. The same
 * candidate rendered again with the rig cleared is the same candidate, and a
 * tally taken across both paths counts a vote for a setting as a vote for a
 * path — the two are not comparable and the result cannot be read back apart.
 * They stay on the page; they are not in the draw.
 */
function displayOrder() {
  const slots = state.take.keys.map((_, i) => i);
  if (state.blind) return state.blindOrder.map((_, i) => i);
  const rank = (slot) => {
    const key = state.take.keys[slot];
    const at = ROLE_ORDER.indexOf(roleOf(key));
    // The direct path after the shipped one within a role, so the two blocks of
    // a role sit together rather than either of them splitting the other.
    return (at < 0 ? ROLE_ORDER.length : at) * 2 + (pathOf(key) ? 1 : 0);
  };
  // Stable, so a role's own versions keep the order the manifest gave them.
  return slots.map((s, i) => [s, i]).sort((a, b) => rank(a[0]) - rank(b[0]) || a[1] - b[1])
    .map(([s]) => s);
}

/* What goes on a button: what the setting IS, over the key it is reached at.
 *
 * The key alone was the button for as long as the switch had one row of them,
 * and it stopped working the moment a voice carried nine candidates: `no-16`,
 * `chiff-none`, `foundations-only` and `wind-unsteady` each say which knob
 * moved and none of them says what it is for, so a lineup of nine is nine
 * guesses. The title is registered beside the setting and refused if it is not
 * there — `calibrations.json`, enforced in `calibration.py`.
 *
 * The key stays under it rather than being replaced by it. It is the address a
 * render is reached at, the file stem on disk and the string a listening note
 * carries, so what is on the button and what is in the URL have to be the same
 * string in sight of each other.
 */
function versionFace(slot) {
  const item = state.items[state.itemIndex];
  const key = state.take.keys[state.blind ? state.blindOrder[slot] : slot];
  if (state.blind) {
    const letter = String.fromCharCode(65 + state.display.indexOf(slot));
    const pick = state.picks[item.id];
    return { name: pick && pick.revealed ? `${letter} · ${key}` : letter, key: '' };
  }
  const title = sourceTitle(key);
  return title ? { name: title, key } : { name: key, key: '' };
}

/// The unmodified build is not a recorded setting and never will be, so its two
/// keys carry their title on the page rather than in the registry.
const BASELINE_TITLES = { model: 'ver.baseline', 'model-di': 'ver.baselineDirect' };

const sourceTitle = (key) =>
  (BASELINE_TITLES[key] ? t(BASELINE_TITLES[key]) : phrase(sourceOf(key).title));

function buildVersionButtons() {
  const box = $('versions');
  box.replaceChildren();
  state.display = displayOrder();

  /* One block per QUESTION, not one strip per page.
   *
   * Three unrelated things used to share one list: which recording is the
   * target, which signal path the library is heard down, and which calibration
   * candidate. On the clean electric guitar that is eighteen buttons, and a
   * pick out of eighteen answers none of the three — so the block is keyed by
   * role and path together, and a manifest declaring neither still gets the
   * single unlabelled block it always had. */
  const blocks = [];
  let seg = null;
  let segBlock = null;
  state.display.forEach((slot, pos) => {
    const key = state.blind ? '' : state.take.keys[slot];
    const role = key ? roleOf(key) : '';
    const path = key ? pathOf(key) : '';
    const block = key ? blockOf(key) : '';
    if (seg === null || block !== segBlock) {
      segBlock = block;
      const wrap = el('div', 'vrow');
      wrap.classList.add(roleClass(role));
      seg = el('div', 'segmented');
      seg.setAttribute('role', 'group');
      seg.setAttribute('aria-label', role ? t(`role.${role}`) : t('role.other'));
      // The head goes in first and is filled once the block's size is known:
      // what a block asks of a listener depends on how many versions are in it.
      const head = el('div', 'vhead');
      wrap.append(head, seg);
      box.append(wrap);
      blocks.push({ role, path, head, seg });
    }
    const b = el('button');
    b.type = 'button';
    b.dataset.slot = String(slot);
    b.append(el('span', 'key', String(pos + 1)));
    const face = versionFace(slot);
    const text = el('span', 'vtext');
    text.append(el('span', 'vname', face.name));
    if (face.key) text.append(el('span', 'vkey', face.key));
    b.append(text);
    b.title = versionTitle(slot);
    b.setAttribute('aria-pressed', String(slot === state.versionIndex));
    b.addEventListener('click', () => setVersion(slot));
    seg.append(b);
  });
  for (const block of blocks) fillHead(block);
  markVersion();
}

/* What a block of versions is, above the block itself.
 *
 * The instruction used to be one line at the FOOT of the whole switch, which put
 * "choose the good one" directly under the reference row — under the two
 * recordings that are the target and are not anybody's to choose between. A
 * sentence about candidates cannot sit under the references; each block says
 * what it is, above itself, or the page is telling the listener to pick the
 * wrong thing.
 *
 * Only a block holding more than one version gets a sentence: one version poses
 * no question, and the role name already says what it is.
 */
function fillHead({ role, path, head, seg }) {
  // A voice with a set of recorded candidates puts eleven buttons in one row,
  // and equal segments across eleven ellipsise every label into uselessness —
  // `foundati…` beside `mixtures…` names neither. Past the point where a name
  // survives, the block wraps instead of narrowing; the segments stay equal,
  // which is the part that matters.
  seg.classList.toggle('many', seg.childElementCount > CROWDED);
  if (role) {
    const lab = el('span', 'role');
    lab.append(el('span', 'dot'), el('span', '', t(`role.${role}`)));
    if (path) lab.append(el('span', 'vpath', t(`path.${path}`)));
    lab.title = t(path ? `path.${path}.long` : `role.${role}.long`);
    head.append(lab);
  }
  if (seg.childElementCount < 2) return;
  // One line, and it is the first thing given up when the row narrows, so it
  // also carries itself as a title rather than ending in an ellipsis nothing
  // can open.
  const hint = el('span', 'vhint', t(hintKey(role, path)));
  hint.title = hint.textContent;
  head.append(hint);
  if (state.blind) return;
  // Named for the block it stands in, and shown only in the block the sounding
  // version belongs to: "keep this one" records whatever is sounding, so in any
  // other block it would be a button pointing away from itself.
  const pick = el('button', 'ghost', t('fb.prefer'));
  pick.type = 'button';
  pick.dataset.block = `${role}|${path}`;
  pick.title = t('fb.preferTitle');
  pick.addEventListener('click', recordPreference);
  head.append(pick);
}

/* WHAT IS BEING ASKED FOR, and it is not the same question on every page.
 *
 * The page used to ask which version was liked, on pages that also declare a
 * reference as the target. Those are different questions with different
 * answers: a voice can move closer to the reference and be liked less, because
 * accuracy is often duller. A tally collected on preference and adopted moves
 * the voice away from the thing it is being fitted to.
 *
 * So where there is a target the criterion is distance from it, and where there
 * is none — fifty-six of the pages hold the model alone — it is whether the
 * thing could pass for the instrument at all. The second is not a weaker form
 * of the first; it is the only question left when nothing is there to be near.
 *
 * The direct path is an axis, so its block says what it is for rather than
 * asking anything: choosing between two paths is not a judgement about the
 * voice.
 */
function hintKey(role, path) {
  if (state.blind) return 'ver.hintBlind';
  if (path) return 'ver.hintDirect';
  if (role === 'reference') return 'ver.hintReference';
  if (!role) return 'ver.hint';
  return hasReference() ? 'ver.hintModel' : 'ver.hintModelAlone';
}

/// Whether this page has anything to be measured against at all.
const hasReference = () =>
  Boolean(state.take) && state.take.keys.some((k) => roleOf(k) === 'reference');

/// Buttons past which a label stops fitting in a shared row.
const CROWDED = 6;

/// Redraw the switch without touching the audio: blind mode going on or off, a
/// reveal and a reshuffle all change what the buttons say and nothing about
/// what is decoded.
export function rebuildVersions() {
  if (!state.take) return;
  buildVersionButtons();
  if (state.playing) applyGains(false);
  renderLevels();
  renderCaptions();
  writeRoute();
}

function versionTitle(slot) {
  if (state.blind) return '';
  const key = state.take.keys[slot];
  const src = sourceOf(key);
  return [key, sourceTitle(key), phrase(src.desc), src.label, src.detail]
    .filter(Boolean).join('\n');
}

/// The slots belonging to a role, in display order.
const slotsInRole = (role) =>
  state.display.filter((s) => roleOf(state.take.keys[s]) === role);

/* One key for the comparison the page exists for. Stepping with the digits
 * means knowing which digit is the reference on this take, and that number
 * moves with every voice; the two sides do not. Whichever version of a role was
 * last chosen is the one it comes back to, so flipping between a candidate
 * setting and the reference does not drop you on the baseline each time. */
export function swapRole() {
  if (!state.take || state.blind) return;
  const here = roleOf(activeKey());
  const other = here === 'reference' ? 'model' : 'reference';
  const slots = slotsInRole(other);
  if (!slots.length) return;
  const want = state.lastByRole[other];
  setVersion(slots.includes(want) ? want : slots[0]);
}

function markSwap() {
  const btn = $('swapBtn');
  const has = state.take && !state.blind && slotsInRole('reference').length > 0
    && slotsInRole('model').length > 0;
  btn.hidden = !has;
  if (!has) return;
  const here = roleOf(activeKey());
  btn.textContent = here === 'reference' ? t('now.swapBack') : t('now.swap');
  // Coloured for where it goes, not for where it is: the button is the other
  // side of the comparison, offered.
  btn.className = 'swap compare-only';
  btn.classList.add(roleClass(here === 'reference' ? 'model' : 'reference'));
}

function markVersion() {
  const box = $('versions');
  [...box.querySelectorAll('.segmented button')].forEach((b) =>
    b.setAttribute('aria-pressed', String(+b.dataset.slot === state.versionIndex)));
  // "Keep this one" belongs to the block whose version is sounding, so it moves
  // with the selection rather than standing under all of them at once.
  const here = state.take && !state.blind ? blockOf(activeKey()) : null;
  for (const b of box.querySelectorAll('.vhead button')) {
    b.hidden = here === null || b.dataset.block !== here;
  }
  renderNow();
  markSwap();
  renderIdent();
}

function selectVersionByKey(key) {
  if (!state.take || state.blind) return;
  const slot = state.take.keys.indexOf(key);
  if (slot >= 0) setVersion(slot);
}

export function setVersion(slot) {
  if (!state.take || slot < 0 || slot >= state.take.keys.length) return;
  state.versionIndex = slot;
  if (!state.blind) {
    state.wantKey = activeKey();
    const role = roleOf(activeKey());
    if (role) state.lastByRole[role] = slot;
  }
  markVersion();
  // Two ways to switch, and they answer different questions. Crossfading in
  // place keeps the comparison sample-aligned, which is the only way to hear a
  // difference in a sustain or a decay. Restarting throws away that alignment
  // and gives back the attack: switching four seconds into a phrase otherwise
  // means the new version's onset has already gone by, and waiting out the take
  // to hear it is long enough for the ear to lose what it was holding.
  if ($('restartOnSwitch').checked) {
    const from = (state.region || [0])[0];
    if (state.playing) startAt(from);
    else state.startOffset = from;  // the rAF loop redraws the playhead
  } else if (state.playing) {
    applyGains(false);
  }
  renderLevels();
  writeRoute();
}

/// Step through the versions in the order they are shown, not in manifest order.
export function stepVersion(delta) {
  const at = state.display.indexOf(state.versionIndex);
  const n = state.display.length;
  if (!n) return;
  setVersion(state.display[((at < 0 ? 0 : at) + delta + n) % n]);
}

/* ------------------------------------------------------------- readouts */

/* The banner. One line that answers "what am I listening to" in words rather
 * than in a key: the role, in the role's own colour, and the source's full
 * label and detail beside it. Everything else on the console is navigation. */
function renderNow() {
  const box = $('nowRole');
  box.replaceChildren();
  box.className = 'now-role';
  if (!state.take) return;
  if (state.blind) {
    box.classList.add(roleClass(''));
    box.append(el('span', 'dot'),
      el('span', 'now-what', String.fromCharCode(65 + state.display.indexOf(state.versionIndex))),
      el('span', 'now-detail', t('now.hidden')));
    return;
  }
  const key = activeKey();
  const role = roleOf(key);
  const src = sourceOf(key);
  box.classList.add(roleClass(role));
  box.append(el('span', 'dot'));
  box.append(el('span', 'now-what', role ? t(`role.${role}`) : t('role.other')));
  // The registered words first where there are any. What a candidate is for is
  // a sentence written for whoever is listening; the label and the override
  // string are the record, and they are what the button's tooltip carries.
  const title = sourceTitle(key);
  const desc = BASELINE_TITLES[key] ? t('ver.baselineDesc') : phrase(src.desc);
  if (title) box.append(el('span', 'now-title', title));
  const detail = [src.label, src.detail].filter((s) => s && s !== key).join('  ·  ');
  box.append(el('span', 'now-detail', desc || detail || sourceLabel(key)));
}

function renderIdent() {
  const crumbs = $('crumbs');
  crumbs.replaceChildren();
  const item = state.items[state.itemIndex];
  const parts = [state.setId, item && item.id];
  if (item && state.take) {
    parts.push(state.blind
      ? String.fromCharCode(65 + state.display.indexOf(state.versionIndex))
      : activeKey());
  }
  parts.filter(Boolean).forEach((p, i, all) => {
    const s = el('span', i === all.length - 1 ? 'now' : '', p);
    crumbs.append(s);
    if (i < all.length - 1) crumbs.append(el('span', 'sep', '›'));
  });
  // The path, so the file a report refers to is never inferred. Hidden in blind
  // mode, where it would name the version the letters are hiding.
  $('identPath').textContent = (item && state.take && !state.blind)
    ? state.base + item.tracks[activeKey()]
    : '';
}

export function renderCaptions() {
  $('waveCaption').textContent = state.compare
    ? t('wave.caption') : t('wave.captionSolo');
  $('specCaption').textContent = t('spec.caption');
  const legend = $('waveLegend');
  legend.replaceChildren();
  if (!state.take || state.blind || !state.compare) return;
  const seen = new Set();
  for (const slot of state.display) {
    const role = roleOf(state.take.keys[slot]) || 'other';
    if (seen.has(role)) continue;
    seen.add(role);
    const chip = el('span', 'wl');
    chip.classList.add(roleClass(role));
    chip.append(el('span', 'dot'), el('span', '', t(`role.${role}`)));
    legend.append(chip);
  }
}

/* CHOOSING IS NOT SWITCHING, and not choosing is a result.
 *
 * A blind run used to record a pick on every version change, so A/B-ing between
 * two versions WAS voting and the vote left standing was whichever one the
 * listener happened to stop on. A take nobody could separate still produced a
 * pick, and `heard.py` files a blind tally as what the ear actually separated —
 * so indifference was being counted as discrimination.
 *
 * The narrowing flow has had the answer all along and this one did not inherit
 * it: "not sure" is an answer. Two explicit acts now, and neither of them is
 * moving between versions.
 */
export function chooseBlind() {
  if (!state.blind || !state.take) return;
  const id = state.items[state.itemIndex].id;
  state.picks[id] = {
    slot: state.versionIndex,
    key: state.take.keys[state.blindOrder[state.versionIndex]],
    revealed: false,
  };
  writePicks();
}

export function abstainBlind() {
  if (!state.blind || !state.take) return;
  const id = state.items[state.itemIndex].id;
  // Recorded rather than left absent, because "could not tell them apart" and
  // "has not been listened to" are different results and the tally is read as
  // though every take in it was decided.
  state.picks[id] = { unseparated: true, revealed: false };
  writePicks();
}

function writePicks() {
  localStorage.setItem(picksKey(), JSON.stringify(state.picks));
  renderScore();
  rebuildVersions();
}

/// What was chosen, per take, what could not be separated, and how it adds up.
function blindTally() {
  const picks = {};
  const unsure = [];
  for (const [id, p] of Object.entries(state.picks)) {
    if (!p) continue;
    if (p.unseparated) unsure.push(id);
    else if (p.key) picks[id] = p.key;
  }
  const tally = {};
  for (const key of Object.values(picks)) tally[key] = (tally[key] || 0) + 1;
  const parts = Object.entries(tally)
    .sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${sourceLabel(k)} ${v}`);
  if (unsure.length) parts.push(`${t('blind.unsure')} ${unsure.length}`);
  return { picks, unsure, n: Object.keys(picks).length + unsure.length, parts };
}

/* The tally names its sources, so it is the answer blind mode is withholding:
 * seeing "the reference 4, the candidate 1" four takes in tells you which one
 * you have been preferring, and the rest of the run is no longer blind. Until
 * the run is concluded the line says only how many takes have been decided; it
 * opens up once the result has been recorded, which is the point at which there
 * is nothing left to bias. */
let blindRevealed = false;

export const resetBlindReveal = () => { blindRevealed = false; };

export function renderScore() {
  const btn = $('blindRecord');
  const choose = $('blindPick');
  const unsure = $('blindUnsure');
  if (!state.blind) {
    $('blindScore').textContent = '';
    for (const b of [btn, choose, unsure]) b.hidden = true;
    return;
  }
  choose.hidden = false;
  unsure.hidden = false;
  // The mark is on the act, not on the switch: it says whether THIS take has
  // been decided, which is the thing the old flow could not distinguish from
  // having been listened to.
  const here = state.picks[state.items[state.itemIndex].id];
  choose.setAttribute('aria-pressed', String(Boolean(here && here.key)));
  unsure.setAttribute('aria-pressed', String(Boolean(here && here.unseparated)));
  const { n, parts } = blindTally();
  btn.hidden = n === 0;
  if (!n) { $('blindScore').textContent = t('blind.pickEach'); return; }
  $('blindScore').textContent = blindRevealed
    ? `${t('blind.preferred')}: ${parts.join('   ')}`
    : t('blind.decided', { n });
}

/// A blind run is a result, so it goes into the same log as everything else
/// rather than into a download nobody remembers to make.
export async function recordBlindResult() {
  const { picks, unsure, n, parts } = blindTally();
  if (!n) return;
  await recordBlind(t('blind.result', { n, tally: parts.join(', ') }), picks, unsure);
  blindRevealed = true;
  renderScore();
}

/// Without it a page says what a voice sounds like and nothing about why it is
/// open: which rung it is on, whether it has a reference, what is unadopted.
/* Which slot of the map, and where the reference came from.
 *
 * The page used to name a program in a prose title and play a reference beside
 * it, and nothing said whether that reference is the KIND of thing the slot is
 * aimed at. Forty slots name a sound the machine invented, whose only possible
 * target is the machine, and every one of them is currently answered by a
 * sample library's idea of it — which sounds like a finished voice and is a
 * gap. `serve.py` resolves the two facts per request against the tracked
 * policy, so this line is current even on a page rendered months ago.
 */
export function renderSubject() {
  const box = $('subject');
  box.replaceChildren();
  const voice = (state.manifest || {}).voice;
  if (!voice) return;

  const prov = (state.manifest || {}).provenance;
  const slot = el('span', 'subj');
  slot.append(el('span', 'lab', t('subj.slot')));
  if (voice.kit) {
    slot.append(el('span', 'subj-key', t('subj.kit', { n: voice.program })),
      el('span', 'subj-more', t('subj.channel10')));
  } else {
    // Both numbers, because the printed map counts from one and every MIDI
    // file counts from zero — the reference timbres are labelled `[GM 041]`
    // and the manifest says program 40, and they are the same slot.
    slot.append(el('span', 'subj-key', t('subj.gm', { n: voice.program + 1 })),
      el('span', 'subj-more', t('subj.program', { n: voice.program })),
      el('span', 'subj-more', t('subj.bank', { n: voice.bank || 0 })));
  }
  slot.append(el('span', 'subj-more',
    voice.patch ? t('subj.patch', { name: voice.patch }) : t('subj.noPatch')));
  box.append(slot);

  /* A kit is one part holding forty-odd instruments, so on a kit the slot
   * above names the part and this names what is actually being struck. The
   * take decides it: a hi-hat take is three notes of the map and a fill is
   * six, and each of them is versioned on its own as `dNNN`. Without this the
   * line says "kit 0" on every page of the kit and identifies nothing. */
  const struck = voice.kit ? takeInstruments() : [];
  if (struck.length) {
    const box2 = el('span', 'subj');
    box2.append(el('span', 'lab', t('subj.struck')));
    for (const one of struck) {
      box2.append(el('span', one === struck[0] ? 'subj-key' : 'subj-more', one));
    }
    box.append(box2);
  }

  if (!prov || state.blind) return;
  const ref = el('span', 'subj subj-ref');
  ref.classList.add(roleClass('reference'));
  ref.append(el('span', 'dot'), el('span', 'lab', t('subj.reference')));
  const cap = prov.capture;
  if (cap) {
    ref.append(el('span', 'subj-key', t(`src.${cap.source_class || 'unclassified'}`)));
    // The product comes from the untracked overlay, so a clone without one
    // still gets the class above and simply does not get the name.
    if (cap.product) ref.append(el('span', 'subj-more', cap.product));
    ref.append(el('span', 'subj-more', roomWord(cap)));
  } else if (prov.declined) {
    const d = prov.declined;
    const line = d.names && d.carries
      ? t('subj.declined', { names: d.names, carries: d.carries })
      : t('subj.declinedBare');
    const span = el('span', 'subj-key dim', line);
    span.title = d.reason || '';
    ref.append(span);
  } else {
    ref.append(el('span', 'subj-key dim', t('subj.none')));
  }

  const want = prov.want || {};
  if (want.timbre) {
    const badge = el('span', 'badge',
      t('subj.target', { what: t(`want.${want.timbre}`) }));
    // The policy's own sentence, so the answer to "why that target" is one
    // hover away rather than in a file nobody opens mid-session.
    badge.title = want.reason || '';
    if (prov.state === 'off-target') {
      badge.classList.add('warn');
      badge.append(el('span', 'badge-why', t('prov.offTarget')));
    }
    ref.append(badge);
  }
  if (prov.state === 'unclassified') {
    ref.append(el('span', 'badge warn', t('prov.unclassified')));
  }
  box.append(ref);
}

/// The distinct drum notes this take strikes, in the order it strikes them,
/// named where the map has a name for them.
function takeInstruments() {
  const item = state.items[state.itemIndex];
  const notes = item && item.meta && item.meta.notes;
  if (!notes || !notes.length) return [];
  const names = ((state.manifest || {}).provenance || {}).drum_names || {};
  const seen = [];
  for (const n of [...notes].sort((a, b) => a.start - b.start)) {
    const key = String(n.note);
    if (!seen.includes(key)) seen.push(key);
  }
  return seen.map((key) => (names[key] ? `${names[key]} ${key}` : `note ${key}`));
}

function roomWord(cap) {
  // `room` is measured and `dry` only says whether there was an effect section
  // to switch off, so a capture that answered the first question is read on it
  // — 77 of them measure no room while declaring `dry` false, which the other
  // field alone would report as a reference carrying a building.
  if (cap.room === 'none') return t('room.none');
  if (cap.room) return t('room.present');
  return cap.dry ? t('room.dry') : t('room.undeclared');
}

export function renderHeadStage() {
  const box = $('headStage');
  box.replaceChildren();
  const v = state.bank.voices.find((x) => x.slug === state.setId);
  if (!v) return;
  box.append(dotEl(v.engine), el('span', '', v.engine || t('bank.notReported')),
    stageBar(v), el('span', '', `${v.stage.toFixed(1)} ${t(`stage.${Math.round(v.stage * 5)}`)}`));
  const open = v.open_candidates || [];
  if (open.length) {
    const badge = el('span', 'badge warn', t('bank.nUnwritten', { n: open.length }));
    badge.title = open.join(', ');
    box.append(badge);
  }
  box.title = v.next;
}

/* --------------------------------------------------------------- clipboard */

function conditionsText() {
  const c = conditions();
  const url = location.origin + location.pathname
    + `?t=${playhead().toFixed(2)}` + routeHash();
  const lines = [
    `set:      ${c.set}`,
    `take:     ${c.take || '-'}${c.take_label ? ` — ${c.take_label}` : ''}`,
    `version:  ${c.blind ? 'hidden (blind)' : c.version}`,
    `playhead: ${c.playhead.toFixed(2)} s of ${c.duration ? c.duration.toFixed(2) : '?'} s`,
  ];
  const hit = hitAt(playhead());
  if (hit) {
    const plays = hit.notes.map((n) => `note ${n.note} v${n.velocity}`).join(' + ');
    lines.push(`hit:      #${hit.n} — ${plays}, struck at ${hit.start.toFixed(2)} s`
      + ` (${(playhead() - hit.start).toFixed(2)} s in)`);
  }
  if (c.region) lines.push(`region:   ${c.region[0].toFixed(2)}–${c.region[1].toFixed(2)} s`);
  lines.push(`options:  gain-match ${c.match_loudness ? 'on' : 'off'},`
    + ` loop ${c.loop ? 'on' : 'off'}, blind ${c.blind ? 'on' : 'off'}`);
  lines.push(url);
  return lines.join('\n');
}

export async function copyConditions() {
  const text = conditionsText();
  const btn = $('copyLink');
  try {
    await navigator.clipboard.writeText(text);
    btn.textContent = t('now.copied');
  } catch {
    // A page served over plain http from another host has no clipboard, and a
    // block this size does not fit on a button. A prompt is selectable.
    window.prompt(t('now.copy'), text);
    btn.textContent = t('now.shown');
  }
  setTimeout(() => { btn.textContent = t('now.copy'); }, 1600);
}

/* Redrawn wholesale when the language changes: every readout above is built in
 * script, so none of them is reached by the static pass over the markup. */
export function refreshListen() {
  applyStatic();
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
  $('playBtn').textContent = state.playing ? t('transport.pause') : t('transport.play');
}
