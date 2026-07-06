export type PanMode = 'balance' | 'stereoPan' | 'stereo-pan' | 'dualPan' | 'dual-pan' | number;

/**
 * Surround pan position for a strip feeding a >2-channel bus. Phase 1 honors
 * `azimuth`/`divergence`/`lfe`; `elevation`/`distance` are reserved. All fields
 * are optional and default to a centered point source.
 */
export interface SurroundPan {
  /** -180..180 deg, 0 = front-center, positive = right. */
  azimuth?: number;
  /** Reserved (no height beds in phase 1). */
  elevation?: number;
  /** 0 = point source, 1 = spread across the front. */
  divergence?: number;
  /** 0..1 scalar send into the LFE plane. */
  lfe?: number;
  /** Reserved (focus/spread), defaults to 1. */
  distance?: number;
}

export interface MixOptions {
  inputTrimDb?: number | number[];
  faderDb?: number | number[];
  pan?: number | number[];
  panMode?: PanMode | PanMode[];
  width?: number | number[];
  muted?: boolean | boolean[];
}

export interface MixMeterSnapshot {
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
  monoCompatWidth: number;
  monoCompatPeak: number;
  monoCompatSideRms: number;
  likelyMonoCompatible: boolean;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  truePeakDbL: number;
  truePeakDbR: number;
  maxTruePeakDb: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  channelCount: number;
  /** Per-plane peak dB, length channelCount; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length channelCount; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /** Per-plane true-peak dB, length channelCount; [0]/[1] mirror truePeakDbL/R. */
  truePeakDb: number[];
}

export interface MixResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
  meters: MixMeterSnapshot[];
}

/** Mixed stereo master returned by {@link Mixer.processStereo}. */
export interface MixerProcessResult {
  left: Float32Array;
  right: Float32Array;
  sampleRate: number;
}

/**
 * Interpolation curve for scheduled automation events
 * (see {@link Mixer.scheduleInsertAutomation}).
 */
export type AutomationCurve = 'linear' | 'exponential' | 'hold' | 's-curve';

/**
 * Pan law applied when computing left/right gains from a pan position
 * (see {@link Mixer.setPanLaw}). Maps to the underlying integer code.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/**
 * Meter tap point for reading a strip's meter snapshot
 * (see {@link Mixer.meterTap} and {@link Mixer.stripMeter}).
 */
export type MeterTap = 'preFader' | 'postFader';

/** Pre/post-fader send timing (see {@link Mixer.addSend}). */
export type SendTiming = 'preFader' | 'postFader';

/** A single goniometer (left/right) sample returned by {@link Mixer.readGoniometerLatest}. */
export interface GoniometerPoint {
  left: number;
  right: number;
}
