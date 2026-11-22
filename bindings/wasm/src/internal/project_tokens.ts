/**
 * Name tables of the project closed sets: each public token and the ordinal the
 * native layer reads and writes.
 */

import type { ProjectChordQuality, ProjectKeyMode } from '../project_types.js';

/** `SonareProjectTrackKind` ordinals. */
export const TRACK_KINDS = { audio: 0, midi: 1, aux: 2 } as const;
/** Source and clip-source kind ordinals (0 audio, 1 MIDI). */
export const SOURCE_KINDS = { audio: 0, midi: 1 } as const;
/** `SonareProjectOverlapPolicy` ordinals. */
export const OVERLAP_POLICIES = { disallow: 0, allow: 1 } as const;
/** `SonareProjectFadeCurve` ordinals. */
export const FADE_CURVES = { linear: 0, equalPower: 1, exponential: 2, logarithmic: 3 } as const;
/** `SonareProjectLoopMode` ordinals. */
export const LOOP_MODES = { off: 0, loop: 1 } as const;
/** `SonareProjectWarpMode` ordinals. */
export const WARP_MODES = { off: 0, repitch: 1, tempoSync: 2, timeStretch: 3 } as const;
/** Automation lane target-kind ordinals. */
export const AUTOMATION_TARGET_KINDS = { opaque: 0, trackFaderDb: 1, trackPan: 2 } as const;
/** `SonareMarkerKind` ordinals. */
export const MARKER_KINDS = { marker: 0, text: 1, lyric: 2, cuePoint: 3, keySignature: 4 } as const;
/** `SonareExternalStemLayout` ordinals. */
export const STEM_LAYOUTS = { mono: 1, stereo: 2 } as const;
/** Key-annotation mode ordinals (0 unknown, then the seven modes). */
export const KEY_MODES = {
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
export const CHORD_QUALITY_CODES = {
  unknown: 0,
  major: 1,
  minor: 2,
  diminished: 3,
  augmented: 4,
  dominant: 5,
  halfDiminished: 6,
  suspended: 7,
} as const satisfies Record<ProjectChordQuality, number>;
