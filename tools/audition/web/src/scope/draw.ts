/* Canvas painting for the waveform and the spectrogram.
 *
 * Each picture is painted once into an offscreen layer and blitted every frame with only the
 * marked passage and the playhead on top; recomputing either per frame turns a listening tool into
 * a slideshow. Every trace is drawn in its role's colour (none when blind), and the spectrogram's
 * ramp is built from the sounding version's, so the picture says model-or-reference unread.
 */

import { takeHits } from '../listen/note';
import { bufferOf } from '../player/engine';
import { activeKey, roleOf } from '../state/selectors';
import { getState, type Take } from '../state/store';
import { levelDb, peaksFor, type Rgb, rampFor, sampleRamp, specFor } from './compute';
import { FLOOR_DB, SPEC_COLS, WAVE_COLS } from './constants';

/** Colour of a role not in the stylesheet. */
const FALLBACK_RGB: Rgb = [128, 136, 150];

/** A role's colour from the stylesheet's `--role-<role>-rgb`. */
export function roleRgb(role: string): Rgb {
  const name = role || 'other';
  const raw = getComputedStyle(document.documentElement)
    .getPropertyValue(`--role-${name}-rgb`)
    .trim();
  const parts = raw
    .split(/[\s,]+/)
    .map(Number)
    .filter((n) => raw !== '' && Number.isFinite(n));
  return parts.length === 3 ? (parts as Rgb) : FALLBACK_RGB;
}

export const rgba = (rgb: Rgb, a: number): string => `rgba(${rgb[0]},${rgb[1]},${rgb[2]},${a})`;

/** Blind mode has no roles to show; that is the answer it withholds. */
const traceRgb = (key: string, blind: boolean): Rgb =>
  roleRgb(blind ? '' : roleOf(getState(), key));

type Surface = HTMLCanvasElement | OffscreenCanvas;
type Ctx = CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;

/** One cached picture and the signature of what it shows. */
export class Layer {
  sig = '';
  cv: Surface | null = null;

  /** The painted surface for `sig`, repainting only when it or the size changed. */
  get(w: number, h: number, sig: string, paint: (c: Ctx) => void): Surface {
    const fits = this.cv !== null && this.cv.width === w && this.cv.height === h;
    if (this.cv && fits && this.sig === sig) {
      return this.cv;
    }
    let cv = this.cv;
    if (!cv || !fits) {
      cv = makeOffscreen(w, h);
    }
    paint(cv.getContext('2d') as Ctx);
    this.cv = cv;
    this.sig = sig;
    return cv;
  }
}

function makeOffscreen(w: number, h: number): Surface {
  if (typeof OffscreenCanvas === 'function') {
    return new OffscreenCanvas(w, h);
  }
  const cv = document.createElement('canvas');
  cv.width = w;
  cv.height = h;
  return cv;
}

const dpr = (): number => window.devicePixelRatio || 1;

/** Size the backing store to the layout size at the device pixel ratio. */
export function fitCanvas(cv: HTMLCanvasElement): { w: number; h: number } {
  const w = Math.max(1, Math.round(cv.clientWidth * dpr()));
  const h = Math.max(1, Math.round(cv.clientHeight * dpr()));
  if (cv.width !== w) {
    cv.width = w;
  }
  if (cv.height !== h) {
    cv.height = h;
  }
  return { w, h };
}

/** Seconds under a pointer, clamped to the take. */
export function secondsAt(cv: HTMLElement, clientX: number, duration: number): number {
  const r = cv.getBoundingClientRect();
  return Math.max(0, Math.min(1, (clientX - r.left) / r.width)) * duration;
}

/** The marked passage: the rest of the take is taken back, so what plays is what is lit. */
function drawPassage(g: Ctx, w: number, h: number, take: Take, region: [number, number] | null) {
  if (!region) {
    return;
  }
  const x0 = (region[0] / take.duration) * w;
  const x1 = (region[1] / take.duration) * w;
  g.fillStyle = 'rgba(16,18,22,0.58)';
  g.fillRect(0, 0, x0, h);
  g.fillRect(x1, 0, w - x1, h);
  g.strokeStyle = 'rgba(219,224,232,0.32)';
  g.lineWidth = Math.max(1, dpr());
  for (const x of [x0, x1]) {
    g.beginPath();
    g.moveTo(x, 0);
    g.lineTo(x, h);
    g.stroke();
  }
}

function drawPlayhead(g: Ctx, w: number, h: number, take: Take, at: number) {
  const px = Math.max(1, dpr());
  const x = (at / take.duration) * w;
  g.strokeStyle = 'rgba(248,246,242,0.92)';
  g.lineWidth = px;
  g.beginPath();
  g.moveTo(x, 0);
  g.lineTo(x, h);
  g.stroke();
  // A grip on it, because a hairline does not look like anything that can be dragged.
  const s = 4.5 * px;
  g.fillStyle = 'rgba(248,246,242,0.92)';
  g.beginPath();
  g.moveTo(x - s, 0);
  g.lineTo(x + s, 0);
  g.lineTo(x, s * 1.5);
  g.closePath();
  g.fill();
}

function paintWave(c: Ctx, w: number, h: number, take: Take, active: string, blind: boolean) {
  c.clearRect(0, 0, w, h);
  const mid = h / 2;
  const cols = Math.min(w, WAVE_COLS);
  // Inactive versions first and dimmed, so the active one is never hidden behind another.
  const order = [...take.keys.filter((k) => k !== active), active];
  for (const key of order) {
    const buf = bufferOf(key);
    if (!buf) {
      continue;
    }
    const p = peaksFor(take.epoch, key, buf, cols);
    c.fillStyle = rgba(traceRgb(key, blind), key === active ? 0.95 : 0.22);
    const bw = Math.max(1, w / cols);
    for (let x = 0; x < cols; x++) {
      const y0 = mid - (p.hi[x] as number) * mid * 0.94;
      const y1 = mid - (p.lo[x] as number) * mid * 0.94;
      c.fillRect((x / cols) * w, y0, bw, Math.max(1, y1 - y0));
    }
  }
  c.strokeStyle = 'rgba(215,219,226,0.18)';
  c.beginPath();
  c.moveTo(0, mid);
  c.lineTo(w, mid);
  c.stroke();
  // Every strike gets a tick; a label only where the last one has cleared.
  const px = dpr();
  c.font = `${10 * px}px ui-monospace, SFMono-Regular, monospace`;
  c.textBaseline = 'top';
  let freeAt = 0;
  for (const hit of takeHits(getState())) {
    const x = (hit.start / take.duration) * w;
    c.strokeStyle = 'rgba(232,226,212,0.30)';
    c.beginPath();
    c.moveTo(x, 0);
    c.lineTo(x, 14 * px);
    c.stroke();
    if (x < freeAt) {
      continue;
    }
    const text = `#${hit.n} ${hit.notes.map((n) => n.note).join('+')}`;
    c.fillStyle = 'rgba(232,226,212,0.72)';
    c.fillText(text, x + 3 * px, 2 * px);
    freeAt = x + 3 * px + c.measureText(text).width + 8 * px;
  }
}

/** Redraw the waveform canvas with the playhead at `at` seconds. */
export function drawWave(cv: HTMLCanvasElement, layer: Layer, at: number): void {
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!g) {
    return;
  }
  const s = getState();
  if (!s.take) {
    g.clearRect(0, 0, w, h);
    return;
  }
  const take = s.take;
  const active = activeKey(s);
  const sig = `${take.epoch}|${active}|${s.blind}|${w}x${h}`;
  const off = layer.get(w, h, sig, (c) => paintWave(c, w, h, take, active, s.blind));
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPassage(g, w, h, take, s.region);
  drawPlayhead(g, w, h, take, at);
}

function paintSpec(c: Ctx, w: number, h: number, take: Take, key: string, blind: boolean) {
  const buf = bufferOf(key);
  if (!buf) {
    c.clearRect(0, 0, w, h);
    return;
  }
  const s = specFor(take.epoch, key, buf, Math.min(w, SPEC_COLS));
  const stops = rampFor(traceRgb(key, blind));
  const nyq = s.sampleRate / 2;
  const lmin = Math.log(40);
  const lmax = Math.log(nyq);
  const img = c.createImageData(w, h);
  // One row per canvas row, taking the loudest bin in its band: averaging would smear a partial.
  const rowBin = new Int32Array(h + 1);
  for (let y = 0; y <= h; y++) {
    const f = Math.exp(lmax - (y / h) * (lmax - lmin));
    rowBin[y] = Math.min(s.bins - 1, Math.max(0, Math.round((f / nyq) * s.bins)));
  }
  for (let x = 0; x < w; x++) {
    const f = Math.min(s.frames - 1, Math.floor((x / w) * s.frames));
    const row = f * s.bins;
    for (let y = 0; y < h; y++) {
      const ra = rowBin[y] as number;
      const rb = rowBin[y + 1] as number;
      const b0 = Math.min(ra, rb);
      const b1 = Math.max(ra, rb);
      let m = 0;
      for (let b = b0; b <= b1; b++) {
        const v = s.mag[row + b] as number;
        if (v > m) {
          m = v;
        }
      }
      const u = Math.max(0, Math.min(1, (levelDb(m) - FLOOR_DB) / -FLOOR_DB));
      const [r, gg, bb] = sampleRamp(stops, u);
      const o = (y * w + x) * 4;
      img.data[o] = r;
      img.data[o + 1] = gg;
      img.data[o + 2] = bb;
      img.data[o + 3] = 255;
    }
  }
  c.putImageData(img, 0, 0);
  const px = dpr();
  c.strokeStyle = 'rgba(215,219,226,0.12)';
  c.fillStyle = 'rgba(215,219,226,0.55)';
  c.font = `${11 * px}px ui-monospace, monospace`;
  for (const f of [100, 1000, 10000]) {
    if (f >= nyq) {
      continue;
    }
    const y = h - ((Math.log(f) - lmin) / (lmax - lmin)) * h;
    c.beginPath();
    c.moveTo(0, y);
    c.lineTo(w, y);
    c.stroke();
    c.fillText(f >= 1000 ? `${f / 1000}k` : String(f), 4 * px, y - 3 * px);
  }
}

/** Redraw the spectrogram canvas with the playhead at `at` seconds. */
export function drawSpec(cv: HTMLCanvasElement, layer: Layer, at: number): void {
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!g) {
    return;
  }
  const s = getState();
  if (!s.take) {
    g.clearRect(0, 0, w, h);
    return;
  }
  const take = s.take;
  const key = activeKey(s);
  const sig = `${take.epoch}|${key}|${s.blind}|${w}x${h}`;
  const off = layer.get(w, h, sig, (c) => paintSpec(c, w, h, take, key, s.blind));
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPassage(g, w, h, take, s.region);
  drawPlayhead(g, w, h, take, at);
}
