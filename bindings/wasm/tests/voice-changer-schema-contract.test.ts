import { readFile } from 'node:fs/promises';
import Ajv2020 from 'ajv/dist/2020.js';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerValidatePresetJson,
} from '../src/index';

const presetSchemaUrl = new URL(
  '../../../schemas/realtime-voice-changer-preset.schema.json',
  import.meta.url,
);
const presetPackSchemaUrl = new URL(
  '../../../schemas/realtime-voice-changer-preset-pack.schema.json',
  import.meta.url,
);
const factoryPresetPackUrl = new URL(
  '../../../schemas/realtime-voice-changer-presets.example.json',
  import.meta.url,
);
const packagedPresetSchemaUrl = new URL(
  '../dist/schemas/realtime-voice-changer-preset.schema.json',
  import.meta.url,
);
const presetSchema = JSON.parse(await readFile(presetSchemaUrl, 'utf8')) as object;
const presetPackSchema = JSON.parse(await readFile(presetPackSchemaUrl, 'utf8')) as object;
const ajv = new Ajv2020({ strict: false });
const validateSchema = ajv.compile(presetSchema);
const validatePackSchema = ajv.compile(presetPackSchema);

function expectBothValidatorsToReject(preset: object): void {
  const json = JSON.stringify(preset);
  expect(validateSchema(preset)).toBe(false);
  expect(realtimeVoiceChangerValidatePresetJson(json).ok).toBe(false);
}

describe('Realtime Voice Changer preset JSON Schema contract', () => {
  beforeAll(async () => {
    await init();
  });

  it('ships the canonical schema and accepts every factory preset through both validators', async () => {
    expect(await readFile(packagedPresetSchemaUrl, 'utf8')).toBe(
      await readFile(presetSchemaUrl, 'utf8'),
    );

    const factoryPack = JSON.parse(await readFile(factoryPresetPackUrl, 'utf8')) as {
      presets: object[];
    };
    expect(validatePackSchema(factoryPack), JSON.stringify(validatePackSchema.errors)).toBe(true);
    for (const preset of factoryPack.presets) {
      const json = JSON.stringify(preset);
      expect(
        validateSchema(preset),
        validateSchema.errors?.map((error) => error.message).join(', '),
      ).toBe(true);
      expect(realtimeVoiceChangerValidatePresetJson(json).ok).toBe(true);
    }
  });

  it('accepts schema version 3, which needs the transformer sections, and refuses an older document', () => {
    const preset = JSON.parse(realtimeVoiceChangerPresetJson('neutralMonitor')) as {
      dsp: Record<string, Record<string, unknown>>;
      schemaVersion: number;
    };
    expect(validateSchema(preset), JSON.stringify(validateSchema.errors)).toBe(true);
    expect(realtimeVoiceChangerValidatePresetJson(JSON.stringify(preset)).ok).toBe(true);

    const transformerSections = [
      'voiceEngine',
      'pitchCorrection',
      'harmony',
      'vocoder',
      'character',
      'reverbVariation',
      'midi',
    ];
    const legacyDsp = Object.fromEntries(
      Object.entries(preset.dsp).filter(([key]) => !transformerSections.includes(key)),
    );
    expectBothValidatorsToReject({ ...preset, dsp: legacyDsp });

    // A version 1 document in the old spellings is refused by the library as well as the schema.
    const { pitchSemitones, grainSizeSamples, ...retune } = preset.dsp.retune as {
      pitchSemitones: number;
      grainSizeSamples: number;
    };
    const { highPassHz, ...eq } = preset.dsp.eq as { highPassHz: number };
    const old = {
      ...preset,
      schemaVersion: 1,
      id: 'neutral-monitor',
      dsp: {
        ...legacyDsp,
        retune: { ...retune, semitones: pitchSemitones, grainSize: grainSizeSamples },
        eq: { ...eq, highpassHz: highPassHz },
      },
    };
    expect(validateSchema(old)).toBe(false);
    const refused = realtimeVoiceChangerValidatePresetJson(JSON.stringify(old));
    expect(refused.ok).toBe(false);
    expect(refused.ok ? '' : refused.error).toContain(
      'schemaVersion must be 4, the version this release reads; the document was written for an older schema',
    );
  });

  it('rejects representative schema violations through both validators', () => {
    const factoryPreset = JSON.parse(realtimeVoiceChangerPresetJson('neutralMonitor')) as {
      dsp: { retune: { grainSizeSamples?: number } };
      id: string;
      schemaVersion: number;
    };

    expect(factoryPreset.schemaVersion).toBe(4);
    expect('grainSizeSamples' in factoryPreset.dsp.retune).toBe(false);
    expectBothValidatorsToReject({ ...factoryPreset, schemaVersion: 3 });
    expectBothValidatorsToReject({ ...factoryPreset, schemaVersion: 5 });
    expectBothValidatorsToReject({ ...factoryPreset, schemaVersion: 0 });
    expectBothValidatorsToReject({ ...factoryPreset, id: 'Invalid ID' });
    expectBothValidatorsToReject({ ...factoryPreset, unexpected: true });
    expectBothValidatorsToReject({
      ...factoryPreset,
      dsp: {
        ...factoryPreset.dsp,
        retune: { ...factoryPreset.dsp.retune, grainSizeSamples: 1.5 },
      },
    });
    expectBothValidatorsToReject({
      ...factoryPreset,
      dsp: {
        ...factoryPreset.dsp,
        retune: { ...factoryPreset.dsp.retune, grainSizeSamples: 0 },
      },
    });
  });
});
