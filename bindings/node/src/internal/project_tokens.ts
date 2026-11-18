/**
 * Name tables of the project closed sets and the two-way mapping between a public
 * token and the ordinal the native layer reads and writes.
 */

import { ErrorCode, SonareError } from '../errors.js';
import type {
  ProjectChordQuality,
  ProjectFadeCurve,
  ProjectKeyMode,
  ProjectLoopMode,
  ProjectMarkerKind,
  ProjectOverlapPolicy,
  ProjectSourceKind,
  ProjectStemLayout,
  ProjectTrackKind,
  ProjectWarpMode,
} from '../types_project.js';
import { assertClosedToken } from '../validation.js';

/** `SonareProjectTrackKind` ordinals. */
export const TRACK_KIND = { audio: 0, midi: 1, aux: 2 } as const satisfies Record<
  ProjectTrackKind,
  number
>;

/** Source and clip-source kind ordinals (0 audio, 1 MIDI). */
export const SOURCE_KIND = { audio: 0, midi: 1 } as const satisfies Record<
  ProjectSourceKind,
  number
>;

/** `SonareProjectOverlapPolicy` ordinals. */
export const OVERLAP_POLICY = { disallow: 0, allow: 1 } as const satisfies Record<
  ProjectOverlapPolicy,
  number
>;

/** `SonareProjectFadeCurve` ordinals. */
export const FADE_CURVE = {
  linear: 0,
  equalPower: 1,
  exponential: 2,
  logarithmic: 3,
} as const satisfies Record<ProjectFadeCurve, number>;

/** `SonareProjectLoopMode` ordinals. */
export const LOOP_MODE = { off: 0, loop: 1 } as const satisfies Record<ProjectLoopMode, number>;

/** `SonareProjectWarpMode` ordinals. */
export const WARP_MODE = {
  off: 0,
  repitch: 1,
  tempoSync: 2,
  timeStretch: 3,
} as const satisfies Record<ProjectWarpMode, number>;

/** `SonareAutomationTargetKind` ordinals. */
export const AUTOMATION_TARGET_KIND = { opaque: 0, trackFaderDb: 1, trackPan: 2 } as const;

/** `SonareExternalStemLayout` ordinals. */
export const STEM_LAYOUT = { mono: 1, stereo: 2 } as const satisfies Record<
  ProjectStemLayout,
  number
>;

/** `SonareMarkerKind` ordinals. */
export const MARKER_KIND = {
  marker: 0,
  text: 1,
  lyric: 2,
  cuePoint: 3,
  keySignature: 4,
} as const satisfies Record<ProjectMarkerKind, number>;

/** Key-annotation mode ordinals (0 unknown, then the seven modes). */
export const KEY_MODE = {
  unknown: 0,
  major: 1,
  minor: 2,
  dorian: 3,
  phrygian: 4,
  lydian: 5,
  mixolydian: 6,
  locrian: 7,
} as const satisfies Record<ProjectKeyMode, number>;

/** Chord-annotation quality ordinals (0 unknown, then the seven qualities). */
export const CHORD_QUALITY = {
  unknown: 0,
  major: 1,
  minor: 2,
  diminished: 3,
  augmented: 4,
  dominant: 5,
  halfDiminished: 6,
  suspended: 7,
} as const satisfies Record<ProjectChordQuality, number>;

/**
 * The ordinal of a token. A value that is not a string is a `TypeError` and an
 * unknown name a `RangeError`, so a number can never stand in for a name.
 */
export function tokenOrdinal<T extends string>(
  fnName: string,
  value: unknown,
  table: Readonly<Record<T, number>>,
  argName: string,
): number {
  assertClosedToken(fnName, value, Object.keys(table) as T[], argName);
  return table[value];
}

/** The token of an ordinal the native layer returned. */
export function tokenName<T extends string>(
  table: Readonly<Record<T, number>>,
  ordinal: number,
  what: string,
): T {
  const name = (Object.keys(table) as T[]).find((key) => table[key] === ordinal);
  if (name === undefined) {
    throw new SonareError(ErrorCode.Unknown, `unknown ${what} ordinal ${ordinal} from the project`);
  }
  return name;
}
