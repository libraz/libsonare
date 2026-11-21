import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import * as indexModule from '../../dist/index.js';
import {
  type CharacterType,
  init,
  type MidiRelease,
  type NoteDivision,
  type PitchCorrectionMode,
  RealtimeVoiceChanger,
  type RealtimeVoiceChangerConfig,
  type RealtimeVoiceChangerConfigInput,
  type RealtimeVoiceChangerOptions,
  type RealtimeVoiceChangerValidatePresetJsonResult,
  type ReverbVariation,
  realtimeVoiceChangerPresetConfig,
  realtimeVoiceChangerPresetJson,
  realtimeVoiceChangerPresetNames,
  realtimeVoiceChangerRender,
  realtimeVoiceChangerValidatePresetJson,
  type ScaleKind,
  StreamingRetune,
  type StreamingRetuneAppliedConfig,
  type StreamingRetuneConfig,
  type VocoderCarrierSource,
  type VocoderCarrierWave,
  type VoiceEngine,
  type VoicePresetId,
  voiceChange,
  voiceCharacterPresetId,
} from '../../dist/index.js';
import * as workletModule from '../../dist/worklet.js';

const sampleRate = 22050;
const BLOCK = 128;

function tone(frames: number, hz = 220, gain = 0.2): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = gain * Math.sin((2 * Math.PI * hz * i) / sampleRate);
  }
  return out;
}

function caught(fn: () => unknown): { name: string; code?: unknown } {
  try {
    fn();
  } catch (error) {
    return error as { name: string; code?: unknown };
  }
  throw new Error('expected a throw');
}

function expectClose(actual: ArrayLike<number>, expected: ArrayLike<number>, tolerance = 1e-5) {
  expect(actual.length).toBe(expected.length);
  for (let i = 0; i < actual.length; i++) {
    expect(Math.abs((actual[i] as number) - (expected[i] as number))).toBeLessThanOrEqual(
      tolerance,
    );
  }
}

function changer(
  options: Partial<RealtimeVoiceChangerOptions> = {},
  config?: RealtimeVoiceChangerConfigInput,
): RealtimeVoiceChanger {
  return new RealtimeVoiceChanger({
    sampleRate,
    numChannels: 1,
    maxBlockSize: BLOCK,
    ...options,
    ...(config === undefined ? {} : { config }),
  });
}

describe('voice changer surface', () => {
  beforeAll(async () => {
    // dist/worklet.js is its own bundle with its own module singleton.
    await init();
    await workletModule.init();
  });

  describe('construction', () => {
    it('takes one options object with sampleRate required', () => {
      for (const bad of [{}, { maxBlockSize: 128 }]) {
        expect(
          caught(() => new RealtimeVoiceChanger(bad as unknown as RealtimeVoiceChangerOptions))
            .name,
        ).toBe('TypeError');
      }
      expect(
        caught(
          () =>
            new (RealtimeVoiceChanger as unknown as new (c: string) => unknown)('neutralMonitor'),
        ).name,
      ).toBe('TypeError');
      const c = changer();
      try {
        expect(c.latencySamples).toBeGreaterThanOrEqual(0);
      } finally {
        c.destroy();
      }
    });

    it("requires numChannels, the channel count of the caller's stream", () => {
      expect(
        () => new RealtimeVoiceChanger({ sampleRate } as unknown as RealtimeVoiceChangerOptions),
      ).toThrow(new TypeError('RealtimeVoiceChanger: numChannels must be an integer'));
    });

    it.each([
      ['channels', { channels: 1 }],
      ['preset', { preset: 'neutralMonitor' }],
      ['blockSize', { blockSize: 128 }],
    ])('refuses the old option %s as an unknown key', (_name, extra) => {
      const error = caught(
        () =>
          new RealtimeVoiceChanger({
            sampleRate,
            numChannels: 1,
            ...extra,
          } as unknown as RealtimeVoiceChangerOptions),
      );
      expect(error.name).toBe('TypeError');
    });

    it.each([
      ['maxBlockSize 0', { maxBlockSize: 0 }],
      ['maxBlockSize fraction', { maxBlockSize: 12.5 }],
      ['numChannels 3', { numChannels: 3 }],
      ['numChannels 0', { numChannels: 0 }],
      ['sampleRate fraction', { sampleRate: 22050.5 }],
    ])('refuses an out-of-domain option: %s with a RangeError', (_name, extra) => {
      expect(
        caught(
          () =>
            new RealtimeVoiceChanger({
              sampleRate,
              numChannels: 1,
              ...extra,
            } as RealtimeVoiceChangerOptions),
        ).name,
      ).toBe('RangeError');
    });

    it('takes the library default block size when maxBlockSize is omitted', () => {
      const c = new RealtimeVoiceChanger({ sampleRate, numChannels: 1 });
      try {
        expect(c.processMono(new Float32Array(64)).length).toBe(64);
      } finally {
        c.destroy();
      }
    });

    it('reports the library default block size it accepts when maxBlockSize is omitted', () => {
      const c = new RealtimeVoiceChanger({ sampleRate, numChannels: 1 });
      try {
        const max = c.maxBlockSize;
        expect(max).toBeGreaterThan(0);
        expect(c.processMono(new Float32Array(max)).length).toBe(max);
        expect(() => c.processMono(new Float32Array(max + 1))).toThrow(RangeError);
      } finally {
        c.destroy();
      }
    });

    it('reports the given block size as maxBlockSize', () => {
      const c = changer();
      try {
        expect(c.maxBlockSize).toBe(BLOCK);
      } finally {
        c.destroy();
      }
    });

    it('treats null as absent for the optional options', () => {
      const c = new RealtimeVoiceChanger({
        sampleRate,
        maxBlockSize: null,
        numChannels: 1,
        config: null,
      } as unknown as RealtimeVoiceChangerOptions);
      c.destroy();
    });

    it.each([
      'prepare',
      'setPodConfig',
      'bufferGeneration',
      'getCarrierBuffer',
      'getMonoInputBuffer',
      'getMonoOutputBuffer',
      'getInterleavedInputBuffer',
      'getInterleavedOutputBuffer',
      'getPlanarChannelBuffer',
      'processPreparedMono',
      'processPreparedMonoWithCarrier',
      'processPreparedInterleaved',
      'processPreparedInterleavedWithCarrier',
      'processPreparedPlanar',
      'processPreparedPlanarWithCarrier',
      'processInterleaved',
      'processInterleavedWithCarrier',
      'processMonoWithCarrier',
      'processMonoWithCarrierInto',
      'processPlanarWithCarrier',
      'processInterleavedWithCarrierInto',
    ])('no longer has the method %s', (name) => {
      const c = changer();
      try {
        expect(name in c).toBe(false);
      } finally {
        c.destroy();
      }
    });

    it('exposes latencySamples and nonFiniteDiscardCount as getters', () => {
      const c = changer();
      try {
        expect(typeof c.latencySamples).toBe('number');
        expect(c.nonFiniteDiscardCount).toBe(0);
        expectTypeOf(c.latencySamples).toEqualTypeOf<number>();
        expectTypeOf(c.nonFiniteDiscardCount).toEqualTypeOf<number>();
      } finally {
        c.destroy();
      }
    });
  });

  describe('planar and interleaved processing', () => {
    it('processPlanar equals processInterleavedInto and leaves the input untouched', () => {
      const left = tone(BLOCK);
      const right = tone(BLOCK, 330);
      const leftCopy = Float32Array.from(left);
      const a = changer({ numChannels: 2 }, 'brightIdol');
      const b = changer({ numChannels: 2 }, 'brightIdol');
      try {
        const planar = a.processPlanar([left, right]);
        const interleaved = new Float32Array(BLOCK * 2);
        for (let i = 0; i < BLOCK; i++) {
          interleaved[2 * i] = left[i] as number;
          interleaved[2 * i + 1] = right[i] as number;
        }
        const out = new Float32Array(BLOCK * 2);
        b.processInterleavedInto(interleaved, 2, out);
        expect(planar).toHaveLength(2);
        const flat = new Float32Array(BLOCK * 2);
        for (let i = 0; i < BLOCK; i++) {
          flat[2 * i] = (planar[0] as Float32Array)[i] as number;
          flat[2 * i + 1] = (planar[1] as Float32Array)[i] as number;
        }
        expectClose(flat, out);
        expect(left).toEqual(leftCopy);
      } finally {
        a.destroy();
        b.destroy();
      }
    });

    it('processPlanar with one channel equals processMono', () => {
      const input = tone(BLOCK);
      const a = changer();
      const b = changer();
      try {
        const planar = a.processPlanar([input]);
        expect(planar).toHaveLength(1);
        expectClose(planar[0] as Float32Array, b.processMono(input));
      } finally {
        a.destroy();
        b.destroy();
      }
    });

    it('processPlanar with a carrier equals processMono with a carrier for one channel', () => {
      const input = tone(BLOCK);
      const carrier = tone(BLOCK, 110, 0.3);
      const a = changer();
      const b = changer();
      try {
        const [out] = a.processPlanar([input], carrier);
        expectClose(out as Float32Array, b.processMono(input, carrier));
      } finally {
        a.destroy();
        b.destroy();
      }
    });

    it('processPlanar with a carrier returns two planes for stereo', () => {
      const a = changer({ numChannels: 2 });
      try {
        const out = a.processPlanar([tone(BLOCK), tone(BLOCK, 300)], tone(BLOCK, 110));
        expect(out).toHaveLength(2);
        expect((out[1] as Float32Array).length).toBe(BLOCK);
        expectTypeOf(out).toEqualTypeOf<Float32Array[]>();
      } finally {
        a.destroy();
      }
    });

    it.each([
      ['empty list', [], 'RangeError'],
      ['three planes', [tone(8), tone(8), tone(8)], 'RangeError'],
      ['unequal lengths', [tone(8), tone(9)], 'RangeError'],
      ['not an array', tone(8), 'TypeError'],
      ['a non-Float32Array plane', [[0, 1, 2]], 'TypeError'],
    ])('processPlanar refuses %s', (_name, channels, errorName) => {
      const a = changer({ numChannels: 2 });
      try {
        expect(caught(() => a.processPlanar(channels as unknown as Float32Array[])).name).toBe(
          errorName,
        );
      } finally {
        a.destroy();
      }
    });

    it('processInterleavedInto with and without a carrier require numChannels 1 or 2', () => {
      const a = changer({ numChannels: 2 });
      try {
        const buf = new Float32Array(BLOCK * 2);
        expect(caught(() => a.processInterleavedInto(buf, 3 as never, buf)).name).toBe(
          'RangeError',
        );
        expect(caught(() => a.processInterleavedInto(buf, 0 as never, buf, tone(BLOCK))).name).toBe(
          'RangeError',
        );
        expect(caught(() => a.processInterleavedInto(buf, '2' as never, buf)).name).toBe(
          'TypeError',
        );
      } finally {
        a.destroy();
      }
    });
  });

  describe('zero-copy buffers', () => {
    it('mono buffer carries a carrier view and processWithCarrier', () => {
      const a = changer({}, 'vocoderStrings');
      const b = changer({}, 'vocoderStrings');
      try {
        const buf = a.createRealtimeMonoBuffer(BLOCK);
        expect(buf.carrier).toBeInstanceOf(Float32Array);
        expect(buf.carrier.length).toBe(BLOCK);
        expect(typeof buf.processWithCarrier).toBe('function');
        const input = tone(BLOCK);
        const carrier = tone(BLOCK, 110, 0.3);
        buf.input.set(input);
        buf.carrier.set(carrier);
        buf.processWithCarrier();
        expectClose(buf.output, b.processMono(input, carrier));
      } finally {
        a.destroy();
        b.destroy();
      }
    });

    it('interleaved buffer reports numChannels, not channels, and has a carrier', () => {
      const a = changer({ numChannels: 2 });
      try {
        const buf = a.createRealtimeInterleavedBuffer(BLOCK, 2);
        expect(buf.numChannels).toBe(2);
        expect('channels' in buf).toBe(false);
        expect(buf.carrier.length).toBe(BLOCK);
        buf.input.fill(0.1);
        buf.process();
        expect(buf.output.length).toBe(BLOCK * 2);
        buf.processWithCarrier();
        expectTypeOf(buf.numChannels).toEqualTypeOf<1 | 2>();
      } finally {
        a.destroy();
      }
    });

    it('planar buffer processes in place and has a carrier', () => {
      const a = changer({ numChannels: 2 });
      const b = changer({ numChannels: 2 });
      try {
        const buf = a.createRealtimePlanarBuffer(BLOCK, 2);
        expect(buf.channels).toHaveLength(2);
        expect(buf.carrier.length).toBe(BLOCK);
        const left = tone(BLOCK);
        const right = tone(BLOCK, 330);
        buf.channels[0]?.set(left);
        buf.channels[1]?.set(right);
        buf.process();
        const expected = b.processPlanar([left, right]);
        expectClose(buf.channels[0] as Float32Array, expected[0] as Float32Array);
        expectClose(buf.channels[1] as Float32Array, expected[1] as Float32Array);
        buf.processWithCarrier();
      } finally {
        a.destroy();
        b.destroy();
      }
    });

    it.each([
      ['createRealtimeInterleavedBuffer', 3],
      ['createRealtimePlanarBuffer', 0],
    ])('%s refuses a channel count outside 1 or 2', (method, numChannels) => {
      const a = changer({ numChannels: 2 });
      try {
        const call = (a as unknown as Record<string, (n: number, c: number) => unknown>)[
          method
        ] as (n: number, c: number) => unknown;
        expect(caught(() => call.call(a, BLOCK, numChannels)).name).toBe('RangeError');
      } finally {
        a.destroy();
      }
    });
  });

  describe('configuration', () => {
    it('preset config uses the unified field names', () => {
      const config = realtimeVoiceChangerPresetConfig('brightIdol');
      for (const key of [
        'voiceEngine',
        'reverbVariation',
        'eqHighPassHz',
        'retunePitchSemitones',
        'harmonySteps',
        'formantMode',
      ]) {
        expect(config).toHaveProperty(key);
      }
      for (const key of [
        'voiceEngineEngine',
        'reverbVariationVariation',
        'eqHighpassHz',
        'retuneSemitones',
        'retuneGrainSize',
        'harmonyStep1',
        'harmonyStep2',
        'harmonyStep3',
      ]) {
        expect(config).not.toHaveProperty(key);
      }
      expect(config.harmonySteps).toHaveLength(3);
      expectTypeOf(config).toEqualTypeOf<RealtimeVoiceChangerConfig>();
      expectTypeOf(config.harmonySteps).toEqualTypeOf<readonly [number, number, number]>();
    });

    it('feeds a preset config back through the constructor and setConfig', () => {
      const config = realtimeVoiceChangerPresetConfig('choirHarmony');
      const a = changer({}, config);
      const b = changer({}, 'choirHarmony');
      try {
        expect(JSON.parse(a.configJson())).toEqual(JSON.parse(b.configJson()));
        a.setConfig(realtimeVoiceChangerPresetConfig('hardTune'));
        b.setConfig('hardTune');
        expect(JSON.parse(a.configJson())).toEqual(JSON.parse(b.configJson()));
      } finally {
        a.destroy();
        b.destroy();
      }
    });

    it.each([
      ['voiceEngineEngine', 'psola'],
      ['reverbVariationVariation', 'reverb'],
      ['eqHighpassHz', 80],
      ['retuneSemitones', 0],
      ['retuneGrainSize', 0],
      ['harmonyStep1', 0],
    ])('refuses the retired config key %s', (key, value) => {
      const config = { ...realtimeVoiceChangerPresetConfig('neutralMonitor'), [key]: value };
      const a = changer();
      try {
        expect(caught(() => a.setConfig(config as RealtimeVoiceChangerConfig)).name).toBe(
          'RangeError',
        );
      } finally {
        a.destroy();
      }
    });

    it.each([
      ['characterType', 'bbd_chorus'],
      ['reverbVariation', 'tempo_delay'],
      ['reverbVariation', 'dub_echo'],
      ['reverbVariationDivision', 'dotted_eighth'],
      ['pitchCorrectionScale', 'natural_minor'],
      ['characterType', 7],
    ])('refuses the old token spelling %s=%s', (key, value) => {
      const config = { ...realtimeVoiceChangerPresetConfig('neutralMonitor'), [key]: value };
      const a = changer();
      try {
        const error = caught(() => a.setConfig(config as unknown as RealtimeVoiceChangerConfig));
        expect(error.name).toBe(typeof value === 'number' ? 'TypeError' : 'RangeError');
      } finally {
        a.destroy();
      }
    });

    it('accepts the lowerCamel tokens', () => {
      const config = {
        ...realtimeVoiceChangerPresetConfig('neutralMonitor'),
        characterType: 'bbdChorus',
        reverbVariation: 'tempoDelay',
        reverbVariationDivision: 'dottedEighth',
      } as RealtimeVoiceChangerConfig;
      const a = changer({}, config);
      try {
        a.setConfig({ ...config, reverbVariation: 'dubEcho' });
      } finally {
        a.destroy();
      }
    });

    it.each([
      ['not an array', 5, 'TypeError'],
      ['two entries', [0, 0], 'RangeError'],
    ])('harmonySteps %s is refused', (_name, harmonySteps, errorName) => {
      const config = { ...realtimeVoiceChangerPresetConfig('neutralMonitor'), harmonySteps };
      const a = changer();
      try {
        expect(
          caught(() => a.setConfig(config as unknown as RealtimeVoiceChangerConfig)).name,
        ).toBe(errorName);
      } finally {
        a.destroy();
      }
    });

    it.each([
      ['an old kebab id', 'neutral-monitor', 'RangeError'],
      ['an unknown id', 'nope', 'RangeError'],
      ['a number', 3, 'TypeError'],
    ])('setConfig and the constructor refuse %s', (_name, config, errorName) => {
      const a = changer();
      try {
        expect(caught(() => a.setConfig(config as unknown as VoicePresetId)).name).toBe(errorName);
        expect(caught(() => changer({}, config as unknown as VoicePresetId)).name).toBe(errorName);
      } finally {
        a.destroy();
      }
    });
  });

  describe('presets', () => {
    const publicIds = [
      'neutralMonitor',
      'brightIdol',
      'softWhisper',
      'deepNarrator',
      'robotMascot',
      'darkVillain',
      'hardTune',
      'robotMonotone',
      'choirHarmony',
      'vocoderStrings',
      'megaphone',
      'radioVoice',
    ];

    it('names the presets in lowerCamel, in ordinal order', () => {
      expect(realtimeVoiceChangerPresetNames()).toEqual(publicIds);
      publicIds.forEach((id, ordinal) => {
        expect(voiceCharacterPresetId(ordinal)).toBe(id);
      });
    });

    it('builds a preset JSON and config for each public id', () => {
      for (const id of publicIds as VoicePresetId[]) {
        const json = realtimeVoiceChangerPresetJson(id);
        expect(realtimeVoiceChangerValidatePresetJson(json).ok).toBe(true);
        expect(realtimeVoiceChangerPresetConfig(id).formantMode).toMatch(/relative|absolute/);
      }
    });

    it('renames the validator to its C name and declares its result', () => {
      expect((indexModule as Record<string, unknown>).validateRealtimeVoiceChangerPresetJson).toBe(
        undefined,
      );
      const result = realtimeVoiceChangerValidatePresetJson('{"not":"a preset"}');
      expect(result.ok).toBe(false);
      expect(typeof result.error).toBe('string');
      expectTypeOf(result).toEqualTypeOf<RealtimeVoiceChangerValidatePresetJsonResult>();
    });

    it.each([
      ['an old kebab id', 'bright-idol', 'RangeError'],
      ['an unknown id', 'nope', 'RangeError'],
      ['an ordinal', 0, 'TypeError'],
    ])('preset config and preset JSON refuse %s', (_name, preset, errorName) => {
      expect(
        caught(() => realtimeVoiceChangerPresetConfig(preset as unknown as VoicePresetId)).name,
      ).toBe(errorName);
      expect(
        caught(() => realtimeVoiceChangerPresetJson(preset as unknown as VoicePresetId)).name,
      ).toBe(errorName);
    });

    it.each([
      ['an unknown ordinal', 99, 'RangeError'],
      ['a negative ordinal', -1, 'RangeError'],
      ['a fractional ordinal', 1.5, 'RangeError'],
      ['a preset id string', 'brightIdol', 'TypeError'],
    ])('voiceCharacterPresetId refuses %s', (_name, ordinal, errorName) => {
      expect(caught(() => voiceCharacterPresetId(ordinal as number)).name).toBe(errorName);
    });
  });

  describe('one-shots', () => {
    it('voiceChange requires sampleRate in both forms and agrees across them', () => {
      const input = tone(sampleRate / 4);
      expect(caught(() => voiceChange({ samples: input } as never)).name).toBe('TypeError');
      expect(
        caught(() => (voiceChange as unknown as (s: Float32Array) => unknown)(input)).name,
      ).toBe('TypeError');
      const positional = voiceChange(input, sampleRate, { pitchSemitones: 2 });
      const request = voiceChange({ samples: input, sampleRate, pitchSemitones: 2 });
      expectClose(positional, request);
      expect(
        caught(() =>
          (voiceChange as unknown as (...a: unknown[]) => unknown)(
            { samples: input, sampleRate },
            1,
          ),
        ).name,
      ).toBe('TypeError');
    });

    it('realtimeVoiceChangerRender is request-only and planar', () => {
      const left = tone(sampleRate / 4);
      const right = tone(sampleRate / 4, 330);
      const stereo = realtimeVoiceChangerRender({ channels: [left, right], sampleRate });
      expect(stereo).toHaveLength(2);
      expect((stereo[0] as Float32Array).length).toBe(left.length);
      expect((stereo[1] as Float32Array).length).toBe(right.length);
      const mono = realtimeVoiceChangerRender({
        channels: [left],
        sampleRate,
        config: 'neutralMonitor',
      });
      expect(mono).toHaveLength(1);
      expectClose(
        realtimeVoiceChangerRender({ channels: [left], sampleRate })[0] as Float32Array,
        mono[0] as Float32Array,
      );
      expectTypeOf(stereo).toEqualTypeOf<Float32Array[]>();
    });

    it('realtimeVoiceChangerRender accepts a flat config and a preset id alike', () => {
      const input = tone(sampleRate / 4);
      const byId = realtimeVoiceChangerRender({
        channels: [input],
        sampleRate,
        config: 'hardTune',
      });
      const byConfig = realtimeVoiceChangerRender({
        channels: [input],
        sampleRate,
        config: realtimeVoiceChangerPresetConfig('hardTune'),
      });
      expectClose(byConfig[0] as Float32Array, byId[0] as Float32Array);
    });

    it.each([
      ['no sampleRate', { channels: [tone(64)] }, 'TypeError'],
      ['a positional call', tone(64), 'TypeError'],
      ['the old samples key', { samples: tone(64), sampleRate }, 'TypeError'],
      [
        'the old preset key',
        { channels: [tone(64)], sampleRate, preset: 'neutralMonitor' },
        'TypeError',
      ],
      [
        'the removed blockSize key',
        { channels: [tone(64)], sampleRate, blockSize: 128 },
        'TypeError',
      ],
      ['the removed channels count', { channels: 2, sampleRate }, 'TypeError'],
      ['three channels', { channels: [tone(64), tone(64), tone(64)], sampleRate }, 'RangeError'],
      ['unequal channels', { channels: [tone(64), tone(65)], sampleRate }, 'RangeError'],
      ['an empty channel list', { channels: [], sampleRate }, 'RangeError'],
      ['a fractional sampleRate', { channels: [tone(64)], sampleRate: 22050.5 }, 'RangeError'],
      [
        'an old kebab preset',
        { channels: [tone(64)], sampleRate, config: 'bright-idol' },
        'RangeError',
      ],
    ])('realtimeVoiceChangerRender refuses %s', (_name, request, errorName) => {
      expect(caught(() => realtimeVoiceChangerRender(request as never)).name).toBe(errorName);
    });
  });

  describe('streaming retune', () => {
    it('names its controls pitchSemitones and grainSizeSamples and reports them back', () => {
      const retune = new StreamingRetune({ pitchSemitones: 3, mix: 0.5, grainSizeSamples: 1024 });
      try {
        retune.prepare(sampleRate, BLOCK);
        const config = retune.config();
        expect(config.pitchSemitones).toBe(3);
        expect(config.mix).toBe(0.5);
        expect(config.grainSizeSamples).toBe(1024);
        expect(retune.grainSizeSamples).toBe(1024);
        expect(retune.latencySamples).toBeGreaterThan(0);
        expect(config).not.toHaveProperty('semitones');
        expect(config).not.toHaveProperty('grainSize');
        retune.setConfig({ pitchSemitones: -2 });
        expect(retune.config().pitchSemitones).toBe(-2);
        expectTypeOf(retune.grainSizeSamples).toEqualTypeOf<number>();
        expectTypeOf(retune.config()).toEqualTypeOf<StreamingRetuneAppliedConfig>();
      } finally {
        retune.destroy();
      }
    });

    it('derives the grain when grainSizeSamples is omitted', () => {
      const retune = new StreamingRetune({});
      try {
        retune.prepare(sampleRate, BLOCK);
        expect(retune.grainSizeSamples).toBeGreaterThan(0);
      } finally {
        retune.destroy();
      }
    });

    it.each([
      ['semitones', { semitones: 1 }, 'TypeError'],
      ['grainSize', { grainSize: 512 }, 'TypeError'],
      ['an explicit 0 grainSizeSamples', { grainSizeSamples: 0 }, 'RangeError'],
      ['a string grainSizeSamples', { grainSizeSamples: '512' }, 'TypeError'],
    ])('refuses %s', (_name, config, errorName) => {
      expect(caught(() => new StreamingRetune(config as StreamingRetuneConfig)).name).toBe(
        errorName,
      );
      const retune = new StreamingRetune({});
      try {
        expect(caught(() => retune.setConfig(config as StreamingRetuneConfig)).name).toBe(
          errorName,
        );
      } finally {
        retune.destroy();
      }
    });
  });

  describe('worklet processor options', () => {
    it('reads maxBlockSize, numChannels and config', () => {
      const processor = new workletModule.SonareRealtimeVoiceChangerWorkletProcessor({
        config: 'brightIdol',
        sampleRate,
        maxBlockSize: BLOCK,
        numChannels: 2,
      });
      try {
        const left = tone(BLOCK);
        const right = tone(BLOCK, 330);
        const outL = new Float32Array(BLOCK);
        const outR = new Float32Array(BLOCK);
        for (let i = 0; i < 8; i++) {
          expect(processor.process([[left, right]], [[outL, outR]])).toBe(true);
        }
        expect(outL.every(Number.isFinite)).toBe(true);
        expect(outL.some((v) => v !== 0)).toBe(true);
        processor.receiveMessage({
          type: 'setConfig',
          config: realtimeVoiceChangerPresetConfig('hardTune'),
        });
        expect(processor.process([[left, right]], [[outL, outR]])).toBe(true);
      } finally {
        processor.destroy();
      }
    });

    it.each([
      ['numChannels 0', { numChannels: 0 }],
      ['numChannels 1.5', { numChannels: 1.5 }],
      ['numChannels 3', { numChannels: 3 }],
      ['maxBlockSize 0', { maxBlockSize: 0 }],
    ])('refuses %s with a RangeError', (_name, extra) => {
      expect(
        caught(
          () =>
            new workletModule.SonareRealtimeVoiceChangerWorkletProcessor({
              sampleRate,
              numChannels: 1,
              ...extra,
            } as never),
        ).name,
      ).toBe('RangeError');
    });

    it('renames the processor options and drops the old spellings', () => {
      const options: workletModule.SonareRealtimeVoiceChangerWorkletProcessorOptions = {
        config: 'neutralMonitor',
        maxBlockSize: 128,
        numChannels: 1,
      };
      expect(options.numChannels).toBe(1);
      // @ts-expect-error the message types are no longer part of the public worklet entry
      const gone: workletModule.SonareRealtimeVoiceChangerMessage | undefined = undefined;
      expect(gone).toBeUndefined();
      const old: workletModule.SonareRealtimeVoiceChangerWorkletProcessorOptions = {
        // @ts-expect-error the old option names are gone
        preset: 'neutralMonitor',
      };
      expect(old).toBeDefined();
    });
  });

  describe('types', () => {
    it('renames the enum types and lowers the tokens', () => {
      expectTypeOf<VoiceEngine>().toEqualTypeOf<'grain' | 'psola'>();
      expectTypeOf<PitchCorrectionMode>().toEqualTypeOf<'off' | 'auto' | 'robot'>();
      expectTypeOf<ScaleKind>().toEqualTypeOf<'major' | 'naturalMinor' | 'chromatic'>();
      expectTypeOf<VocoderCarrierSource>().toEqualTypeOf<'internal' | 'external'>();
      expectTypeOf<VocoderCarrierWave>().toEqualTypeOf<'saw' | 'pulse' | 'ensemble'>();
      expectTypeOf<CharacterType>().toEqualTypeOf<
        'none' | 'megaphone' | 'radio' | 'bbdChorus' | 'strobo'
      >();
      expectTypeOf<ReverbVariation>().toEqualTypeOf<'reverb' | 'echo' | 'tempoDelay' | 'dubEcho'>();
      expectTypeOf<NoteDivision>().toEqualTypeOf<
        'quarter' | 'eighth' | 'dottedEighth' | 'sixteenth'
      >();
      expectTypeOf<MidiRelease>().toEqualTypeOf<'hold' | 'input'>();
      expectTypeOf<VoicePresetId>().toEqualTypeOf<
        | 'neutralMonitor'
        | 'brightIdol'
        | 'softWhisper'
        | 'deepNarrator'
        | 'robotMascot'
        | 'darkVillain'
        | 'hardTune'
        | 'robotMonotone'
        | 'choirHarmony'
        | 'vocoderStrings'
        | 'megaphone'
        | 'radioVoice'
      >();
      expectTypeOf<RealtimeVoiceChangerConfig['formantMode']>().toEqualTypeOf<
        'relative' | 'absolute'
      >();
      expectTypeOf<RealtimeVoiceChangerConfig['voiceEngine']>().toEqualTypeOf<VoiceEngine>();
      expectTypeOf<
        RealtimeVoiceChangerConfig['reverbVariation']
      >().toEqualTypeOf<ReverbVariation>();
      expectTypeOf<RealtimeVoiceChangerConfig['eqHighPassHz']>().toEqualTypeOf<number>();
      // @ts-expect-error voiceEngineEngine is gone
      expectTypeOf<RealtimeVoiceChangerConfig['voiceEngineEngine']>().toBeNever();
      // @ts-expect-error harmonyStep1 is gone
      expectTypeOf<RealtimeVoiceChangerConfig['harmonyStep1']>().toBeNever();
    });

    it('declares the constructor options', () => {
      expectTypeOf<RealtimeVoiceChangerOptions>().toEqualTypeOf<{
        sampleRate: number;
        maxBlockSize?: number;
        numChannels: 1 | 2;
        config?: RealtimeVoiceChangerConfigInput;
      }>();
    });

    it('drops the old type and function names from the package root', () => {
      for (const name of ['validateRealtimeVoiceChangerPresetJson']) {
        expect(name in indexModule).toBe(false);
      }
    });
  });
});
