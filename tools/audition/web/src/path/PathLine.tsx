/* The signal-path line under the stage, following the playhead.
 *
 * The frame loop only asks which topology is in force; React renders again when the drawn
 * signature changes, never per frame.
 */

import { useEffect, useRef, useState } from 'react';
import { useT } from '../i18n';
import { playhead } from '../player/engine';
import { activeKey, selectedComparison } from '../state/selectors';
import { getState } from '../state/store';
import { buildPathView, EMPTY_VIEW, type PathView, pathIndex } from './model';

/** What the line depends on; equal signatures draw the same line. */
function signature(lang: string): string {
  const s = getState();
  if (!s.take) {
    return '';
  }
  const key = activeKey(s);
  return [
    s.take.epoch,
    key,
    pathIndex(s, key, playhead()),
    s.blind,
    lang,
    selectedComparison(s)?.id,
  ].join('|');
}

export function PathLine() {
  const { lang } = useT();
  const [view, setView] = useState<PathView>(EMPTY_VIEW);
  const drawn = useRef<string | null>(null);

  useEffect(() => {
    drawn.current = null;
    const step = () => {
      const sig = signature(lang);
      if (sig === drawn.current) {
        return;
      }
      drawn.current = sig;
      const s = getState();
      setView(s.take ? buildPathView(s, activeKey(s), playhead()) : EMPTY_VIEW);
    };
    step();
    let raf = requestAnimationFrame(function tick() {
      step();
      raf = requestAnimationFrame(tick);
    });
    return () => cancelAnimationFrame(raf);
  }, [lang]);

  return (
    <div id="pathLine" className="path-line" title={view.title || undefined}>
      {view.spans.map((span, i) => (
        // The spans are replaced wholesale, so their position is their identity.
        // biome-ignore lint/suspicious/noArrayIndexKey: ordered, never reordered
        <span key={i} className={span.cls}>
          {span.text}
        </span>
      ))}
    </div>
  );
}
