import { refuse, refuseType, withEntry } from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { sampleNeed, withHeapNeed } from './internal/memory_budget.js';
import { parseRecordJson } from './internal/record_json.js';
import { isAbsent } from './internal/value_guards.js';
import { getSonareModule } from './module_state.js';
import type { MixAssistantResult, MixAssistantTrack, MixSourceClass } from './public_types.js';
import { assertClosedToken, assertSampleRate, requestOnly } from './validation.js';

/**
 * Inputs for {@link mixingAssistantSuggest}. Every tunable is optional and falls back to the core
 * default noted on it; the same field names and defaults are used by the Node and Python surfaces.
 *
 * A non-finite track is not refused: its profile comes back with `usable: false` and an
 * `exclusionReason`.
 */
export interface MixingAssistantSuggestRequest {
  /** Tracks to mix, in the order their profiles are reported. */
  tracks: MixAssistantTrack[];
  /**
   * Analysis rate in Hz, and the rate of every track that does not state its
   * own; a track at another rate is resampled onto it. Required: every band edge,
   * high-pass corner, sibilance band and alignment lag is derived from it, so a
   * guessed rate would silently misread 44.1 kHz material by 8.8% and report
   * `channelDelaySamples` and `durationSec` wrong with it.
   */
  sampleRate: number;
  /** Absolute integrated-loudness target each track is staged towards, in LUFS. Defaults to -18. */
  targetTrackLufs?: number;
  /**
   * Overall strength of the suggestion in `[0, 1]`, scaling every level-like
   * decision: trims, fader offsets, send levels, EQ cut depths, compression
   * ratios and ranges, and how far a track is spread from the centre. Defaults
   * to 1.
   *
   * `0` is not an empty suggestion. It is every one of those taken and set to
   * zero, plus the decisions that are not levels and so do not scale: the bus
   * topology and routing, and the physical corrections for a measured
   * cancellation (polarity, alignment delay, low-end mono fold). To suggest
   * nothing, switch the domains off instead — that also skips the work.
   */
  suggestionStrength?: number;
  /** Largest cut a single suggested EQ band may apply, in dB. Defaults to 4. */
  eqMaxCutDb?: number;
  /** Headroom the summed mix is left with on the master bus, in dBTP. Defaults to -6. */
  mixBusHeadroomDbtp?: number;
  /**
   * Tempo the suggested delay times are voiced against, in BPM.
   *
   * Omit it to use the transport's own fallback tempo: the assistant is handed
   * bare stems and cannot measure a tempo the set as a whole agrees on. Pass the
   * song's tempo and the delay lands on the beat.
   *
   * A supplied tempo must be finite and in [20, 400] BPM; zero is refused.
   */
  tempoBpm?: number;
  /** Evaluate the structure domain. Defaults to true. */
  enableStructure?: boolean;
  /** Evaluate the gain-staging domain. Defaults to true. */
  enableGain?: boolean;
  /** Evaluate the balance domain. Defaults to true. */
  enableBalance?: boolean;
  /** Evaluate the EQ domain. Defaults to true. */
  enableEq?: boolean;
  /** Evaluate the dynamics domain. Defaults to true. */
  enableDynamics?: boolean;
  /** Evaluate the stereo-image domain. Defaults to true. */
  enableImage?: boolean;
  /**
   * Suggest a high-pass filter on tracks carrying residue below their register.
   * Defaults to false.
   *
   * Off by default: a survey of mixing best practices found the rule that every
   * track without low-frequency content should be high-passed to be seldom used
   * in studio mixing and unsupported by subjective testing. Switched on, the
   * filter is proposed from the track's measured low-frequency content rather
   * than from its source class, so a part playing below its class's usual
   * register keeps what it plays.
   */
  enableHighPass?: boolean;
  /** Shared STFT size for every track. Defaults to 2048. */
  nFft?: number;
  /** Shared STFT hop for every track. Defaults to 512. */
  hopLength?: number;
}

/** The four parallel arrays the embind entry points take. */
interface PlanarTracks {
  left: Float32Array[];
  right: (Float32Array | null)[];
  ids: string[];
  names: (string | null)[];
  sampleRates: (number | null)[] | null;
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
  return [first, second];
}

/**
 * Splits the request's track list into the planar per-track arrays the binding
 * takes, rejecting the shapes that would otherwise reach the analysis as a
 * missing buffer or a nameless strip.
 */
function planarTracks(tracks: MixAssistantTrack[], _sampleRate: number): PlanarTracks {
  if (!Array.isArray(tracks)) {
    throw refuseType(`tracks must be an array`);
  }
  requireListBudget(tracks, 'tracks');
  const left: Float32Array[] = [];
  const right: (Float32Array | null)[] = [];
  const ids: string[] = [];
  const names: (string | null)[] = [];
  const sampleRates: (number | null)[] = [];
  let anySampleRate = false;
  for (let index = 0; index < tracks.length; index++) {
    const track = tracks[index];
    if (track === null || typeof track !== 'object') {
      throw refuseType(`tracks[${index}] must be an object`);
    }
    if (typeof track.id !== 'string' || track.id.length === 0) {
      throw refuseType(`tracks[${index}].id must be a non-empty string`);
    }
    // An empty or non-finite track is not refused: the core reports it as an excluded track so a
    // batch needs no per-track error handling.
    const [first, second] = trackChannels(`tracks[${index}]`, track.channels);
    if (track.name !== undefined && typeof track.name !== 'string') {
      throw refuseType(`tracks[${index}].name must be a string when present`);
    }
    if (track.sampleRate !== undefined && typeof track.sampleRate !== 'number') {
      throw refuseType(`tracks[${index}].sampleRate must be an integer`);
    }
    left.push(first);
    right.push(second ?? null);
    ids.push(track.id);
    names.push(track.name ?? null);
    sampleRates.push(track.sampleRate ?? null);
    anySampleRate ||= track.sampleRate !== undefined;
  }
  // null when no track states its own rate: every track is at `sampleRate`.
  return { left, right, ids, names, sampleRates: anySampleRate ? sampleRates : null };
}

function assertAssistantTrackLengths(tracks: PlanarTracks): void {
  for (let index = 0; index < tracks.left.length; index++) {
    const right = tracks.right[index];
    if (right !== null && right !== undefined && right.length !== tracks.left[index]?.length) {
      throw refuse(`tracks[${index}].channels must have equal lengths`);
    }
  }
}

function assertTempoDomain(value: unknown): void {
  if (value === 0) {
    throw refuse('tempoBpm must be in [20, 400] BPM');
  }
}

function assertAssistantRateDomains(tracks: PlanarTracks, sampleRate: number): void {
  for (const [index, rate] of (tracks.sampleRates ?? []).entries()) {
    if (rate !== null) {
      assertSampleRate(rate, `tracks[${index}].sampleRate`);
    }
  }
  assertSampleRate(sampleRate, 'sampleRate');
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

/**
 * Analyze a set of tracks and suggest a mixer scene.
 *
 * Offline only: the pipeline runs an STFT per track and evaluates every track
 * pair, so it is measured in milliseconds per track and must never be called
 * from an audio callback.
 *
 * The assistant suggests, it does not apply. Nothing is processed and no audio
 * is returned; realizing the suggestion means handing `result.scene` to
 * {@link Mixer.fromJson} as an explicit second step.
 *
 * A degenerate track (empty, holding a NaN or Inf sample, too short for a gated
 * loudness, silent, or without energy in the analysis bands) is excluded rather
 * than refused: its profile reports `usable: false` and an `exclusionReason`
 * naming which, and the call succeeds. This is documented behaviour, not a
 * missing check.
 *
 * With no usable track at all the scene and the `explanation` are empty.
 *
 * @param request - Tracks, analysis sample rate and assistant tunables
 * @returns The suggested scene, per-track profiles, cross-track measurements
 *   and the explanation behind each change
 * @throws TypeError for a wrong-typed or missing field or an unknown key; RangeError for an
 *   out-of-domain rate or tunable
 */
export function mixingAssistantSuggest(request: MixingAssistantSuggestRequest): MixAssistantResult;
export function mixingAssistantSuggest(...args: unknown[]): MixAssistantResult {
  return withEntry('mixingAssistantSuggest', () => {
    const fnName = 'mixingAssistantSuggest';
    const req = requestOnly<MixingAssistantSuggestRequest>(fnName, args);
    // Required, not defaulted: a guessed rate would misread every band edge and lag.
    if (typeof req.sampleRate !== 'number') {
      throw refuseType('sampleRate must be an integer');
    }
    const { left, right, ids, names, sampleRates } = planarTracks(req.tracks, req.sampleRate);
    const params: Record<string, number | boolean> = {};
    for (const key of OPTION_KEYS) {
      const value = req[key];
      if (isAbsent(value)) {
        continue;
      }
      const expectsBoolean = key.startsWith('enable');
      if (expectsBoolean ? typeof value !== 'boolean' : typeof value !== 'number') {
        throw refuseType(`${key} must be a ${expectsBoolean ? 'boolean' : 'number'}`);
      }
      params[key] = value;
    }
    const tracks = { left, right, ids, names, sampleRates };
    assertAssistantTrackLengths(tracks);
    assertAssistantRateDomains(tracks, req.sampleRate);
    assertTempoDomain(req.tempoBpm);
    const call = (): string =>
      getSonareModule().mixingAssistantSuggest(
        left,
        right,
        ids,
        names,
        sampleRates,
        req.sampleRate,
        params,
      );
    // A track at its own rate is resampled in the native reader, which changes its length.
    const json =
      sampleRates === null
        ? withHeapNeed(
            sampleNeed(
              'tracks',
              left.reduce((sum, plane) => sum + plane.length, 0) +
                right.reduce((sum, plane) => sum + (plane?.length ?? 0), 0),
              undefined,
              (n) => 4 * n,
            ),
            call,
          )
        : call();
    const result = parseRecordJson(json, 'mix_assistant_result') as unknown as MixAssistantResult;
    for (const track of result.tracks) {
      if (track.exclusionReason === '') {
        delete track.exclusionReason;
      }
    }
    return result;
  });
}

/**
 * Source-class identifiers the assistant can report, in enum order.
 *
 * The index of a name in this list is the value
 * {@link mixingAssistantSourceClassFromName} resolves it to.
 */
export function mixingAssistantSourceClassNames(): MixSourceClass[] {
  return withEntry('mixingAssistantSourceClassNames', () => {
    return Array.from(getSonareModule().mixingAssistantSourceClassNames()) as MixSourceClass[];
  });
}

/**
 * Resolve a source-class identifier to its index in {@link mixingAssistantSourceClassNames}.
 *
 * @param name - Source-class identifier, e.g. `"kick"`
 * @returns The index
 * @throws TypeError when `name` is not a string; RangeError for an unknown name
 */
export function mixingAssistantSourceClassFromName(name: MixSourceClass): number {
  return withEntry('mixingAssistantSourceClassFromName', () => {
    assertClosedToken(name, mixingAssistantSourceClassNames(), 'name');
    return getSonareModule().mixingAssistantSourceClassFromName(name);
  });
}
