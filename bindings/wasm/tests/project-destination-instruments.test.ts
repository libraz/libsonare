import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  Project,
  type ProjectInstrument,
  type SynthPatch,
  type SynthPatchEnumParamInfo,
  synthEngineParamInfo,
  synthPatchParamInfo,
} from '../dist/index.js';

beforeAll(async () => {
  await init();
});

describe('project destination instruments', () => {
  it('reads every instrument variant and preserves a write-read-write snapshot', () => {
    const project = new Project();
    try {
      for (const destinationId of [3, 7, 12]) {
        const trackId = project.addTrack({ kind: 'midi' });
        project.setTrackMidiDestination(trackId, destinationId);
      }

      project.setDestinationInstrument(
        3,
        {
          kind: 'synth',
          patch: { preset: 'sine', gain: 0, modRoutings: [] },
          useGmPrograms: true,
        },
        { bank: { msb: 2, lsb: 5 }, program: 41 },
      );
      project.setDestinationInstrument(7, {
        kind: 'builtin',
        config: {
          waveform: 'triangle',
          gain: 0.375,
          attackMs: 12,
          decayMs: 34,
          sustain: 0.625,
          releaseMs: 78,
          polyphony: 24,
        },
      });
      project.setDestinationInstrument(12, {
        kind: 'sf2',
        config: {
          gain: 0.75,
          polyphony: 32,
          preferModelForModeledFamilies: true,
          clearBankRig: true,
          gsEfxRealization: 'classic',
        },
        soundFontUri: 'urn:bank:missing',
      });

      const before = project.toJson();
      const bindings = project.destinationInstruments();
      expect(bindings).toEqual([
        {
          destinationId: 3,
          instrument: {
            kind: 'synth',
            patch: { preset: 'sine', gain: 0, modRoutings: [] },
            useGmPrograms: true,
          },
          bank: { msb: 2, lsb: 5 },
          program: 41,
        },
        {
          destinationId: 7,
          instrument: {
            kind: 'builtin',
            config: {
              waveform: 'triangle',
              gain: 0.375,
              attackMs: 12,
              decayMs: 34,
              sustain: 0.625,
              releaseMs: 78,
              polyphony: 24,
            },
          },
        },
        {
          destinationId: 12,
          instrument: {
            kind: 'sf2',
            config: {
              gain: 0.75,
              polyphony: 32,
              preferModelForModeledFamilies: true,
              clearBankRig: true,
              gsEfxRealization: 'classic',
            },
            soundFontUri: 'urn:bank:missing',
          },
        },
      ]);
      for (const binding of bindings) {
        project.setDestinationInstrument(binding.destinationId, binding.instrument, {
          ...(binding.bank === undefined ? {} : { bank: binding.bank }),
          ...(binding.program === undefined ? {} : { program: binding.program }),
        });
      }
      expect(project.toJson()).toBe(before);
      expect(project.destinationInstruments()).toEqual(bindings);
    } finally {
      project.destroy();
    }
  });

  it('reads each non-default synth patch enum as its catalog token', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      const enumFields = synthPatchParamInfo().filter(
        (field): field is SynthPatchEnumParamInfo => field.type === 'enum',
      );
      expect(enumFields.map((field) => field.name).sort()).toEqual(
        [
          'body',
          'filterModel',
          'filterOutput',
          'lfoShape',
          'retrigger',
          'sampleKeyTrack',
          'sampleLoop',
          'waveform',
        ].sort(),
      );
      for (const field of enumFields) {
        const token = field.values.find((value) => value !== field.default);
        expect(token, field.name).toBeDefined();
        project.setDestinationInstrument(0, {
          kind: 'synth',
          patch: { [field.name]: token } as unknown as SynthPatch,
        });
        expect(project.destinationInstruments()[0]?.instrument).toMatchObject({
          kind: 'synth',
          patch: { [field.name]: token },
        });
      }
    } finally {
      project.destroy();
    }
  });

  it('distinguishes an omitted mod matrix from a clear and copies engine parameters', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      project.setDestinationInstrument(0, { kind: 'synth', patch: { preset: 'sine' } });
      const absent = project.destinationInstruments()[0]?.instrument;
      expect(absent).toMatchObject({ kind: 'synth', patch: { preset: 'sine' } });
      expect(absent && 'patch' in absent ? 'modRoutings' in absent.patch : false).toBe(false);

      project.setDestinationInstrument(0, {
        kind: 'synth',
        patch: { preset: 'sine', modRoutings: [] },
      });
      expect(project.destinationInstruments()[0]?.instrument).toMatchObject({
        kind: 'synth',
        patch: { preset: 'sine', modRoutings: [] },
      });

      const parameter = synthEngineParamInfo('fm')[0];
      expect(parameter).toBeDefined();
      if (parameter === undefined) {
        throw new Error('FM engine parameter catalog is empty');
      }
      const value =
        typeof parameter.default === 'boolean' ? Number(parameter.default) : parameter.default;
      project.setDestinationInstrument(0, {
        kind: 'synth',
        patch: {
          preset: 'sine',
          engineMode: 'fm',
          engineParams: { [parameter.name]: value },
        },
      });
      expect(project.destinationInstruments()[0]?.instrument).toMatchObject({
        kind: 'synth',
        patch: { engineParams: { [parameter.name]: value } },
      });
    } finally {
      project.destroy();
    }
  });

  it('clears a binding and reports unsupported persistence with its reason', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      project.setDestinationInstrument(0, { kind: 'builtin', config: {} });
      expect(project.destinationInstruments()).toHaveLength(1);
      project.setDestinationInstrument(0);
      expect(project.destinationInstruments()).toEqual([]);

      for (const instrument of [{ kind: 'callback' }, { render: () => undefined }]) {
        let caught: unknown;
        try {
          project.setDestinationInstrument(0, instrument as never);
        } catch (error) {
          caught = error;
        }
        expect(isSonareError(caught)).toBe(true);
        expect(isSonareError(caught) && caught.code).toBe(ErrorCode.NotSupported);
        expect(isSonareError(caught) && caught.reason).toBe('notPersistable');
      }
    } finally {
      project.destroy();
    }
  });

  it('names config gain refusals with the public config path', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      expect(() =>
        project.setDestinationInstrument(0, {
          kind: 'builtin',
          config: { gain: 9 },
        } as ProjectInstrument),
      ).toThrow(/instrument\.config\.gain/);
      expect(() =>
        project.setDestinationInstrument(0, {
          kind: 'sf2',
          config: { gain: 9 },
        } as ProjectInstrument),
      ).toThrow(/instrument\.config\.gain/);
    } finally {
      project.destroy();
    }
  });
});
