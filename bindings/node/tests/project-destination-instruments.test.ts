import { describe, expect, it } from 'vitest';
import {
  Project,
  type SynthPatch,
  synthEngineParamInfo,
  synthPatchParamInfo,
} from '../src/index.js';

describe('project destination instruments', () => {
  it('names the public config field in a native refusal', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      expect(() =>
        project.setDestinationInstrument(0, { kind: 'builtin', config: { gain: 5 } }),
      ).toThrow(/instrument\.config\.gain/);
    } finally {
      project.destroy();
    }
  });

  it('reads synth, built-in, and SoundFont bindings with bank and program', () => {
    const project = new Project();
    try {
      for (const destinationId of [3, 7, 12]) {
        const trackId = project.addTrack({ kind: 'midi' });
        project.setTrackMidiDestination(trackId, destinationId);
      }

      project.setDestinationInstrument(
        3,
        { kind: 'synth', patch: { preset: 'sine', useGmPrograms: true } },
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

      const bindings = project.destinationInstruments();
      expect(bindings).toEqual([
        {
          destinationId: 3,
          instrument: { kind: 'synth', patch: { preset: 'sine' }, useGmPrograms: true },
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
      const beforeReset = project.toJson();
      for (const binding of bindings) {
        project.setDestinationInstrument(binding.destinationId, binding.instrument, {
          ...(binding.bank === undefined ? {} : { bank: binding.bank }),
          ...(binding.program === undefined ? {} : { program: binding.program }),
        });
      }
      expect(project.toJson()).toBe(beforeReset);
      expect(project.destinationInstruments()).toEqual(bindings);
    } finally {
      project.destroy();
    }
  });

  it('round-trips every synth patch enum field as a catalog token', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      const enumFields = synthPatchParamInfo().filter((field) => field.type === 'enum');
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
        const token = field.values.find((name) => name !== field.default);
        expect(token, field.name).toBeDefined();
        project.setDestinationInstrument(0, {
          kind: 'synth',
          patch: { [field.name]: token } as unknown as SynthPatch,
        });
        const binding = project.destinationInstruments()[0];
        expect(binding?.instrument).toMatchObject({
          kind: 'synth',
          patch: { [field.name]: token },
        });
        const beforeReset = project.toJson();
        if (binding !== undefined) {
          project.setDestinationInstrument(binding.destinationId, binding.instrument);
        }
        expect(project.toJson()).toBe(beforeReset);
      }

      project.setDestinationInstrument(0, { kind: 'synth', patch: { engineMode: 'fm' } });
      expect(project.destinationInstruments()[0]?.instrument).toMatchObject({
        kind: 'synth',
        patch: { engineMode: 'fm' },
      });
      const binding = project.destinationInstruments()[0];
      const beforeReset = project.toJson();
      if (binding !== undefined) {
        project.setDestinationInstrument(binding.destinationId, binding.instrument);
      }
      expect(project.toJson()).toBe(beforeReset);
    } finally {
      project.destroy();
    }
  });

  it('keeps absent and cleared mod matrices distinct and copies engine parameters', () => {
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

  it('clears a stored instrument when the instrument argument is omitted', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      project.setDestinationInstrument(0, { kind: 'builtin', config: {} });
      expect(project.destinationInstruments()).toHaveLength(1);

      project.setDestinationInstrument(0);
      expect(project.destinationInstruments()).toEqual([]);
    } finally {
      project.destroy();
    }
  });

  it('hoists the synth GM flag and refuses conflicts or nonpersistent kinds', () => {
    const project = new Project();
    try {
      project.addTrack({ kind: 'midi' });
      project.setDestinationInstrument(0, {
        kind: 'synth',
        patch: { preset: 'sine', useGmPrograms: true },
      });
      expect(project.destinationInstruments()[0]?.instrument).toEqual({
        kind: 'synth',
        patch: { preset: 'sine' },
        useGmPrograms: true,
      });
      expect(() =>
        project.setDestinationInstrument(0, {
          kind: 'synth',
          patch: { useGmPrograms: true },
          useGmPrograms: false,
        }),
      ).toThrow(/useGmPrograms conflicts/);
      expect(() => project.setDestinationInstrument(0, { kind: 'callback' } as never)).toThrow(
        expect.objectContaining({ codeName: 'NotSupported', reason: 'notPersistable' }),
      );
      expect(() =>
        project.setDestinationInstrument(0, {
          kind: 'synth',
          patch: { engineMode: 'sample' },
        }),
      ).toThrow(expect.objectContaining({ codeName: 'NotSupported', reason: 'notPersistable' }));
    } finally {
      project.destroy();
    }
  });
});
