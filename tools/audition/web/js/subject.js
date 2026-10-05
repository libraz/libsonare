/* What the page is about, as opposed to what is sounding: the slot of the map,
 * where its reference came from, and how far the voice has got.
 */

'use strict';

import { $, el, state, roleClass, selectedComparison } from './state.js';
import { t } from './i18n.js';
import { dotEl, stageBar } from './bank.js';

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
  // The voice's capture answers only the comparisons that list it; a comparison
  // with no oracle says so rather than naming the other one's reference.
  const comparison = selectedComparison();
  const cap = comparison && !(comparison.oracle_sources || []).length ? null : prov.capture;
  if (comparison && !cap) {
    ref.append(el('span', 'subj-key dim', t('compare.noReference')));
  } else if (cap) {
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

/// Without it a page says what a voice sounds like and nothing about why it is
/// open: which rung it is on, whether it has a reference, what is unadopted.
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
