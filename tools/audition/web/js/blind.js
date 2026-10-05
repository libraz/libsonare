/* Blind mode: the versions under letters, in an order that says nothing, and a
 * tally that stays withheld until the run is recorded.
 */

'use strict';

import {
  $, state, scopeOf, picksKey, sourceLabel, selectedComparison, comparisonKeys,
} from './state.js';
import { t } from './i18n.js';
import { recordBlind } from './feedback.js';
import { rebuildVersions } from './versions.js';

/* A blind run is a ranking, so it runs only on a comparison whose two sides
 * stand at the same boundary — `matched` in the manifest. Anything else would
 * have the ear separating an amplifier or a room rather than judging the voice.
 *
 * A page written before comparisons existed falls back to the rule it had: a
 * direct-input reference (`rig` `none`) set against the shipped, amplified
 * path is exactly that mismatch, so its blind mode stays off. */
export const blindBlocked = () => {
  const c = selectedComparison();
  if (c) return c.status !== 'matched';
  const voice = (state.manifest || {}).voice;
  return Boolean(voice) && voice.rig === 'none';
};

const blockedReason = () => {
  const c = selectedComparison();
  return c ? t('blind.unmatched', { status: t(`compare.status.${c.status}`) }) : t('blind.refDi');
};

/* Brings the control and the run in line with the set just loaded. The reason
 * is shown in the readout beside the transport, which is where blind mode
 * reports itself, and as the checkbox's tooltip. */
export function applyBlindGate() {
  const box = $('blind');
  const blocked = blindBlocked();
  box.disabled = blocked;
  box.parentElement.title = blocked ? blockedReason() : '';
  if (blocked) {
    state.blind = false;
    box.checked = false;
    document.body.classList.remove('blind-on');
  }
}

// The B shortcut flips the box and fires `change` without looking at whether the
// box is disabled, so a blocked set is refused here, ahead of the handler that
// would start the run.
$('blind').addEventListener('change', (ev) => {
  if (!blindBlocked()) return;
  $('blind').checked = false;
  ev.stopImmediatePropagation();
  renderScore();
}, true);

/* Another comparison selected: its own gate, its own draw, its own picks —
 * `versions.js` has already loaded them and redraws the switch after this. */
document.addEventListener('audition:comparison', () => {
  applyBlindGate();
  reshuffleBlind();
  renderScore();
});

/* The draw for a blind run: which versions are in it, in an order that says
 * nothing. The selected comparison's two sides and nothing else, so a product
 * run never holds an instrument render; a page without comparisons draws the
 * shipped path only. A slot in blind mode is a position in this list rather
 * than an index into the take. */
export function reshuffleBlind() {
  const c = selectedComparison();
  const drawn = c ? comparisonKeys(c) : null;
  const order = state.take
    ? state.take.keys.map((_, i) => i).filter((i) => {
      const key = state.take.keys[i];
      return drawn ? drawn.has(key) : scopeOf(key) !== 'instrument';
    })
    : [];
  for (let i = order.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [order[i], order[j]] = [order[j], order[i]];
  }
  state.blindOrder = order;
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
    $('blindScore').textContent = blindBlocked() ? blockedReason() : '';
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
