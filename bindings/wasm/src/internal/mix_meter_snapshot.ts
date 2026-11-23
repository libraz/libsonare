import type { MixMeterSnapshot } from '../public_types_mixing.js';
import type { WasmMixMeterSnapshot } from '../sonare.js';
import { measured } from './metering_native.js';

function optionalMeasured(value: number | undefined): number | undefined {
  return value === undefined ? undefined : measured(value);
}

/** The raw mixer meter record with its levels represented by measured planes. */
export function mixMeterSnapshotFromNative(record: WasmMixMeterSnapshot): MixMeterSnapshot {
  const correlation = optionalMeasured(record.correlation);
  const monoCompatWidth = optionalMeasured(record.monoCompatWidth);
  const maxTruePeakDbtp = optionalMeasured(record.maxTruePeakDbtp);
  const momentaryLufs = optionalMeasured(record.momentaryLufs);
  const shortTermLufs = optionalMeasured(record.shortTermLufs);
  const integratedLufs = optionalMeasured(record.integratedLufs);
  const planes = (values: number[]): (number | undefined)[] =>
    Array.from({ length: record.numChannels }, (_, index) => measured(values[index] ?? Number.NaN));

  return {
    monoCompatPeak: record.monoCompatPeak,
    monoCompatSideRms: record.monoCompatSideRms,
    likelyMonoCompatible: record.likelyMonoCompatible,
    gainReductionDb: record.gainReductionDb,
    seq: record.seq,
    ...(correlation === undefined ? {} : { correlation }),
    ...(monoCompatWidth === undefined ? {} : { monoCompatWidth }),
    ...(maxTruePeakDbtp === undefined ? {} : { maxTruePeakDbtp }),
    ...(momentaryLufs === undefined ? {} : { momentaryLufs }),
    ...(shortTermLufs === undefined ? {} : { shortTermLufs }),
    ...(integratedLufs === undefined ? {} : { integratedLufs }),
    numChannels: record.numChannels,
    peakDb: planes(record.peakDb),
    rmsDb: planes(record.rmsDb),
    truePeakDbtp: planes(record.truePeakDbtp),
  };
}
