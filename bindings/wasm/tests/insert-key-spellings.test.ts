/**
 * Insert parameter keys carry their unit and split a compound word: each row is
 * the key a processor publishes and the spelling it replaced, which the
 * processor no longer names.
 */

import { describe, expect, it } from 'vitest';
import {
  init,
  type MasteringInsertName,
  masteringApplyProcessor,
  masteringChainConfigFromJson,
  masteringInsertParamNames,
  masteringInsertTiming,
} from '../dist/index.js';
import { chainUnknownKeyText } from './_helpers.js';

await init();

const RENAMED: readonly (readonly [MasteringInsertName, string, string])[] = [
  ['effects.reverb.room', 'maxSeconds', 'maxSec'],
  ['effects.acoustic.roomMorph', 'maxSeconds', 'maxSec'],
  ['effects.modulation.rotary', 'accelTauS', 'accelTauSec'],
  ['effects.modulation.rotary', 'decelTauS', 'decelTauSec'],
  ['effects.reverb.velvet', 'reverbTimeS', 'reverbTimeSec'],
  ['dynamics.deesser', 'bandpassQ', 'bandPassQ'],
  ['voice.changer', 'eqHighpassHz', 'eqHighPassHz'],
  ['voice.changer', 'retuneSemitones', 'retunePitchSemitones'],
  ['voice.changer', 'retuneGrainSize', 'retuneGrainSizeSamples'],
  ['voice.changer', 'voiceEngineEngine', 'voiceEngine'],
  ['voice.changer', 'reverbVariationVariation', 'reverbVariation'],
  ['effects.modulation.pitchShifter', 'semitones', 'pitchSemitones'],
  ['effects.modulation.pitchShifter', 'semitones2', 'pitchSemitones2'],
  ['effects.modulation.pitchShifter', 'cents', 'pitchCents'],
  ['effects.modulation.pitchShifter', 'cents2', 'pitchCents2'],
];

/** The refusal of an undeclared key: the accepted list, never a pointer to a successor. */
function unknownKeyRefusal(call: () => unknown, key: string, current: string, root = ''): void {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  const message = (caught as Error | undefined)?.message ?? '';
  const escapeRegExp = (text: string) => text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  expect(message).toMatch(
    new RegExp(
      `unknown key ${escapeRegExp(root)}${escapeRegExp(key)} \\(expected [^)]*\\b${escapeRegExp(current)}\\b`,
    ),
  );
  expect(message).not.toMatch(/renamed to/);
}

describe('insert key spellings', () => {
  it.each(RENAMED)('%s names %s as %s', (insert, old, current) => {
    const names = masteringInsertParamNames(insert);
    expect(names).toContain(current);
    expect(names).not.toContain(old);
    unknownKeyRefusal(
      () => masteringInsertTiming({ insert, params: { [old]: 1 } as never, sampleRate: 48000 }),
      old,
      current,
      'params.',
    );
  });

  it.each([
    ['final.bitDepth', 'targetBits'],
    ['final.outputChain', 'ditherType, targetBits'],
  ] as const)('%s refuses the retired clamp key as an unknown key', (processor, expected) => {
    expect(() =>
      masteringApplyProcessor({
        samples: new Float32Array(64),
        sampleRate: 48000,
        processor,
        params: { clamp: true } as never,
      }),
    ).toThrow(
      new TypeError(`masteringApplyProcessor: unknown key params.clamp (expected ${expected})`),
    );
  });

  it('reads the repair.trimSilence threshold as thresholdDb in dBFS', () => {
    const samples = new Float32Array(4800);
    samples.fill(0.5, 1200, 3600);
    const trimmed = masteringApplyProcessor({
      processor: 'repair.trimSilence',
      samples,
      sampleRate: 48000,
      params: { thresholdDb: -26 },
    });
    expect(trimmed.samples.length).toBeLessThan(samples.length);
    unknownKeyRefusal(
      () =>
        masteringApplyProcessor({
          processor: 'repair.trimSilence',
          samples,
          sampleRate: 48000,
          params: { threshold: 0.05 } as never,
        }),
      'threshold',
      'thresholdDb',
      'params.',
    );
  });

  it('reads the de-esser band Q in a chain document as dynamics.deesser.bandPassQ', () => {
    const config = masteringChainConfigFromJson(
      '{"version":2,"params":{"dynamics.deesser.bandPassQ":2}}',
    );
    expect(config.dynamics?.deesser?.bandPassQ).toBe(2);
    // A chain document names the refused key alone; its accepted set is the whole catalog.
    expect(() =>
      masteringChainConfigFromJson('{"version":2,"params":{"dynamics.deesser.bandpassQ":2}}'),
    ).toThrow(
      expect.objectContaining({
        name: 'SonareError',
        message: chainUnknownKeyText(
          'masteringChainConfigFromJson: unknown key params.dynamics.deesser.bandpassQ',
        ),
      }),
    );
  });
});
