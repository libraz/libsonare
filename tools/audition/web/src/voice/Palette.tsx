/* The voice palette: every voice with the three facts that decide whether it is worth opening --
 * how far its calibration has got, whether anybody has listened, and the worst thing they said --
 * rendered by the same chips the bank view uses. Anchored under the header and dismissed by a
 * click away rather than a scrim, so a click meant for the transport still reaches it.
 */

import { type KeyboardEvent, useEffect, useRef, useState } from 'react';
import { stageIndex } from '../bank/logic';
import { useBank } from '../bank/store';
import { useT } from '../i18n';
import { setState, useApp } from '../state/store';
import { HeardChip, MethodDot, SignoffChip, StageBar } from './chips';
import { addressOf, oneDecimal } from './identity';
import { paletteRows } from './paletteRows';

/** The `.pick` header control that opens the palette; it names the voice being listened to. */
export function VoicePickButton() {
  const { t } = useT();
  const open = useApp((s) => s.paletteOpen);
  const setId = useApp((s) => s.setId);
  const v = useBank((s) => s.voices.find((x) => x.slug === setId));
  return (
    <div className="pick listen-only">
      <span className="pick-lab">{t('head.voice')}</span>
      <button
        id="voicePick"
        type="button"
        className="voice-pick"
        aria-expanded={open}
        aria-haspopup="listbox"
        aria-controls="voicePanel"
        aria-label={t('head.voice')}
        onClick={() => setState((s) => ({ paletteOpen: !s.paletteOpen }))}
      >
        {v && <MethodDot engine={v.engine} />}
        <span className="vp-name">{v ? v.name : (setId ?? '')}</span>
        <span className="vp-id">{setId ?? ''}</span>
      </button>
    </div>
  );
}

export function VoicePalette({ onPick }: { onPick: (setId: string) => void }) {
  const { t, lang } = useT();
  const open = useApp((s) => s.paletteOpen);
  const sets = useApp((s) => s.sets);
  const setId = useApp((s) => s.setId);
  const voices = useBank((s) => s.voices);
  const [find, setFind] = useState('');
  const panel = useRef<HTMLDivElement>(null);
  const input = useRef<HTMLInputElement>(null);

  // Opening starts from an empty filter, focused, with the current voice in view.
  useEffect(() => {
    if (!open) {
      return;
    }
    setFind('');
    input.current?.focus();
    panel.current?.querySelector('[aria-selected="true"]')?.scrollIntoView?.({ block: 'center' });
  }, [open]);

  useEffect(() => {
    if (!open) {
      return;
    }
    const onDown = (ev: PointerEvent) => {
      const target = ev.target as Element | null;
      if (!panel.current?.contains(target) && !target?.closest('#voicePick')) {
        setState({ paletteOpen: false });
      }
    };
    const onEscape = (ev: globalThis.KeyboardEvent) => {
      if (ev.key === 'Escape') {
        setState({ paletteOpen: false });
        document.getElementById('voicePick')?.focus();
      }
    };
    document.addEventListener('pointerdown', onDown);
    document.addEventListener('keydown', onEscape);
    return () => {
      document.removeEventListener('pointerdown', onDown);
      document.removeEventListener('keydown', onEscape);
    };
  }, [open]);

  const rows = open ? paletteRows(sets, voices, find) : [];

  // Down from the find box and through the rows, so the whole list is reachable from the keyboard.
  const onKeyDown = (ev: KeyboardEvent) => {
    const buttons = [...(panel.current?.querySelectorAll<HTMLElement>('.palette-row') ?? [])];
    if (!buttons.length) {
      return;
    }
    const at = buttons.indexOf(document.activeElement as HTMLElement);
    if (ev.key === 'ArrowDown') {
      ev.preventDefault();
      buttons[Math.min(at + 1, buttons.length - 1)]?.focus();
    } else if (ev.key === 'ArrowUp') {
      ev.preventDefault();
      if (at <= 0) {
        input.current?.focus();
      } else {
        buttons[at - 1]?.focus();
      }
    } else if (ev.key === 'Enter' && at < 0) {
      ev.preventDefault();
      buttons[0]?.click();
    }
  };

  const choose = (id: string) => {
    setState({ paletteOpen: false });
    onPick(id);
  };

  return (
    // biome-ignore lint/a11y/noStaticElementInteractions: keys bubble from the find box and rows
    <div
      id="voicePanel"
      ref={panel}
      className="palette listen-only"
      hidden={!open}
      onKeyDown={onKeyDown}
    >
      {open && (
        <>
          <div className="palette-bar">
            <input
              id="voiceFind"
              ref={input}
              type="search"
              value={find}
              placeholder={t('pick.findPlaceholder')}
              aria-label={t('pick.find')}
              onChange={(ev) => setFind(ev.target.value)}
            />
            <span className="mono">
              {rows.length === sets.length
                ? String(sets.length)
                : `${rows.length} / ${sets.length}`}
            </span>
          </div>
          <div className="palette-rows" role="listbox" aria-label={t('head.voice')}>
            {rows.flatMap(({ set, voice, heading }) => [
              heading ? (
                <div key={`h-${heading}`} className="palette-group">
                  {heading}
                </div>
              ) : null,
              <button
                key={set.id}
                type="button"
                className="palette-row"
                role="option"
                aria-selected={set.id === setId}
                onClick={() => choose(set.id)}
              >
                <span className="addr">{voice ? addressOf(voice) : ''}</span>
                <span className="who">
                  {voice ? voice.name : set.title || set.id}
                  {voice?.patch && <span className="patch">{`  ${voice.patch}`}</span>}
                </span>
                {voice ? <MethodDot engine={voice.engine} /> : <span className="dot" />}
                <span
                  className="stage"
                  title={voice ? `${t(`stage.${stageIndex(voice)}`)} — ${voice.next}` : undefined}
                >
                  {voice && (
                    <>
                      <StageBar stage={voice.stage} />
                      <span>{oneDecimal(voice.stage, lang)}</span>
                    </>
                  )}
                </span>
                <HeardChip id={set.id} />
                <SignoffChip voice={voice} />
                {/* A voice with no reference plays rather than compares; set faintly, not as a badge. */}
                <span className="solo">{set.compare ? '' : t('pick.playOnly')}</span>
              </button>,
            ])}
            {!rows.length && <p className="bank-empty">{t('bank.nothingMatches')}</p>}
          </div>
          <p className="palette-legend">{t('pick.legend')}</p>
        </>
      )}
    </div>
  );
}
