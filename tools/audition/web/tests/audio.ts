/* A recording stand-in for the Web Audio graph, and a fetch that serves synthetic sets. */

import { vi } from 'vitest';

export class FakeParam {
  value = 0;
  events: [string, number, number][] = [];
  setValueAtTime(v: number, t: number) {
    this.value = v;
    this.events.push(['set', v, t]);
  }
  linearRampToValueAtTime(v: number, t: number) {
    this.value = v;
    this.events.push(['ramp', v, t]);
  }
  cancelScheduledValues(t: number) {
    this.events.push(['cancel', 0, t]);
  }
}

export class FakeNode {
  connect(n: unknown) {
    return n;
  }
}

export class FakeGain extends FakeNode {
  gain = new FakeParam();
}

export class FakeSource extends FakeNode {
  buffer: unknown = null;
  loop = false;
  loopStart = 0;
  loopEnd = 0;
  started: number | null = null;
  stopped = false;
  start(_when: number, offset: number) {
    this.started = offset;
  }
  stop() {
    this.stopped = true;
  }
}

export function fakeBuffer(duration: number, sampleRate = 48000, level = 0.5) {
  const length = Math.round(duration * sampleRate);
  const data = new Float32Array(length).fill(level);
  return { duration, sampleRate, length, numberOfChannels: 1, getChannelData: () => data };
}

export class FakeContext {
  currentTime = 0;
  destination = new FakeNode();
  sources: FakeSource[] = [];
  gains: FakeGain[] = [];
  resumed = 0;
  /** Duration per decoded file, keyed by the first byte of its body. */
  durations: Record<number, number> = {};
  createGain() {
    const g = new FakeGain();
    this.gains.push(g);
    return g;
  }
  createBufferSource() {
    const s = new FakeSource();
    this.sources.push(s);
    return s;
  }
  resume() {
    this.resumed += 1;
    return Promise.resolve();
  }
  decodeAudioData(bytes: ArrayBuffer) {
    const tag = new Uint8Array(bytes)[0] ?? 0;
    return Promise.resolve(fakeBuffer(this.durations[tag] ?? 4));
  }
  /** Sources started by the last `startAt`, still running. */
  live() {
    return this.sources.filter((s) => s.started !== null && !s.stopped);
  }
}

/** A minimal RIFF/WAVE header declaring `rate`, its first byte tagged for the fake decoder. */
export function wav(rate = 44100, tag = 0): ArrayBuffer {
  const b = new ArrayBuffer(44);
  const v = new DataView(b);
  const put = (at: number, s: string) => {
    for (let i = 0; i < 4; i++) {
      v.setUint8(at + i, s.charCodeAt(i));
    }
  };
  put(0, 'RIFF');
  v.setUint32(4, 36, true);
  put(8, 'WAVE');
  put(12, 'fmt ');
  v.setUint32(16, 16, true);
  v.setUint32(24, rate, true);
  put(36, 'data');
  v.setUint32(40, 0, true);
  if (tag) {
    // The fake decoder reads the first byte; the rate parser has already read the header by then.
    v.setUint8(0, tag);
  }
  return b;
}

export interface Route {
  status?: number;
  body?: unknown;
  /** Resolve only when `release()` is called, to order two loads by hand. */
  hold?: boolean;
}

/** Install a fetch that answers by path; returns the calls made and a way to release held answers. */
export function serveFetch(routes: Record<string, Route | ((init?: RequestInit) => Route)>) {
  const calls: { url: string; init?: RequestInit }[] = [];
  const held: (() => void)[] = [];
  const fetch = vi.fn(async (input: string, init?: RequestInit) => {
    calls.push({ url: input, init });
    const path = input.split('?')[0] ?? input;
    const found = routes[path] ?? routes[input];
    const route = typeof found === 'function' ? found(init) : found;
    if (!route) {
      return new Response('not found', { status: 404 });
    }
    if (route.hold) {
      await new Promise<void>((resolve) => held.push(resolve));
    }
    const body = route.body;
    if (body instanceof ArrayBuffer) {
      return new Response(body.slice(0), { status: route.status ?? 200 });
    }
    return new Response(JSON.stringify(body ?? {}), {
      status: route.status ?? 200,
      headers: { 'Content-Type': 'application/json' },
    });
  });
  vi.stubGlobal('fetch', fetch);
  return {
    calls,
    release: () => {
      for (const r of held.splice(0)) {
        r();
      }
    },
  };
}
