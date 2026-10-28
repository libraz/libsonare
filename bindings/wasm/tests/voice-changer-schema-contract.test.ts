import { readFile } from 'node:fs/promises';
import Ajv2020 from 'ajv/dist/2020.js';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  realtimeVoiceChangerPresetJson,
  validateRealtimeVoiceChangerPresetJson,
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
  expect(validateRealtimeVoiceChangerPresetJson(json).ok).toBe(false);
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
      expect(validateRealtimeVoiceChangerPresetJson(json).ok).toBe(true);
    }
  });

  it('accepts schema versions 1 and 2, and a version 2 document needs the transformer sections', () => {
    const preset = JSON.parse(realtimeVoiceChangerPresetJson('neutral-monitor')) as {
      dsp: Record<string, unknown>;
      schemaVersion: number;
    };
    expect(validateSchema(preset), JSON.stringify(validateSchema.errors)).toBe(true);
    expect(validateRealtimeVoiceChangerPresetJson(JSON.stringify(preset)).ok).toBe(true);

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
    const legacy = { ...preset, schemaVersion: 1, dsp: legacyDsp };
    expect(validateSchema(legacy), JSON.stringify(validateSchema.errors)).toBe(true);
    expect(validateRealtimeVoiceChangerPresetJson(JSON.stringify(legacy)).ok).toBe(true);

    expectBothValidatorsToReject({ ...preset, dsp: legacyDsp });
  });

  it('rejects representative schema violations through both validators', () => {
    const factoryPreset = JSON.parse(realtimeVoiceChangerPresetJson('neutral-monitor')) as {
      dsp: { retune: { grainSize: number } };
      id: string;
      schemaVersion: number;
    };

    expect(factoryPreset.schemaVersion).toBe(2);
    expectBothValidatorsToReject({ ...factoryPreset, schemaVersion: 3 });
    expectBothValidatorsToReject({ ...factoryPreset, schemaVersion: 0 });
    expectBothValidatorsToReject({ ...factoryPreset, id: 'Invalid ID' });
    expectBothValidatorsToReject({ ...factoryPreset, unexpected: true });
    expectBothValidatorsToReject({
      ...factoryPreset,
      dsp: {
        ...factoryPreset.dsp,
        retune: { ...factoryPreset.dsp.retune, grainSize: 1.5 },
      },
    });
  });
});
