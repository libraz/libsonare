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

'use strict';

import { $, el, state } from './state.js';
import { t } from './i18n.js';
import { dotEl, heardChip, signoffChip, stageBar } from './bank.js';

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
  row.addEventListener('click', () => { closePalette(); onPick(s.id); });

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

export function renderPickLabel() {
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

/// What a row does when chosen, handed in by the page so the palette does not
/// reach back into the listening surface that loads a set.
let onPick = () => {};

export function wirePalette(pick) {
  onPick = pick;
  $('voicePick').addEventListener('click', () => {
    if (paletteOpen()) closePalette(); else openPalette();
  });
  $('voiceFind').addEventListener('input', buildSetPicker);
  // Click-away rather than a scrim: the palette is a control on the header and
  // the page behind it stays live, so a click meant for the transport should
  // reach the transport.
  document.addEventListener('pointerdown', (ev) => {
    if (!paletteOpen()) return;
    if (!$('voicePanel').contains(ev.target) && !$('voicePick').contains(ev.target)) {
      closePalette();
    }
  });
}
