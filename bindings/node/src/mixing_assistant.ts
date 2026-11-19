import { refuse, refuseType, withEntry } from './internal/entry.js';
import { parseRecordJson } from './internal/record_json.js';
import { addon } from './native.js';
import type { MixAssistantResult, MixAssistantTrack, MixSourceClass } from './types.js';
import {
  assertClosedToken,
  assertPositiveInteger,
  assertSampleRate,
  requestOnly,
} from './validation.js';

/**
 * Inputs for {@link mixingAssistantSuggest}. Every tunable is optional; an omitted field keeps the
 * native default rather than being sent as an explicit value.
 *
 * A non-finite track is not refused: its profile comes back with `usable: false` and an
 * `exclusionReason`.
 */
export interface MixingAssistantSuggestRequest {
  /** Tracks to mix. An empty array yields an empty suggestion. */
  tracks: MixAssistantTrack[];
  /**
   * Analysis rate in Hz, and the rate of every track that does not state its own; a track at
   * another rate is resampled onto it. Required.
   */
  sampleRate: number;
  /** Absolute integrated-loudness target each track is staged towards, in LUFS. */
  targetTrackLufs?: number;
  /**
   * Overall strength of the suggestion in `[0, 1]`, scaling every level-like
   * decision: trims, fader offsets, send levels, EQ cut depths, compression
   * ratios and ranges, and how far a track is spread from the centre.
   *
   * `0` is not an empty suggestion. It is every one of those taken and set to
   * zero, plus the decisions that are not levels and so do not scale: the bus
   * topology and routing, and the physical corrections for a measured
   * cancellation (polarity, alignment delay, low-end mono fold). To suggest
   * nothing, switch the domains off instead — that also skips the work.
   */
  suggestionStrength?: number;
  /** Largest cut a single suggested EQ band may apply, in dB. */
  eqMaxCutDb?: number;
  /** Headroom the summed mix is left with on the master bus, in dBTP. */
  mixBusHeadroomDbtp?: number;
  /**
   * Tempo the suggested delay times are voiced against, in BPM.
   *
   * Omit it to use the transport's own fallback tempo: the assistant is handed
   * bare stems and cannot measure a tempo the set as a whole agrees on. Pass the
   * song's tempo and the delay lands on the beat.
   *
   * Zero or a negative value is refused, and a value outside 20–400 BPM is
   * rejected rather than clamped.
   */
  tempoBpm?: number;
  /** Evaluate the bus/structure domain. */
  enableStructure?: boolean;
  /** Evaluate the gain-staging domain. */
  enableGain?: boolean;
  /** Evaluate the level-balance domain. */
  enableBalance?: boolean;
  /** Evaluate the EQ domain. */
  enableEq?: boolean;
  /** Evaluate the dynamics domain. */
  enableDynamics?: boolean;
  /** Evaluate the stereo-image domain. */
  enableImage?: boolean;
  /**
   * Suggest a high-pass filter on tracks carrying residue below their register.
   *
   * Off by default: a survey of mixing best practices found the rule that every
   * track without low-frequency content should be high-passed to be seldom used
   * in studio mixing and unsupported by subjective testing. Switched on, the
   * filter is proposed from the track's measured low-frequency content rather
   * than from its source class, so a part playing below its class's usual
   * register keeps what it plays.
   */
  enableHighPass?: boolean;
  /** Shared STFT size for every track. */
  nFft?: number;
  /** Shared STFT hop length for every track. */
  hopLength?: number;
}

/** Planar parallel arrays in the shape the addon entry points take. */
interface NativeTrackArrays {
  left: Float32Array[];
  right: (Float32Array | null)[] | null;
  ids: string[];
  names: (string | null)[] | null;
  sampleRates: number[] | null;
}

/** A track's one or two planes; a stereo pair must agree in length. */
function trackChannels(
  prefix: string,
  channels: unknown,
): [Float32Array, Float32Array | undefined] {
  if (!Array.isArray(channels)) {
    throw refuseType(`${prefix}.channels must be an array of Float32Array`);
  }
  if (channels.length < 1 || channels.length > 2) {
    throw refuse(`${prefix}.channels must hold 1 or 2 channels`);
  }
  channels.forEach((plane, ch) => {
    if (!(plane instanceof Float32Array)) {
      throw refuseType(`${prefix}.channels[${ch}] must be a Float32Array`);
    }
  });
  const [first, second] = channels as Float32Array[];
  if (second !== undefined && second.length !== first.length) {
    throw refuse(`${prefix}.channels must have equal lengths`);
  }
  return [first, second];
}

function normalizeTracks(tracks: MixAssistantTrack[], sampleRate: number): NativeTrackArrays {
  if (!Array.isArray(tracks)) {
    throw refuseType(`tracks must be an array`);
  }
  const left: Float32Array[] = [];
  const right: (Float32Array | null)[] = [];
  const ids: string[] = [];
  const names: (string | null)[] = [];
  const sampleRates: number[] = [];
  const seen = new Set<string>();
  let anyRight = false;
  let anyName = false;
  let anySampleRate = false;

  tracks.forEach((track, index) => {
    const prefix = `tracks[${index}]`;
    if (track === null || typeof track !== 'object') {
      throw refuseType(`${prefix} must be an object`);
    }
    if (typeof track.id !== 'string' || track.id === '') {
      throw refuseType(`${prefix}.id must be a non-empty string`);
    }
    if (seen.has(track.id)) {
      throw refuse(`duplicate track id '${track.id}'`);
    }
    seen.add(track.id);
    const [first, second] = trackChannels(prefix, track.channels);
    if (track.name !== undefined && typeof track.name !== 'string') {
      throw refuseType(`${prefix}.name must be a string when present`);
    }
    if (track.sampleRate !== undefined) {
      assertSampleRate(track.sampleRate, `${prefix}.sampleRate`);
    }
    left.push(first);
    right.push(second ?? null);
    ids.push(track.id);
    names.push(track.name ?? null);
    sampleRates.push(track.sampleRate ?? sampleRate);
    anyRight ||= second !== undefined;
    anyName ||= track.name !== undefined;
    anySampleRate ||= track.sampleRate !== undefined;
  });

  // An all-mono set, a set with no names and a set with no per-track rate pass
  // null rather than an array, which is what the native side treats as "not
  // supplied at all".
  return {
    left,
    right: anyRight ? right : null,
    ids,
    names: anyName ? names : null,
    sampleRates: anySampleRate ? sampleRates : null,
  };
}

const OPTION_KEYS = [
  'targetTrackLufs',
  'suggestionStrength',
  'eqMaxCutDb',
  'mixBusHeadroomDbtp',
  'tempoBpm',
  'enableStructure',
  'enableGain',
  'enableBalance',
  'enableEq',
  'enableDynamics',
  'enableImage',
  'enableHighPass',
  'nFft',
  'hopLength',
] as const;

/** Absence is the transport fallback; the C entry's `0` for it is not a value a caller may pass. */
function assertTempo(value: unknown): void {
  if (typeof value === 'number' && !(Number.isFinite(value) && value > 0)) {
    throw refuse(
      `tempoBpm must be finite and greater than 0; omit it to use the transport's fallback tempo`,
    );
  }
}

/** The two analysis-geometry keys, which reach the core as a truncating cast. */
const INTEGER_OPTION_KEYS = ['nFft', 'hopLength'] as const;

function normalizeOptions(
  request: MixingAssistantSuggestRequest,
): Record<string, number | boolean> {
  // Only the supplied keys are forwarded, so the core refuses a wrongly typed value exactly as it
  // does on every other surface. An undefined value is an omitted key: the native default stands.
  const params: Record<string, number | boolean> = {};
  for (const key of OPTION_KEYS) {
    const value = request[key];
    if (value !== undefined && value !== null) {
      params[key] = value;
    }
  }
  // Positivity is the STFT geometry's own rule; the size ceiling stays the core's, which names it.
  for (const key of INTEGER_OPTION_KEYS) {
    const value = request[key];
    if (value !== undefined && value !== null) {
      assertPositiveInteger(value, key);
    }
  }
  return params;
}

/**
 * Analyse a set of tracks and return a suggested mixer scene, the measurements
 * behind it and a human-readable explanation.
 *
 * The assistant only ever returns parameters: it does not process audio and it
 * does not apply anything. Realising the suggestion is a separate explicit step
 * through {@link Mixer.fromJson} with `result.scene`.
 *
 * Offline / control thread only: this runs an STFT per track and evaluates
 * every track pair. Tracks may differ in length.
 *
 * A degenerate track (empty, holding a NaN or Inf sample, too short for a gated
 * loudness, silent, or without energy in the analysis bands) is excluded rather
 * than refused: its profile reports `usable: false` and an `exclusionReason`
 * naming which, and the call succeeds. This is documented behaviour, not a
 * missing check.
 *
 * @param request - Tracks, analysis sample rate and assistant tunables
 * @throws TypeError for a wrong-typed or missing field or an unknown key; RangeError for an
 *   out-of-domain rate or tunable
 */
export function mixingAssistantSuggest(request: MixingAssistantSuggestRequest): MixAssistantResult;
export function mixingAssistantSuggest(...args: unknown[]): MixAssistantResult {
  return withEntry('mixingAssistantSuggest', () => {
    const fnName = 'mixingAssistantSuggest';
    const req = requestOnly<MixingAssistantSuggestRequest>(fnName, args);
    assertSampleRate(req.sampleRate);
    const tracks = normalizeTracks(req.tracks, req.sampleRate);
    assertTempo(req.tempoBpm);
    const params = normalizeOptions(req);
    const json = addon.mixingAssistantSuggest(
      tracks.left,
      tracks.right,
      tracks.ids,
      tracks.names,
      tracks.sampleRates,
      req.sampleRate,
      params,
    );
    return parseRecordJson(json) as unknown as MixAssistantResult;
  });
}

/** The source-class identifiers the assistant can report, in native ordinal order. */
export function mixingAssistantSourceClassNames(): MixSourceClass[] {
  return addon.mixingAssistantSourceClassNames() as MixSourceClass[];
}

/**
 * Resolve a source-class identifier to its ordinal in {@link mixingAssistantSourceClassNames}.
 *
 * @throws TypeError when `name` is not a string; RangeError for an unknown name
 */
export function mixingAssistantSourceClassFromName(name: MixSourceClass): number {
  return withEntry('mixingAssistantSourceClassFromName', () => {
    assertClosedToken(name, mixingAssistantSourceClassNames(), 'name');
    return addon.mixingAssistantSourceClassFromName(name) as number;
  });
}
