/* The transport: every version of a take decoded up front and played at once.
 *
 * Only the gains differ, so a switch can be a gain change on a sound that never
 * stopped, which keeps the versions sample-aligned — the only way to compare a
 * sustain or a decay. It is not what an attack wants, though: switch four
 * seconds into a phrase and the new version's onset is already behind you. So
 * "restart on switch" is on by default and the sample-aligned form stays one
 * keystroke away.
 */

'use strict';

import { $, state, SWITCH_RAMP, FUSED_S } from './state.js';
import { t } from './i18n.js';

export function audio() {
  if (!state.ctx) {
    state.ctx = new (window.AudioContext || window.webkitAudioContext)();
    state.master = state.ctx.createGain();
    state.master.gain.value = 1;
    state.master.connect(state.ctx.destination);
  }
  return state.ctx;
}

export async function loadTake(item) {
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

/* Shorter than this is not a passage: a stray two-pixel drag would otherwise
 * confine playback to a click. */
const MIN_SPAN = 0.02;

/* The passage being listened to: the marked region, or the whole take.
 *
 * Every move the transport makes is defined against this one span rather than
 * against the file — where play starts, where rewind goes, where a pass ends,
 * what loop repeats. A region that only the loop button made real was the
 * confusion worth ending: marking a passage looked like choosing what to hear
 * and playback walked straight out of it.
 */
export function span() {
  const total = state.take ? state.take.duration : 0;
  const [a, b] = state.region || [0, total];
  return b - a > MIN_SPAN ? [a, b] : [0, total];
}

export function targetGain(key) {
  if (!$('matchRms').checked) return 1;
  const vals = state.take.keys.map((k) => state.take.rms[k]).filter((v) => v > 0);
  if (!vals.length) return 1;
  const ref = vals.reduce((a, b) => a + b, 0) / vals.length;
  const r = state.take.rms[key];
  return r > 0 ? Math.min(ref / r, 8) : 1;
}

export function activeKey() {
  const idx = state.blind ? state.blindOrder[state.versionIndex] : state.versionIndex;
  return state.take.keys[idx];
}

export function applyGains(immediate) {
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

export function stopSources() {
  for (const s of state.sources) { try { s.stop(); } catch { /* already ended */ } }
  state.sources = [];
}

/* The play button says what pressing it will do, and carries the glyph a
 * transport is scanned by. The label keeps its own key rather than being
 * written in plain text, so switching language mid-pass re-renders the state
 * the button is actually in instead of resetting it to `play`. */
export function markPlay(playing) {
  const btn = $('playBtn');
  const lab = btn.querySelector('.tlabel');
  btn.querySelector('.ticon').textContent = playing ? '❚❚' : '▶︎';
  lab.dataset.i18n = playing ? 'transport.pause' : 'transport.play';
  lab.textContent = t(lab.dataset.i18n);
  if (playing) btn.setAttribute('aria-pressed', 'true');
  else btn.removeAttribute('aria-pressed');
}

export function startAt(offset) {
  const ctx = audio();
  // Here rather than at the play button: every control that names a version now
  // sounds it, and a context left suspended by the browser's autoplay rule made
  // whichever of them was pressed first do nothing at all.
  ctx.resume();
  stopSources();
  state.gains = {};
  const [a, b] = span();
  const looping = state.loop && b - a > MIN_SPAN;
  const from = Math.min(Math.max(offset, a), Math.max(a, b - 0.001));

  for (const k of state.take.keys) {
    const src = ctx.createBufferSource();
    src.buffer = state.take.buffers[k];
    const g = ctx.createGain();
    g.gain.value = 0;
    src.connect(g).connect(state.master);
    src.loopStart = a;
    src.loopEnd = b;
    src.loop = looping;
    src.start(0, from);
    state.sources.push(src);
    state.gains[k] = g;
  }
  state.startedAt = ctx.currentTime;
  state.startOffset = from;
  state.playing = true;
  applyGains(true);
  markPlay(true);
}

export function pause() {
  const at = playhead();
  stopSources();
  state.startOffset = at;
  state.playing = false;
  markPlay(false);
}

/* The end of a pass, which is a different thing from a pause: the playhead goes
 * back to the top of what was playing.
 *
 * Parked on the last sample, `play` could not mean play — it either had to be
 * rewound first or it silently started somewhere the button does not name. On a
 * page where one take is heard twenty times that is the press that gets made
 * most, so it is the one that has to need no thought.
 */
export function stop() {
  stopSources();
  state.startOffset = span()[0];
  state.playing = false;
  markPlay(false);
}

/// True once a pass has run out of passage. Never true under loop, which is
/// what loop means.
export function atEnd() {
  if (!state.take || !state.playing || state.loop) return false;
  return playhead() >= span()[1] - 0.004;
}

export function playhead() {
  if (!state.take) return 0;
  if (!state.playing) return state.startOffset;
  const t0 = state.startOffset + (audio().currentTime - state.startedAt);
  const [a, b] = span();
  if (state.loop && b - a > MIN_SPAN) return a + ((t0 - a) % (b - a));
  return Math.min(t0, b);
}

/* Loop on or off without a gap in what is sounding.
 *
 * The sources carry the loop themselves and it is settable while they run, so
 * the only thing that has to move is the anchor the playhead is measured from:
 * read it on the old rule, write it back, and the picture and the sound agree
 * across the switch. Restarting them instead cut a hole in the middle of the
 * passage the listener was in — at the moment they asked to hear it again.
 */
export function setLoop(on) {
  const at = playhead();
  state.loop = on;
  $('loopBtn').setAttribute('aria-pressed', String(on));
  if (!state.playing) return;
  const [a, b] = span();
  state.startOffset = at;
  state.startedAt = audio().currentTime;
  for (const s of state.sources) s.loop = on && b - a > MIN_SPAN;
}

/// The passage chip: what is being played, and the way back to the whole take.
export function markRegion() {
  const on = Boolean(state.region);
  $('passage').hidden = !on;
  $('regionSpan').textContent = on
    ? `${state.region[0].toFixed(2)}–${state.region[1].toFixed(2)} s` : '';
}

/* The marked passage changed while something is sounding.
 *
 * Inside the new span the playhead stays where it is: somebody who marked a
 * passage around the note they were on did not ask to hear it from the top.
 * Outside it there is nowhere to stay, so the pass begins again.
 */
export function applyRegion() {
  if (!state.take) return;
  const [a, b] = span();
  const at = playhead();
  const inside = at >= a && at <= b;
  if (!state.playing) {
    // To the top rather than to the near edge: parked on the end of a passage
    // it has not played, the playhead says the pass is over before it began.
    state.startOffset = inside ? at : a;
    return;
  }
  if (!inside) { startAt(a); return; }
  state.startOffset = at;
  state.startedAt = audio().currentTime;
  for (const s of state.sources) {
    s.loopStart = a;
    s.loopEnd = b;
    s.loop = state.loop && b - a > MIN_SPAN;
  }
}

/* Move the playhead, whether or not anything is sounding.
 *
 * One function for every way the page seeks — a click on a picture, the button
 * under the transport — because they are the same move and were two copies of
 * it, which is how a seek came to restart playback from one of them and not
 * from the other.
 */
export function seekTo(offset) {
  if (!state.take) return;
  const [a, b] = span();
  const at = Math.min(Math.max(offset, a), b);
  state.startOffset = at;
  if (state.playing) startAt(at);
}

/* Back to the top, which on a page where a take is played twenty times is the
 * move that was missing: without it the only way back was a click landing on
 * the first few pixels of a picture, and on an attack the first few pixels are
 * where the answer is.
 *
 * To the region's start rather than the file's where one is marked. A region
 * is the passage being listened to, and a rewind that leaves it is a rewind
 * that has to be undone.
 */
export function rewind() {
  seekTo(span()[0]);
}

export function togglePlay() {
  if (!state.take) return;
  if (state.playing) { pause(); return; }
  // Stopped at the far end, play starts over rather than sounding the last
  // millisecond. The end of a pass already rewinds; this covers a playhead put
  // there by hand.
  const [a, b] = span();
  startAt(state.startOffset >= b - 0.01 ? a : state.startOffset);
}

export function renderLevels() {
  if (!state.take) return;
  const key = activeKey();
  const db = (v) => (v > 0 ? (20 * Math.log10(v)).toFixed(1) : '-inf');
  const g = targetGain(key);
  const shown = state.blind ? state.take.rms[key] * g : state.take.rms[key];
  const parts = [t('level.rms', { db: db(shown) })];
  if ($('matchRms').checked) {
    parts.push(t('level.gain', { db: (20 * Math.log10(g)).toFixed(1) }));
  }
  $('levels').textContent = parts.join('   ');
}

/* The take's schedule as numbered strikes, which is what a listening note has
 * to be able to name. The number is not what carries the meaning: every readout
 * beside it names the note and the time it was struck, so the two ends agree
 * even where a grouping would not. */
export function takeHits() {
  const item = state.items[state.itemIndex];
  const notes = item && item.meta && item.meta.notes;
  if (!notes || !notes.length) return [];
  const out = [];
  for (const n of [...notes].sort((a, b) => a.start - b.start || a.note - b.note)) {
    const last = out[out.length - 1];
    if (last && n.start - last.start <= FUSED_S) last.notes.push(n);
    else out.push({ n: out.length + 1, start: n.start, notes: [n] });
  }
  return out;
}

/// The strike the playhead is inside, or null before the first one.
export function hitAt(at) {
  let cur = null;
  for (const hit of takeHits()) if (hit.start <= at + 0.005) cur = hit;
  return cur;
}

/* Everything that decides what the ear actually met, as data.
 *
 * An address names the set, the take and the version and stops there, so where
 * the playhead was, which strike that is, whether the levels were matched and
 * whether the names were hidden all had to be described from memory at the
 * other end or reconstructed by counting strikes against a phrase set. Both are
 * a way of getting it wrong quietly: "the second one" is a different note in
 * every take, and neither end finds out that they disagreed.
 *
 * One producer, two consumers — the clipboard button and the feedback the page
 * sends — so a report typed into the page and a report pasted out of it carry
 * the same fields. */
export function conditions() {
  const item = state.items[state.itemIndex];
  const at = playhead();
  const hit = hitAt(at);
  return {
    set: state.setId,
    take: item ? item.id : null,
    take_label: (item && item.label) || null,
    version: state.blind ? null : (state.take ? activeKey() : null),
    blind: state.blind,
    playhead: Number(at.toFixed(3)),
    duration: state.take ? Number(state.take.duration.toFixed(3)) : null,
    hit: hit ? {
      n: hit.n,
      start: Number(hit.start.toFixed(3)),
      into: Number((at - hit.start).toFixed(3)),
      notes: hit.notes.map((n) => ({ note: n.note, velocity: n.velocity })),
    } : null,
    region: state.region
      ? [Number(state.region[0].toFixed(3)), Number(state.region[1].toFixed(3))]
      : null,
    match_loudness: $('matchRms').checked,
    loop: state.loop,
  };
}
