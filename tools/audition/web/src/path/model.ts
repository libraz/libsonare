/* The signal path the sounding render took, as pure functions over the manifest's evidence.
 *
 * A model render carries topology events (each part's rig stages, its insertion unit, where its
 * sends are taken) and the EFX bytes an inline SysEx moved. The line shows the topology in force
 * at the playhead, so a bypassed stage reads as bypassed. A pre-rig send is a branch off the
 * voice, never a stage in series. Blind mode keeps the comparison and hides every name.
 */

import { isKey, t } from '../i18n';
import { roleOf, selectedComparison } from '../state/selectors';
import type { AppState } from '../state/store';
import {
  Evidence,
  type ParamEvent,
  type Part,
  type PathRecord,
  TopologyEvent,
  type Unit,
} from './schema';

/** Sample rate assumed for a version the take records none for. */
export const DEFAULT_RATE = 48000;

export type PathClass = 'path-head' | 'path-chain' | 'path-branch' | 'path-change' | 'path-warn';

export interface PathSpan {
  cls: PathClass;
  text: string;
}

/** What the line shows: its spans in order, and the raw record for the tooltip. */
export interface PathView {
  spans: PathSpan[];
  title: string;
}

export const EMPTY_VIEW: PathView = { spans: [], title: '' };

/** The topology events of a record, in the order written. */
export const topologies = (record: PathRecord | null | undefined): TopologyEvent[] =>
  (record?.events ?? []).filter((e): e is TopologyEvent => TopologyEvent.safeParse(e).success);

/** The index of the topology in force at `frame`: the last one starting at or before it, else the first. */
export function topologyIndexAt(events: { frame: number }[], frame: number): number {
  let at = 0;
  events.forEach((e, i) => {
    if (e.frame <= frame) {
      at = i;
    }
  });
  return at;
}

/** A processor's display name: a translated one where there is one, else its last segment. */
function stageName(name: string): string {
  const key = `path.stage.${name}`;
  return isKey(key) ? t(key) : String(name).split('.').pop() || name;
}

function stageText(name: string, on: boolean | undefined): string {
  const shown = stageName(name);
  return on === false ? t('path.bypassed', { stage: shown }) : shown;
}

/** True when the score wrote every send at zero, so none leaves the part at all. */
const sendsZero = (sends: number[] | null | undefined): boolean =>
  Array.isArray(sends) && sends.length > 0 && sends.every((v) => v === 0);

function realizationName(r: string): string {
  const key = `path.realization.${r}`;
  return isKey(key) ? t(key) : r;
}

/** One part's chain as text, plus the branch line when its sends leave before the rig. */
export function partChain(
  part: Part,
  units: Unit[],
  sends?: number[] | null,
): { chain: string; branch: string | null } {
  const steps = [t('path.voice')];
  part.stages.forEach((s, i) => {
    steps.push(stageText(s, part.enabled[i]));
  });
  const unit = part.unit == null ? null : units.find((u) => u.unit === part.unit);
  if (unit) {
    const stages = unit.stages.map((s, i) => stageText(s, unit.enabled[i]));
    const head = t('path.unit', {
      type: unit.type,
      realization: realizationName(unit.realization),
    });
    steps.push(head + (stages.length ? ` [${stages.join(', ')}]` : ''));
  }
  const tap = sendsZero(sends) ? 'zero' : part.send_tap || 'none';
  if (tap === 'zero') {
    return { chain: `${steps.join(' → ')}  ·  ${t('path.sends.zero')}`, branch: null };
  }
  if (tap === 'post_unit') {
    steps.push(t('path.sends.post_unit'));
  }
  const chain = steps.join(' → ');
  return {
    chain: tap === 'none' ? `${chain}  ·  ${t('path.sends.none')}` : chain,
    branch: tap === 'pre_rig' ? `${t('path.voice')} ↳ ${t('path.sends.pre_rig')}` : null,
  };
}

/** The raw record for the tooltip: the topology in force and every parameter byte moved inside it. */
function rawText(record: PathRecord, events: TopologyEvent[], index: number, rate: number): string {
  const from = events[index]?.frame ?? 0;
  const next = events[index + 1];
  const to = next ? next.frame : Number.POSITIVE_INFINITY;
  const params = record.events
    .filter((e): e is ParamEvent => e.kind === 'param' && e.frame >= from && e.frame < to)
    .map((e) =>
      t('path.raw.param', {
        at: (e.frame / rate).toFixed(2),
        unit: e.unit,
        slot: e.slot,
        value: e.value,
      }),
    );
  return [JSON.stringify(events[index]), ...params].join('\n');
}

type Found = { kind: 'none' } | { kind: 'unreadable' } | { kind: 'found'; evidence: Evidence };

/** The evidence a version carries in the current take. */
export function evidenceOf(s: AppState, key: string): Found {
  const bag = s.items[s.itemIndex]?.evidence;
  if (!bag || typeof bag !== 'object') {
    return { kind: 'none' };
  }
  const raw = (bag as Record<string, unknown>)[key];
  if (raw == null) {
    return { kind: 'none' };
  }
  const parsed = Evidence.safeParse(raw);
  return parsed.success ? { kind: 'found', evidence: parsed.data } : { kind: 'unreadable' };
}

/** Which topology the line shows at `playhead` seconds; -1 where there is none. */
export function pathIndex(s: AppState, key: string, playhead: number): number {
  const found = evidenceOf(s, key);
  if (found.kind !== 'found') {
    return -1;
  }
  const events = topologies(found.evidence.path);
  const rate = s.take?.rates[key] || DEFAULT_RATE;
  return events.length ? topologyIndexAt(events, Math.round(playhead * rate)) : -1;
}

const head = (): PathSpan => ({ cls: 'path-head', text: t('path.label') });
const warn = (reason: string): PathSpan => ({
  cls: 'path-warn',
  text: t('path.notRecorded', { reason }),
});

function named(prefix: 'compare' | 'compare.status', id: string): string {
  const key = `${prefix}.${id}`;
  return isKey(key) ? t(key) : id;
}

/** The line for the version sounding at `playhead` seconds. */
export function buildPathView(s: AppState, key: string, playhead: number): PathView {
  if (!s.take) {
    return EMPTY_VIEW;
  }
  if (s.blind) {
    const c = selectedComparison(s);
    const what = c ? `${named('compare', c.id)} · ${named('compare.status', c.status)}` : '';
    const text = [what, t('path.hidden')].filter(Boolean).join('  ·  ');
    return { spans: [head(), { cls: 'path-chain', text }], title: '' };
  }
  const role = roleOf(s, key);
  if (role && role !== 'model') {
    return { spans: [head(), { cls: 'path-chain', text: t('path.reference') }], title: '' };
  }
  const found = evidenceOf(s, key);
  if (found.kind === 'none') {
    return { spans: [head(), warn(t('path.legacy'))], title: '' };
  }
  if (found.kind === 'unreadable') {
    return { spans: [head(), warn(t('path.unreadable'))], title: '' };
  }
  const ev = found.evidence;
  const record = ev.path ?? null;
  const events = topologies(record);
  if (ev.status !== 'recorded' || !events.length) {
    const reason =
      ev.reason || (ev.status === 'recorded' ? t('path.noTopology') : ev.status) || '?';
    return { spans: [head(), warn(reason)], title: '' };
  }
  const rate = s.take.rates[key] || DEFAULT_RATE;
  const index = topologyIndexAt(events, Math.round(playhead * rate));
  const topo = events[index];
  if (!topo || !record) {
    return { spans: [head()], title: '' };
  }
  // Only the part the score plays; a record without that says nothing to filter by.
  const parts = topo.parts.filter((p) => ev.channel == null || p.part === ev.channel);
  const many = parts.length > 1;
  const spans: PathSpan[] = [head()];
  for (const part of parts) {
    const { chain, branch } = partChain(part, topo.units, ev.sends);
    const prefix = many ? `${t('path.part', { n: part.part + 1 })}: ` : '';
    spans.push({ cls: 'path-chain', text: prefix + chain });
    if (branch) {
      spans.push({ cls: 'path-branch', text: branch });
    }
  }
  if (events.length > 1) {
    const at = events
      .slice(1)
      .map((e) => t('path.seconds', { n: (e.frame / rate).toFixed(2) }))
      .join(', ');
    spans.push({
      cls: 'path-change',
      text: t('path.changes', { at, i: index + 1, n: events.length }),
    });
  }
  if (ev.complete === false) {
    spans.push({
      cls: 'path-warn',
      text: t('path.incomplete', { reason: record.reason || ev.reason || '?' }),
    });
  }
  return { spans, title: rawText(record, events, index, rate) };
}
