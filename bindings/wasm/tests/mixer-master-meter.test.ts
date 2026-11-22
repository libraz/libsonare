import { readFileSync } from 'node:fs';
import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, Mixer, type MixMeterSnapshot, type SonareError } from '../dist/index.js';
import { expectRefusalOf } from './_helpers';

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
    peakDbL: actual.peakDbL,
    peakDbR: actual.peakDbR,
    rmsDbL: actual.rmsDbL,
    rmsDbR: actual.rmsDbR,
    truePeakDbL: actual.truePeakDbtpL,
    truePeakDbR: actual.truePeakDbtpR,
  };
  for (const [field, value] of Object.entries(dB)) {
    expect(
      Math.abs(value - expected[field as keyof Reading]),
      `${field}: ${value} vs ${expected[field as keyof Reading]}`,
    ).toBeLessThanOrEqual(fixture.toleranceDb);
  }
  expect(Math.abs(actual.correlation - expected.correlation)).toBeLessThanOrEqual(
    fixture.toleranceCorrelation,
  );
}

describe('Mixer master meter', () => {
  beforeAll(async () => {
    await init();
  });

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
      let caught: unknown;
      try {
        mixer.meterSnapshot();
      } catch (error) {
        caught = error;
      }
      expect((caught as SonareError).code).toBe(ErrorCode.InvalidState);
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
        const error = expectRefusalOf(
          () => mixer.configureMeter({ enabled: true, truePeakOversample: factor }),
          RangeError,
        );
        expect(error.message).toMatch(/truePeakOversample/);
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
    // Switched off: the readings are absent, not a floor value.
    expect('momentaryLufs' in unmeasured).toBe(false);
    expect('maxTruePeakDbtp' in unmeasured).toBe(false);
    expect(unmeasured.peakDbL).toBeCloseTo(measured.peakDbL, 3);
  });

  it('refuses a non-boolean measureLufs or measureTruePeak by name', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      for (const key of ['measureLufs', 'measureTruePeak'] as const) {
        expect(
          expectRefusalOf(
            () => mixer.configureMeter({ enabled: true, [key]: 1 } as never),
            TypeError,
          ).message,
        ).toMatch(new RegExp(key));
      }
    } finally {
      mixer.destroy();
    }
  });

  it('refuses a non-boolean enabled, a missing options bag and an unknown key', () => {
    const mixer = Mixer.fromJson(SCENE, { sampleRate: 48000, maxBlockSize: 256 });
    try {
      expect(
        expectRefusalOf(() => mixer.configureMeter({ enabled: 1 } as never), TypeError).message,
      ).toMatch(/enabled/);
      expect(expectRefusalOf(() => mixer.configureMeter({} as never), TypeError).message).toMatch(
        /enabled/,
      );
      expectRefusalOf(() => mixer.configureMeter(undefined as never), TypeError);
      expectRefusalOf(
        () => mixer.configureMeter({ enabled: true, measureRms: true } as never),
        TypeError,
      );
    } finally {
      mixer.destroy();
    }
  });
});
