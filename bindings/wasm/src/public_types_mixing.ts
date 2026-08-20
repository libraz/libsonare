export type PanMode =
  | 'balance'
  | 'pan'
  | 'stereoPan'
  | 'stereo-pan'
  | 'dualPan'
  | 'dual-pan'
  | number;

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
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement: the centered
   * reconstruction stencil needs a few future samples a realtime path does not
   * have, so each block's last samples read marginally low (about 0.1 dB across
   * 64..8192-sample blocks on a near-Nyquist tone, always under-reading). Use
   * `meteringTruePeakDb` over the whole signal for an exact dBTP number.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDbL}. */
  maxTruePeakDb: number;
  seq: number;
  /** Number of valid surround planes (5.1/7.1); 0 before the meter sees audio. */
  channelCount: number;
  /** Per-plane peak dB, length channelCount; [0]/[1] mirror peakDbL/peakDbR. */
  peakDb: number[];
  /** Per-plane RMS dB, length channelCount; [0]/[1] mirror rmsDbL/rmsDbR. */
  rmsDb: number[];
  /**
   * Per-plane true-peak dB, length channelCount; [0]/[1] mirror
   * {@link truePeakDbL}/R and carry the same streaming caveat.
   */
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
 * (see {@link Mixer.setPanLaw}). On mono strips it changes the centre gain;
 * on stereo Balance strips it changes only the far-channel taper, while centre
 * remains unity. Maps to the underlying integer code.
 */
export type PanLaw = 'const3dB' | 'const4.5dB' | 'const6dB' | 'linear0dB';

/** Accepted pan-law name aliases for mixer and realtime-engine inputs. */
export type PanLawName =
  | PanLaw
  | 'const-3db'
  | '-3db'
  | 'const-4.5db'
  | '-4.5db'
  | 'const-6db'
  | '-6db'
  | 'linear-0db'
  | 'linear'
  | '0db';

/** Pan-law name or raw C ABI ordinal. */
export type PanLawInput = PanLawName | number;

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
