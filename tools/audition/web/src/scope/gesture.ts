/* The pictures are the timeline, so every gesture a timeline has works on them: grab the playhead
 * to scrub it, drag anywhere else to mark a passage, click to seek. */

import { pause, playhead, seekTo, setRegion, startAt } from '../player/engine';
import { getState, setState } from '../state/store';
import { GRAB_PX, MIN_DRAG } from './constants';

/** One pointer interaction at a time; the caller turns events into seconds and a pixel width. */
export class Gesture {
  private dragFrom: number | null = null;
  private scrub: { resume: boolean } | null = null;

  /** True when `at` seconds is close enough to the playhead to grab it. */
  near(at: number, widthPx: number): boolean {
    const take = getState().take;
    return !!take && Math.abs(at - playhead()) <= (GRAB_PX / Math.max(widthPx, 1)) * take.duration;
  }

  down(at: number, widthPx: number): void {
    if (!getState().take) {
      return;
    }
    if (this.near(at, widthPx)) {
      // Silent while it moves: restarting every source per frame is what an audible scrub costs.
      this.scrub = { resume: getState().playing };
      if (this.scrub.resume) {
        pause();
      }
      seekTo(at);
      return;
    }
    this.dragFrom = at;
  }

  /** Returns the cursor the canvas should show. */
  move(at: number, widthPx: number): string {
    if (this.scrub) {
      seekTo(at);
      return 'ew-resize';
    }
    if (this.dragFrom === null) {
      return getState().take && this.near(at, widthPx) ? 'ew-resize' : '';
    }
    if (Math.abs(at - this.dragFrom) > MIN_DRAG) {
      // Only the mark moves while dragging; playback follows once the pointer is released.
      setState({ region: [Math.min(this.dragFrom, at), Math.max(this.dragFrom, at)] });
    }
    return '';
  }

  up(at: number): void {
    if (this.scrub) {
      seekTo(at);
      if (this.scrub.resume) {
        startAt(playhead());
      }
      this.scrub = null;
      return;
    }
    if (this.dragFrom === null) {
      return;
    }
    if (Math.abs(at - this.dragFrom) <= MIN_DRAG) {
      // A click inside the passage seeks inside it; outside it is how you leave it.
      const r = getState().region;
      if (r && (at < r[0] || at > r[1])) {
        setRegion(null);
      }
      seekTo(at);
    } else {
      setRegion(getState().region);
    }
    this.dragFrom = null;
  }
}
