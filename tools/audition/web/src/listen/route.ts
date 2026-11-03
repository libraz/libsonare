/* The address of what is sounding.
 *
 * The fragment is `#<set>/<take>/<version>?c=<comparison>` and is rewritten on
 * every move, so the address bar always names the render being heard. The
 * playhead rides in the query instead: it is a starting position rather than
 * live state, and writing it back would change the address sixty times a second.
 */

import { activeKey, comparisons, currentItem, selectedComparison } from '../state/selectors';
import type { AppState } from '../state/store';

export interface Route {
  set: string;
  take: string;
  ver: string;
  cmp: string;
  t: number | null;
}

export const EMPTY_ROUTE: Route = { set: '', take: '', ver: '', cmp: '', t: null };

export function readRoute(loc: Pick<Location, 'search' | 'hash'> = location): Route {
  const q = new URLSearchParams(loc.search);
  const at = Number.parseFloat(q.get('t') ?? '');
  const t = Number.isFinite(at) ? at : null;
  const [frag = '', fragQuery = ''] = loc.hash.replace(/^#\/?/, '').split('?');
  if (frag) {
    const [set = '', take = '', ver = ''] = frag.split('/');
    return {
      set: decodeURIComponent(set),
      take: decodeURIComponent(take),
      ver: decodeURIComponent(ver),
      cmp: new URLSearchParams(fragQuery).get('c') || '',
      t,
    };
  }
  return {
    set: q.get('set') || '',
    take: q.get('take') || '',
    ver: q.get('v') || '',
    cmp: q.get('c') || '',
    t,
  };
}

/** The fragment naming the render on the page; blind mode leaves the version out, since hiding it is the point. */
export function routeHash(s: AppState): string {
  const parts: (string | null)[] = [s.setId];
  const item = currentItem(s);
  if (item) {
    parts.push(item.id);
  }
  if (item && s.take && !s.blind) {
    parts.push(activeKey(s));
  }
  const c = comparisons(s).length ? selectedComparison(s) : null;
  const path = parts
    .filter((p): p is string => Boolean(p))
    .map(encodeURIComponent)
    .join('/');
  return `#${path}${c ? `?c=${encodeURIComponent(c.id)}` : ''}`;
}

/** Rewritten rather than pushed: a hundred arrow keys in the back stack makes the back button useless. */
export function writeRoute(s: AppState): void {
  const hash = routeHash(s);
  if (location.hash !== hash) {
    history.replaceState(null, '', hash);
  }
}
