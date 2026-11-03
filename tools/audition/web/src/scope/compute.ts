/* The numbers behind the two pictures: min/max peaks per column and a magnitude spectrogram.
 *
 * Both are cached per (take epoch, version, columns): two sets share take ids, and a take loaded
 * again is a new decode, so the epoch rather than the id is what says the buffers are the same.
 */

import { FFT_SIZE, HOP_SIZE } from './constants';

/** The slice of an AudioBuffer the pictures read. */
export type Pcm = Pick<AudioBuffer, 'getChannelData' | 'numberOfChannels' | 'sampleRate'>;

export interface Peaks {
  lo: Float32Array;
  hi: Float32Array;
  cols: number;
}

export interface Spectrogram {
  frames: number;
  bins: number;
  /** `frames * bins` magnitudes, frame-major. */
  mag: Float32Array;
  sampleRate: number;
}

const peakCache = new Map<string, Peaks>();
const specCache = new Map<string, Spectrogram>();
let cacheEpoch = -1;

/** Drop everything cached; a new epoch does this by itself. */
export function clearScopeCache(): void {
  peakCache.clear();
  specCache.clear();
  cacheEpoch = -1;
}

function enter(epoch: number): void {
  if (epoch !== cacheEpoch) {
    peakCache.clear();
    specCache.clear();
    cacheEpoch = epoch;
  }
}

/** Mono sample reader: the mean of both channels when there are two. */
function mono(buf: Pcm): (i: number) => number {
  const a = buf.getChannelData(0);
  if (buf.numberOfChannels > 1) {
    const b = buf.getChannelData(1);
    return (i) => ((a[i] ?? 0) + (b[i] ?? 0)) * 0.5;
  }
  return (i) => a[i] ?? 0;
}

/** The lowest and highest sample in each of `cols` equal slices. */
export function computePeaks(buf: Pcm, cols: number): Peaks {
  const read = mono(buf);
  const length = buf.getChannelData(0).length;
  const per = length / cols;
  const lo = new Float32Array(cols);
  const hi = new Float32Array(cols);
  for (let x = 0; x < cols; x++) {
    const a = Math.floor(x * per);
    const b = Math.min(length, Math.floor((x + 1) * per));
    let mn = 0;
    let mx = 0;
    for (let i = a; i < b; i++) {
      const v = read(i);
      if (v < mn) {
        mn = v;
      }
      if (v > mx) {
        mx = v;
      }
    }
    lo[x] = mn;
    hi[x] = mx;
  }
  return { lo, hi, cols };
}

export function peaksFor(epoch: number, key: string, buf: Pcm, cols: number): Peaks {
  enter(epoch);
  const id = `${key}:${cols}`;
  let hit = peakCache.get(id);
  if (!hit) {
    hit = computePeaks(buf, cols);
    peakCache.set(id, hit);
  }
  return hit;
}

/** Iterative radix-2 FFT in place on split real/imaginary arrays. */
export function fft(re: Float32Array, im: Float32Array): void {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j] as number, re[i] as number];
      [im[i], im[j]] = [im[j] as number, im[i] as number];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wr = Math.cos(ang);
    const wi = Math.sin(ang);
    const half = len >> 1;
    for (let i = 0; i < n; i += len) {
      let cr = 1;
      let ci = 0;
      for (let k = 0; k < half; k++) {
        const p = i + k;
        const q = p + half;
        const ur = re[p] as number;
        const ui = im[p] as number;
        const xr = re[q] as number;
        const xi = im[q] as number;
        const vr = xr * cr - xi * ci;
        const vi = xr * ci + xi * cr;
        re[p] = ur + vr;
        im[p] = ui + vi;
        re[q] = ur - vr;
        im[q] = ui - vi;
        const nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nr;
      }
    }
  }
}

/** Hann-windowed magnitudes, at most about `cols` frames spread over the whole buffer. */
export function computeSpectrogram(buf: Pcm, cols: number): Spectrogram {
  const read = mono(buf);
  const length = buf.getChannelData(0).length;
  const bins = FFT_SIZE / 2;
  const win = new Float32Array(FFT_SIZE);
  for (let i = 0; i < FFT_SIZE; i++) {
    win[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / FFT_SIZE);
  }
  const frames = Math.max(1, Math.floor((length - FFT_SIZE) / HOP_SIZE) + 1);
  const step = Math.max(1, Math.floor(frames / cols));
  const used = Math.max(1, Math.floor(frames / step));
  const mag = new Float32Array(used * bins);
  const re = new Float32Array(FFT_SIZE);
  const im = new Float32Array(FFT_SIZE);
  for (let f = 0; f < used; f++) {
    const base = f * step * HOP_SIZE;
    for (let i = 0; i < FFT_SIZE; i++) {
      const s = base + i;
      re[i] = (s < length ? read(s) : 0) * (win[i] as number);
      im[i] = 0;
    }
    fft(re, im);
    for (let b = 0; b < bins; b++) {
      mag[f * bins + b] = Math.hypot(re[b] as number, im[b] as number);
    }
  }
  return { frames: used, bins, mag, sampleRate: buf.sampleRate };
}

export function specFor(epoch: number, key: string, buf: Pcm, cols: number): Spectrogram {
  enter(epoch);
  const id = `${key}:${cols}`;
  let hit = specCache.get(id);
  if (!hit) {
    hit = computeSpectrogram(buf, cols);
    specCache.set(id, hit);
  }
  return hit;
}

export type Rgb = [number, number, number];
type Stop = [number, Rgb];

/** dark, to the role's colour, to near-white: ordered so a level reads as a level, tinted so the role shows. */
export function rampFor(rgb: Rgb): Stop[] {
  const mix = (a: Rgb, b: Rgb, u: number): Rgb => [
    a[0] + (b[0] - a[0]) * u,
    a[1] + (b[1] - a[1]) * u,
    a[2] + (b[2] - a[2]) * u,
  ];
  const bg: Rgb = [18, 20, 24];
  const white: Rgb = [246, 242, 236];
  return [
    [0.0, bg],
    [0.34, mix(bg, rgb, 0.45)],
    [0.62, rgb],
    [0.86, mix(rgb, white, 0.6)],
    [1.0, white],
  ];
}

export function sampleRamp(stops: Stop[], u: number): Rgb {
  for (let i = 1; i < stops.length; i++) {
    const [p1, c1] = stops[i] as Stop;
    if (u <= p1) {
      const [p0, c0] = stops[i - 1] as Stop;
      const k = (u - p0) / (p1 - p0);
      return [
        c0[0] + (c1[0] - c0[0]) * k,
        c0[1] + (c1[1] - c0[1]) * k,
        c0[2] + (c1[2] - c0[2]) * k,
      ];
    }
  }
  return (stops[stops.length - 1] as Stop)[1];
}

/** A level in decibels relative to a full-scale sine's bin magnitude. */
export const levelDb = (mag: number): number => 20 * Math.log10(mag / (FFT_SIZE / 4) + 1e-9);
