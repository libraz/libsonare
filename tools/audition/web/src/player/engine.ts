/* The transport: every version of a take decoded up front and played at once.
 *
 * Only the gains differ, so a switch can be a gain change on a sound that never
 * stopped, which keeps the versions sample-aligned — the only way to compare a
 * sustain or a decay. An attack wants the opposite, so "restart on switch" is
 * on by default and the sample-aligned form stays one keystroke away.
 *
 * The clock (`startedAt`, `startOffset`) and the audio graph live here rather
 * than in the store: they move every frame, and `playhead()` is read by a frame
 * loop rather than rendered. `playing`, `loop` and `region` are the store's.
 */

import type { Item } from '../api/schemas';
import { activeKey, SWITCH_RAMP } from '../state/selectors';
import { getState, setState, type Take } from '../state/store';

/** Shorter than this is not a passage: a stray two-pixel drag would otherwise confine playback to a click. */
export const MIN_SPAN = 0.02;

type ContextFactory = () => AudioContext;

const defaultFactory: ContextFactory = () => new AudioContext();

const engine = {
  factory: defaultFactory,
  ctx: null as AudioContext | null,
  master: null as GainNode | null,
  buffers: {} as Record<string, AudioBuffer>,
  sources: [] as AudioBufferSourceNode[],
  gains: {} as Record<string, GainNode>,
  startedAt: 0,
  startOffset: 0,
};

/** Replace how the context is made, and drop the one held; for tests. */
export function setContextFactory(factory: ContextFactory | null): void {
  engine.factory = factory ?? defaultFactory;
  engine.ctx = null;
  engine.master = null;
  engine.sources = [];
  engine.gains = {};
  engine.buffers = {};
  engine.startedAt = 0;
  engine.startOffset = 0;
}

export function audio(): AudioContext {
  if (!engine.ctx) {
    engine.ctx = engine.factory();
    engine.master = engine.ctx.createGain();
    engine.master.gain.value = 1;
    engine.master.connect(engine.ctx.destination);
  }
  return engine.ctx;
}

/** The decoded buffer for a version of the take on the transport. */
export const bufferOf = (key: string): AudioBuffer | undefined => engine.buffers[key];

/** The sample rate a RIFF/WAVE file was written at, or 0 for anything else; a path record counts frames at this rate. */
export function wavRate(bytes: ArrayBuffer): number {
  const v = new DataView(bytes);
  const tag = (at: number) =>
    String.fromCharCode(v.getUint8(at), v.getUint8(at + 1), v.getUint8(at + 2), v.getUint8(at + 3));
  if (v.byteLength < 12 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') {
    return 0;
  }
  for (let at = 12; at + 8 <= v.byteLength; ) {
    const size = v.getUint32(at + 4, true);
    if (tag(at) === 'fmt ' && at + 16 <= v.byteLength) {
      return v.getUint32(at + 12, true);
    }
    at += 8 + size + (size & 1);
  }
  return 0;
}

/** Level of a buffer from every 7th sample: good enough to match two renders, cheap enough not to delay a take. */
export function bufferRms(buf: AudioBuffer): number {
  let sum = 0;
  let n = 0;
  for (let c = 0; c < buf.numberOfChannels; c++) {
    const d = buf.getChannelData(c);
    for (let i = 0; i < d.length; i += 7) {
      const x = d[i] ?? 0;
      sum += x * x;
      n++;
    }
  }
  return Math.sqrt(sum / Math.max(n, 1));
}

export interface Decoded {
  take: Omit<Take, 'epoch'>;
  buffers: Record<string, AudioBuffer>;
}

/** Fetch and decode every version of a take. Nothing is committed: the caller decides whether the load still matters. */
export async function decodeTake(item: Item, base: string): Promise<Decoded> {
  const ctx = audio();
  const keys = Object.keys(item.tracks);
  const buffers: Record<string, AudioBuffer> = {};
  const rms: Record<string, number> = {};
  const rates: Record<string, number> = {};
  await Promise.all(
    keys.map(async (k) => {
      const url = base + item.tracks[k];
      const res = await fetch(url);
      if (!res.ok) {
        throw new Error(`${url}: ${res.status}`);
      }
      const bytes = await res.arrayBuffer();
      // Read before decoding, which resamples to the context rate and detaches the bytes.
      const rate = wavRate(bytes);
      const buf = await ctx.decodeAudioData(bytes);
      buffers[k] = buf;
      rms[k] = bufferRms(buf);
      rates[k] = rate || buf.sampleRate;
    }),
  );
  const duration = Math.max(0, ...keys.map((k) => buffers[k]?.duration ?? 0));
  return { take: { id: item.id, keys, rms, rates, duration }, buffers };
}

/** Make a decoded take the one on the transport. */
export function commitBuffers(buffers: Record<string, AudioBuffer>): void {
  engine.buffers = buffers;
  engine.startOffset = 0;
}

/** The passage being listened to: the marked region, or the whole take. Every transport move is defined against it. */
export function span(): [number, number] {
  const s = getState();
  const total = s.take ? s.take.duration : 0;
  const [a, b] = s.region ?? [0, total];
  return b - a > MIN_SPAN ? [a, b] : [0, total];
}

/** The gain a version plays at: matched to the take's mean level when asked, never more than ×8. */
export function targetGain(key: string): number {
  const s = getState();
  if (!s.matchRms || !s.take) {
    return 1;
  }
  const take = s.take;
  const vals = take.keys.map((k) => take.rms[k] ?? 0).filter((v) => v > 0);
  if (!vals.length) {
    return 1;
  }
  const ref = vals.reduce((a, b) => a + b, 0) / vals.length;
  const r = take.rms[key] ?? 0;
  return r > 0 ? Math.min(ref / r, 8) : 1;
}

export function applyGains(immediate: boolean): void {
  const s = getState();
  if (!s.take || !engine.ctx) {
    return;
  }
  const now = engine.ctx.currentTime;
  const active = activeKey(s);
  for (const k of s.take.keys) {
    const g = engine.gains[k];
    if (!g) {
      continue;
    }
    const want = k === active ? targetGain(k) : 0;
    g.gain.cancelScheduledValues(now);
    if (immediate) {
      g.gain.setValueAtTime(want, now);
    } else {
      g.gain.setValueAtTime(g.gain.value, now);
      g.gain.linearRampToValueAtTime(want, now + SWITCH_RAMP);
    }
  }
}

export function stopSources(): void {
  for (const src of engine.sources) {
    try {
      src.stop();
    } catch {
      // already ended
    }
  }
  engine.sources = [];
}

export function startAt(offset: number): void {
  const s = getState();
  if (!s.take) {
    return;
  }
  const ctx = audio();
  // Here rather than at the play button: a context left suspended by the
  // autoplay rule otherwise makes whichever control was pressed first silent.
  void ctx.resume();
  stopSources();
  engine.gains = {};
  const [a, b] = span();
  const looping = s.loop && b - a > MIN_SPAN;
  const from = Math.min(Math.max(offset, a), Math.max(a, b - 0.001));
  for (const k of s.take.keys) {
    const buf = engine.buffers[k];
    if (!buf || !engine.master) {
      continue;
    }
    const src = ctx.createBufferSource();
    src.buffer = buf;
    const g = ctx.createGain();
    g.gain.value = 0;
    src.connect(g).connect(engine.master);
    src.loopStart = a;
    src.loopEnd = b;
    src.loop = looping;
    src.start(0, from);
    engine.sources.push(src);
    engine.gains[k] = g;
  }
  engine.startedAt = ctx.currentTime;
  engine.startOffset = from;
  setState({ playing: true });
  applyGains(true);
}

export function playhead(): number {
  const s = getState();
  if (!s.take) {
    return 0;
  }
  if (!s.playing || !engine.ctx) {
    return engine.startOffset;
  }
  const t0 = engine.startOffset + (engine.ctx.currentTime - engine.startedAt);
  const [a, b] = span();
  if (s.loop && b - a > MIN_SPAN) {
    return a + ((((t0 - a) % (b - a)) + (b - a)) % (b - a));
  }
  return Math.min(t0, b);
}

/** Park the playhead without sounding anything. */
export function setOffset(at: number): void {
  engine.startOffset = at;
}

export function pause(): void {
  const at = playhead();
  stopSources();
  engine.startOffset = at;
  setState({ playing: false });
}

/** The end of a pass, which is not a pause: the playhead goes back to the top of what was playing. */
export function stop(): void {
  stopSources();
  engine.startOffset = span()[0];
  setState({ playing: false });
}

/** True once a pass has run out of passage. Never true under loop, which is what loop means. */
export function atEnd(): boolean {
  const s = getState();
  if (!s.take || !s.playing || s.loop) {
    return false;
  }
  return playhead() >= span()[1] - 0.004;
}

/** Loop on or off without a gap: the sources carry the loop themselves, so only the clock's anchor moves. */
export function setLoop(on: boolean): void {
  const at = playhead();
  setState({ loop: on });
  if (!getState().playing || !engine.ctx) {
    return;
  }
  const [a, b] = span();
  engine.startOffset = at;
  engine.startedAt = engine.ctx.currentTime;
  for (const src of engine.sources) {
    src.loop = on && b - a > MIN_SPAN;
  }
}

/** The marked passage changed: inside the new span the playhead stays, outside it the pass begins again. */
export function setRegion(region: [number, number] | null): void {
  const before = playhead();
  setState({ region });
  const s = getState();
  if (!s.take) {
    return;
  }
  const [a, b] = span();
  const inside = before >= a && before <= b;
  if (!s.playing || !engine.ctx) {
    // To the top rather than the near edge: parked on the end of an unplayed passage reads as a pass already over.
    engine.startOffset = inside ? before : a;
    return;
  }
  if (!inside) {
    startAt(a);
    return;
  }
  engine.startOffset = before;
  engine.startedAt = engine.ctx.currentTime;
  for (const src of engine.sources) {
    src.loopStart = a;
    src.loopEnd = b;
    src.loop = s.loop && b - a > MIN_SPAN;
  }
}

/** Move the playhead, whether or not anything is sounding: one function for every way the page seeks. */
export function seekTo(offset: number): void {
  if (!getState().take) {
    return;
  }
  const [a, b] = span();
  const at = Math.min(Math.max(offset, a), b);
  engine.startOffset = at;
  if (getState().playing) {
    startAt(at);
  }
}

/** Back to the top of the passage being listened to. */
export function rewind(): void {
  seekTo(span()[0]);
}

export function togglePlay(): void {
  const s = getState();
  if (!s.take) {
    return;
  }
  if (s.playing) {
    pause();
    return;
  }
  // Stopped at the far end, play starts over rather than sounding the last millisecond.
  const [a, b] = span();
  startAt(engine.startOffset >= b - 0.01 ? a : engine.startOffset);
}
