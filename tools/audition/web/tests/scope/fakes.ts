/* Test doubles: a recording 2D context, synthetic audio and a minimal take in the store. */

import { vi } from 'vitest';
import type { Manifest } from '../../src/api/schemas';
import type { Pcm } from '../../src/scope/compute';
import { type AppState, resetStore } from '../../src/state/store';

export interface Call {
  op: string;
  args: unknown[];
  /** fillStyle / strokeStyle / lineWidth as they stood when the call was made. */
  fill: string;
  stroke: string;
  lineWidth: number;
}

export interface Recorder {
  calls: Call[];
  ctx: Record<string, unknown>;
  of: (op: string) => Call[];
}

const recorders = new WeakMap<object, Recorder>();

function makeRecorder(): Recorder {
  const calls: Call[] = [];
  const ctx: Record<string, unknown> = { fillStyle: '', strokeStyle: '', lineWidth: 1, font: '' };
  const ops = [
    'clearRect',
    'fillRect',
    'beginPath',
    'moveTo',
    'lineTo',
    'stroke',
    'fill',
    'closePath',
    'fillText',
    'drawImage',
    'putImageData',
  ];
  for (const op of ops) {
    ctx[op] = (...args: unknown[]) => {
      calls.push({
        op,
        args,
        fill: String(ctx.fillStyle),
        stroke: String(ctx.strokeStyle),
        lineWidth: Number(ctx.lineWidth),
      });
    };
  }
  ctx.measureText = () => ({ width: 10 });
  ctx.createImageData = (w: number, h: number) => ({
    width: w,
    height: h,
    data: new Uint8ClampedArray(w * h * 4),
  });
  return { calls, ctx, of: (op) => calls.filter((c) => c.op === op) };
}

/** Make every canvas hand out a recording context; returns the lookup for one canvas. */
export function stubCanvas(): (cv: HTMLCanvasElement) => Recorder {
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(function (
    this: HTMLCanvasElement,
  ) {
    let rec = recorders.get(this);
    if (!rec) {
      rec = makeRecorder();
      recorders.set(this, rec);
    }
    return rec.ctx as unknown as CanvasRenderingContext2D;
  } as never);
  return (cv) => {
    let rec = recorders.get(cv);
    if (!rec) {
      rec = makeRecorder();
      recorders.set(cv, rec);
    }
    return rec;
  };
}

/** Give a canvas a layout size, as jsdom lays nothing out. */
export function sizeCanvas(cv: HTMLCanvasElement, w: number, h: number, left = 0): void {
  Object.defineProperty(cv, 'clientWidth', { configurable: true, value: w });
  Object.defineProperty(cv, 'clientHeight', { configurable: true, value: h });
  cv.getBoundingClientRect = () =>
    ({
      left,
      top: 0,
      width: w,
      height: h,
      right: left + w,
      bottom: h,
      x: left,
      y: 0,
      toJSON: () => ({}),
    }) as DOMRect;
}

export function pcm(channels: Float32Array<ArrayBuffer>[], sampleRate = 8000): Pcm {
  return {
    numberOfChannels: channels.length,
    sampleRate,
    getChannelData: (i: number) => channels[i] as Float32Array<ArrayBuffer>,
  };
}

export function sine(
  freq: number,
  seconds: number,
  sampleRate = 8000,
  amp = 0.5,
): Float32Array<ArrayBuffer> {
  const out = new Float32Array(Math.round(seconds * sampleRate));
  for (let i = 0; i < out.length; i++) {
    out[i] = amp * Math.sin((2 * Math.PI * freq * i) / sampleRate);
  }
  return out;
}

export const SOURCES = {
  m: { role: 'model', label: 'm' },
  r: { role: 'reference', label: 'r' },
} as unknown as Manifest['sources'];

/** A take of `keys` in the store with the manifest roles above. */
export function loadTake(over: Partial<AppState> = {}, epoch = 1): void {
  resetStore({
    manifest: { sources: SOURCES } as unknown as Manifest,
    take: { id: 't', keys: ['m', 'r'], rms: {}, rates: { m: 1000, r: 1000 }, duration: 10, epoch },
    display: [0, 1],
    ...over,
  });
}

/** Replace requestAnimationFrame with a queue the test steps by hand. */
export function controlFrames(): {
  run: () => void;
  pending: () => number;
  cancelled: () => number;
} {
  let queue = new Map<number, FrameRequestCallback>();
  let next = 1;
  let cancelled = 0;
  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    queue.set(next, cb);
    return next++;
  });
  vi.stubGlobal('cancelAnimationFrame', (id: number) => {
    if (queue.delete(id)) {
      cancelled++;
    }
  });
  return {
    run: () => {
      const due = queue;
      queue = new Map();
      for (const cb of due.values()) {
        cb(0);
      }
    },
    pending: () => queue.size,
    cancelled: () => cancelled,
  };
}

/** Set a role colour custom property on the document, as the stylesheet does. */
export function setRoleColours(): void {
  const style = document.documentElement.style;
  style.setProperty('--role-model-rgb', '94 199 168');
  style.setProperty('--role-reference-rgb', '226 163 63');
  style.setProperty('--role-comparison-rgb', '163 140 214');
  style.setProperty('--role-other-rgb', '133 141 155');
}
