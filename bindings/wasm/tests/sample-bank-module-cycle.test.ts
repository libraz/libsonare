/**
 * Module-initialisation order across the sample-bank import cycle.
 *
 * `project_internal.ts` imports `SampleBank` as a VALUE — `normalizeSynthInstrument`
 * needs the class for its `instanceof` check — and `sample_bank.ts` imports
 * `projectModule` back, so the two form a live ESM cycle rather than an erased
 * type-only one. It is safe only because neither module BODY reads the other's
 * binding: both accesses sit inside function bodies, so whichever module is
 * evaluated first finishes without touching the partner's TDZ.
 *
 * That property is invisible to review the moment someone adds a module-scope
 * `new SampleBank()` or a lookup table, and it would fail at IMPORT time in one
 * direction only — latent until an unrelated change reorders the graph. So both
 * orders are pinned here, each in a fresh module registry.
 *
 * Nothing below touches the WASM module: the two `normalizeSynthInstrument`
 * paths exercised never reach `projectModule()`, so this runs without a build.
 */

import { beforeEach, describe, expect, it, vi } from 'vitest';

async function expectCycleIsSafe(internalFirst: boolean): Promise<void> {
  // Whichever import runs first evaluates its module body while the other is
  // still in progress; a body-level read of the partner throws here.
  const internal = internalFirst ? await import('../src/project_internal.js') : undefined;
  const { SampleBank } = await import('../src/sample_bank.js');
  const { normalizeSynthInstrument } = internal ?? (await import('../src/project_internal.js'));

  expect(typeof SampleBank).toBe('function');
  // A preset-name string and a non-bank object are the two branches that stay
  // clear of the native module, and between them they read both cycle
  // participants: the function from one module, the class from the other.
  expect(normalizeSynthInstrument('saw-lead')).toBe('saw-lead');
  expect(() => normalizeSynthInstrument({ sampleBank: {} })).toThrow(TypeError);
  expect(normalizeSynthInstrument({ engineMode: 'sample' })).toEqual({ engineMode: 'sample' });
}

describe('sample-bank import cycle initialises in either order', () => {
  beforeEach(() => {
    vi.resetModules();
  });

  it('project_internal evaluated first', async () => {
    await expectCycleIsSafe(true);
  });

  it('sample_bank evaluated first', async () => {
    await expectCycleIsSafe(false);
  });
});
