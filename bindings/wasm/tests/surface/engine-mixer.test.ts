import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePreset } from '../../dist/index.js';

const SR = 48000;
const BLOCK = 256;

/** Run `fn`, return what it threw. */
function thrown(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

/** Runs a few blocks of a tone through every strip so the meters have something to read. */
function feedTone(mixer: Mixer): void {
  for (let block = 0; block < 4; block++) {
    const tone = new Float32Array(BLOCK);
    for (let i = 0; i < BLOCK; i++) {
      tone[i] = 0.3 * Math.sin((2 * Math.PI * 440 * (block * BLOCK + i)) / SR);
    }
    const lanes = Array.from({ length: mixer.stripCount }, () => tone);
    mixer.processStereo(lanes, lanes);
  }
}

function withPreset(fn: (mixer: Mixer) => void): void {
  const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
    sampleRate: SR,
    maxBlockSize: BLOCK,
  });
  try {
    fn(mixer);
  } finally {
    mixer.destroy();
  }
}

beforeAll(async () => {
  await init();
});

describe('mixer construction', () => {
  it('builds an empty mixer from new Mixer({ sampleRate })', () => {
    const mixer = new Mixer({ sampleRate: SR });
    try {
      expect(mixer.stripCount).toBe(0);
    } finally {
      mixer.destroy();
    }
  });

  it('builds from a scene document through new Mixer({ scene, sampleRate, maxBlockSize })', () => {
    const scene = mixingScenePreset('vocalReverbSend');
    const mixer = new Mixer({ scene, sampleRate: SR, maxBlockSize: BLOCK });
    try {
      expect(mixer.stripCount).toBeGreaterThan(0);
    } finally {
      mixer.destroy();
    }
  });

  it('constructor refuses a missing sample rate, an unknown key and a zero block size', () => {
    expect(thrown(() => new Mixer({} as never))).toBeInstanceOf(TypeError);
    const unknown = thrown(() => new Mixer({ sampleRate: SR, scenes: [] } as never));
    expect(unknown).toBeInstanceOf(TypeError);
    expect((unknown as Error).message).toContain('scenes');
    expect(thrown(() => new Mixer({ sampleRate: 100 }))).toBeInstanceOf(RangeError);
    const zero = thrown(() => new Mixer({ sampleRate: SR, maxBlockSize: 0 }));
    expect(zero).toBeInstanceOf(RangeError);
    expect((zero as Error).message).toContain('maxBlockSize must be an integer in [1, 1048576]');
    expect(thrown(() => new Mixer(null as never))).toBeInstanceOf(TypeError);
  });

  it('fromJson builds from a scene JSON string and defaults the block size', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: SR,
    });
    try {
      expect(mixer.stripCount).toBeGreaterThan(0);
    } finally {
      mixer.destroy();
    }
  });

  it('fromJson refuses a missing sample rate and an unknown option', () => {
    const json = JSON.stringify(mixingScenePreset('vocalReverbSend'));
    expect(thrown(() => Mixer.fromJson(json, {} as never))).toBeInstanceOf(TypeError);
    expect(
      thrown(() => Mixer.fromJson(json, { sampleRate: SR, block: 1 } as never)),
    ).toBeInstanceOf(TypeError);
    expect(thrown(() => Mixer.fromJson(null as never, { sampleRate: SR }))).toBeInstanceOf(
      TypeError,
    );
  });

  it('replaces fromSceneJson, toSceneJson and meterTap', () => {
    const proto = Mixer.prototype as unknown as Record<string, unknown>;
    expect((Mixer as unknown as Record<string, unknown>).fromSceneJson).toBeUndefined();
    expect(proto.toSceneJson).toBeUndefined();
    expect(proto.meterTap).toBeUndefined();
    expect(typeof proto.toJson).toBe('function');
  });

  it('toJson round-trips through fromJson', () => {
    withPreset((mixer) => {
      const again = Mixer.fromJson(mixer.toJson(), { sampleRate: SR, maxBlockSize: BLOCK });
      try {
        expect(again.stripCount).toBe(mixer.stripCount);
        expect(again.toJson()).toBe(mixer.toJson());
      } finally {
        again.destroy();
      }
    });
  });
});

describe('mixer getters', () => {
  it('reads counts, tail and latency as getters', () => {
    withPreset((mixer) => {
      for (const name of [
        'stripCount',
        'busCount',
        'vcaGroupCount',
        'tailSamples',
        'latencySamples',
      ]) {
        expect(typeof (mixer as unknown as Record<string, unknown>)[name], name).toBe('number');
      }
      expect(mixer.stripCount).toBeGreaterThan(0);
    });
  });
});

describe('mixer strips', () => {
  it('stripById returns the index and throws RangeError for an unknown id', () => {
    withPreset((mixer) => {
      expect(typeof mixer.stripById('vocal')).toBe('number');
      expect(thrown(() => mixer.stripById('definitely-not-a-strip'))).toBeInstanceOf(RangeError);
    });
  });

  it('addStrip returns the new strip index', () => {
    withPreset((mixer) => {
      const before = mixer.stripCount;
      const index = mixer.addStrip('late-arrival', { enabled: false });
      expect(index).toBe(mixer.stripById('late-arrival'));
      expect(mixer.stripCount).toBe(before + 1);
    });
  });

  it('addresses a strip by index or by id', () => {
    withPreset((mixer) => {
      const index = mixer.stripById('vocal');
      expect(() => mixer.setFaderDb(index, -3)).not.toThrow();
      expect(() => mixer.setFaderDb('vocal', -4)).not.toThrow();
      expect(thrown(() => mixer.setFaderDb('ghost', -4))).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.setFaderDb(null as never, -4))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.settle(mixer.stripCount))).toBeDefined();
    });
  });

  it('resolves a digit-only string as a strip id, not an index', () => {
    withPreset((mixer) => {
      mixer.addStrip('7');
      expect(mixer.stripById('7')).toBe(mixer.stripCount - 1);
      expect(() => mixer.setFaderDb('7', -1)).not.toThrow();
    });
  });

  it('keeps one boolean after the strip on setMuted, setSoloed and setSoloSafe', () => {
    withPreset((mixer) => {
      expect(() => mixer.setMuted('vocal', true)).not.toThrow();
      expect(() => mixer.setSoloed('vocal', false)).not.toThrow();
      expect(() => mixer.setSoloSafe('vocal', true)).not.toThrow();
      expect(thrown(() => mixer.setMuted('vocal', 1 as never))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.setSoloed('vocal', 'yes' as never))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.setSoloSafe('vocal', null as never))).toBeInstanceOf(TypeError);
    });
  });

  it('setPolarityInvert takes a left/right object', () => {
    withPreset((mixer) => {
      expect(() => mixer.setPolarityInvert('vocal', { left: true, right: false })).not.toThrow();
      expect(
        thrown(() => mixer.setPolarityInvert('vocal', { left: true } as never)),
      ).toBeInstanceOf(TypeError);
      expect(
        thrown(() =>
          mixer.setPolarityInvert('vocal', { left: true, right: true, both: 1 } as never),
        ),
      ).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.setPolarityInvert('vocal', true as never))).toBeInstanceOf(
        TypeError,
      );
    });
  });

  it('setPanLaw refuses a number and an unknown name', () => {
    withPreset((mixer) => {
      expect(() => mixer.setPanLaw('vocal', 'const6Db')).not.toThrow();
      expect(thrown(() => mixer.setPanLaw('vocal', 2 as never))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.setPanLaw('vocal', 'bogus' as never))).toBeInstanceOf(RangeError);
    });
  });

  it('stripMeter takes a tap token and refuses a number', () => {
    withPreset((mixer) => {
      const beforeAudio = mixer.stripMeter('vocal');
      expect(beforeAudio.numChannels).toBe(0);
      expect(beforeAudio.peakDb).toEqual([]);
      expect(beforeAudio.rmsDb).toEqual([]);
      expect(beforeAudio.truePeakDbtp).toEqual([]);
      feedTone(mixer);
      expect(Number.isFinite(mixer.stripMeter('vocal').peakDb[0])).toBe(true);
      expect(Number.isFinite(mixer.stripMeter('vocal', 'preFader').peakDb[0])).toBe(true);
      expect(Number.isFinite(mixer.stripMeter('vocal', 'postFader').peakDb[0])).toBe(true);
      expect(thrown(() => mixer.stripMeter('vocal', 1 as never))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.stripMeter('vocal', 'middle' as never))).toBeInstanceOf(RangeError);
    });
  });
});

describe('mixer buses and sends', () => {
  it('addBus takes a role token and refuses an unknown role', () => {
    withPreset((mixer) => {
      expect(() => mixer.addBus('sub-a', 'submix')).not.toThrow();
      expect(() => mixer.addBus('sub-b')).not.toThrow();
      expect(thrown(() => mixer.addBus('sub-c', 'group' as never))).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.addBus('sub-d', 1 as never))).toBeInstanceOf(TypeError);
    });
  });

  it('addSend takes a send object and refuses a numeric timing', () => {
    withPreset((mixer) => {
      const index = mixer.addSend('vocal', {
        id: 'extra-send',
        destinationBusId: 'vocal-verb',
        sendDb: -18,
        timing: 'preFader',
      });
      expect(typeof index).toBe('number');
      expect(
        typeof mixer.addSend('vocal', { id: 'bare-send', destinationBusId: 'vocal-verb' }),
      ).toBe('number');
      const timing = thrown(() =>
        mixer.addSend('vocal', { id: 'n', destinationBusId: 'vocal-verb', timing: 1 as never }),
      );
      expect(timing).toBeInstanceOf(TypeError);
      expect(
        thrown(() =>
          mixer.addSend('vocal', {
            id: 'n',
            destinationBusId: 'vocal-verb',
            timing: 'later' as never,
          }),
        ),
      ).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.addSend('vocal', { id: 'n' } as never))).toBeInstanceOf(TypeError);
      expect(
        thrown(() =>
          mixer.addSend('vocal', { id: 'n', destinationBusId: 'vocal-verb', db: -3 } as never),
        ),
      ).toBeInstanceOf(TypeError);
    });
  });
});

describe('mixer automation', () => {
  it('scheduleFaderAutomation takes a point object', () => {
    withPreset((mixer) => {
      expect(() =>
        mixer.scheduleFaderAutomation('vocal', { samplePos: 0, faderDb: -6, curve: 'linear' }),
      ).not.toThrow();
      expect(() =>
        mixer.scheduleFaderAutomation('vocal', { samplePos: 100, faderDb: -3 }),
      ).not.toThrow();
      expect(
        thrown(() => mixer.scheduleFaderAutomation('vocal', { samplePos: 0 } as never)),
      ).toBeInstanceOf(TypeError);
      expect(
        thrown(() => mixer.scheduleFaderAutomation('vocal', { samplePos: 0, faderDb: Number.NaN })),
      ).toBeInstanceOf(RangeError);
      expect(
        thrown(() =>
          mixer.scheduleFaderAutomation('vocal', { samplePos: 0, faderDb: 0, curve: 1 as never }),
        ),
      ).toBeInstanceOf(TypeError);
      expect(
        thrown(() =>
          mixer.scheduleFaderAutomation('vocal', {
            samplePos: 0,
            faderDb: 0,
            curve: 'wavy' as never,
          }),
        ),
      ).toBeInstanceOf(RangeError);
    });
  });

  it('schedulePanAutomation takes a point object', () => {
    withPreset((mixer) => {
      expect(() =>
        mixer.schedulePanAutomation('vocal', { samplePos: 0, pan: 0.1, curve: 'exponential' }),
      ).not.toThrow();
      expect(
        thrown(() => mixer.schedulePanAutomation('vocal', { samplePos: 0, pan: -1.5 })),
      ).toBeInstanceOf(RangeError);
      expect(
        thrown(() => mixer.schedulePanAutomation('vocal', { samplePos: 0, faderDb: 0 } as never)),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('scheduleWidthAutomation takes a point object', () => {
    withPreset((mixer) => {
      expect(() =>
        mixer.scheduleWidthAutomation('vocal', { samplePos: 0, width: 1.2 }),
      ).not.toThrow();
      expect(
        thrown(() => mixer.scheduleWidthAutomation('vocal', { samplePos: 0, width: 2.5 })),
      ).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.scheduleWidthAutomation('vocal', 0 as never))).toBeInstanceOf(
        TypeError,
      );
    });
  });

  it('scheduleSendAutomation takes sendDb in a point object', () => {
    withPreset((mixer) => {
      const index = mixer.addSend('vocal', { id: 'auto-send', destinationBusId: 'vocal-verb' });
      expect(() =>
        mixer.scheduleSendAutomation('vocal', index, { samplePos: 0, sendDb: -18 }),
      ).not.toThrow();
      expect(
        thrown(() =>
          mixer.scheduleSendAutomation('vocal', index, { samplePos: 0, db: -18 } as never),
        ),
      ).toBeInstanceOf(TypeError);
    });
  });

  it('scheduleInsertAutomation takes a point object', () => {
    withPreset((mixer) => {
      mixer.compile();
      expect(() =>
        mixer.scheduleInsertAutomation(0, 0, { paramId: 0, samplePos: 0, value: 0 }),
      ).not.toThrow();
      expect(() =>
        mixer.scheduleInsertAutomation('vocal', 0, {
          paramId: 0,
          samplePos: 48000,
          value: 1,
          curve: 'exponential',
        }),
      ).not.toThrow();
      expect(
        thrown(() =>
          mixer.scheduleInsertAutomation(999, 0, { paramId: 0, samplePos: 0, value: 0 }),
        ),
      ).toBeDefined();
      expect(
        thrown(() =>
          mixer.scheduleInsertAutomation('vocal', 0, { samplePos: 0, value: 0 } as never),
        ),
      ).toBeInstanceOf(TypeError);
    });
  });
});

describe('mixer master meter', () => {
  it('configureMeter takes an options object', () => {
    withPreset((mixer) => {
      expect(() => mixer.configureMeter({ enabled: true })).not.toThrow();
      expect(() => mixer.configureMeter({ enabled: true, truePeakOversample: 8 })).not.toThrow();
      expect(
        thrown(() => mixer.configureMeter({ enabled: true, truePeakOversample: 3 })),
      ).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.configureMeter(true as never))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.configureMeter({} as never))).toBeInstanceOf(TypeError);
      expect(thrown(() => mixer.configureMeter({ enabled: 1 } as never))).toBeInstanceOf(TypeError);
    });
  });

  it('meterScratchValue reads a silent level as -Infinity and an unmeasured correlation as undefined', () => {
    withPreset((mixer) => {
      mixer.configureMeter({ enabled: true });
      const silence = Array.from({ length: mixer.stripCount }, () => new Float32Array(BLOCK));
      for (let block = 0; block < 4; block++) {
        mixer.processStereo(silence, silence);
      }
      expect(mixer.latchMeterSnapshot()).toBe(true);
      for (const field of [0, 1, 2, 3, 5, 6] as const) {
        expect(mixer.meterScratchValue(field), String(field)).toBe(Number.NEGATIVE_INFINITY);
      }
      expect(mixer.meterScratchValue(4)).toBeUndefined();
    });
  });

  it('meterScratchValue refuses a field outside the map', () => {
    withPreset((mixer) => {
      mixer.configureMeter({ enabled: true });
      feedTone(mixer);
      expect(mixer.latchMeterSnapshot()).toBe(true);
      for (let field = 0; field <= 6; field++) {
        expect(Number.isFinite(mixer.meterScratchValue(field as never))).toBe(true);
      }
      expect(thrown(() => mixer.meterScratchValue(7 as never))).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.meterScratchValue(-1 as never))).toBeInstanceOf(RangeError);
      expect(thrown(() => mixer.meterScratchValue('peakDbL' as never))).toBeInstanceOf(TypeError);
    });
  });

  it('processStereoInto keeps its positional arguments', () => {
    withPreset((mixer) => {
      const left = new Float32Array(BLOCK);
      const right = new Float32Array(BLOCK);
      expect(() =>
        mixer.processStereoInto([left], [right], left.slice(), right.slice()),
      ).not.toThrow();
    });
  });
});
