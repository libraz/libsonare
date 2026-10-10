import { getSonareModule } from './module_state.js';
import type { MixAssistantOptions, MixAssistantResult, MixAssistantTrack } from './public_types.js';
import { assertSampleRate, assertString, requestObject } from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** Inputs for the {@link suggestMixScene} / {@link suggestMixSceneJson} facades. */
export interface SuggestMixSceneRequest {
  /** Tracks to mix, in the order their profiles are reported. */
  tracks: MixAssistantTrack[];
  /**
   * Shared sample rate in Hz for every track. Required: every band edge,
   * high-pass corner, sibilance band and alignment lag is derived from it, so a
   * guessed rate would silently misread 44.1 kHz material by 8.8% and report
   * `channelDelaySamples` and `durationSec` wrong with it.
   */
  sampleRate: number;
  /** Assistant tunables; every field falls back to the core default. */
  options?: MixAssistantOptions;
}

/** The four parallel arrays the embind entry points take. */
interface PlanarTracks {
  left: Float32Array[];
  right: (Float32Array | null)[];
  ids: string[];
  names: (string | null)[];
}

/**
 * Splits the request's track list into the planar per-track arrays the binding
 * takes, rejecting the shapes that would otherwise reach the analysis as a
 * missing buffer or a nameless strip.
 */
function assertTrackBuffer(fnName: string, value: unknown, argName: string): void {
  if (!(value instanceof Float32Array)) {
    throw new TypeError(`${fnName}: ${argName} must be a Float32Array`);
  }
}

function planarTracks(fnName: string, tracks: MixAssistantTrack[]): PlanarTracks {
  if (!Array.isArray(tracks)) {
    throw new TypeError(`${fnName}: tracks must be an array`);
  }
  const left: Float32Array[] = [];
  const right: (Float32Array | null)[] = [];
  const ids: string[] = [];
  const names: (string | null)[] = [];
  for (let index = 0; index < tracks.length; index++) {
    const track = tracks[index];
    if (track === null || typeof track !== 'object') {
      throw new TypeError(`${fnName}: tracks[${index}] must be an object`);
    }
    if (typeof track.id !== 'string' || track.id.length === 0) {
      throw new TypeError(`${fnName}: tracks[${index}].id must be a non-empty string`);
    }
    // An empty or non-finite track is not refused: the core reports it as an excluded track so a
    // batch needs no per-track error handling.
    assertTrackBuffer(fnName, track.left, `tracks[${index}].left`);
    if (track.right !== undefined) {
      assertTrackBuffer(fnName, track.right, `tracks[${index}].right`);
    }
    left.push(track.left);
    right.push(track.right ?? null);
    ids.push(track.id);
    names.push(track.name ?? null);
  }
  return { left, right, ids, names };
}

function suggestJson(fnName: string, request: SuggestMixSceneRequest, sceneOnly: boolean): string {
  // Required, not defaulted: this surface used to invent 48000, which read
  // 44.1 kHz material 8.8% off across every band edge and every lag without
  // saying so. Node and Python both demand it, and a request ported from either
  // must not change behaviour by arriving here.
  const sampleRate = request.sampleRate;
  if (typeof sampleRate !== 'number') {
    throw new TypeError(`${fnName}: sampleRate must be a number`);
  }
  assertSampleRate(fnName, sampleRate);
  const { left, right, ids, names } = planarTracks(fnName, request.tracks);
  const params = (request.options ?? {}) as Record<string, number | boolean>;
  const module = requireModule();
  return sceneOnly
    ? module.mixingAssistantSuggestSceneJson(left, right, ids, names, sampleRate, params)
    : module.mixingAssistantSuggest(left, right, ids, names, sampleRate, params);
}

/**
 * Analyze a set of tracks and suggest a mixer scene.
 *
 * Offline only: the pipeline runs an STFT per track and evaluates every track
 * pair, so it is measured in milliseconds per track and must never be called
 * from an audio callback.
 *
 * The assistant suggests, it does not apply. Nothing is processed and no audio
 * is returned; realizing the suggestion means handing the scene to
 * {@link Mixer.fromSceneJson} as an explicit second step, for which
 * {@link suggestMixSceneJson} returns the scene already serialized.
 *
 * Degenerate input is not an error: no tracks, all-silent tracks or tracks too
 * short to measure yield an empty scene and an empty `explanation`.
 *
 * @param request - Tracks, shared sample rate and assistant options
 * @returns The suggested scene, per-track profiles, cross-track measurements
 *   and the explanation behind each change
 */
export function suggestMixScene(request: SuggestMixSceneRequest): MixAssistantResult {
  requestObject('suggestMixScene', request, 'request', true);
  return JSON.parse(suggestJson('suggestMixScene', request, false)) as MixAssistantResult;
}

/**
 * Suggest a mixer scene and return only the scene, as JSON.
 *
 * The same analysis as {@link suggestMixScene}, serialized in the schema
 * {@link Mixer.fromSceneJson} reads, so a caller that only wants to apply the
 * suggestion neither digs the scene out of the fuller result nor re-serializes
 * it.
 *
 * @param request - Tracks, shared sample rate and assistant options
 * @returns Scene JSON string
 */
export function suggestMixSceneJson(request: SuggestMixSceneRequest): string {
  requestObject('suggestMixSceneJson', request, 'request', true);
  return suggestJson('suggestMixSceneJson', request, true);
}

/**
 * Source-class identifiers the assistant can report, in enum order.
 *
 * The index of a name in this list is the value
 * {@link mixSourceClassFromName} resolves it to.
 */
export function mixSourceClassNames(): string[] {
  return Array.from(requireModule().mixingAssistantSourceClassNames());
}

/**
 * Resolve a source-class identifier to its index in {@link mixSourceClassNames}.
 *
 * @param name - Source-class identifier, e.g. `"kick"`
 * @returns The index, or -1 when the name is unknown
 */
export function mixSourceClassFromName(name: string): number {
  assertString('mixSourceClassFromName', name, 'name');
  return requireModule().mixingAssistantSourceClassFromName(name);
}
