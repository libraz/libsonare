import type { VocalReason, VocalStateToken } from './public_types_vocal_edit.js';

/** A full source render and persisted vocal session ready for Project apply. */
export interface ProjectVocalEditApplyRequest {
  /** The clip whose audio the edit replaces. */
  clipId: number;
  /** A take of the clip to edit; omit to edit the clip's base source. Zero is refused. */
  takeId?: number;
  /** The source the edit was authored against; a mismatch is refused. */
  expectedSourceId: number;
  /** Sample rate of that source in hertz. */
  expectedSourceSampleRate: number;
  /** Length of that source in samples. */
  expectedSourceSampleCount: number;
  /** Canonical source digest as exactly 64 hexadecimal characters. */
  expectedSourceSha256: string;
  /** Clip length the edit was made against, in quarter notes, in (0, 1e12]. */
  expectedClipLengthPpq: number;
  /** Source offset the edit was made against, in quarter notes, in [0, 1e12]. */
  expectedSourceOffsetPpq: number;
  /** Full-source mono render, starting at source sample 0. */
  renderedMono: Float32Array;
  /** Sample rate of `renderedMono` in hertz; it must equal `expectedSourceSampleRate`. */
  renderedSampleRate: number;
  /** Token of the committed session state the render came from. */
  renderToken: VocalStateToken;
  /** State exported from the committed vocal edit session with `exportState()`. */
  vocalState: Uint8Array;
}

/** The outcome of applying a vocal edit to a Project. */
export interface ProjectVocalEditApplyResult {
  /** The edited clip. */
  clipId: number;
  /** The edited take; absent when the clip's base source was edited. */
  takeId?: number;
  /** Source id of the audio before the edit. */
  originalSourceId: number;
  /** Source id of the edited audio. */
  derivedSourceId: number;
  /** Committed revision of the vocal session, a decimal string (64-bit counter). */
  committedRevision: string;
  /** Identifier of the rendering profile the edit used. */
  profileId: number;
  /** SHA-256 of the edited audio as 64 lowercase hex characters. */
  derivedSourceSha256: string;
  /** Key under which the Project stores the vocal state. */
  sidecarKey: string;
}

/** One vocal edit a Project depends on, with what is needed to restore its audio. */
export interface ProjectVocalEditDependency {
  /** The edited clip. */
  clipId: number;
  /** The edited take; absent when the clip's base source was edited. */
  takeId?: number;
  /** Source id of the audio before the edit. */
  originalSourceId: number;
  /** Source id of the edited audio. */
  derivedSourceId: number;
  /** Sample rate of the sources in hertz. */
  sourceSampleRate: number;
  /** Identifier of the rendering profile the edit used. */
  profileId: number;
  /** Length of the sources in samples. */
  sourceSampleCount: number;
  /** Committed revision of the vocal session, a decimal string (64-bit counter). */
  committedRevision: string;
  /** SHA-256 of the original audio as 64 lowercase hex characters. */
  originalSourceSha256: string;
  /** SHA-256 of the edited audio as 64 lowercase hex characters. */
  derivedSourceSha256: string;
  /** Whether the original PCM is currently loaded. */
  originalPcmAvailable: boolean;
  /** Whether the edited PCM is currently loaded. */
  derivedPcmAvailable: boolean;
  /** Why the edit cannot be used as is, or `'none'`. */
  reason: VocalReason;
  /** Key under which the Project stores the vocal state. */
  sidecarKey: string;
}

/** Original PCM supplied to rehydrate an edited source. */
export interface ProjectVocalOriginalSource {
  /** The original source this PCM belongs to. */
  sourceId: number;
  /** Mono PCM of the original. */
  mono: Float32Array;
  /** Sample rate of `mono` in hertz. */
  sampleRate: number;
}

/** Outcome of rehydrating one edited source. */
export type ProjectVocalRehydrateStatus = 'rehydrated' | 'alreadyReady' | 'unresolved';

/** Per-edit outcome of a rehydrate call. */
export interface ProjectVocalRehydrateItem {
  /** The edited clip. */
  clipId: number;
  /** The edited take; absent when the clip's base source was edited. */
  takeId?: number;
  /** Source id of the edited audio. */
  derivedSourceId: number;
  /** What the call did for this edit. */
  status: ProjectVocalRehydrateStatus;
  /** Why the edit stayed unresolved, or `'none'`. */
  reason: VocalReason;
}

/** Options of `Project.rehydrateVocalEdits`. */
export interface ProjectVocalRehydrateOptions {
  /** Polled during the call; returning `true` cancels it. */
  cancel?: () => boolean;
}
