/**
 * `SoloProcessor` is generated from the tracked capability catalog rather than
 * hand-maintained. This holds the generated declaration against what the WASM
 * module actually returns, so a stale declaration fails here instead of typing
 * `masteringProcessorNames()` as a set the library does not ship. The
 * declaration is read from `src/` on purpose: it is the published artifact
 * under test, while `dist/` supplies the runtime list to diff it against.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, masteringProcessorNames } from '../dist/index.js';
import { SOLO_PROCESSORS } from '../src/public_types_mastering.js';

beforeAll(async () => {
  await init();
});

describe('SoloProcessor', () => {
  it('declares exactly the processor names the module returns', () => {
    const runtime = masteringProcessorNames();
    expect(runtime.length).toBeGreaterThan(0);
    expect([...runtime].sort()).toEqual([...SOLO_PROCESSORS].sort());
  });

  it('covers the creative effects the apply path dispatches', () => {
    const effects = masteringProcessorNames().filter((name) => name.startsWith('effects.'));
    expect(effects.length).toBeGreaterThan(0);
    for (const name of effects) {
      expect(SOLO_PROCESSORS).toContain(name);
    }
  });
});
