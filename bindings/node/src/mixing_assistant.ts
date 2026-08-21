import { addon } from './native.js';
import type {
  MixAssistantOptions,
  MixAssistantResult,
  MixAssistantTrack,
  SuggestMixSceneRequest,
} from './types.js';
import { assertSampleRate } from './validation.js';

/**
 * Assistant tunables, in the order the native config declares them. Only keys
 * the caller actually set are forwarded, so an omitted field keeps the native
 * default instead of this file having to restate it.
 */
const OPTION_KEYS = [
  'targetTrackLufs',
  'suggestionStrength',
  'eqMaxCutDb',
  'mixBusHeadroomDbtp',
  'enableStructure',
  'enableGain',
  'enableBalance',
  'enableEq',
  'enableDynamics',
  'enableImage',
  'enableHighPass',
  'nFft',
  'hopLength',
] as const satisfies ReadonlyArray<keyof MixAssistantOptions>;

/** Planar parallel arrays in the shape the addon entry points take. */
interface NativeTrackArrays {
  left: Float32Array[];
  right: (Float32Array | null)[] | null;
  ids: string[];
  names: (string | null)[] | null;
}

function normalizeTracks(fnName: string, tracks: MixAssistantTrack[]): NativeTrackArrays {
  if (!Array.isArray(tracks)) {
    throw new TypeError(`${fnName}: tracks must be an array`);
  }
  const left: Float32Array[] = [];
  const right: (Float32Array | null)[] = [];
  const ids: string[] = [];
  const names: (string | null)[] = [];
  const seen = new Set<string>();
  let anyRight = false;
  let anyName = false;

  tracks.forEach((track, index) => {
    const prefix = `tracks[${index}]`;
    if (track === null || typeof track !== 'object') {
      throw new TypeError(`${fnName}: ${prefix} must be an object`);
    }
    if (typeof track.id !== 'string' || track.id === '') {
      throw new TypeError(`${fnName}: ${prefix}.id must be a non-empty string`);
    }
    if (seen.has(track.id)) {
      throw new RangeError(`${fnName}: duplicate track id '${track.id}'`);
    }
    seen.add(track.id);
    if (!(track.left instanceof Float32Array)) {
      throw new TypeError(`${fnName}: ${prefix}.left must be a Float32Array`);
    }
    if (track.right !== undefined && !(track.right instanceof Float32Array)) {
      throw new TypeError(`${fnName}: ${prefix}.right must be a Float32Array when present`);
    }
    if (track.right !== undefined && track.right.length !== track.left.length) {
      throw new RangeError(`${fnName}: ${prefix}.left and ${prefix}.right must have equal lengths`);
    }
    if (track.name !== undefined && typeof track.name !== 'string') {
      throw new TypeError(`${fnName}: ${prefix}.name must be a string when present`);
    }
    left.push(track.left);
    right.push(track.right ?? null);
    ids.push(track.id);
    names.push(track.name ?? null);
    anyRight ||= track.right !== undefined;
    anyName ||= track.name !== undefined;
  });

  // An all-mono set and a set with no names pass NULL rather than an array of
  // nulls, which is what the native side treats as "not supplied at all".
  return { left, right: anyRight ? right : null, ids, names: anyName ? names : null };
}

function normalizeOptions(options: MixAssistantOptions = {}): Record<string, number | boolean> {
  const params: Record<string, number | boolean> = {};
  for (const key of OPTION_KEYS) {
    const value = options[key];
    if (value !== undefined) {
      params[key] = value;
    }
  }
  return params;
}

function suggest(fnName: string, request: SuggestMixSceneRequest, sceneOnly: boolean): string {
  if (request === null || typeof request !== 'object') {
    throw new TypeError(`${fnName}: expected a request object`);
  }
  assertSampleRate(fnName, request.sampleRate);
  const tracks = normalizeTracks(fnName, request.tracks);
  const params = normalizeOptions(request.options);
  const entry = sceneOnly ? addon.mixingAssistantSuggestSceneJson : addon.mixingAssistantSuggest;
  return entry(tracks.left, tracks.right, tracks.ids, tracks.names, request.sampleRate, params);
}

/**
 * Analyse a set of tracks and return a suggested mixer scene, the measurements
 * behind it and a human-readable explanation.
 *
 * The assistant only ever returns parameters: it does not process audio and it
 * does not apply anything. Realising the suggestion is a separate explicit step
 * through {@link Mixer.fromSceneJson} — use {@link suggestMixSceneJson} to get
 * the scene as the JSON text that entry point reads.
 *
 * Offline / control thread only: this runs an STFT per track and evaluates
 * every track pair. Tracks may differ in length.
 */
export function suggestMixScene(request: SuggestMixSceneRequest): MixAssistantResult {
  return JSON.parse(suggest('suggestMixScene', request, false)) as MixAssistantResult;
}

/**
 * As {@link suggestMixScene}, but returns only the suggested scene, as the JSON
 * text {@link Mixer.fromSceneJson} reads. Provided so a caller that wants to
 * apply a suggestion does not have to dig the scene out of the fuller result
 * document and re-serialise it.
 */
export function suggestMixSceneJson(request: SuggestMixSceneRequest): string {
  return suggest('suggestMixSceneJson', request, true);
}

/** The source-class identifiers the assistant can report. */
export function mixSourceClassNames(): string[] {
  return addon.mixingAssistantSourceClassNames();
}

/** Resolve a source-class identifier to its ordinal, or `-1` when unknown. */
export function mixSourceClassFromName(name: string): number {
  return addon.mixingAssistantSourceClassFromName(name);
}
