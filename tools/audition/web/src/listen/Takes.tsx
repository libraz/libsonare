/* The take list: every take of the open set, grouped as the manifest groups them. */

import { type ReactNode, useEffect, useRef } from 'react';
import { useT } from '../i18n';
import { roleOf } from '../state/selectors';
import { useApp } from '../state/store';
import { takeText } from '../takes/text';
import { selectTake } from './actions';

export function Takes() {
  const { t } = useT();
  const s = useApp((x) => x);
  const current = useRef<HTMLButtonElement>(null);
  const here = `${s.setId}:${s.itemIndex}`;
  useEffect(() => {
    if (here) {
      current.current?.scrollIntoView?.({ block: 'nearest' });
    }
  }, [here]);
  const rows: ReactNode[] = [];
  let group: string | undefined;
  s.items.forEach((item, i) => {
    if (item.group && item.group !== group) {
      group = item.group;
      rows.push(
        <div key={`g:${group}:${item.id}`} className="group">
          {takeText(group)}
        </div>,
      );
    }
    // The id is what the URL carries, so it is shown rather than left to be guessed from the label.
    const sub = item.sub ? `${item.id} — ${takeText(item.sub)}` : item.id;
    // A take holding a reference is the one worth opening first.
    const hasReference = Object.keys(item.tracks).some((k) => roleOf(s, k) === 'reference');
    const here = i === s.itemIndex;
    rows.push(
      <button
        key={item.id}
        ref={here ? current : undefined}
        type="button"
        aria-current={here || undefined}
        onClick={() => void selectTake(i)}
      >
        <span className="take-name">{takeText(item.label) || item.id}</span>
        <span className="sub">{sub}</span>
        {hasReference && <span className="take-ref" title={t('takes.hasReference')} />}
      </button>,
    );
  });
  return (
    <nav id="takes" aria-label={t('takes.title')}>
      {rows}
    </nav>
  );
}
