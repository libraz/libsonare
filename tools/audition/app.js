/* audition — compare several renders of the same take from one transport.
 *
 * Every version of the selected take is decoded up front and played
 * simultaneously; only the gains differ, so a switch can be a gain change on a
 * sound that never stopped. That keeps the two versions sample-aligned, which
 * is the only way to compare a sustain or a decay.
 *
 * It is not what you want for an attack, though: switch four seconds into a
 * phrase and the new version's onset is already behind you, and a take is far
 * too long to wait out — the ear loses what it was holding. So "switch
 * restarts" (on by default) makes a switch seek back to the start instead, and
 * the sample-aligned form stays one keystroke away.
 *
 * The waveform and the spectrogram are drawn once into offscreen canvases and
 * blitted each frame with only the playhead on top. Recomputing either one per
 * frame is what turns a listening tool into a slideshow.
 */

'use strict';

const $ = (id) => document.getElementById(id);

const state = {
  sets: [],            // [{ id, title, takes, compare }]
  setId: null,         // the one being listened to
  base: '',            // URL prefix its audio is under
  compare: true,       // false once a set turns out to hold one version per take
  manifest: null,
  items: [],
  itemIndex: 0,
  versionIndex: 0,
  take: null,          // { id, keys[], buffers{}, rms{}, duration, specs{}, peaks{} }
  ctx: null,
  sources: [],
  gains: {},
  master: null,
  playing: false,
  startedAt: 0,        // ctx.currentTime when the sources started
  startOffset: 0,      // buffer seconds at that moment
  loop: false,
  region: null,        // [a, b] in seconds
  blind: false,
  blindOrder: [],      // slot -> real version index
  picks: {},           // itemId -> { slot, key, revealed }
  notes: {},
};

const SWITCH_RAMP = 0.006;   // seconds: instant to the ear, long enough not to click
const SPEC_FFT = 1024;
const SPEC_HOP = 512;
const SPEC_FLOOR_DB = -78;

const layers = { wave: { sig: '', cv: null }, spec: { sig: '', cv: null } };

/* ------------------------------------------------------------------ audio */

function audio() {
  if (!state.ctx) {
    state.ctx = new (window.AudioContext || window.webkitAudioContext)();
    state.master = state.ctx.createGain();
    state.master.gain.value = 1;
    state.master.connect(state.ctx.destination);
  }
  return state.ctx;
}

async function loadTake(item) {
  const ctx = audio();
  const keys = Object.keys(item.tracks);
  const buffers = {};
  const rms = {};
  await Promise.all(keys.map(async (k) => {
    const url = state.base + item.tracks[k];
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${url}: ${res.status}`);
    const buf = await ctx.decodeAudioData(await res.arrayBuffer());
    buffers[k] = buf;
    rms[k] = bufferRms(buf);
  }));
  const duration = Math.max(...keys.map((k) => buffers[k].duration));
  return { id: item.id, keys, buffers, rms, duration, specs: {}, peaks: {} };
}

function bufferRms(buf) {
  let sum = 0, n = 0;
  for (let c = 0; c < buf.numberOfChannels; c++) {
    const d = buf.getChannelData(c);
    // Every 7th sample: this estimate only has to be good enough to match two
    // renders in level, and reading every sample of every take would make
    // selecting one feel slow for a tenth of a decibel nobody hears.
    for (let i = 0; i < d.length; i += 7) { sum += d[i] * d[i]; n++; }
  }
  return Math.sqrt(sum / Math.max(n, 1));
}

function targetGain(key) {
  if (!$('matchRms').checked) return 1;
  const vals = state.take.keys.map((k) => state.take.rms[k]).filter((v) => v > 0);
  if (!vals.length) return 1;
  const ref = vals.reduce((a, b) => a + b, 0) / vals.length;
  const r = state.take.rms[key];
  return r > 0 ? Math.min(ref / r, 8) : 1;
}

function activeKey() {
  const idx = state.blind ? state.blindOrder[state.versionIndex] : state.versionIndex;
  return state.take.keys[idx];
}

function applyGains(immediate) {
  const ctx = audio();
  const now = ctx.currentTime;
  const active = activeKey();
  for (const k of state.take.keys) {
    const g = state.gains[k];
    if (!g) continue;
    const want = k === active ? targetGain(k) : 0;
    g.gain.cancelScheduledValues(now);
    if (immediate) {
      g.gain.setValueAtTime(want, now);
    } else {
      g.gain.setValueAtTime(g.gain.value, now);
      g.gain.linearRampToValueAtTime(want, now + SWITCH_RAMP);
    }
  }
  renderLevels();
}

function stopSources() {
  for (const s of state.sources) { try { s.stop(); } catch { /* already ended */ } }
  state.sources = [];
}

function startAt(offset) {
  const ctx = audio();
  stopSources();
  state.gains = {};
  const [a, b] = state.region || [0, state.take.duration];
  const looping = state.loop && b - a > 0.02;
  const from = looping ? Math.min(Math.max(offset, a), b - 0.001) : Math.max(0, offset);

  for (const k of state.take.keys) {
    const src = ctx.createBufferSource();
    src.buffer = state.take.buffers[k];
    const g = ctx.createGain();
    g.gain.value = 0;
    src.connect(g).connect(state.master);
    if (looping) { src.loop = true; src.loopStart = a; src.loopEnd = b; }
    src.start(0, from);
    state.sources.push(src);
    state.gains[k] = g;
  }
  state.startedAt = ctx.currentTime;
  state.startOffset = from;
  state.playing = true;
  applyGains(true);
  $('playBtn').textContent = 'pause';
  $('playBtn').setAttribute('aria-pressed', 'true');
}

function pause() {
  const at = playhead();
  stopSources();
  state.startOffset = at;
  state.playing = false;
  $('playBtn').textContent = 'play';
  $('playBtn').removeAttribute('aria-pressed');
}

function playhead() {
  if (!state.take) return 0;
  if (!state.playing) return state.startOffset;
  const t = state.startOffset + (audio().currentTime - state.startedAt);
  const [a, b] = state.region || [0, state.take.duration];
  if (state.loop && b - a > 0.02) return a + ((t - a) % (b - a));
  return Math.min(t, state.take.duration);
}

function togglePlay() {
  if (!state.take) return;
  audio().resume();
  if (state.playing) pause();
  else startAt(state.startOffset >= state.take.duration - 0.01 ? 0 : state.startOffset);
}

/* ------------------------------------------------------------------- data */

const notesKey = () => `audition:notes:${state.setId || 'untitled'}`;
const picksKey = () => `audition:picks:${state.setId || 'untitled'}`;

const SET_KEY = 'audition:set';

async function boot() {
  state.sets = await (await fetch('sets.json')).json();
  if (!state.sets.length) {
    $('title').textContent = 'audition';
    $('notes').textContent =
      'No renders found. Generate a set with tools/voicematch/make_audition.py — '
      + '--model-only needs no plugin — then reload.';
    return;
  }
  buildSetPicker();
  const remembered = localStorage.getItem(SET_KEY);
  const start = state.sets.some((s) => s.id === remembered) ? remembered : state.sets[0].id;
  wire();
  await loadSet(start);
}

const specCaptionText = () => state.compare
  ? 'spectrogram — active version, log frequency'
  : 'spectrogram — log frequency';

/// Switch to a set: its manifest, its takes, and whether it can compare at all.
async function loadSet(id) {
  const entry = state.sets.find((s) => s.id === id) || state.sets[0];
  stopSources();
  state.playing = false;
  $('playBtn').textContent = 'play';
  state.setId = entry.id;
  state.base = `s/${entry.id}/`;
  localStorage.setItem(SET_KEY, entry.id);

  const m = await (await fetch(`${state.base}manifest.json`)).json();
  state.manifest = m;
  state.items = m.items || [];
  // A set whose takes hold one version each has nothing to switch between, so
  // the comparison controls come off rather than sitting there doing nothing.
  // That is the ordinary case for anyone without the reference plugin.
  state.compare = state.items.some((it) => Object.keys(it.tracks || {}).length > 1);
  document.body.classList.toggle('no-compare', !state.compare);
  if (!state.compare) {
    state.blind = false;
    $('blind').checked = false;
  }
  $('waveCaption').textContent = state.compare
    ? 'waveform — all versions overlaid, active one solid'
    : 'waveform';
  $('specCaption').textContent = specCaptionText();

  $('title').textContent = m.title || 'audition';
  $('notes').textContent = m.notes || '';
  document.title = m.title ? `${m.title} — audition` : 'audition';
  // Keyed on the set, so notes taken on one instrument do not surface on another.
  state.notes = JSON.parse(localStorage.getItem(notesKey()) || '{}');
  state.picks = JSON.parse(localStorage.getItem(picksKey()) || '{}');
  [...$('sets').children].forEach((b) =>
    b.setAttribute('aria-pressed', String(b.dataset.set === state.setId)));

  state.itemIndex = 0;
  state.versionIndex = 0;
  buildTakeList();
  if (state.items.length) await selectTake(0);
}

function buildSetPicker() {
  const box = $('sets');
  box.replaceChildren();
  // One set needs no picker; the title already says which it is.
  box.hidden = state.sets.length < 2;
  if (box.hidden) return;
  state.sets.forEach((s) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.dataset.set = s.id;
    b.textContent = s.title || s.id;
    if (!s.compare) {
      b.appendChild(Object.assign(document.createElement('span'),
        { className: 'tag', textContent: 'no reference' }));
    }
    b.addEventListener('click', () => loadSet(s.id));
    box.appendChild(b);
  });
}

function buildTakeList() {
  const nav = $('takes');
  nav.replaceChildren();
  let group = null;
  state.items.forEach((item, i) => {
    if (item.group && item.group !== group) {
      group = item.group;
      const h = document.createElement('div');
      h.className = 'group';
      h.textContent = group;
      nav.appendChild(h);
    }
    const b = document.createElement('button');
    b.type = 'button';
    b.appendChild(document.createTextNode(item.label || item.id));
    if (item.sub) {
      const s = document.createElement('span');
      s.className = 'sub';
      s.textContent = item.sub;
      b.appendChild(s);
    }
    b.addEventListener('click', () => selectTake(i));
    nav.appendChild(b);
  });
}

function markTakeList() {
  const buttons = [...$('takes').querySelectorAll('button')];
  buttons.forEach((b, i) => {
    if (i === state.itemIndex) {
      b.setAttribute('aria-current', 'true');
      b.scrollIntoView({ block: 'nearest' });
    } else {
      b.removeAttribute('aria-current');
    }
  });
}

async function selectTake(i) {
  if (i < 0 || i >= state.items.length) return;
  const wasPlaying = state.playing;
  pause();
  state.itemIndex = i;
  state.startOffset = 0;
  state.region = null;
  markTakeList();
  const item = state.items[i];
  $('specCaption').textContent = 'decoding…';
  $('specCaption').className = 'loading';
  try {
    state.take = await loadTake(item);
  } catch (err) {
    $('specCaption').textContent = `could not load: ${err.message}`;
    state.take = null;
    return;
  }
  $('specCaption').className = '';
  $('specCaption').textContent = specCaptionText();
  reshuffleBlind();
  state.versionIndex = Math.min(state.versionIndex, state.take.keys.length - 1);
  buildVersionButtons();
  $('takeNotes').value = state.notes[item.id] || '';
  renderLevels();
  renderScore();
  if (wasPlaying) startAt(0);
}

function reshuffleBlind() {
  const n = state.take ? state.take.keys.length : 0;
  state.blindOrder = [...Array(n).keys()];
  for (let i = n - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [state.blindOrder[i], state.blindOrder[j]] = [state.blindOrder[j], state.blindOrder[i]];
  }
}

function sourceLabel(key) {
  const src = (state.manifest.sources || {})[key];
  return (src && src.label) || key;
}

function versionLabel(slot) {
  const item = state.items[state.itemIndex];
  const key = state.take.keys[state.blind ? state.blindOrder[slot] : slot];
  if (!state.blind) return sourceLabel(key);
  const letter = String.fromCharCode(65 + slot);
  const pick = state.picks[item.id];
  return pick && pick.revealed ? `${letter} · ${sourceLabel(key)}` : letter;
}

function buildVersionButtons() {
  const box = $('versions');
  box.replaceChildren();
  state.take.keys.forEach((_, slot) => {
    const b = document.createElement('button');
    b.type = 'button';
    const k = document.createElement('span');
    k.className = 'key';
    k.textContent = String(slot + 1);
    b.appendChild(k);
    b.appendChild(document.createTextNode(versionLabel(slot)));
    b.setAttribute('aria-pressed', String(slot === state.versionIndex));
    b.addEventListener('click', () => setVersion(slot));
    box.appendChild(b);
  });
}

function setVersion(slot) {
  if (!state.take || slot < 0 || slot >= state.take.keys.length) return;
  state.versionIndex = slot;
  if (state.blind) {
    const id = state.items[state.itemIndex].id;
    state.picks[id] = { slot, key: state.take.keys[state.blindOrder[slot]], revealed: false };
    localStorage.setItem(picksKey(), JSON.stringify(state.picks));
    renderScore();
  }
  [...$('versions').children].forEach((b, i) => b.setAttribute('aria-pressed', String(i === slot)));
  // Two ways to switch, and they answer different questions. Crossfading in
  // place keeps the comparison sample-aligned, which is the only way to hear a
  // difference in a sustain or a decay. Restarting throws away that alignment
  // and gives back the attack: switching four seconds into a phrase otherwise
  // means the new version's onset has already gone by, and waiting out the take
  // to hear it is long enough for the ear to lose what it was holding.
  const restart = $('restartOnSwitch').checked;
  if (restart) {
    const from = (state.region || [0])[0];
    if (state.playing) startAt(from);
    else state.startOffset = from;  // the rAF loop redraws the playhead
  } else if (state.playing) {
    applyGains(false);
  }
  renderLevels();
}

function renderLevels() {
  if (!state.take) return;
  const key = activeKey();
  const db = (v) => (v > 0 ? (20 * Math.log10(v)).toFixed(1) : '-inf');
  const g = targetGain(key);
  const matched = $('matchRms').checked ? `  gain ${(20 * Math.log10(g)).toFixed(1)} dB` : '';
  $('levels').textContent = state.blind
    ? `rms ${db(state.take.rms[key] * g)} dBFS${matched}`
    : `${sourceLabel(key)}  rms ${db(state.take.rms[key])} dBFS${matched}`;
}

function renderScore() {
  if (!state.blind) { $('blindScore').textContent = ''; return; }
  const tally = {};
  let n = 0;
  for (const p of Object.values(state.picks)) {
    if (!p || !p.key) continue;
    tally[p.key] = (tally[p.key] || 0) + 1;
    n++;
  }
  const parts = Object.entries(tally)
    .sort((a, b) => b[1] - a[1])
    .map(([k, v]) => `${sourceLabel(k)} ${v}`);
  $('blindScore').textContent = n ? `preferred: ${parts.join('   ')}` : 'pick a version on each take';
}

/* ------------------------------------------------------------------ draw */

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

function drawWave() {
  const cv = $('wave');
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!state.take) { g.clearRect(0, 0, w, h); return; }
  const active = activeKey();
  const sig = `${state.take.id}|${active}|${w}x${h}|${state.region ? state.region.join(',') : ''}`;
  const off = layer('wave', w, h, sig, (c) => {
    c.clearRect(0, 0, w, h);
    const mid = h / 2;
    const cols = Math.min(w, 3000);
    if (state.region) {
      const [a, b] = state.region;
      c.fillStyle = 'rgba(111,179,160,0.13)';
      c.fillRect((a / state.take.duration) * w, 0, ((b - a) / state.take.duration) * w, h);
    }
    // The inactive versions first and dimmed, so the active one is never hidden
    // behind a louder take that happens to be selected somewhere else.
    const order = [...state.take.keys.filter((k) => k !== active), active];
    for (const key of order) {
      const p = peaksFor(key, cols);
      c.fillStyle = key === active ? 'rgba(111,179,160,0.95)' : 'rgba(133,141,155,0.30)';
      const bw = Math.max(1, w / cols);
      for (let x = 0; x < cols; x++) {
        const y0 = mid - p.hi[x] * mid * 0.94;
        const y1 = mid - p.lo[x] * mid * 0.94;
        c.fillRect((x / cols) * w, y0, bw, Math.max(1, y1 - y0));
      }
    }
    c.strokeStyle = 'rgba(215,219,226,0.18)';
    c.beginPath(); c.moveTo(0, mid); c.lineTo(w, mid); c.stroke();
  });
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPlayhead(g, w, h, 'rgba(232,226,212,0.9)');
}

function drawPlayhead(g, w, h, color) {
  const x = (playhead() / state.take.duration) * w;
  g.strokeStyle = color;
  g.lineWidth = Math.max(1, window.devicePixelRatio || 1);
  g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
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

function specColor(t) {
  // Dark to teal to sand: an ordered two-hue ramp rather than a rainbow, so a
  // level reads as a level instead of as a colour that has to be decoded.
  const stops = [
    [0.00, 20, 22, 26],
    [0.35, 26, 66, 68],
    [0.62, 111, 179, 160],
    [0.85, 208, 154, 90],
    [1.00, 246, 240, 228],
  ];
  for (let i = 1; i < stops.length; i++) {
    if (t <= stops[i][0]) {
      const [p0, r0, g0, b0] = stops[i - 1];
      const [p1, r1, g1, b1] = stops[i];
      const u = (t - p0) / (p1 - p0);
      return [r0 + (r1 - r0) * u, g0 + (g1 - g0) * u, b0 + (b1 - b0) * u];
    }
  }
  return [246, 240, 228];
}

function drawSpec() {
  const cv = $('spec');
  const { w, h } = fitCanvas(cv);
  const g = cv.getContext('2d');
  if (!state.take) { g.clearRect(0, 0, w, h); return; }
  const key = activeKey();
  const sig = `${state.take.id}|${key}|${w}x${h}`;
  const off = layer('spec', w, h, sig, (c) => {
    const s = specFor(key, Math.min(w, 1400));
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
        const t = Math.max(0, Math.min(1, (db - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB));
        const [r, gg, bb] = specColor(t);
        const o = (y * w + x) * 4;
        img.data[o] = r; img.data[o + 1] = gg; img.data[o + 2] = bb; img.data[o + 3] = 255;
      }
    }
    c.putImageData(img, 0, 0);
    c.strokeStyle = 'rgba(215,219,226,0.10)';
    c.fillStyle = 'rgba(215,219,226,0.5)';
    c.font = `${11 * (window.devicePixelRatio || 1)}px ui-monospace, monospace`;
    for (const f of [100, 1000, 10000]) {
      if (f >= nyq) continue;
      const y = h - ((Math.log(f) - lmin) / (lmax - lmin)) * h;
      c.beginPath(); c.moveTo(0, y); c.lineTo(w, y); c.stroke();
      c.fillText(f >= 1000 ? `${f / 1000}k` : String(f), 4, y - 3);
    }
  });
  g.clearRect(0, 0, w, h);
  g.drawImage(off, 0, 0);
  drawPlayhead(g, w, h, 'rgba(232,226,212,0.85)');
}

/* ------------------------------------------------------------------ wire */

function seekFromEvent(cv, ev) {
  const r = cv.getBoundingClientRect();
  return Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width)) * state.take.duration;
}

function wire() {
  $('playBtn').addEventListener('click', togglePlay);

  $('loopBtn').addEventListener('click', () => {
    state.loop = !state.loop;
    $('loopBtn').setAttribute('aria-pressed', String(state.loop));
    if (state.playing) startAt(playhead());
  });

  $('matchRms').addEventListener('change', () => { applyGains(false); renderLevels(); });

  $('blind').addEventListener('change', () => {
    state.blind = $('blind').checked;
    reshuffleBlind();
    if (state.take) { buildVersionButtons(); applyGains(false); renderLevels(); }
    renderScore();
  });

  $('exportBtn').addEventListener('click', exportNotes);

  $('takeNotes').addEventListener('input', () => {
    state.notes[state.items[state.itemIndex].id] = $('takeNotes').value;
    localStorage.setItem(notesKey(), JSON.stringify(state.notes));
  });

  for (const cv of [$('wave'), $('spec')]) {
    let dragFrom = null;
    cv.addEventListener('pointerdown', (ev) => {
      if (!state.take) return;
      cv.setPointerCapture(ev.pointerId);
      dragFrom = seekFromEvent(cv, ev);
    });
    cv.addEventListener('pointermove', (ev) => {
      if (dragFrom === null) return;
      const to = seekFromEvent(cv, ev);
      if (Math.abs(to - dragFrom) > 0.02) {
        state.region = [Math.min(dragFrom, to), Math.max(dragFrom, to)];
      }
    });
    cv.addEventListener('pointerup', (ev) => {
      if (dragFrom === null) return;
      const to = seekFromEvent(cv, ev);
      if (Math.abs(to - dragFrom) <= 0.02) {
        // A click with no drag is a seek, and drops whatever region it lands in.
        state.region = null;
        state.startOffset = to;
        if (state.playing) startAt(to);
      } else if (state.playing) {
        startAt(state.region[0]);
      }
      dragFrom = null;
    });
  }

  document.addEventListener('keydown', (ev) => {
    if (ev.target.tagName === 'TEXTAREA' || ev.metaKey || ev.ctrlKey || ev.altKey) return;
    const k = ev.key;
    if (k === ' ') { ev.preventDefault(); togglePlay(); return; }
    if (!state.take) return;
    const n = state.take.keys.length;
    if (k >= '1' && k <= '9') { setVersion(+k - 1); return; }
    if (k === 'ArrowRight') { ev.preventDefault(); setVersion((state.versionIndex + 1) % n); return; }
    if (k === 'ArrowLeft') { ev.preventDefault(); setVersion((state.versionIndex - 1 + n) % n); return; }
    if (k === 'ArrowDown') { ev.preventDefault(); selectTake(state.itemIndex + 1); return; }
    if (k === 'ArrowUp') { ev.preventDefault(); selectTake(state.itemIndex - 1); return; }
    if (k === 'l' || k === 'L') { $('loopBtn').click(); return; }
    if (k === 'm' || k === 'M') {
      $('matchRms').checked = !$('matchRms').checked;
      $('matchRms').dispatchEvent(new Event('change'));
      return;
    }
    if (k === 's' || k === 'S') {
      $('restartOnSwitch').checked = !$('restartOnSwitch').checked;
      return;
    }
    if (k === 'b' || k === 'B') {
      $('blind').checked = !$('blind').checked;
      $('blind').dispatchEvent(new Event('change'));
      return;
    }
    if (k === 'r' || k === 'R') {
      const id = state.items[state.itemIndex].id;
      if (state.blind && state.picks[id] && !state.picks[id].revealed) {
        state.picks[id].revealed = true;
        localStorage.setItem(picksKey(), JSON.stringify(state.picks));
      } else {
        reshuffleBlind();
      }
      buildVersionButtons();
      applyGains(false);
    }
  });

  const tick = () => {
    if (state.take) {
      $('clock').textContent = `${playhead().toFixed(2)} / ${state.take.duration.toFixed(2)}`;
      if (state.playing && !state.loop && playhead() >= state.take.duration - 0.02) pause();
      drawWave();
      drawSpec();
    }
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

function exportNotes() {
  const payload = {
    title: state.manifest.title,
    exported: new Date().toISOString(),
    notes: state.notes,
    blind_picks: Object.fromEntries(
      Object.entries(state.picks).map(([id, p]) => [id, p && p.key]),
    ),
  };
  const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `audition-notes-${(state.manifest.title || 'takes').replace(/\W+/g, '-')}.json`;
  a.click();
  URL.revokeObjectURL(url);
}

boot().catch((err) => {
  $('notes').textContent = `could not load the renders: ${err.message}`;
});
