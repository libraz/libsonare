/**
 * One module per realm, shared by every entry point of one build.
 *
 * Each published entry is bundled self-contained, so each carries its own copy
 * of the lifecycle code; the holder those copies read lives on `globalThis`
 * under a key naming the build. These cases run against the built `dist/`.
 */

import { readFileSync } from 'node:fs';
import { Worker } from 'node:worker_threads';
import { beforeEach, describe, expect, it } from 'vitest';
import * as analysis from '../dist/analysis.js';
import * as index from '../dist/index.js';
import * as worklet from '../dist/worklet.js';
import { moduleSlotKey } from '../src/internal/module_registry.js';
import { clearModuleRealm } from './_module_realm.js';

const DIST = new URL('../dist/', import.meta.url);
const SRC = new URL('../src/', import.meta.url);

/** The published entries, as `tsup.config.ts` lists them. */
const ENTRIES = ['index', 'analysis', 'worklet', 'worker', 'vocal_edit_worker'] as const;

/** The build id stamped into a bundle, read off the holder key it builds. */
function stampedBuildId(entry: string): string {
  const text = readFileSync(new URL(`${entry}.js`, DIST), 'utf8');
  const found = [...text.matchAll(/BUILD_ID\d*\s*=\s*(?:true\s*\?\s*)?"([0-9a-f]{16})"/g)].map(
    (m) => m[1],
  );
  expect(found, `${entry}.js stamps one build id`).toHaveLength(1);
  return found[0];
}

/** A stand-in AudioWorkletNode; the playback wrapper only posts to it. */
function fakeNode() {
  return {
    port: { onmessage: null, postMessage: () => undefined },
    disconnect: () => undefined,
  } as unknown as AudioWorkletNode;
}

beforeEach(() => {
  clearModuleRealm();
});

describe('the module holder of a realm', () => {
  it('is not initialized in a fresh realm', async () => {
    const url = new URL('index.js', DIST).href;
    const worker = new Worker(
      `import(${JSON.stringify(url)}).then((m) => {
         require('node:worker_threads').parentPort.postMessage(m.isInitialized());
       });`,
      { eval: true },
    );
    try {
      await index.init();
      expect(index.isInitialized()).toBe(true);
      const answer = await new Promise((resolve, reject) => {
        worker.once('message', resolve);
        worker.once('error', reject);
      });
      expect(answer).toBe(false);
    } finally {
      await worker.terminate();
    }
  });

  it('serves the worklet entry once the index entry has initialized it', async () => {
    expect(worklet.isInitialized()).toBe(false);
    await index.init();
    expect(worklet.isInitialized()).toBe(true);
    // The analysis binary is a different module and keeps its own holder.
    expect(analysis.isInitialized()).toBe(false);
  });

  it('lets SonarePlaybackNode.create read the module the index entry loaded', async () => {
    // A wrapper finding no module calls init({ wasmBinary }), and these four
    // bytes do not instantiate; once the index entry has loaded one, they are
    // never read.
    const create = () =>
      worklet.SonarePlaybackNode.create({ sampleRate: 48000 } as BaseAudioContext, {
        config: { input: { layout: 'auto' }, target: { kind: 'speakers', layout: '5.1' } },
        wasmBinary: new Uint8Array([0, 97, 115, 109]),
        nodeFactory: () => fakeNode(),
      });
    await expect(create()).rejects.toBeDefined();
    await index.init();
    expect((await create()).node).toBeDefined();
  });

  it('refuses a worklet init whose options differ from the index init', async () => {
    await index.init();
    await expect(worklet.init({ locateFile: (path) => path })).rejects.toMatchObject({
      code: index.ErrorCode.InvalidState,
    });
  });

  it('is not shared between two builds', () => {
    expect(moduleSlotKey('full', 'aaaaaaaaaaaaaaaa')).not.toBe(
      moduleSlotKey('full', 'bbbbbbbbbbbbbbbb'),
    );
    const realm = globalThis as unknown as Record<symbol, unknown>;
    const other = moduleSlotKey('full', 'ffffffffffffffff');
    realm[other] = { module: { version: () => 'other' }, pending: null, firstOptions: undefined };
    try {
      expect(index.isInitialized()).toBe(false);
      expect(worklet.isInitialized()).toBe(false);
    } finally {
      delete realm[other];
    }
  });
});

describe('every entry taking the lifecycle from the index entry', () => {
  const importers = ENTRIES.filter(
    (entry) =>
      entry !== 'index' &&
      /from '\.\/index\.js'/.test(readFileSync(new URL(`${entry}.ts`, SRC), 'utf8')),
  );

  it('is found by the scan', () => {
    expect(importers).toEqual(expect.arrayContaining(['worklet', 'worker', 'vocal_edit_worker']));
  });

  it.each(importers)('%s is stamped with the index build id', (entry) => {
    expect(stampedBuildId(entry)).toBe(stampedBuildId('index'));
  });

  it('every entry of the build carries one build id', () => {
    const ids = new Set(ENTRIES.map(stampedBuildId));
    expect(ids.size).toBe(1);
  });
});
