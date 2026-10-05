/* The address of what is sounding.
 *
 * The fragment is `#<set>/<take>/<version>?c=<comparison>` and it is rewritten on every move,
 * so the page can be pointed at one exact render and the address bar always
 * names the one being heard. The breadcrumbs and the copied block are the same
 * address, read back to whoever is listening.
 */

'use strict';

import { $, el, state, comparisons, selectedComparison } from './state.js';
import { t } from './i18n.js';
import { activeKey, conditions, hitAt, playhead } from './player.js';

export function readRoute() {
  const q = new URLSearchParams(location.search);
  // The playhead rides in the query rather than in the fragment. It is a
  // starting position rather than live state, and writing it back the way the
  // fragment is written back would put a number in the address bar that changes
  // sixty times a second.
  const at = Number.parseFloat(q.get('t'));
  const tt = Number.isFinite(at) ? at : null;
  const [frag, fragQuery] = location.hash.replace(/^#\/?/, '').split('?');
  if (frag) {
    const [set, take, ver] = frag.split('/');
    return {
      set: decodeURIComponent(set || ''),
      take: decodeURIComponent(take || ''),
      ver: decodeURIComponent(ver || ''),
      cmp: new URLSearchParams(fragQuery || '').get('c') || '',
      t: tt,
    };
  }
  return {
    set: q.get('set') || '', take: q.get('take') || '', ver: q.get('v') || '',
    cmp: q.get('c') || '', t: tt,
  };
}

export function routeHash() {
  const parts = [state.setId];
  const item = state.items[state.itemIndex];
  if (item) parts.push(item.id);
  // Left out in blind mode along with the path and the label: an address bar is
  // visible, and hiding the name is the whole point of that mode.
  if (item && state.take && !state.blind) parts.push(activeKey());
  // The comparison decides which versions are on the switch, so a link names it.
  const c = comparisons().length ? selectedComparison() : null;
  return '#' + parts.filter(Boolean).map(encodeURIComponent).join('/')
    + (c ? `?c=${encodeURIComponent(c.id)}` : '');
}

/// Rewritten rather than pushed: every arrow key is a move, and a hundred of
/// them in the back stack makes the browser's own back button useless.
export function writeRoute() {
  const hash = routeHash();
  if (location.hash !== hash) history.replaceState(null, '', hash);
  renderIdent();
}

export function renderIdent() {
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
