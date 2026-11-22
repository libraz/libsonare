/** Options for `detectAcoustic`. All fields are optional. */
export interface AcousticOptions {
  /** Number of octave bands. Default 6. */
  nOctaveBands?: number;
  /** Number of 1/3-octave sub-bands. Default 24. */
  nThirdOctaveSubbands?: number;
  /** Minimum decay range (dB) for a valid RT60 fit. Default 30. */
  minDecayDb?: number;
  /** Margin (dB) above the noise floor for the decay fit. Default 10. */
  noiseFloorMarginDb?: number;
}

/**
 * Room acoustic parameters from a blind recording or a measured impulse
 * response (`isBlind` distinguishes the two).
 *
 * Only `rt60` (and `rt60Bands`) is estimated in both modes. `c50`/`c80`/`d50`
 * and `edt` require a known direct-sound arrival time, which only a measured
 * impulse response provides, so they are NaN when `isBlind` is true -- `edt`
 * measures the 0 to -10 dB decay and the blind estimator only fits the late
 * decay `rt60` comes from.
 *
 * A band array that was not computed is EMPTY, never zero-filled: a zero-filled
 * clarity array is indistinguishable from a genuine 0 dB measurement. So
 * `c50Bands` and `c80Bands` have length 0 when `isBlind` is true. `edtBands` is
 * the exception -- it keeps its full length and is all NaN, so it can be indexed
 * by the same band index as `rt60Bands`.
 */
export interface AcousticResult {
  /** Broadband reverberation time (RT60) in seconds. */
  rt60: number;
  /** Early decay time in seconds; NaN when `isBlind`. */
  edt: number;
  /** Clarity C50 in dB; NaN when `isBlind`. */
  c50: number;
  /** Clarity C80 in dB; NaN when `isBlind`. */
  c80: number;
  /** Definition D50 (early-to-total energy ratio); NaN when `isBlind`. */
  d50: number;
  /** Per-octave-band RT60 in seconds. */
  rt60Bands: Float32Array;
  /** Per-octave-band early decay time in seconds; all NaN when `isBlind`. */
  edtBands: Float32Array;
  /** Per-octave-band C50 in dB; empty when `isBlind`. */
  c50Bands: Float32Array;
  /** Per-octave-band C80 in dB; empty when `isBlind`. */
  c80Bands: Float32Array;
  /** Support for the estimate, in [0, 1]. */
  confidence: number;
  /** True when measured from a blind recording rather than an impulse response. */
  isBlind: boolean;
}

/** Names accepted for `materialPreset`. */
export type MaterialPresetName = 'none' | 'concrete' | 'wood' | 'curtain' | 'carpet' | 'glass';

/** Names accepted for the room-estimate `mode`. */
export type AcousticModeName = 'auto' | 'blind' | 'impulseResponse';

/** Shoebox geometry and placement: what {@link roomGeometryFromEstimate} returns, and what RIR synthesis and the room morph extend. */
export interface RoomGeometry {
  /** Room length in metres. */
  lengthM?: number;
  /** Room width in metres. */
  widthM?: number;
  /** Room height in metres. */
  heightM?: number;
  /**
   * Uniform wall absorption in [0, 1], used when no band absorption or preset
   * is given; a value outside is refused, and the model holds a wall at most
   * 0.999 absorbent. Defaults to 0.2. Higher absorption shortens RT60, so it changes both the
   * rendered content and the tail length rather than only the timbre.
   */
  absorption?: number;
  /**
   * Optional per-octave-band wall absorption (125/250/500/1k/2k/4k.. Hz). When
   * provided it overrides `absorption` unless `materialPreset` is set.
   *
   * The late tail's decay time runs continuously between octave centres, so
   * where absorption (a preset's included) changes steeply from one octave to
   * the next, the octave-band RT60 measured back from the result leans toward
   * the slower neighbour, as it does for a real room. The design value holds
   * at the octave centre.
   */
  bandAbsorption?: Float32Array | number[];
  /**
   * Optional per-band wall scattering; missing bands default to 0. Independent
   * of `bandAbsorption` and `materialPreset` — it applies to whichever material
   * the absorption precedence selected, so a preset or a scalar-absorption room
   * can still be given rough walls.
   */
  bandScattering?: Float32Array | number[];
  /**
   * Wall-material preset, by name (`'none'`, `'concrete'`, `'wood'`,
   * `'curtain'`, `'carpet'`, `'glass'`). Any preset but `'none'` wins over
   * `bandAbsorption`/`absorption`. An unknown name throws a `RangeError` and a
   * number a `TypeError`, both listing the valid names.
   */
  materialPreset?: MaterialPresetName;
  /** Source position in metres, with finite `x`, `y` and `z`. Omitted: the default placement scaled to the room. */
  source?: RoomPlacement;
  /** Listener position in metres, with finite `x`, `y` and `z`. Omitted: the default placement scaled to the room. */
  listener?: RoomPlacement;
  /** Image-source reflection order; clamped to the safe maximum. */
  ismOrder?: number;
  /**
   * Seed of the diffuse tail, a uint32. Omit for the default seed 1; 0 is a seed
   * like any other.
   */
  seed?: number;
  /** Maximum RIR length in seconds. Omit for the natural length (no cap); an explicit 0 is refused. */
  maxSec?: number;
  /**
   * Add the ISO 9613-1 atmospheric-absorption term to the late tail's per-band
   * RT60. On by default; it mainly shortens the high bands of a large room.
   * `false` gives the geometry-only RT60.
   */
  airAbsorptionEnabled?: boolean;
  /**
   * Air temperature in degrees Celsius. Omit for the ISO reference climate's
   * 20 degC; 0 is 0 degC.
   */
  airTemperatureC?: number;
  /**
   * Relative humidity in percent. Omit for the ISO reference climate's 50 %; 0 is
   * dry air. Both climate values are read only while `airAbsorptionEnabled` is set.
   */
  airHumidityPercent?: number;
}

/** Request for {@link synthesizeRir}. */
export interface SynthesizeRirRequest extends RoomGeometry {
  /** Sample rate in Hz, an integer in `[8000, 384000]`; there is no default. */
  sampleRate: number;
  /** Use the Eyring statistical late-tail model (default true); false = Sabine. */
  preferEyring?: boolean;
  /** Early/late crossover in ms. Omit for auto (~sqrt(V) ms); an explicit 0 is refused. */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms. Omit for 5 ms; 0 is a hard switch. */
  crossfadeMs?: number;
}

/** One diagnostic reported by the RIR synthesizer. */
export interface RirDiagnostic {
  /** Stable machine-readable id, e.g. `acoustic.source_outside_room`. */
  code: string;
  /** Human-readable description of the diagnostic. */
  message: string;
  /** Severity of the diagnostic. */
  severity: 'info' | 'warning' | 'error';
}

/** Synthesized room impulse response with its sample rate and diagnostics. */
export interface RirResult {
  /** Impulse response samples. */
  rir: Float32Array;
  /** Sample rate of `rir` in Hz. */
  sampleRate: number;
  /**
   * Every diagnostic the synthesizer reported, in order. A refused synthesis
   * throws instead, so every entry is a warning — a
   * `maxSec` clamp that cut the tail is one, and is otherwise
   * indistinguishable from an untruncated RIR.
   */
  diagnostics: RirDiagnostic[];
}

/** Settings of {@link estimateRoom}. */
export interface RoomEstimateOptions {
  /** Length/width ratio hint of the room. Omit for no hint; an explicit 0 is refused. */
  aspectHintLw?: number;
  /** Length/height ratio hint of the room. Omit for no hint; an explicit 0 is refused. */
  aspectHintLh?: number;
  /**
   * Mean-absorption prior anchoring the volume scale. Omit for the library
   * default, 0.15; an explicit 0 is refused. Clamped into `[0.01, 0.99]` rather than refused: a value outside that
   * range still returns a successful estimate, computed from the clamped prior.
   * The reported volume scales with the cube of the prior, so the substitution
   * is worth three orders of magnitude at the low end.
   */
  referenceAbsorption?: number;
  /** Use the Eyring statistical late-tail model (default true); false = Sabine. */
  preferEyring?: boolean;
  /** Octave bands analysed. Omit for the library default; an explicit 0 is refused. */
  nOctaveBands?: number;
  /**
   * Analyzer routing, by name: `'auto'`, `'blind'`, `'impulseResponse'`. An
   * unknown name throws a `RangeError` and a number a `TypeError`, both listing
   * the valid names.
   */
  mode?: AcousticModeName;
  /** Analyzer decay-fit span in dB. Omit for the library default; an explicit 0 is refused. */
  minDecayDb?: number;
  /** Analyzer noise-floor margin in dB. Omit for the library default; an explicit 0 is refused. */
  noiseFloorMarginDb?: number;
}

/** Equivalent-room estimate of a recording: volume, representative dimensions, direct-to-reverberant ratio and per-band absorption. */
export interface RoomEstimateResult {
  /**
   * Equivalent volume (m^3) and representative dimensions (m), under the names
   * {@link RoomGeometry} takes. NaN, with `confidence` 0, when the
   * recording has no measurable broadband decay (silence, or an RT60 the
   * analyzer could not fit) — the acoustic family's "not measurable", as in
   * `rt60Bands`.
   */
  volume: number;
  /** Representative room length in metres; NaN when no decay is measurable. */
  lengthM: number;
  /** Representative room width in metres; NaN when no decay is measurable. */
  widthM: number;
  /** Representative room height in metres; NaN when no decay is measurable. */
  heightM: number;
  /** Direct-to-reverberant ratio in dB. */
  drrDb: number;
  /** Support for the estimate, in [0, 1]. */
  confidence: number;
  /** Per-octave-band mean wall absorption. */
  bandAbsorption: Float32Array;
  /** Per-octave-band RT60 in seconds. */
  rt60Bands: Float32Array;
}

/** A position inside a room, in metres. */
export interface RoomPlacement {
  /** X coordinate in metres. */
  x: number;
  /** Y coordinate in metres. */
  y: number;
  /** Z coordinate in metres. */
  z: number;
}

/** Settings of {@link roomMorph} and {@link roomMorphStereo}. */
export interface RoomMorphOptions extends RoomGeometry {
  /** Wet level of the target room. */
  wet?: number;
  /** How far the source recording's own room tail is suppressed. */
  sourceTailSuppression?: number;
  /**
   * Use the Eyring statistical late-tail model for the target room (default
   * true); false = Sabine. Matches {@link SynthesizeRirRequest.preferEyring}.
   */
  preferEyring?: boolean;
  /** Early/late crossover in ms. Omit for auto (~sqrt(V) ms); an explicit 0 is refused. */
  mixingTimeMs?: number;
  /** Equal-power crossfade width around the mixing time in ms. Omit for 5 ms; 0 is a hard switch. */
  crossfadeMs?: number;
}

/**
 * Morphed audio and what the target-room synthesis had to change to make it.
 *
 * Shaped like {@link RirResult} because the same synthesis runs underneath.
 * An unusable morph throws, so every entry in {@link RoomMorphResult.diagnostics}
 * is a warning.
 */
export interface RoomMorphResult {
  /** Morphed samples: input length plus the target room's reverb tail. */
  audio: Float32Array;
  /** Sample rate of `audio` in Hz. */
  sampleRate: number;
  /**
   * Every diagnostic the target-room synthesis reported, in order. Each says the
   * morph went through a room other than the one requested — an image-source
   * order reduced to the safe maximum (`acoustic.ism_order_clamped`), a tail cut
   * against `maxSec` (`acoustic.rir_length_clamped`), a `maxSec` shorter
   * than the direct sound's flight time and extended to fit it
   * (`acoustic.rir_length_floored`), a `maxSec` shorter than the longest
   * band's RT60, which cuts that band before it decays by 60 dB so its
   * reverberation time cannot be measured from the RIR
   * (`acoustic.rir_tail_truncated`), a request that produced no diffuse tail
   * (`acoustic.no_late_tail`) — and is otherwise invisible.
   *
   * These are the five codes the synthesis can emit here, so a `switch` over
   * them needs no fall-through case. `roomMorph` forwards `maxSec`
   * unchanged, which is why the floored one reaches a morph at all.
   */
  diagnostics: RirDiagnostic[];
}

/** Result of {@link roomMorphStereo}: one morphed channel per input channel. */
export interface RoomMorphStereoResult {
  /** Morphed left samples: input length plus the target room's reverb tail. */
  left: Float32Array;
  /** Morphed right samples, the same length as `left`. */
  right: Float32Array;
  /** Sample rate of `left` and `right` in Hz. */
  sampleRate: number;
  /** Target-room synthesis diagnostics, with the same codes as {@link RoomMorphResult.diagnostics}. */
  diagnostics: RirDiagnostic[];
}
