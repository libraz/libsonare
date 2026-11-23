import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixStereo } from '../dist/index.js';
import { mixMeterSnapshotFromNative } from '../src/internal/mix_meter_snapshot.js';
import type { MixMeterSnapshot } from '../src/public_types_mixing.js';
import type { WasmMixMeterSnapshot } from '../src/sonare.js';

const REMOVED_LEVEL_FIELDS = [
  'peakDbL',
  'peakDbR',
  'rmsDbL',
  'rmsDbR',
  'truePeakDbtpL',
  'truePeakDbtpR',
] as const;

function nativeMeter(numChannels: number): WasmMixMeterSnapshot {
  const values = Array.from({ length: Math.max(3, numChannels) }, (_, index) =>
    index === 0 ? Number.NaN : index === 1 ? Number.NEGATIVE_INFINITY : -3 - index,
  );
  return {
    peakDbL: -4,
    peakDbR: -5,
    rmsDbL: -10,
    rmsDbR: -11,
    correlation: Number.NaN,
    monoCompatWidth: Number.POSITIVE_INFINITY,
    monoCompatPeak: 0.25,
    monoCompatSideRms: 0.125,
    likelyMonoCompatible: true,
    momentaryLufs: Number.NEGATIVE_INFINITY,
    shortTermLufs: Number.NaN,
    integratedLufs: Number.NaN,
    gainReductionDb: 0,
    truePeakDbtpL: -2,
    truePeakDbtpR: -3,
    maxTruePeakDbtp: Number.NEGATIVE_INFINITY,
    seq: 4,
    numChannels,
    peakDb: values,
    rmsDb: values,
    truePeakDbtp: values,
  };
}

function expectSnapshotShape(meter: MixMeterSnapshot): void {
  expect(meter.peakDb).toHaveLength(meter.numChannels);
  expect(meter.rmsDb).toHaveLength(meter.numChannels);
  expect(meter.truePeakDbtp).toHaveLength(meter.numChannels);
  for (const field of REMOVED_LEVEL_FIELDS) {
    expect(meter).not.toHaveProperty(field);
  }
  for (const plane of [meter.peakDb, meter.rmsDb, meter.truePeakDbtp]) {
    expect(plane.some(Number.isNaN)).toBe(false);
  }
}

function expectNoNaN(value: unknown): void {
  if (typeof value === 'number') {
    expect(Number.isNaN(value)).toBe(false);
  } else if (Array.isArray(value)) {
    value.forEach(expectNoNaN);
  } else if (typeof value === 'object' && value !== null) {
    Object.values(value).forEach(expectNoNaN);
  }
}

describe('mixer meter record conversion', () => {
  it.each([
    ['mono', 1],
    ['stereo', 2],
    ['wide', 6],
  ] as const)('%s arrays contain exactly the measured planes', (_layout, numChannels) => {
    const meter = mixMeterSnapshotFromNative(nativeMeter(numChannels));
    const expected = Array.from({ length: numChannels }, (_, index) =>
      index === 0 ? undefined : index === 1 ? Number.NEGATIVE_INFINITY : -3 - index,
    );
    expectSnapshotShape(meter);
    expect(meter.peakDb).toEqual(expected);
    expect(meter.rmsDb).toEqual(expected);
    expect(meter.truePeakDbtp).toEqual(expected);
    expect(meter.correlation).toBeUndefined();
    expect(meter.monoCompatWidth).toBe(Number.POSITIVE_INFINITY);
    expect(meter.momentaryLufs).toBe(Number.NEGATIVE_INFINITY);
    expect(meter.shortTermLufs).toBeUndefined();
    expect(meter.maxTruePeakDbtp).toBe(Number.NEGATIVE_INFINITY);
    expect(meter.monoCompatPeak).toBe(0.25);
    expect(meter.monoCompatSideRms).toBe(0.125);
    expectNoNaN(meter);
  });

  it('keeps zero channels as empty arrays', () => {
    const meter = mixMeterSnapshotFromNative(nativeMeter(0));
    expectSnapshotShape(meter);
    expect(meter.peakDb).toEqual([]);
    expect(meter.rmsDb).toEqual([]);
    expect(meter.truePeakDbtp).toEqual([]);
    expectNoNaN(meter);
  });
});

describe('mixer meter readers', () => {
  beforeAll(async () => {
    await init();
  });

  it('returns channel arrays from master, strip, bus and one-shot readers', () => {
    const block = Float32Array.from(
      { length: 256 },
      (_, index) => 0.5 * Math.sin((2 * Math.PI * 440 * index) / 48000),
    );
    const mixer = Mixer.fromJson(JSON.stringify({ version: 2, strips: [{ id: 'source' }] }), {
      sampleRate: 48000,
      maxBlockSize: block.length,
    });
    try {
      mixer.configureMeter({ enabled: true });
      const beforeAudio = mixer.meterSnapshot();
      expectSnapshotShape(beforeAudio);
      expect(beforeAudio.numChannels).toBe(0);
      mixer.processStereo([block], [block]);
      for (const meter of [
        mixer.meterSnapshot(),
        mixer.stripMeter('source'),
        mixer.busMeter('master'),
      ]) {
        expectSnapshotShape(meter);
        expect(meter.peakDb).toHaveLength(2);
        expectNoNaN(meter);
      }
    } finally {
      mixer.destroy();
    }

    const result = mixStereo({ strips: [{ left: block, right: block }], sampleRate: 48000 });
    expect(result.meters).toHaveLength(1);
    expectSnapshotShape(result.meters[0]);
    expect(result.meters[0].peakDb).toHaveLength(2);
    expectNoNaN(result.meters[0]);
  });

  it('keeps all planes on a wide bus meter', () => {
    const mixer = Mixer.fromJson(
      JSON.stringify({
        version: 2,
        strips: [{ id: 'source', surroundPan: { azimuth: 0, divergence: 0, lfe: 0 } }],
        buses: [
          { id: 'wide', role: 'submix', layout: '5.1' },
          { id: 'master', role: 'master', layout: '5.1' },
        ],
        connections: [
          { source: 'source', destination: 'wide' },
          { source: 'wide', destination: 'master' },
        ],
      }),
      { sampleRate: 48000, maxBlockSize: 256 },
    );
    try {
      const block = new Float32Array(256).fill(0.25);
      mixer.processStereo([block], [block]);
      const meter = mixer.busMeter('wide');
      expectSnapshotShape(meter);
      expect(meter.numChannels).toBe(6);
      expect(Number.isFinite(meter.peakDb[2])).toBe(true);
      expectNoNaN(meter);
    } finally {
      mixer.destroy();
    }
  });
});
