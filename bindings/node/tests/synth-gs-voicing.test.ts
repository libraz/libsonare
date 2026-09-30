import { describe, expect, it } from 'vitest';
import {
  synthGsDrumKitIsVoicedApart,
  synthGsDrumKitName,
  synthGsVariationIsVoicedApart,
} from '../src/index.js';

describe('GS voicing queries', () => {
  it('reports the sets that render as Standard as the ones that say so', () => {
    // The three states stay distinct: null where no set sits, false where a set
    // renders exactly as Standard, true where it is voiced apart. A truthiness
    // check collapses the first two: `!voicedApart` is the 102 programs holding
    // no set plus the 5 answering false, so 107 entries read as placeholders
    // where there are four -- Standard answers false about itself and is a real
    // choice, so the count is wrong at both ends.
    const named = new Map<number, string>();
    for (let program = 0; program < 128; program++) {
      const name = synthGsDrumKitName(program);
      if (name === null) {
        expect(synthGsDrumKitIsVoicedApart(program)).toBeNull();
      } else {
        named.set(program, name);
      }
    }
    expect(named.size).toBe(26);
    const sameAsStandard = [...named]
      .filter(([program]) => synthGsDrumKitIsVoicedApart(program) === false)
      .map(([program, name]) => `${program}:${name}`);
    expect(sameAsStandard).toEqual([
      '0:Standard',
      '53:Cymbal & Claps',
      '56:SFX',
      '57:Rhythm FX',
      '58:Rhythm FX 2',
    ]);
  });

  it('reports whether a variation bank is voiced or falls back to the capital', () => {
    expect(synthGsVariationIsVoicedApart(0, 0)).toBe(false);
    expect(synthGsVariationIsVoicedApart(8, 0)).toBe(true);
    expect(synthGsVariationIsVoicedApart(1, 0)).toBe(true);
    // GS resolving a variation this build does not voice to the capital tone.
    expect(synthGsVariationIsVoicedApart(24, 16)).toBe(false);
    // The bank's upper end was bounded by what a uint16_t holds rather than by
    // what a Bank Select means, so 128 and up answered false -- indistinguishable
    // from a real capital-tone resolution.
    expect(synthGsVariationIsVoicedApart(128, 0)).toBeNull();
    expect(synthGsVariationIsVoicedApart(0xffff, 0)).toBeNull();
    expect(synthGsVariationIsVoicedApart(127, 0)).not.toBeNull();
    expect(synthGsVariationIsVoicedApart(0, -1)).toBeNull();
    expect(synthGsVariationIsVoicedApart(0, 128)).toBeNull();
  });

  it('refuses a GS query argument that is not a number', () => {
    const notANumber = '8' as unknown as number;
    expect(() => synthGsDrumKitName(notANumber)).toThrow(TypeError);
    expect(() => synthGsDrumKitIsVoicedApart(notANumber)).toThrow(TypeError);
    expect(() => synthGsVariationIsVoicedApart(notANumber, 0)).toThrow(TypeError);
    expect(() => synthGsVariationIsVoicedApart(0, notANumber)).toThrow(TypeError);
  });
});
