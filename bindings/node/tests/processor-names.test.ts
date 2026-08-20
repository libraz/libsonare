/**
 * `SoloProcessor` is generated from the tracked capability catalog rather than
 * hand-maintained. This holds the generated declaration against what the addon
 * actually returns, so a stale declaration fails here instead of typing
 * `masteringProcessorNames()` as a set the library does not ship.
 */

import { describe, expect, it } from 'vitest';
import { masteringProcessorNames } from '../src/index.js';
import { SOLO_PROCESSORS } from '../src/types_mastering.js';

describe('SoloProcessor', () => {
  it('declares exactly the processor names the addon returns', () => {
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
