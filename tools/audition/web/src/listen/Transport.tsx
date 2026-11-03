/* The transport row: the three controls that make a sound, the clock, the
 * marked passage and its way out, the levels, blind mode's two acts, and the
 * address of what is sounding.
 */

import { useEffect, useRef, useState } from 'react';
import { useT } from '../i18n';
import {
  atEnd,
  playhead,
  rewind,
  setLoop,
  setRegion,
  stop,
  targetGain,
  togglePlay,
} from '../player/engine';
import { activeKey, blindLetter, currentItem } from '../state/selectors';
import { getState, useApp } from '../state/store';
import {
  abstainBlind,
  blindRefusal,
  blindTally,
  chooseBlind,
  earlierCount,
  recordBlindResult,
} from './actions';
import { conditions, hitAt } from './note';
import { routeHash } from './route';

const db = (v: number): string => (v > 0 ? (20 * Math.log10(v)).toFixed(1) : '-inf');

/** The clock, and the end of a pass: both follow the audio clock rather than React. */
function Clock() {
  const at = useRef<HTMLSpanElement>(null);
  const of = useRef<HTMLSpanElement>(null);
  useEffect(() => {
    let raf = 0;
    const tick = () => {
      const s = getState();
      if (s.take) {
        if (at.current) {
          at.current.textContent = playhead().toFixed(2);
        }
        if (of.current) {
          of.current.textContent = ` / ${s.take.duration.toFixed(2)}`;
        }
        if (atEnd()) {
          stop();
        }
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);
  return (
    <span id="clock" className="clock mono">
      <span id="clockAt" ref={at}>
        0.00
      </span>
      <span id="clockOf" ref={of} className="of">
        {' / 0.00'}
      </span>
    </span>
  );
}

function Levels() {
  const { t } = useT();
  const s = useApp((x) => x);
  if (!s.take) {
    return <span id="levels" className="mono" />;
  }
  const key = activeKey(s);
  const g = targetGain(key);
  const rms = s.take.rms[key] ?? 0;
  // Blind, the level heard is the matched one; naming the raw level would name the version.
  const parts = [t('level.rms', { db: db(s.blind ? rms * g : rms) })];
  if (s.matchRms) {
    parts.push(t('level.gain', { db: (20 * Math.log10(g)).toFixed(1) }));
  }
  return (
    <span id="levels" className="mono">
      {parts.join('   ')}
    </span>
  );
}

/* Blind mode's readout and its two acts. The tally names its sources, so until
 * the run is recorded the line says only how many takes have been decided. */
function BlindControls() {
  const { t } = useT();
  const s = useApp((x) => x);
  const item = currentItem(s);
  if (!s.blind) {
    return (
      <>
        <span id="blindScore" className="mono">
          {blindRefusal(s)}
        </span>
        <button id="blindPick" type="button" className="ghost" hidden />
        <button id="blindUnsure" type="button" className="ghost" hidden />
        <button id="blindRecord" type="button" className="ghost" hidden />
      </>
    );
  }
  const here = item ? s.picks[item.id] : undefined;
  const { n, parts } = blindTally(s);
  const old = earlierCount(s);
  const earlier = old ? `   ${t('blind.earlier', { n: old })}` : '';
  let score = t('blind.pickEach');
  if (n) {
    score = s.blindRevealed
      ? `${t('blind.preferred')}: ${parts.join('   ')}`
      : t('blind.decided', { n });
  }
  return (
    <>
      <span id="blindScore" className="mono">
        {score + earlier}
      </span>
      <button
        id="blindPick"
        type="button"
        className="ghost"
        title={t('blind.chooseTitle')}
        aria-pressed={Boolean(here?.key)}
        onClick={chooseBlind}
      >
        {t('blind.choose')}
      </button>
      <button
        id="blindUnsure"
        type="button"
        className="ghost"
        title={t('blind.unsureTitle')}
        aria-pressed={Boolean(here?.unseparated)}
        onClick={abstainBlind}
      >
        {t('blind.unsure')}
      </button>
      <button
        id="blindRecord"
        type="button"
        className="ghost"
        hidden={n === 0}
        onClick={() => void recordBlindResult()}
      >
        {t('blind.record')}
      </button>
    </>
  );
}

/** The conditions block the copy button puts on the clipboard; the note a page sends carries the same fields. */
export function conditionsText(at: number, origin: string, path: string): string {
  const s = getState();
  const c = conditions(s, at);
  const url = `${origin}${path}?t=${at.toFixed(2)}${routeHash(s)}`;
  const lines = [
    `set:      ${c.set}`,
    `take:     ${c.take || '-'}${c.take_label ? ` — ${c.take_label}` : ''}`,
    `version:  ${c.blind ? 'hidden (blind)' : c.version}`,
    `playhead: ${c.playhead.toFixed(2)} s of ${c.duration ? c.duration.toFixed(2) : '?'} s`,
  ];
  const hit = hitAt(s, at);
  if (hit) {
    const plays = hit.notes.map((n) => `note ${n.note} v${n.velocity}`).join(' + ');
    lines.push(
      `hit:      #${hit.n} — ${plays}, struck at ${hit.start.toFixed(2)} s (${(at - hit.start).toFixed(2)} s in)`,
    );
  }
  if (c.region) {
    lines.push(`region:   ${c.region[0].toFixed(2)}–${c.region[1].toFixed(2)} s`);
  }
  lines.push(
    `options:  gain-match ${c.match_loudness ? 'on' : 'off'}, loop ${c.loop ? 'on' : 'off'}, blind ${c.blind ? 'on' : 'off'}`,
  );
  lines.push(url);
  return lines.join('\n');
}

/** A crumb is a position in the address, not a value: set, take, version. */
const CRUMB_KEYS = ['set', 'take', 'version'] as const;

/** The address of what is sounding, at the end of the row that plays it: the file a report refers to is never inferred. */
function Ident() {
  const { t } = useT();
  const s = useApp((x) => x);
  const [said, setSaid] = useState<'copied' | 'shown' | null>(null);
  const item = currentItem(s);
  const parts = [s.setId, item?.id];
  if (item && s.take) {
    parts.push(s.blind ? blindLetter(s, s.versionIndex) : activeKey(s));
  }
  const shown = parts.filter((p): p is string => Boolean(p));
  const path = item && s.take && !s.blind ? s.base + (item.tracks[activeKey(s)] ?? '') : '';
  const copy = async () => {
    const text = conditionsText(playhead(), location.origin, location.pathname);
    try {
      await navigator.clipboard.writeText(text);
      setSaid('copied');
    } catch {
      // Plain http from another host has no clipboard, and the block does not fit on a button.
      window.prompt(t('now.copy'), text);
      setSaid('shown');
    }
    setTimeout(() => setSaid(null), 1600);
  };
  let label = t('now.copy');
  if (said === 'copied') {
    label = t('now.copied');
  } else if (said === 'shown') {
    label = t('now.shown');
  }
  return (
    <div className="ident">
      <div className="crumbs" id="crumbs">
        {shown.map((p, i) => (
          <span key={CRUMB_KEYS[i]}>
            <span className={i === shown.length - 1 ? 'now' : ''}>{p}</span>
            {i < shown.length - 1 && <span className="sep">›</span>}
          </span>
        ))}
      </div>
      <code id="identPath" className="path">
        {path}
      </code>
      <button
        id="copyLink"
        type="button"
        className="ghost"
        title={t('now.copyTitle')}
        onClick={() => void copy()}
      >
        {label}
      </button>
    </div>
  );
}

export function Transport() {
  const { t } = useT();
  const playing = useApp((x) => x.playing);
  const loop = useApp((x) => x.loop);
  const region = useApp((x) => x.region);
  return (
    <div id="transport">
      <div className="tport">
        <button
          id="rewindBtn"
          type="button"
          className="tbtn"
          title={t('transport.rewindTitle')}
          onClick={rewind}
        >
          <span className="ticon" aria-hidden="true">
            ❙◀︎
          </span>
          <span>{t('transport.rewind')}</span>
        </button>
        <button
          id="playBtn"
          type="button"
          className="tbtn play"
          aria-pressed={playing || undefined}
          onClick={togglePlay}
        >
          <span className="ticon" aria-hidden="true">
            {playing ? '❚❚' : '▶︎'}
          </span>
          <span className="tlabel">{playing ? t('transport.pause') : t('transport.play')}</span>
        </button>
        <button
          id="loopBtn"
          type="button"
          className="tbtn"
          aria-pressed={loop}
          title={t('transport.loopTitle')}
          onClick={() => setLoop(!getState().loop)}
        >
          <span className="ticon" aria-hidden="true">
            ↻
          </span>
          <span>{t('transport.loop')}</span>
        </button>
      </div>
      <Clock />
      <span id="passage" className="passage" hidden={!region}>
        <span id="regionSpan" className="mono">
          {region ? `${region[0].toFixed(2)}–${region[1].toFixed(2)} s` : ''}
        </span>
        <button
          id="clearRegion"
          type="button"
          className="passage-x"
          title={t('transport.clearRegion')}
          aria-label={t('transport.clearRegion')}
          onClick={() => setRegion(null)}
        >
          ×
        </button>
      </span>
      <Levels />
      <span className="grow" />
      <BlindControls />
      <Ident />
    </div>
  );
}
