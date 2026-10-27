/* What the page is about, as opposed to what is sounding: the voice and its
 * place on the map, what it is aimed at, whether it is what ships, and how far
 * the voice has got.
 */

'use strict';

import { $, el, state, roleClass, selectedComparison } from './state.js';
import { t } from './i18n.js';
import { dotEl, stageBar } from './bank.js';
import {
  methodTag, rerenderCommand, shipChip, shipReasons, shipsOf, slotLong, slotTag, targetState,
  targetWords,
} from './identity.js';

/* Which voice this page is of, what it is aimed at, and whether what plays is
 * what ships — three cells, because they are three questions and a listener
 * asks all of them before trusting anything heard.
 *
 * The voice cell places the slot on the GM/GS map and says how it is made.
 * The target cell is the policy's answer for this slot and the capture that
 * answers it; its product half is withheld in blind mode, where naming the
 * product would name the version. The ship cell compares the page's
 * unmodified render against the bank registry: a page rendered before the
 * voice last moved plays an older voice than the one that ships, and nothing
 * else on the page can say so. `serve.py` resolves all three per request, so
 * a page rendered months ago still answers against today's policy and bank.
 */
export function renderSubject() {
  const box = $('subject');
  box.replaceChildren();
  const voice = (state.manifest || {}).voice;
  if (!voice) return;
  const prov = (state.manifest || {}).provenance || null;
  const row = state.bank.voices.find((x) => x.slug === state.setId) || null;
  box.append(voiceCell(voice, row), targetCell(voice, prov, row), shipCell(voice, row));
}

function cell(kind, label) {
  const c = el('section', `idcell ${kind}`);
  c.append(el('h3', 'idhead', label));
  return c;
}

function voiceCell(voice, row) {
  const c = cell('id-voice', t('id.voice'));
  const name = el('p', 'idname', voice.name || state.setId || '');
  // Both numbers, because the printed map counts from one and every MIDI
  // file counts from zero — the reference timbres are labelled `[GM 041]`
  // and the manifest says program 40, and they are the same slot.
  const addr = voice.kit
    ? [t('subj.kit', { n: voice.program }), t('subj.channel10')]
    : [t('subj.gm', { n: voice.program + 1 }), t('subj.program', { n: voice.program }),
      t('subj.bank', { n: voice.bank || 0 })];
  name.append(el('span', 'idaddr', addr.join(' · ')));
  c.append(name);

  const tags = el('p', 'idtags');
  const v = row || voice;
  tags.append(slotTag(v), el('span', 'idlong', slotLong(v)));
  c.append(tags);

  const how = el('p', 'idtags');
  const engine = row ? row.engine : '';
  how.append(methodTag(engine), el('span', 'idmono',
    [engine || t('bank.notReported'),
      voice.patch ? t('subj.patch', { name: voice.patch }) : t('subj.noPatch')].join(' · ')));
  c.append(how);

  /* A kit is one part holding forty-odd instruments, so on a kit the slot
   * above names the part and this names what is actually being struck. The
   * take decides it: a hi-hat take is three notes of the map and a fill is
   * six, and each of them is versioned on its own as `dNNN`. */
  const struck = voice.kit ? takeInstruments() : [];
  if (struck.length) {
    const line = el('p', 'idtags');
    line.append(el('span', 'lab', t('subj.struck')), el('span', 'idmono', struck.join(' · ')));
    c.append(line);
  }
  return c;
}

function targetCell(voice, prov, row) {
  const c = cell('id-target', t('id.target'));
  c.classList.add(roleClass('reference'));
  const target = (row && row.target) || (prov && prov.want ? { ...prov.want, state: prov.state } : null);
  if (!target || !target.branch) {
    c.append(el('p', 'idline dim', t('id.noPolicy')));
    return c;
  }
  const aim = el('p', 'idbig', targetWords(target));
  // The policy's own sentence, so "why that target" is one hover away.
  aim.title = (prov && prov.want && prov.want.reason) || '';
  c.append(aim);

  const line = el('p', 'idtags');
  // Whether the capture is on target is a fact about the reference side, so a
  // blind run withholds it with the product.
  if (state.blind) {
    line.append(el('span', 'idlong', t('id.blindHidden')));
    c.append(line);
    return c;
  }
  const st = targetState({ ...target, state: (prov && prov.state) || target.state });
  if (st) line.append(st);
  // The voice's capture answers only the comparisons that list it; a comparison
  // with no oracle says so rather than naming the other one's reference.
  const comparison = selectedComparison();
  const cap = prov && (comparison && !(comparison.oracle_sources || []).length ? null : prov.capture);
  if (cap) {
    line.append(el('span', 'idlong', [t(`src.${cap.source_class || 'unclassified'}`),
      cap.product, roomWord(cap)].filter(Boolean).join(' · ')));
  } else if (comparison && prov && prov.capture) {
    line.append(el('span', 'idlong', t('compare.noReference')));
  } else if (prov && prov.declined) {
    const d = prov.declined;
    const span = el('span', 'idlong', d.names && d.carries
      ? t('subj.declined', { names: d.names, carries: d.carries }) : t('subj.declinedBare'));
    span.title = d.reason || '';
    line.append(span);
  }
  c.append(line);

  const approx = (prov && prov.approximated) || (row && row.target && row.target.approximated);
  if (approx) {
    const p = el('p', 'idline warn', t('tgt.approximated', { slot: approx.answered_by }));
    p.title = approx.reason || '';
    c.append(p);
  }
  const answers = (prov && prov.answers) || (row && row.target && row.target.answers);
  if (answers && answers.length) {
    c.append(el('p', 'idline dim', t('tgt.answers', { slots: answers.join(', ') })));
  }
  return c;
}

function shipCell(voice, row) {
  const c = cell('id-ships', t('id.ships'));
  const ships = shipsOf(state.setId);
  const big = el('p', 'idbig');
  big.append(shipChip(ships));
  c.append(big);
  const now = (row && row.ships) || null;
  if (now) {
    c.append(el('p', 'idline',
      t('ship.shipping', { unit: now.unit, v: now.version, date: now.date })));
  } else if (ships && ships.units && ships.units.length > 1) {
    c.append(el('p', 'idline', t('ship.kitUnits', { n: ships.units.length })));
  }
  if (ships && ships.rendered) {
    const then = ships.then || {};
    const unit = Object.keys(then)[0];
    const p = el('p', 'idline dim', unit && Object.keys(then).length === 1
      ? t('ship.renderedThen', { unit, v: then[unit], date: ships.rendered })
      : t('ship.rendered', { date: ships.rendered }));
    if ((ships.since || []).length) {
      const since = el('span', 'idsince', t('ship.since', { n: ships.since.length }));
      since.title = shipReasons(ships);
      p.append(since);
    }
    c.append(p);
  }
  if (ships && ships.state === 'stale') {
    const cmd = el('code', 'idcmd', rerenderCommand(voice));
    cmd.title = t('ship.rerenderTitle');
    c.append(cmd);
  }
  return c;
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
