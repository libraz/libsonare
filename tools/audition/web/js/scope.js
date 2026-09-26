/* The two pictures: the waveform of every version at once, and a spectrogram of
 * the one sounding.
 *
 * Both are drawn once into an offscreen canvas and blitted each frame with only
 * the playhead on top. Recomputing either per frame is what turns a listening
 * tool into a slideshow.
 *
 * COLOUR IS THE ANSWER TO "WHICH ONE IS THIS". Every trace is drawn in its own
 * role's colour and the spectrogram's ramp is built from the sounding version's,
 * so the pictures say model-or-reference without anyone reading a label. With
 * one accent for all of them, the waveform showed which trace was selected and
 * nothing at all about which side of the comparison it was.
 */

'use strict';

import {
  $, state, roleOf, roleRgb, rgba, SPEC_FFT, SPEC_HOP, SPEC_FLOOR_DB,
} from './state.js';
import { activeKey, playhead, takeHits } from './player.js';

const layers = { wave: { sig: '', cv: null }, spec: { sig: '', cv: null } };

function fitCanvas(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(cv.clientWidth * dpr));
  const h = Math.max(1, Math.round(cv.clientHeight * dpr));
  if (cv.width !== w) cv.width = w;
  if (cv.height !== h) cv.height = h;
  return { w, h };
}

function makeOffscreen(w, h) {
  if (typeof OffscreenCanvas === 'function') return new OffscreenCanvas(w, h);
  const cv = document.createElement('canvas');
  cv.width = w;
  cv.height = h;
  return cv;
}

function layer(name, w, h, sig, paint) {
  const l = layers[name];
  if (l.sig === sig && l.cv && l.cv.width === w && l.cv.height === h) return l.cv;
  const cv = l.cv && l.cv.width === w && l.cv.height === h ? l.cv : makeOffscreen(w, h);
  paint(cv.getContext('2d'), w, h);
  l.cv = cv;
  l.sig = sig;
  return cv;
}

/// Blind mode has no roles to show — that is the answer it is withholding.
const traceRgb = (key) => roleRgb(state.blind ? '' : roleOf(key));

function peaksFor(key, cols) {
  const cache = state.take.peaks;
  const id = `${key}:${cols}`;
  if (cache[id]) return cache[id];
  const buf = state.take.buffers[key];
  const ch = buf.getChannelData(0);
  const ch2 = buf.numberOfChannels > 1 ? buf.getChannelData(1) : null;
  const per = ch.length / cols;
  const lo = new Float32Array(cols);
  const hi = new Float32Array(cols);
  for (let x = 0; x < cols; x++) {
    const a = Math.floor(x * per);
    const b = Math.min(ch.length, Math.floor((x + 1) * per));
    let mn = 0, mx = 0;
    for (let i = a; i < b; i++) {
      const v = ch2 ? (ch[i] + ch2[i]) * 0.5 : ch[i];
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    lo[x] = mn; hi[x] = mx;
  }
  cache[id] = { lo, hi, cols };
  return cache[id];
}

export function drawWave() {
  const cv = $('wave');
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!state.take) { g.clearRect(0, 0, w, h); return; }
  const active = activeKey();
  const sig = `${state.take.id}|${active}|${state.blind}|${w}x${h}`;
  const off = layer('wave', w, h, sig, (c) => {
    c.clearRect(0, 0, w, h);
    const mid = h / 2;
    const cols = Math.min(w, 3000);
    // The inactive versions first and dimmed, so the active one is never hidden
    // behind a louder take that happens to be selected somewhere else.
    const order = [...state.take.keys.filter((k) => k !== active), active];
    for (const key of order) {
      const p = peaksFor(key, cols);
      c.fillStyle = rgba(traceRgb(key), key === active ? 0.95 : 0.22);
      const bw = Math.max(1, w / cols);
      for (let x = 0; x < cols; x++) {
        const y0 = mid - p.hi[x] * mid * 0.94;
        const y1 = mid - p.lo[x] * mid * 0.94;
        c.fillRect((x / cols) * w, y0, bw, Math.max(1, y1 - y0));
      }
    }
    c.strokeStyle = 'rgba(215,219,226,0.18)';
    c.beginPath(); c.moveTo(0, mid); c.lineTo(w, mid); c.stroke();
    // Every strike gets a tick; a label only where the last one has cleared.
    // The musical take is seventy-six strikes in thirteen seconds and labelling
    // all of them writes one illegible band across the top of the waveform,
    // which loses the sparse takes' labels as well as its own.
    const dpr = window.devicePixelRatio || 1;
    c.font = `${10 * dpr}px ui-monospace, SFMono-Regular, monospace`;
    c.textBaseline = 'top';
    let freeAt = 0;
    for (const hit of takeHits()) {
      const x = (hit.start / state.take.duration) * w;
      c.strokeStyle = 'rgba(232,226,212,0.30)';
      c.beginPath(); c.moveTo(x, 0); c.lineTo(x, 14 * dpr); c.stroke();
      const text = `#${hit.n} ${hit.notes.map((n) => n.note).join('+')}`;
      if (x < freeAt) continue;
      c.fillStyle = 'rgba(232,226,212,0.72)';
      c.fillText(text, x + 3 * dpr, 2 * dpr);
      freeAt = x + 3 * dpr + c.measureText(text).width + 8 * dpr;
    }
  });
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPassage(g, w, h);
  drawPlayhead(g, w, h);
}

/* The marked passage, drawn by taking the rest of the take back rather than by
 * tinting it: what plays is what is lit, which needs no legend. Neutral,
 * because teal and amber mean model and reference everywhere on this page and a
 * passage is neither.
 *
 * On the blit rather than into the cached layer. It moves under the pointer
 * through a whole drag, and both layers cost a full repaint — the spectrogram
 * a per-pixel one — to change a rectangle that is two fills.
 */
function drawPassage(g, w, h) {
  if (!state.region) return;
  const [a, b] = state.region;
  const x0 = (a / state.take.duration) * w;
  const x1 = (b / state.take.duration) * w;
  g.fillStyle = 'rgba(16,18,22,0.58)';
  g.fillRect(0, 0, x0, h);
  g.fillRect(x1, 0, w - x1, h);
  g.strokeStyle = 'rgba(219,224,232,0.32)';
  g.lineWidth = Math.max(1, window.devicePixelRatio || 1);
  for (const x of [x0, x1]) {
    g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
  }
}

function drawPlayhead(g, w, h) {
  const dpr = Math.max(1, window.devicePixelRatio || 1);
  const x = (playhead() / state.take.duration) * w;
  g.strokeStyle = 'rgba(248,246,242,0.92)';
  g.lineWidth = dpr;
  g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
  // A grip on it, because it can be dragged and a hairline does not look like
  // anything that can be.
  const s = 4.5 * dpr;
  g.fillStyle = 'rgba(248,246,242,0.92)';
  g.beginPath();
  g.moveTo(x - s, 0); g.lineTo(x + s, 0); g.lineTo(x, s * 1.5);
  g.closePath();
  g.fill();
}

/* --- FFT: iterative radix-2, in place on split real/imaginary arrays --- */

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const wr = Math.cos(ang), wi = Math.sin(ang);
    const half = len >> 1;
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < half; k++) {
        const ur = re[i + k], ui = im[i + k];
        const xr = re[i + k + half], xi = im[i + k + half];
        const vr = xr * cr - xi * ci;
        const vi = xr * ci + xi * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + half] = ur - vr; im[i + k + half] = ui - vi;
        const nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr; cr = nr;
      }
    }
  }
}

function specFor(key, cols) {
  const cache = state.take.specs;
  const id = `${key}:${cols}`;
  if (cache[id]) return cache[id];
  const buf = state.take.buffers[key];
  const ch = buf.getChannelData(0);
  const ch2 = buf.numberOfChannels > 1 ? buf.getChannelData(1) : null;
  const bins = SPEC_FFT / 2;
  const win = new Float32Array(SPEC_FFT);
  for (let i = 0; i < SPEC_FFT; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / SPEC_FFT);

  const frames = Math.max(1, Math.floor((ch.length - SPEC_FFT) / SPEC_HOP) + 1);
  const step = Math.max(1, Math.floor(frames / cols));
  const used = Math.max(1, Math.floor(frames / step));
  const mag = new Float32Array(used * bins);
  const re = new Float32Array(SPEC_FFT), im = new Float32Array(SPEC_FFT);

  for (let f = 0; f < used; f++) {
    const base = f * step * SPEC_HOP;
    for (let i = 0; i < SPEC_FFT; i++) {
      const s = base + i;
      const v = s < ch.length ? (ch2 ? (ch[s] + ch2[s]) * 0.5 : ch[s]) : 0;
      re[i] = v * win[i]; im[i] = 0;
    }
    fft(re, im);
    for (let b = 0; b < bins; b++) mag[f * bins + b] = Math.hypot(re[b], im[b]);
  }
  cache[id] = { frames: used, bins, mag, sampleRate: buf.sampleRate };
  return cache[id];
}

/* An ordered ramp built from the sounding version's own role colour: dark, to
 * that colour, to near-white. Ordered rather than a rainbow, so a level reads
 * as a level; role-tinted rather than fixed, so flipping between the model and
 * the reference recolours the picture and there is never a moment where two
 * screenshots of the same take are indistinguishable. */
function rampFor(rgb) {
  const mix = (a, b, u) => a.map((v, i) => v + (b[i] - v) * u);
  const bg = [18, 20, 24];
  const white = [246, 242, 236];
  return [
    [0.00, bg],
    [0.34, mix(bg, rgb, 0.45)],
    [0.62, rgb],
    [0.86, mix(rgb, white, 0.60)],
    [1.00, white],
  ];
}

function sampleRamp(stops, u) {
  for (let i = 1; i < stops.length; i++) {
    if (u <= stops[i][0]) {
      const [p0, c0] = stops[i - 1];
      const [p1, c1] = stops[i];
      const k = (u - p0) / (p1 - p0);
      return [c0[0] + (c1[0] - c0[0]) * k,
        c0[1] + (c1[1] - c0[1]) * k,
        c0[2] + (c1[2] - c0[2]) * k];
    }
  }
  return stops[stops.length - 1][1];
}

export function drawSpec() {
  const cv = $('spec');
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!state.take) { g.clearRect(0, 0, w, h); return; }
  const key = activeKey();
  const sig = `${state.take.id}|${key}|${state.blind}|${w}x${h}`;
  const off = layer('spec', w, h, sig, (c) => {
    const s = specFor(key, Math.min(w, 1400));
    const stops = rampFor(traceRgb(key));
    const nyq = s.sampleRate / 2;
    const lmin = Math.log(40), lmax = Math.log(nyq);
    const img = c.createImageData(w, h);
    // One image row per canvas row, taking the loudest bin in that row's band:
    // low on a log axis a row spans many bins, and averaging there smears a
    // partial into the gap beside it.
    const rowBin = new Int32Array(h + 1);
    for (let y = 0; y <= h; y++) {
      const f = Math.exp(lmax - (y / h) * (lmax - lmin));
      rowBin[y] = Math.min(s.bins - 1, Math.max(0, Math.round(f / nyq * s.bins)));
    }
    for (let x = 0; x < w; x++) {
      const f = Math.min(s.frames - 1, Math.floor(x / w * s.frames));
      const row = f * s.bins;
      for (let y = 0; y < h; y++) {
        const b0 = Math.min(rowBin[y], rowBin[y + 1]);
        const b1 = Math.max(rowBin[y], rowBin[y + 1]);
        let m = 0;
        for (let b = b0; b <= b1; b++) { const v = s.mag[row + b]; if (v > m) m = v; }
        const db = 20 * Math.log10(m / (SPEC_FFT / 4) + 1e-9);
        const u = Math.max(0, Math.min(1, (db - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB));
        const [r, gg, bb] = sampleRamp(stops, u);
        const o = (y * w + x) * 4;
        img.data[o] = r; img.data[o + 1] = gg; img.data[o + 2] = bb; img.data[o + 3] = 255;
      }
    }
    c.putImageData(img, 0, 0);
    const dpr = window.devicePixelRatio || 1;
    c.strokeStyle = 'rgba(215,219,226,0.12)';
    c.fillStyle = 'rgba(215,219,226,0.55)';
    c.font = `${11 * dpr}px ui-monospace, monospace`;
    for (const f of [100, 1000, 10000]) {
      if (f >= nyq) continue;
      const y = h - ((Math.log(f) - lmin) / (lmax - lmin)) * h;
      c.beginPath(); c.moveTo(0, y); c.lineTo(w, y); c.stroke();
      c.fillText(f >= 1000 ? `${f / 1000}k` : String(f), 4 * dpr, y - 3 * dpr);
    }
  });
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPassage(g, w, h);
  drawPlayhead(g, w, h);
}

export const seekFromEvent = (cv, ev) => {
  const r = cv.getBoundingClientRect();
  return Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width)) * state.take.duration;
};
