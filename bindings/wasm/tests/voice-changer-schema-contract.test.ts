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

  it('rejects representative schema violations through both validators', () => {
    const factoryPreset = JSON.parse(realtimeVoiceChangerPresetJson('neutral-monitor')) as {
      dsp: { retune: { grainSize: number } };
      id: string;
      schemaVersion: number;
    };

    expectBothValidatorsToReject({ ...factoryPreset, schemaVersion: 2 });
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
