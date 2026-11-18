import { readFileSync } from 'node:fs';
import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  AutomationCurve,
  ChannelLayout,
  MeterTap,
  MixAnalysisBand,
  MixAssistantTrackProfile,
  MixBusRole,
  MixingAssistantSuggestRequest,
  MixMeterSnapshot,
  MixSceneBus,
  MixSceneDocumentInput,
  MixScenePreset,
  MixSceneSend,
  MixSceneStrip,
  MixSourceClass,
  MixStereoRequest,
  MixTrackAlignment,
  PanLaw,
  PanMode,
  SendTiming,
  StripMeteringOptions,
  SurroundPan,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import {
  Mixer,
  mixingAssistantSourceClassFromName,
  mixingAssistantSourceClassNames,
  mixingAssistantSuggest,
  mixingScenePreset,
  mixingScenePresetNames,
  mixStereo,
} from '../../dist/index.js';
import { addon } from '../../src/native.js';

const SR = 22050;
const BLOCK = 512;
const api = sonare as unknown as Record<string, unknown>;

type Loose = (...args: unknown[]) => unknown;
const loose = (fn: unknown): Loose => fn as Loose;

function tone(seconds: number, hz = 220, amplitude = 0.3): Float32Array {
  const out = new Float32Array(Math.round(SR * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = amplitude * Math.sin((2 * Math.PI * hz * i) / SR);
  }
  return out;
}

const leftOnly = () => new Float32Array(BLOCK).fill(1);
const silence = () => new Float32Array(BLOCK);
const strip = (extra: Record<string, unknown> = {}) => ({
  left: leftOnly(),
  right: silence(),
  ...extra,
});
const mix = (extra: Record<string, unknown> = {}, sampleRate = SR) =>
  mixStereo({ strips: [strip(extra)], sampleRate });

/** Runs `fn` and returns what it threw, or undefined. */
function thrown(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function assistantTracks() {
  return [
    { id: 'bass', name: 'Bass', channels: [tone(0.6, 80)] },
    { id: 'keys', name: 'Keys', channels: [tone(0.6, 440), tone(0.6, 441)] },
  ];
}

describe('mastering mixing surface', () => {
  describe('removed names are not exported', () => {
    it.each([
      'mixingScenePresetJson',
      'suggestMixScene',
      'suggestMixSceneJson',
      'mixSourceClassNames',
      'mixSourceClassFromName',
    ])('%s', (name) => {
      expect(api[name]).toBeUndefined();
    });
  });

  describe('mixStereo takes a request over strips', () => {
    it('MixStereoRequest.leftChannels and MixStereoRequest.rightChannels are replaced by strips', () => {
      expectTypeOf<MixStereoRequest['strips']>().toBeArray();
      const result = mix();
      expect(result.left).toHaveLength(BLOCK);
      expect(result.meters).toHaveLength(1);
      for (const key of ['leftChannels', 'rightChannels', 'faderDb', 'pan', 'panMode']) {
        expect(
          thrown(() => loose(mixStereo)({ strips: [strip()], sampleRate: SR, [key]: [0] })),
        ).toBeInstanceOf(TypeError);
      }
    });

    it('mixStereo is request-only', () => {
      expect(thrown(() => loose(mixStereo)([leftOnly()], [silence()], SR))).toBeInstanceOf(
        TypeError,
      );
      expect(
        thrown(() => loose(mixStereo)({ strips: [strip()], sampleRate: SR }, 1)),
      ).toBeInstanceOf(TypeError);
    });

    it('MixStereoRequest.sampleRate is required and an integer in range', () => {
      expect(thrown(() => loose(mixStereo)({ strips: [strip()] }))).toBeInstanceOf(TypeError);
      for (const sampleRate of [7999, 384001, 44100.5]) {
        expect(thrown(() => mix({}, sampleRate))).toBeInstanceOf(RangeError);
      }
    });

    it.each([
      ['no strips', () => mixStereo({ strips: [], sampleRate: SR }), RangeError],
      ['strips not an array', () => loose(mixStereo)({ strips: 1, sampleRate: SR }), TypeError],
      [
        'a strip not an object',
        () => loose(mixStereo)({ strips: [null], sampleRate: SR }),
        TypeError,
      ],
      ['an unknown strip key', () => mix({ gain: 1 }), TypeError],
      ['a left that is not a Float32Array', () => mix({ left: [1] }), TypeError],
      ['unequal left and right', () => mix({ right: new Float32Array(3) }), RangeError],
      ['a non-numeric pan', () => mix({ pan: '0.3' }), TypeError],
      ['a non-boolean muted', () => mix({ muted: 0 }), TypeError],
    ])('refuses %s', (_name, call, errorClass) => {
      expect(thrown(call)).toBeInstanceOf(errorClass);
    });

    it('validate: false skips only the non-finite scan', () => {
      const poisoned = leftOnly();
      poisoned[3] = Number.NaN;
      expect(thrown(() => mix({ left: poisoned }))).toBeInstanceOf(RangeError);
      expect(
        thrown(() =>
          mixStereo({ strips: [strip({ left: poisoned })], sampleRate: SR, validate: false }),
        ),
      ).not.toBeInstanceOf(TypeError);
    });

    it('applies per-strip settings, leaving a strip that omits a field at its default', () => {
      const trimmed = mixStereo({
        strips: [strip({ faderDb: -6 }), strip()],
        sampleRate: SR,
      });
      const plain = mixStereo({ strips: [strip(), strip()], sampleRate: SR });
      expect(trimmed.left[BLOCK - 1]).toBeLessThan(plain.left[BLOCK - 1]);
      const modeOnly = mix({ panMode: 'stereoPan' });
      expect(modeOnly.right[BLOCK - 1]).toBeGreaterThan(0.1);
      expect(mix().right[BLOCK - 1]).toBe(0);
    });
  });

  describe('closed sets refuse numbers and synonyms', () => {
    it.each([
      ['PanMode', 'stereo-pan'],
      ['PanMode', 'dual-pan'],
      ['PanMode', 'stereopan'],
      ['PanMode', 'Balance'],
    ])('%s %j in mixStereo', (_type, value) => {
      expect(thrown(() => mix({ panMode: value }))).toBeInstanceOf(RangeError);
    });

    it.each([1, 99])('PanMode %j in mixStereo is a wrong type', (value) => {
      expect(thrown(() => mix({ panMode: value }))).toBeInstanceOf(TypeError);
    });

    it('PanMode names three tokens', () => {
      expectTypeOf<'balance' | 'stereoPan' | 'dualPan'>().toExtend<PanMode>();
      for (const panMode of ['balance', 'stereoPan', 'dualPan']) {
        expect(thrown(() => mix({ panMode }))).toBeUndefined();
      }
    });

    it('mixingScenePreset refuses an unknown or non-string name', () => {
      expect(thrown(() => loose(mixingScenePreset)('nope'))).toBeInstanceOf(RangeError);
      expect(thrown(() => loose(mixingScenePreset)(1))).toBeInstanceOf(TypeError);
      expect(thrown(() => loose(mixingScenePreset)())).toBeInstanceOf(TypeError);
    });

    it('mixingAssistantSourceClassFromName refuses an unknown name', () => {
      expect(thrown(() => loose(mixingAssistantSourceClassFromName)('nope'))).toBeInstanceOf(
        RangeError,
      );
      expect(thrown(() => loose(mixingAssistantSourceClassFromName)(3))).toBeInstanceOf(TypeError);
    });
  });

  describe('token spellings are exact', () => {
    const withMixer = (fn: (mixer: Mixer) => void) => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
        sampleRate: SR,
        maxBlockSize: 256,
      });
      try {
        fn(mixer);
      } finally {
        mixer.destroy();
      }
    };

    it('PanLaw names four tokens and refuses the old spellings, synonyms and numbers', () => {
      expectTypeOf<'const3Db' | 'const4.5Db' | 'const6Db' | 'linear0Db'>().toExtend<PanLaw>();
      withMixer((mixer) => {
        for (const panLaw of ['const3Db', 'const4.5Db', 'const6Db', 'linear0Db']) {
          expect(thrown(() => mixer.setPanLaw('host', panLaw as PanLaw))).toBeUndefined();
        }
        for (const panLaw of ['const3dB', 'const4.5dB', 'const-3db', '-3db', '0db', 'linear']) {
          expect(thrown(() => mixer.setPanLaw('host', panLaw as never))).toBeInstanceOf(RangeError);
        }
        for (const panLaw of [0, 3]) {
          expect(thrown(() => mixer.setPanLaw('host', panLaw as never))).toBeInstanceOf(TypeError);
        }
      });
    });

    it('MeterTap is preFader or postFader and refuses numbers', () => {
      expectTypeOf<MeterTap>().toEqualTypeOf<'preFader' | 'postFader'>();
      withMixer((mixer) => {
        expect(thrown(() => mixer.stripMeter('host', 'preFader'))).toBeUndefined();
        expect(thrown(() => mixer.stripMeter('host', 1 as never))).toBeInstanceOf(Error);
      });
    });

    it('AutomationCurve is linear, exponential, hold or sCurve and refuses s-curve and numbers', () => {
      expectTypeOf<AutomationCurve>().toEqualTypeOf<'linear' | 'exponential' | 'hold' | 'sCurve'>();
      withMixer((mixer) => {
        for (const [curve, errorClass] of [
          ['s-curve', RangeError],
          ['scurve', RangeError],
          [3, TypeError],
        ] as const) {
          expect(
            thrown(() =>
              mixer.scheduleInsertAutomation('host', 0, {
                paramId: 0,
                samplePos: 0,
                value: 0,
                curve: curve as never,
              }),
            ),
          ).toBeInstanceOf(errorClass);
        }
      });
    });
  });

  describe('scene presets are typed documents', () => {
    it('mixingScenePresetNames lists the presets', () => {
      expectTypeOf(mixingScenePresetNames).returns.toEqualTypeOf<MixScenePreset[]>();
      expect([...mixingScenePresetNames()].sort()).toEqual([
        'commentaryDucking',
        'drumBusSubgroup',
        'vocalReverbSend',
      ]);
    });

    it('mixingScenePreset returns the parsed document with token fields', () => {
      const scene = mixingScenePreset('vocalReverbSend');
      expect(typeof scene).toBe('object');
      expect(scene.strips.length).toBeGreaterThan(0);
      for (const entry of scene.strips) {
        expect(entry.panMode).toBe('balance');
        expect(entry.panLaw).toBe('const3Db');
      }
      const sends = scene.strips.flatMap((entry) => entry.sends);
      expect(sends.length).toBeGreaterThan(0);
      for (const send of sends) {
        expect(['preFader', 'postFader']).toContain(send.timing);
      }
    });

    it('a returned preset builds a Mixer', () => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
        sampleRate: SR,
        maxBlockSize: 256,
      });
      try {
        expect(mixer.stripCount).toBeGreaterThan(0);
      } finally {
        mixer.destroy();
      }
    });

    it('MixSceneStrip.panMode and MixSceneStrip.panLaw and MixSceneBus.panMode and MixSceneBus.panLaw are tokens', () => {
      expectTypeOf<MixSceneStrip['panMode']>().toEqualTypeOf<PanMode>();
      expectTypeOf<NonNullable<MixSceneBus['panMode']>>().toEqualTypeOf<PanMode>();
      expectTypeOf<MixSceneStrip['panLaw']>().not.toBeNumber();
      expectTypeOf<NonNullable<MixSceneBus['panLaw']>>().not.toBeNumber();
    });

    it('MixSceneSend.timing is a SendTiming', () => {
      expectTypeOf<MixSceneSend['timing']>().toEqualTypeOf<SendTiming>();
    });

    it('MixSceneBus.layout and MixSceneBus.role and MixSceneStrip.sourceLayout are closed sets', () => {
      expectTypeOf<NonNullable<MixSceneBus['layout']>>().toEqualTypeOf<ChannelLayout>();
      expectTypeOf<MixSceneBus['role']>().toEqualTypeOf<MixBusRole>();
      expectTypeOf<NonNullable<MixSceneStrip['sourceLayout']>>().toEqualTypeOf<ChannelLayout>();
    });

    it('MixSceneStrip.surroundPan and MixSceneStrip.metering are named, fully populated records', () => {
      expectTypeOf<NonNullable<MixSceneStrip['surroundPan']>>().toEqualTypeOf<
        Required<SurroundPan>
      >();
      expectTypeOf<NonNullable<MixSceneStrip['metering']>>().toEqualTypeOf<
        Required<StripMeteringOptions>
      >();
    });

    it('MixSceneInsert is discriminated by processor', () => {
      type Insert = MixSceneStrip['inserts'][number];
      expectTypeOf<Insert['processor']>().toBeString();
      expectTypeOf<Insert>().toHaveProperty('params');
    });

    it('the input document types make every field but the ids optional', () => {
      const input: MixSceneDocumentInput = {
        strips: [{ id: 'a', panMode: 'dualPan', sends: [{ id: 's', destinationBusId: 'b' }] }],
        buses: [{ id: 'b', role: 'aux' }],
        vcaGroups: [{ id: 'v' }],
      };
      expect(input.strips).toHaveLength(1);
    });
  });

  describe('meters and profiles use the unit-bearing names', () => {
    const meterFields: Array<[string, string, string]> = [
      ['MixMeterSnapshot.truePeakDbL', 'truePeakDbtpL', 'truePeakDbL'],
      ['MixMeterSnapshot.truePeakDbR', 'truePeakDbtpR', 'truePeakDbR'],
      ['MixMeterSnapshot.maxTruePeakDb', 'maxTruePeakDbtp', 'maxTruePeakDb'],
      ['MixMeterSnapshot.channelCount', 'numChannels', 'channelCount'],
      ['MixMeterSnapshot.truePeakDb', 'truePeakDbtp', 'truePeakDb'],
    ];

    it.each(meterFields)('%s -> %s on a Mixer strip meter', (_entry, name, oldName) => {
      const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('commentaryDucking')), {
        sampleRate: SR,
        maxBlockSize: 256,
      });
      try {
        const meter = mixer.stripMeter('host') as unknown as Record<string, unknown>;
        expect(meter).toHaveProperty(name);
        expect(meter).not.toHaveProperty(oldName);
      } finally {
        mixer.destroy();
      }
    });

    // The one-shot mix's native meter carries no plane count or per-plane arrays.
    it.each(meterFields.slice(0, 3))('%s -> %s on a mixStereo meter', (_entry, name, oldName) => {
      const meter = mix().meters[0] as unknown as Record<string, unknown>;
      expect(meter).toHaveProperty(name);
      expect(meter).not.toHaveProperty(oldName);
    });

    it('MixMeterSnapshot names the fields in the type', () => {
      expectTypeOf<MixMeterSnapshot>().toHaveProperty('truePeakDbtpL');
      expectTypeOf<MixMeterSnapshot>().toHaveProperty('numChannels');
      expectTypeOf<MixMeterSnapshot>().not.toHaveProperty('truePeakDbL');
    });

    const profileFields: Array<[string, string, string]> = [
      ['MixAssistantTrackProfile.truePeakDb', 'truePeakDbtp', 'truePeakDb'],
      ['MixAssistantTrackProfile.channelCount', 'numChannels', 'channelCount'],
      ['MixAssistantTrackProfile.attackDensity', 'attackPerSecond', 'attackDensity'],
    ];

    it.each(profileFields)('%s -> %s on the assistant profile', (_entry, name, oldName) => {
      const result = mixingAssistantSuggest({ tracks: assistantTracks(), sampleRate: SR });
      for (const profile of result.tracks as unknown as Record<string, unknown>[]) {
        expect(profile).toHaveProperty(name);
        expect(profile).not.toHaveProperty(oldName);
      }
      expectTypeOf<MixAssistantTrackProfile>().toHaveProperty(name as 'numChannels');
    });

    it('MixAssistantTrackProfile.source is a MixSourceClass', () => {
      expectTypeOf<MixAssistantTrackProfile['source']>().toEqualTypeOf<MixSourceClass>();
      const result = mixingAssistantSuggest({ tracks: assistantTracks(), sampleRate: SR });
      expect(mixingAssistantSourceClassNames()).toContain(result.tracks[0].source);
    });

    it('MixingAssistantSuggestRequest names the true-peak headroom in dBTP', () => {
      expectTypeOf<MixingAssistantSuggestRequest>().toHaveProperty('mixBusHeadroomDbtp');
    });
  });

  describe('assistant record types', () => {
    it('MixAlignmentPair is renamed MixTrackAlignment and MixBandName MixAnalysisBand', () => {
      expectTypeOf<MixTrackAlignment>().toHaveProperty('lagSamples');
      expectTypeOf<MixAnalysisBand>().toEqualTypeOf<
        'sub' | 'low' | 'lowMid' | 'mid' | 'highMid' | 'high' | 'air'
      >();
      expect(api.mixSourceClassNames).toBeUndefined();
    });
  });

  describe('the assistant takes a flat request', () => {
    it('suggestMixScene is renamed mixingAssistantSuggest and takes tunables beside the tracks', () => {
      const full = mixingAssistantSuggest({ tracks: assistantTracks(), sampleRate: SR });
      const none = mixingAssistantSuggest({
        tracks: assistantTracks(),
        sampleRate: SR,
        suggestionStrength: 0,
      });
      expect(none.scene.strips.every((entry) => entry.inputTrimDb === 0)).toBe(true);
      expect(full.scene.strips.some((entry) => entry.inputTrimDb !== 0)).toBe(true);
      for (const entry of full.scene.strips) {
        expect(entry.panMode).toBe('balance');
        for (const send of entry.sends) {
          expect(['preFader', 'postFader']).toContain(send.timing);
        }
      }
    });

    it('SuggestMixSceneRequest drops the options bag and the misspelt or removed keys', () => {
      for (const extra of [
        { options: { suggestionStrength: 0 } },
        { suggestion_strength: 0 },
        { suggestionStrenght: 0.5 },
      ]) {
        expect(
          thrown(() =>
            loose(mixingAssistantSuggest)({ tracks: assistantTracks(), sampleRate: SR, ...extra }),
          ),
        ).toBeInstanceOf(TypeError);
      }
    });

    it('requires an integer sampleRate and refuses a wrong-typed tunable', () => {
      expect(
        thrown(() => loose(mixingAssistantSuggest)({ tracks: assistantTracks() })),
      ).toBeInstanceOf(TypeError);
      expect(
        thrown(() => mixingAssistantSuggest({ tracks: assistantTracks(), sampleRate: 7999 })),
      ).toBeInstanceOf(RangeError);
      expect(
        thrown(() =>
          loose(mixingAssistantSuggest)({
            tracks: assistantTracks(),
            sampleRate: SR,
            validate: 'no',
          }),
        ),
      ).toBeInstanceOf(TypeError);
    });

    it('the source-class lookups are renamed and round-trip', () => {
      const names = mixingAssistantSourceClassNames();
      expect(names.length).toBe(16);
      names.forEach((name, index) => {
        expect(mixingAssistantSourceClassFromName(name)).toBe(index);
      });
    });

    it('is request-only', () => {
      expect(
        thrown(() => loose(mixingAssistantSuggest)({ tracks: [], sampleRate: SR }, 1)),
      ).toBeInstanceOf(TypeError);
    });
  });

  describe('StripMeteringOptions.truePeakOversample', () => {
    it('an explicit 0 is a RangeError, omission is the library default', () => {
      const mixer = new Mixer({ sampleRate: SR });
      try {
        expect(thrown(() => mixer.addStrip('a', { truePeakOversample: 0 }))).toBeInstanceOf(
          RangeError,
        );
        expect(thrown(() => mixer.addStrip('b', {}))).toBeUndefined();
        expect(thrown(() => mixer.addStrip('c', { truePeakOversample: 8 }))).toBeUndefined();
      } finally {
        mixer.destroy();
      }
    });

    it('StripMeteringOptions is declared with the scene types', () => {
      expectTypeOf<StripMeteringOptions>().toHaveProperty('truePeakOversample');
    });
  });

  describe('native still uses the old spelling', () => {
    const module = () => addon;

    it('the scene document holds pan mode, pan law and send timing as tokens', () => {
      const scene = JSON.parse(module().mixingScenePresetJson('vocalReverbSend'));
      const first = scene.strips[0];
      expect(['balance', 'stereoPan', 'dualPan']).toContain(first.panMode);
      expect(['const3Db', 'const4.5Db', 'const6Db', 'linear0Db']).toContain(first.panLaw);
      expect(['preFader', 'postFader']).toContain(first.sends[0].timing);
    });

    it('a native mixer scene takes the token spellings and refuses ints and pre/post', () => {
      const strip = { id: 'a', sends: [{ id: 's', destinationBusId: 'master', sendDb: 0 }] };
      const bus = { id: 'master', role: 'master' };
      const create = (patch: Record<string, unknown>) => () =>
        new (addon as unknown as { Mixer: new (...args: unknown[]) => unknown }).Mixer(
          JSON.stringify({ strips: [{ ...strip, ...patch }], buses: [bus] }),
          SR,
          BLOCK,
        );
      expect(
        create({
          panMode: 'balance',
          panLaw: 'const3Db',
          sends: [{ ...strip.sends[0], timing: 'postFader' }],
        }),
      ).not.toThrow();
      const refused: Array<Record<string, unknown>> = [
        { panMode: 0 },
        { panLaw: 0 },
        { sends: [{ ...strip.sends[0], timing: 'post' }] },
      ];
      for (const patch of refused) {
        expect(create(patch), Object.keys(patch)[0]).toThrow();
      }
    });

    it('mixStereo still takes per-field arrays and a pan mode spelling it lowercases', () => {
      const raw = module().mixStereo([leftOnly(), leftOnly()], [silence(), silence()], SR, {
        faderDb: [-6],
        panMode: [1],
      }) as { left: Float32Array };
      expect(raw.left).toHaveLength(BLOCK);
    });

    it('the native sources still read the per-field mix options and the old profile keys', () => {
      const source = (path: string) =>
        readFileSync(new URL(`../../src/addon/${path}`, import.meta.url), 'utf8');
      expect(source('effects/mixing.cpp')).toContain(
        '"inputTrimDb", "faderDb", "pan", "panMode", "width", "muted"',
      );
    });
  });
});
