/* The signal path the sounding render actually took, read from its evidence.
 *
 * A model render made by a tuning build carries the library's own path record:
 * topology events (each part's rig stages, its insertion unit, where its sends
 * are taken) and the EFX parameter bytes an inline SysEx moved. The line shows
 * the topology in force at the playhead — the first one while stopped — so a
 * bypassed stage reads as bypassed rather than as part of the chain. A pre-rig
 * send is drawn as a branch off the voice, never as a stage in series.
 *
 * Blind mode keeps the comparison and whether it is matched, and hides every
 * name that would say which version is sounding.
 */

'use strict';

import { $, el, state, roleOf, selectedComparison } from './state.js';
import { t, currentLang } from './i18n.js';
import { activeKey, playhead } from './player.js';

/// The evidence a version carries in this take, or null on a page without any.
const evidenceOf = (key) => {
  const item = state.items[state.itemIndex];
  return (item && item.evidence && item.evidence[key]) || null;
};

/// The topology events of a record, in frame order.
export const topologies = (record) =>
  ((record && record.events) || []).filter((e) => e && e.kind === 'topology');

/// The topology in force at `frame`: the last one starting at or before it,
/// else the first. Returns its index among the record's topologies.
export function topologyIndexAt(events, frame) {
  let at = 0;
  events.forEach((e, i) => { if (e.frame <= frame) at = i; });
  return at;
}

const stageText = (name, on) => (on === false ? t('path.bypassed', { stage: name }) : name);

/// One part's chain as text, plus the branch line when its sends leave before the rig.
export function partChain(part, units) {
  const steps = [t('path.voice')];
  for (const s of part.stages || []) steps.push(s);
  const unit = part.unit == null ? null : units.find((u) => u.unit === part.unit);
  if (unit) {
    const stages = (unit.stages || []).map((s, i) => stageText(s, (unit.enabled || [])[i]));
    steps.push(t('path.unit', {
      type: unit.type,
      realization: t(`path.realization.${unit.realization}`),
    }) + (stages.length ? ` [${stages.join(', ')}]` : ''));
  }
  const tap = part.send_tap || 'none';
  if (tap === 'post_unit') steps.push(t('path.sends.post_unit'));
  const chain = steps.join(' → ');
  return {
    chain: tap === 'none' ? `${chain}  ·  ${t('path.sends.none')}` : chain,
    branch: tap === 'pre_rig' ? `${t('path.voice')} ↳ ${t('path.sends.pre_rig')}` : null,
  };
}

/// The raw record behind the line, for the tooltip: the topology in force and
/// every parameter byte moved inside its interval.
function rawText(record, events, index, rate) {
  const from = events[index].frame;
  const to = index + 1 < events.length ? events[index + 1].frame : Infinity;
  const params = (record.events || []).filter((e) => e.kind === 'param'
    && e.frame >= from && e.frame < to)
    .map((e) => `@${(e.frame / rate).toFixed(2)}s unit ${e.unit} slot ${e.slot} = ${e.value}`);
  return [JSON.stringify(events[index]), ...params].join('\n');
}

let drawn = '';

/// Redrawn from the frame loop; cheap when nothing it depends on moved.
export function renderPath() {
  const box = $('pathLine');
  if (!state.take) {
    if (drawn) { box.replaceChildren(); drawn = ''; }
    return;
  }
  const key = activeKey();
  const ev = evidenceOf(key);
  const record = ev && ev.path;
  const events = topologies(record);
  const rate = state.take.rates[key] || 48000;
  const index = events.length ? topologyIndexAt(events, Math.round(playhead() * rate)) : -1;
  const c = selectedComparison();
  const sig = [state.take.epoch, key, index, state.blind, currentLang(), c && c.id].join('|');
  if (sig === drawn) return;
  drawn = sig;
  box.replaceChildren();
  box.title = '';

  if (state.blind) {
    const what = c ? `${t(`compare.${c.id}`)} · ${t(`compare.status.${c.status}`)}` : '';
    box.append(el('span', 'path-head', t('path.label')),
      el('span', 'path-chain', [what, t('path.hidden')].filter(Boolean).join('  ·  ')));
    return;
  }
  box.append(el('span', 'path-head', t('path.label')));
  if (roleOf(key) && roleOf(key) !== 'model') {
    box.append(el('span', 'path-chain', t('path.reference')));
    return;
  }
  if (!ev) {
    box.append(el('span', 'path-warn', t('path.notRecorded', { reason: t('path.legacy') })));
    return;
  }
  if (ev.status !== 'recorded' || !events.length) {
    box.append(el('span', 'path-warn',
      t('path.notRecorded', {
        reason: ev.reason || (ev.status === 'recorded' ? 'no topology event' : ev.status) || '?',
      })));
    return;
  }
  const topo = events[index];
  const parts = topo.parts || [];
  const many = parts.length > 1;
  for (const part of parts) {
    const { chain, branch } = partChain(part, topo.units || []);
    const prefix = many ? `${t('path.part', { n: part.part + 1 })}: ` : '';
    box.append(el('span', 'path-chain', prefix + chain));
    if (branch) box.append(el('span', 'path-branch', branch));
  }
  if (events.length > 1) {
    const at = events.slice(1).map((e) => `${(e.frame / rate).toFixed(2)} s`).join(', ');
    box.append(el('span', 'path-change',
      t('path.changes', { at, i: index + 1, n: events.length })));
  }
  if (ev.complete === false) {
    box.append(el('span', 'path-warn',
      t('path.incomplete', { reason: (record && record.reason) || ev.reason || '?' })));
  }
  box.title = rawText(record, events, index, rate);
}
