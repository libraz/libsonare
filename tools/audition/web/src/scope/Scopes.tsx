/* The two pictures of the stage: every version's waveform at once, and a spectrogram of the one
 * sounding. A frame loop repaints them while mounted; React renders only when a caption changes.
 */

import { useEffect, useRef } from 'react';
import { useT } from '../i18n';
import { playhead } from '../player/engine';
import { roleClass } from '../state/selectors';
import { getState, useApp } from '../state/store';
import { legendRoles } from './captions';
import { drawSpec, drawWave, Layer, secondsAt } from './draw';
import { Gesture } from './gesture';

type PointerHandlers = Pick<
  React.ComponentProps<'canvas'>,
  'onPointerDown' | 'onPointerMove' | 'onPointerUp' | 'onPointerCancel'
>;

/** Pointer handlers that turn one canvas into a timeline. */
function useTimeline(): PointerHandlers {
  const gesture = useRef(new Gesture());
  const at = (ev: React.PointerEvent<HTMLCanvasElement>) =>
    secondsAt(ev.currentTarget, ev.clientX, getState().take?.duration ?? 0);
  const width = (ev: React.PointerEvent<HTMLCanvasElement>) =>
    ev.currentTarget.getBoundingClientRect().width;
  return {
    onPointerDown: (ev) => {
      if (!getState().take) {
        return;
      }
      ev.currentTarget.setPointerCapture?.(ev.pointerId);
      gesture.current.down(at(ev), width(ev));
    },
    onPointerMove: (ev) => {
      ev.currentTarget.style.cursor = gesture.current.move(at(ev), width(ev));
    },
    onPointerUp: (ev) => gesture.current.up(at(ev)),
    onPointerCancel: (ev) => gesture.current.up(at(ev)),
  };
}

export function Scopes() {
  const { t } = useT();
  const compare = useApp((s) => s.compare);
  const status = useApp((s) => s.takeStatus);
  const roles = useApp((s) => legendRoles(s).join('|'));
  const wave = useRef<HTMLCanvasElement>(null);
  const spec = useRef<HTMLCanvasElement>(null);
  const waveTimeline = useTimeline();
  const specTimeline = useTimeline();

  useEffect(() => {
    const layers = { wave: new Layer(), spec: new Layer() };
    const frame = () => {
      const at = playhead();
      if (wave.current) {
        drawWave(wave.current, layers.wave, at);
      }
      if (spec.current) {
        drawSpec(spec.current, layers.spec, at);
      }
    };
    let raf = requestAnimationFrame(function tick() {
      if (getState().take) {
        frame();
      }
      raf = requestAnimationFrame(tick);
    });
    return () => cancelAnimationFrame(raf);
  }, []);

  let specCaption = t('spec.caption');
  let specClass = 'cap';
  if (status?.state === 'loading') {
    specCaption = t('spec.decoding');
    specClass = 'cap loading';
  } else if (status?.state === 'failed') {
    specCaption = t('spec.failed', { msg: status.msg });
  }

  return (
    <div className="scopes">
      <figure className="pane">
        <canvas id="wave" ref={wave} height={142} {...waveTimeline} />
        <figcaption className="cap">
          <span id="waveCaption">{compare ? t('wave.caption') : t('wave.captionSolo')}</span>
          <span id="waveLegend" className="legend-row">
            {roles
              .split('|')
              .filter(Boolean)
              .map((role) => (
                <span key={role} className={`wl ${roleClass(role)}`}>
                  <span className="dot" />
                  <span>{t(`role.${role}` as 'role.other')}</span>
                </span>
              ))}
          </span>
        </figcaption>
      </figure>

      <figure className="pane">
        <canvas id="spec" ref={spec} height={212} {...specTimeline} />
        <figcaption id="specCaption" className={specClass}>
          {specCaption}
        </figcaption>
      </figure>
    </div>
  );
}
