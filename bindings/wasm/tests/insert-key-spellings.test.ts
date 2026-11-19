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
  ['effects.modulation.pitchShifter', 'semitones', 'pitchSemitones'],
  ['effects.modulation.pitchShifter', 'semitones2', 'pitchSemitones2'],
  ['effects.modulation.pitchShifter', 'cents', 'pitchCents'],
  ['effects.modulation.pitchShifter', 'cents2', 'pitchCents2'],
];

describe('insert key spellings', () => {
  it.each(RENAMED)('%s names %s as %s', (insert, old, current) => {
    const names = masteringInsertParamNames(insert);
    expect(names).toContain(current);
    expect(names).not.toContain(old);
    expect(() =>
      masteringInsertTiming({ insert, params: { [old]: 1 } as never, sampleRate: 48000 }),
    ).toThrow(`${old} (renamed to ${current})`);
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
    expect(() =>
      masteringApplyProcessor({
        processor: 'repair.trimSilence',
        samples,
        sampleRate: 48000,
        params: { threshold: 0.05 } as never,
      }),
    ).toThrow('threshold (renamed to thresholdDb)');
  });

  it('reads the de-esser band Q in a chain document as dynamics.deesser.bandPassQ', () => {
    const config = masteringChainConfigFromJson(
      '{"version":2,"params":{"dynamics.deesser.bandPassQ":2}}',
    );
    expect(config.dynamics?.deesser?.bandPassQ).toBe(2);
    expect(() =>
      masteringChainConfigFromJson('{"version":2,"params":{"dynamics.deesser.bandpassQ":2}}'),
    ).toThrow(
      'unknown key params.dynamics.deesser.bandpassQ (renamed to params.dynamics.deesser.bandPassQ)',
    );
  });
});
