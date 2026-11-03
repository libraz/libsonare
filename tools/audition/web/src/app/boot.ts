/* Starting the page, and moving between its three views. */

import { loadBank } from '../bank/store';
import { t } from '../i18n';
import { applyRoute, loadFeedbackIndex, loadSet, loadSets } from '../listen/actions';
import { readRoute } from '../listen/route';
import { pause, setOffset } from '../player/engine';
import { runOn, startRun, stopRun } from '../run/run';
import { getState, SET_KEY, setState, VIEW_KEY, type View } from '../state/store';

const VIEWS: readonly View[] = ['run', 'bank', 'listen'];
const isView = (v: unknown): v is View => VIEWS.includes(v as View);

/** `autoplay` is off only when a view is restored at boot: nothing has been pressed, so there is no gesture for audio. */
export async function setView(
  view: View,
  { autoplay = true }: { autoplay?: boolean } = {},
): Promise<void> {
  if (view !== 'run' && runOn()) {
    stopRun();
  }
  setState({ view });
  localStorage.setItem(VIEW_KEY, view);
  if (view === 'bank') {
    if (getState().playing) {
      pause();
    }
    await loadBank();
  }
  if (view === 'run' && !runOn()) {
    await startRun({ autoplay });
  }
}

/** Open a voice from the bank on the listening surface. */
export async function openVoice(slug: string): Promise<void> {
  await setView('listen');
  await loadSet(slug, { set: slug, take: '', ver: '', cmp: '', t: null });
}

function fail(title: string): void {
  // Held, because the bank view clears the title otherwise.
  setState({ title, failed: true });
}

/* The server reads every committed bank registry beside its first requests, so
 * a set whose render names an older registry says `checking` until that is
 * done. Ask again until nothing does. */
function settleShips(): void {
  if (!getState().sets.some((x) => x.ships?.state === 'checking')) {
    return;
  }
  setTimeout(async () => {
    try {
      await loadSets();
    } catch {
      return;
    }
    settleShips();
  }, 2500);
}

/** An address names a render and is therefore a request to listen to it, also when it arrives at an open page. */
export async function onHashChange(): Promise<void> {
  const route = readRoute();
  if (route.set) {
    await setView('listen');
  }
  await applyRoute(route);
}

export async function boot(): Promise<void> {
  try {
    await loadSets();
    await loadFeedbackIndex();
    // Up front: the listening surface's own header line reads from the bank too.
    await loadBank();
    settleShips();
    const { sets } = getState();
    if (!sets.length) {
      // A fresh clone has no renders; the bank says which voices exist without any.
      fail(t('empty.noRenders'));
      await setView('bank');
      return;
    }
    // An address wins over what was last listened to.
    const route = readRoute();
    const remembered = localStorage.getItem(SET_KEY);
    let start = sets[0]?.id ?? '';
    if (sets.some((x) => x.id === route.set)) {
      start = route.set;
    } else if (sets.some((x) => x.id === remembered)) {
      start = remembered ?? start;
    }
    await loadSet(start, route);
    const take = getState().take;
    if (route.t !== null && take) {
      setOffset(Math.max(0, Math.min(route.t, take.duration)));
    }
    const last = localStorage.getItem(VIEW_KEY);
    if (!route.set && isView(last) && last !== 'listen') {
      await setView(last, { autoplay: false });
    }
  } catch (err) {
    fail(t('empty.loadFailed', { msg: (err as Error).message }));
  }
}
