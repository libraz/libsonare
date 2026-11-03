/* What every key does.
 *
 * Two key maps, because the two views have different subjects: on the
 * listening surface every shortcut acts on what is sounding, on the bank
 * nothing is sounding and the rows are what is navigated. The listening run
 * answers only its own keys, play and the side swap.
 */

import { bankKey } from '../bank/BankView';
import { answerAt, send } from '../feedback/actions';
import {
  pickShown,
  revealOrReshuffle,
  selectTake,
  setBlind,
  setMatchRms,
  setRestartOnSwitch,
  stepVersion,
  swapRole,
} from '../listen/actions';
import { rewind, setLoop, togglePlay } from '../player/engine';
import { runKey } from '../run/run';
import { getState, setState } from '../state/store';

/** Fields a letter is typed into; a checkbox or a button holds focus without owning the keyboard. */
const TEXT_INPUTS = new Set(['', 'text', 'search', 'number', 'email', 'url', 'tel', 'password']);

export function isTextEntry(n: EventTarget | null): boolean {
  if (!(n instanceof HTMLElement)) {
    return false;
  }
  if (n.tagName === 'TEXTAREA' || n.tagName === 'SELECT' || n.isContentEditable) {
    return true;
  }
  return n instanceof HTMLInputElement && TEXT_INPUTS.has(n.type);
}

export function toggleHelp(want?: boolean): void {
  setState({ helpOpen: want ?? !getState().helpOpen });
}

/** The arrows and home: true when the key was one of them. */
function move(k: string, itemIndex: number): boolean {
  switch (k) {
    case 'ArrowRight':
      stepVersion(1);
      return true;
    case 'ArrowLeft':
      stepVersion(-1);
      return true;
    case 'ArrowDown':
      void selectTake(itemIndex + 1);
      return true;
    case 'ArrowUp':
      void selectTake(itemIndex - 1);
      return true;
    case 'Home':
      rewind();
      return true;
    default:
      return false;
  }
}

/** The run's own keys plus play and the side swap; every other key does nothing, so nothing the run decided can be changed from the keyboard. */
function runViewKey(ev: KeyboardEvent): void {
  if (runKey(ev, { answerAt, send })) {
    return;
  }
  if (ev.key === ' ') {
    ev.preventDefault();
    togglePlay();
  } else if (ev.key === 'Tab' && getState().take) {
    ev.preventDefault();
    swapRole();
  }
}

export function onKey(ev: KeyboardEvent): void {
  const target = ev.target as HTMLElement | null;
  // The palette's find box owns its own keys (the palette handles them); escape leaves an input.
  if (target?.id === 'voiceFind') {
    return;
  }
  if (target?.tagName === 'INPUT' && ev.key === 'Escape') {
    target.blur();
    return;
  }
  if (isTextEntry(target) || ev.metaKey || ev.ctrlKey || ev.altKey) {
    return;
  }
  const s = getState();
  if (s.paletteOpen) {
    return;
  }
  if (ev.key === '?') {
    ev.preventDefault();
    toggleHelp();
    return;
  }
  if (ev.key === 'Escape' && s.helpOpen) {
    toggleHelp(false);
    return;
  }
  if (s.view === 'bank') {
    bankKey(ev);
    return;
  }
  if (s.view === 'run') {
    runViewKey(ev);
    return;
  }
  const k = ev.key;
  if (k === 'v' || k === 'V') {
    ev.preventDefault();
    setState({ paletteOpen: true });
    return;
  }
  if (k === ' ') {
    ev.preventDefault();
    togglePlay();
    return;
  }
  if (!s.take) {
    return;
  }
  if (k === 'Tab') {
    ev.preventDefault();
    swapRole();
    return;
  }
  if (k.length === 1 && k >= '1' && k <= '9') {
    pickShown(Number(k) - 1);
    return;
  }
  if (move(k, s.itemIndex)) {
    ev.preventDefault();
    return;
  }
  const toggles: Record<string, () => void> = {
    l: () => setLoop(!s.loop),
    m: () => setMatchRms(!s.matchRms),
    s: () => setRestartOnSwitch(!s.restartOnSwitch),
    b: () => setBlind(!s.blind),
    r: revealOrReshuffle,
  };
  toggles[k.toLowerCase()]?.();
}
