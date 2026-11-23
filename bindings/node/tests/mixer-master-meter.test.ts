import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { ErrorCode } from '../src/errors.js';
import { Mixer, type MixMeterSnapshot } from '../src/index.js';
import { expectRefusal, present } from './_helpers.js';

interface Reading {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  truePeakDbL: number;
  truePeakDbR: number;
}

interface MeterCase {
  name: string;
  scene: string;
  sampleRate: number;
  blockSize: number;
  blocks: number;
  frequencyHz: number;
  amplitude: number;
  phaseRad: number;
  rightGain: number;
  truePeakOversample: number;
  expected: Reading;
  expectedAfterFlush: Reading;
}

const fixture = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/master_meter_reading.json', import.meta.url),
    'utf8',
  ),
) as { toleranceDb: number; toleranceCorrelation: number; cases: MeterCase[] };

const SCENE = fixture.cases[0].scene;

function expectReading(actual: MixMeterSnapshot, expected: Reading): void {
  const dB = {
    peakDbL: actual.peakDb[0],
    peakDbR: actual.peakDb[1],
    rmsDbL: actual.rmsDb[0],
    rmsDbR: actual.rmsDb[1],
    truePeakDbL: actual.truePeakDbtp[0],
    truePeakDbR: actual.truePeakDbtp[1],
  };
  expect(actual.numChannels).toBe(2);
  expect(actual.peakDb).toHaveLength(actual.numChannels);
  expect(actual.rmsDb).toHaveLength(actual.numChannels);
  expect(actual.truePeakDbtp).toHaveLength(actual.numChannels);
  for (const [field, value] of Object.entries(dB)) {
    expect(
      Math.abs(present(value, field) - expected[field as keyof Reading]),
      `${field}: ${value} vs ${expected[field as keyof Reading]}`,
    ).toBeLessThanOrEqual(fixture.toleranceDb);
  }
  expect(
    Math.abs(present(actual.correlation, 'correlation') - expected.correlation),
  ).toBeLessThanOrEqual(fixture.toleranceCorrelation);
}

describe('Mixer master meter', () => {
  it.each(fixture.cases.map((c) => [c.name, c] as const))(
    'reproduces the shared reading: %s',
    (_name, c) => {
      const mixer = Mixer.fromJson(c.scene, {
        sampleRate: c.sampleRate,
        maxBlockSize: c.blockSize,
      });
      try {
        mixer.configureMeter({ enabled: true, truePeakOversample: c.truePeakOversample });
        for (let block = 0; block < c.blocks; block++) {
          const left = new Float32Array(c.blockSize);
          const right = new Float32Array(c.blockSize);
          for (let i = 0; i < c.blockSize; i++) {
            const n = block * c.blockSize + i;
            const v =
              c.amplitude * Math.sin((2 * Math.PI * c.frequencyHz * n) / c.sampleRate + c.phaseRad);
            left[i] = v;
            right[i] = v * c.rightGain;
          }
          mixer.processStereo([left], [right]);
        }
        expectReading(mixer.meterSnapshot(), c.expected);
        mixer.flushMeters();
        expectReading(mixer.meterSnapshot(), c.expectedAfterFlush);
      } finally {
        mixer.destroy();
      }
    },
  );

  it('reads as InvalidState before the meter is first enabled', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      expectRefusal(() => mixer.meterSnapshot(), ErrorCode.InvalidState);
    } finally {
      mixer.destroy();
    }
  });

  it('defaults truePeakOversample to 4 and refuses any factor but 4 and 8', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      expect(() => mixer.configureMeter({ enabled: true })).not.toThrow();
      expect(() => mixer.configureMeter({ enabled: true, truePeakOversample: 8 })).not.toThrow();
      for (const factor of [0, 1, 2, 3, 5, 16]) {
        expectRefusal(
          () => mixer.configureMeter({ enabled: true, truePeakOversample: factor }),
          RangeError,
          /truePeakOversample/,
        );
      }
    } finally {
      mixer.destroy();
    }
  });

  it('measures loudness and true peak by default, and skips loudness when measureLufs is false', () => {
    const read = (options: Parameters<Mixer['configureMeter']>[0]) => {
      const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 480 });
      try {
        mixer.configureMeter(options);
        for (let block = 0; block < 100; block++) {
          const tone = new Float32Array(480);
          for (let i = 0; i < 480; i++) {
            tone[i] = 0.5 * Math.sin((2 * Math.PI * 1000 * (block * 480 + i)) / 48000);
          }
          mixer.processStereo([tone], [tone]);
        }
        return mixer.meterSnapshot();
      } finally {
        mixer.destroy();
      }
    };
    const measured = read({ enabled: true });
    expect(measured.momentaryLufs).toBeGreaterThan(-20);
    expect(measured.maxTruePeakDbtp).toBeGreaterThan(-10);
    const unmeasured = read({ enabled: true, measureLufs: false, measureTruePeak: false });
    expect('momentaryLufs' in unmeasured).toBe(false);
    expect('maxTruePeakDbtp' in unmeasured).toBe(false);
    expect(unmeasured.peakDb[0]).toBeCloseTo(present(measured.peakDb[0], 'peakDb[0]'), 3);
  });

  it('keeps unmeasured channel planes undefined in arrays of the reported width', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      mixer.configureMeter({ enabled: true, measureTruePeak: false });
      mixer.processStereo([new Float32Array(32).fill(0.5)], [new Float32Array(32)]);

      const meter = mixer.meterSnapshot();
      expect(meter.numChannels).toBe(2);
      expect(meter.peakDb).toHaveLength(2);
      expect(meter.rmsDb).toHaveLength(2);
      expect(meter.truePeakDbtp).toHaveLength(2);
      expect(meter.peakDb[0]).toBeGreaterThan(-10);
      expect(meter.peakDb[1]).toBe(Number.NEGATIVE_INFINITY);
      expect(meter.truePeakDbtp).toEqual([undefined, undefined]);
      expect(
        Object.values(meter).some((value) => typeof value === 'number' && Number.isNaN(value)),
      ).toBe(false);
    } finally {
      mixer.destroy();
    }
  });

  it('refuses a non-boolean measureLufs or measureTruePeak by name', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      for (const key of ['measureLufs', 'measureTruePeak'] as const) {
        expectRefusal(
          () => mixer.configureMeter({ enabled: true, [key]: 1 } as never),
          TypeError,
          new RegExp(key),
        );
      }
    } finally {
      mixer.destroy();
    }
  });

  it('refuses a non-boolean enabled, a missing options bag and an unknown key', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      expectRefusal(() => mixer.configureMeter({ enabled: 1 } as never), TypeError, /enabled/);
      expectRefusal(() => mixer.configureMeter({} as never), TypeError, /enabled/);
      expectRefusal(() => mixer.configureMeter(undefined as never), TypeError);
      expectRefusal(
        () => mixer.configureMeter({ enabled: true, measureRms: true } as never),
        TypeError,
      );
    } finally {
      mixer.destroy();
    }
  });
});
