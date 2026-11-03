/* A synthetic set on screen and a stand-in for serve.py's note endpoints. */

import { vi } from 'vitest';
import type { FeedbackEntry, Manifest } from '../../src/api/schemas';
import { Manifest as ManifestSchema } from '../../src/api/schemas';
import { resetFeedback } from '../../src/feedback/store';
import { resetStore, setState } from '../../src/state/store';

export const SET = 'set-x';
export const PATH = '/tmp/notes/set-x.jsonl';

export const manifest: Manifest = ManifestSchema.parse({
  set_generation: 'gen1',
  sources: {
    a: { label: 'Alpha render', role: 'model' },
    r: { label: 'Ref one', role: 'reference' },
    c: { label: 'Cmp one', role: 'comparison' },
  },
  comparisons: [
    {
      id: 'cmp1',
      scope: 'product',
      model_sources: ['a'],
      oracle_sources: ['r'],
      status: 'matched',
    },
  ],
  items: [{ id: 't1', label: 'Take one', meta: { notes: [{ start: 0, note: 60, velocity: 90 }] } }],
});

/** The store as it is with a take open on `set-x`, the reference last heard and sending allowed. */
export function seedApp(over: Record<string, unknown> = {}): void {
  setState({
    setId: SET,
    setEpoch: 1,
    manifest,
    items: manifest.items,
    itemIndex: 0,
    versionIndex: 0,
    canSend: true,
    attach: true,
    lastOracle: { role: 'reference', key: 'r' },
    take: { id: 't1', keys: ['a', 'r', 'c'], rms: {}, rates: {}, duration: 4, epoch: 1 },
    ...over,
  });
}

export function freshState(): void {
  resetStore();
  resetFeedback();
}

export interface Call {
  url: string;
  method: string;
  body: unknown;
}

export interface Server {
  calls: Call[];
  /** The notes sent so far, as `feedback.json` would list them. */
  log: FeedbackEntry[];
  posts: () => Call[];
}

type Reply = { status: number; body?: unknown } | undefined;

/** Stub `fetch`: a log that grows per note, with `reply` able to refuse or hold a request. */
export function stubServer(reply?: (call: Call) => Reply | Promise<Reply>): Server {
  const server: Server = {
    calls: [],
    log: [],
    posts: () => server.calls.filter((c) => c.method === 'POST'),
  };
  let n = 0;
  const json = (status: number, body: unknown) =>
    new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
  vi.stubGlobal(
    'fetch',
    vi.fn(async (url: string, init?: RequestInit) => {
      const call: Call = {
        url: String(url),
        method: init?.method ?? 'GET',
        body: init?.body ? JSON.parse(String(init.body)) : null,
      };
      server.calls.push(call);
      const forced = await reply?.(call);
      if (forced) {
        return json(forced.status, forced.body ?? {});
      }
      if (call.url.startsWith('feedback.json')) {
        return json(200, { entries: server.log, path: PATH });
      }
      if (call.url === 'feedback' && call.method === 'POST') {
        const body = call.body as Record<string, unknown>;
        if (body.op === 'undo') {
          server.log = server.log.filter((e) => e.id !== body.id);
          return json(200, { entries: server.log, path: PATH });
        }
        n += 1;
        const entry = { id: `e${n}`, at: '2026-01-02T03:04:05Z', ...body } as FeedbackEntry;
        server.log = [...server.log, entry];
        return json(200, { entries: server.log, path: PATH, entry_id: entry.id });
      }
      return json(200, {});
    }),
  );
  return server;
}

/** A reply the test settles by hand, to hold a request in flight. */
export function deferred<T>() {
  let resolve!: (v: T) => void;
  const promise = new Promise<T>((r) => {
    resolve = r;
  });
  return { promise, resolve };
}
