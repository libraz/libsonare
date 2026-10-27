/* What a voice IS, in the three facts a listener asks before anything else:
 * where it sits in the GM/GS map, how it is synthesised, and what it is aimed
 * at. Plus the fourth fact the page itself owes: whether the render being
 * played is the voice that ships today.
 *
 * Shared by the listening surface's identity strip, the bank's rows and the
 * bank's inspector, so the three cannot word one fact three ways. Every fact
 * here is resolved by `serve.py` per request from the tracked policy and the
 * bank registry; nothing is inferred on the page.
 */

'use strict';

import { el, state } from './state.js';
import { t } from './i18n.js';

/* Sixteen engines, three methods. What the eye is being asked here is "physical
 * model, FM, or waveform" — sixteen hues would answer nothing, and the engine's
 * own name is beside it. */
const METHODS = { fm: 'fm', subtractive: 'classic', additive: 'classic' };

export const methodOf = (engine) => (engine ? (METHODS[engine] || 'physical') : 'classic');

/// GS variation banks are Bank Select MSB numbers; the GM2 addresses of the
/// same patches are LSB 1-3. A kit is told by its flag, never by a number.
export function slotKind(v) {
  if (!v) return '';
  if (v.kit) return 'kit';
  if (!v.bank) return 'gm';
  return v.bank < 8 ? 'gm2' : 'gs';
}

/// The other address of the same patch: a GM2 LSB row and its GS MSB row
/// point at one patch, so a verdict on either is a verdict on both.
export function aliasOf(v) {
  if (!v || v.kit || !v.bank || !v.patch) return null;
  return state.bank.voices.find((o) => o !== v && !o.kit && o.program === v.program
    && o.bank && o.patch === v.patch && slotKind(o) !== slotKind(v)) || null;
}

export const addressOf = (v) =>
  (v.kit ? `kit ${v.program}` : `${v.program}${v.bank ? `:${v.bank}` : ''}`);

/// A small tag naming which part of the map a slot belongs to.
export function slotTag(v, { short = false } = {}) {
  const kind = slotKind(v);
  const tag = el('span', 'idtag', kind ? t(short ? `slot.short.${kind}` : `slot.${kind}`) : '—');
  tag.dataset.slot = kind || 'none';
  tag.title = slotLong(v);
  return tag;
}

export function slotLong(v) {
  const kind = slotKind(v);
  if (!kind) return '';
  const alias = aliasOf(v);
  return t(`slot.${kind}.long`, {
    gm: v.program + 1,
    bank: v.bank || 0,
    alias: alias ? addressOf(alias) : '—',
  });
}

/// The synthesis method in words, the engine as the identifier beside it.
export function methodTag(engine, { short = false } = {}) {
  const m = methodOf(engine);
  const tag = el('span', 'idtag', t(short ? `method.short.${m}` : `method.${m}`));
  tag.dataset.method = m;
  tag.title = t(`method.${m}.long`, { engine: engine || t('bank.notReported') });
  return tag;
}

/* ---- what the slot is aimed at ---- */

/// The policy branch in words. A variation, a kit and a machine-invented sound
/// all take the module, but for three different reasons, and the reason is the
/// part a listener needs to know whether the target can ever change.
export const targetWords = (target) =>
  (target && target.branch ? t(`tgt.${target.branch}`) : '');

export const targetShort = (target) =>
  (target && target.timbre ? t(`tgt.short.${target.timbre}`) : '');

/// How the captured reference stands against that aim: one state, one word.
export function targetState(target) {
  if (!target || !target.state) return null;
  const s = target.state;
  const chip = el('span', 'idstate', t(`tst.${s}`));
  chip.dataset.state = s;
  chip.title = t(`tst.${s}.long`);
  return chip;
}

/* ---- whether the render is the voice that ships ---- */

/// The set index entry for a voice's page, or null where none is rendered.
export const setOf = (slug) => state.sets.find((s) => s.id === slug) || null;

export const shipsOf = (slug) => ((setOf(slug) || {}).ships) || null;

/// One chip saying whether the page plays today's voice.
export function shipChip(ships, { short = false } = {}) {
  const st = ships && ships.state ? ships.state : 'none';
  const chip = el('span', 'idstate', t(short ? `ship.short.${st}` : `ship.${st}`));
  chip.dataset.state = st;
  chip.title = shipReasons(ships);
  return chip;
}

/// The reasons and every bump since the render, one per line, for a tooltip.
export function shipReasons(ships) {
  if (!ships) return t('ship.none.long');
  const lines = [...(ships.reasons || [])];
  for (const b of ships.since || []) {
    lines.push(`${b.unit} v${b.version}  ${b.date}  ${b.note}`);
  }
  return lines.join('\n') || t(`ship.${ships.state}.long`);
}

/// The command that renders this voice again, so a stale page names its fix.
export function rerenderCommand(v) {
  if (!v) return '';
  const pick = v.kit ? `--kits ${v.program}` : `--program ${v.program} --banks ${v.bank || 0}`;
  return `tools/voicematch/make_audition.py ${pick}`;
}
