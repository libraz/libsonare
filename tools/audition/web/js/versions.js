/* The version switch, and the words and colours that say which side of the
 * comparison is sounding.
 *
 * A role owns a colour and a sentence: the banner names it, the block is
 * labelled with it, the button carries its dot, and the two pictures are drawn
 * in it. One key swaps between the two sides, which is the comparison the page
 * exists for and previously required knowing which digit was which.
 */

'use strict';

import {
  $, el, state, ROLE_ORDER, roleClass, roleOf, pathOf, blockOf, sourceOf, sourceLabel,
} from './state.js';
import { t, phrase } from './i18n.js';
import { activeKey, applyGains, renderLevels, span, startAt } from './player.js';
import { recordPreference } from './feedback.js';
import { renderIdent, writeRoute } from './address.js';

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

export function buildVersionButtons() {
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
    b.addEventListener('click', () => setVersion(slot, { play: true }));
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
  seg.parentElement.style.setProperty('--n', String(seg.childElementCount));
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
  setVersion(slots.includes(want) ? want : slots[0], { play: true });
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

export function selectVersionByKey(key) {
  if (!state.take || state.blind) return;
  const slot = state.take.keys.indexOf(key);
  if (slot >= 0) setVersion(slot);
}

/* Choose a version — and, unless the caller is restoring an address, hear it.
 *
 * `play` is what makes the switch a listening act rather than a selection.
 * Nothing on this page is worth selecting for its own sake: the page exists to
 * compare two sounds, and with the transport stopped every one of these
 * controls changed a label and produced silence, so the shortest route from
 * opening the page to hearing the two sides was three presses with a hunt for
 * the play button in the middle. Restoring an address is the one case that must
 * stay silent — a link opens a page, it does not start a sound in a tab
 * somebody has not looked at yet.
 */
export function setVersion(slot, { play = false } = {}) {
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
    const from = span()[0];
    if (state.playing || play) startAt(from);
    else state.startOffset = from;  // the rAF loop redraws the playhead
  } else if (state.playing) {
    applyGains(false);
  } else if (play) {
    startAt(state.startOffset);
  }
  renderLevels();
  writeRoute();
}

/// Step through the versions in the order they are shown, not in manifest order.
export function stepVersion(delta) {
  const at = state.display.indexOf(state.versionIndex);
  const n = state.display.length;
  if (!n) return;
  setVersion(state.display[((at < 0 ? 0 : at) + delta + n) % n], { play: true });
}

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
