import type { VocalReason, VocalStateToken } from './types_vocal_edit.js';

/** A full source render and persisted vocal session ready for Project apply. */
export interface ProjectVocalEditApplyRequest {
  /** Clip the edit belongs to. */
  clipId: number;
  /** Take to edit; omit to edit the clip's base source. Zero is refused. */
  takeId?: number;
  /** Id of the source the render was made from. */
  expectedSourceId: number;
  /** Sample rate the source must have, in hertz. */
  expectedSourceSampleRate: number;
  /** Length the source must have, in samples. */
  expectedSourceSampleCount: number;
  /** Canonical source digest as exactly 64 hexadecimal characters. */
  expectedSourceSha256: string;
  /** Clip length the edit was made against, in quarter notes, in (0, 1e12]. */
  expectedClipLengthPpq: number;
  /** Source offset the edit was made against, in quarter notes, in [0, 1e12]. */
  expectedSourceOffsetPpq: number;
  /** Full-source mono render, starting at sample 0 of the source. */
  renderedMono: Float32Array;
  /** Sample rate of `renderedMono`; must equal `expectedSourceSampleRate`. */
  renderedSampleRate: number;
  /** State token of the committed session the render came from. */
  renderToken: VocalStateToken;
  /** State exported from the committed vocal edit session. */
  vocalState: Uint8Array;
}

/** Outcome of {@link Project.applyVocalEdit}. */
export interface ProjectVocalEditApplyResult {
  /** Clip that was edited. */
  clipId: number;
  /** Take that was edited; absent when the base source was. */
  takeId?: number;
  /** Source the edit was made from. */
  originalSourceId: number;
  /** Source that now carries the edited audio. */
  derivedSourceId: number;
  /** Decimal revision of the committed edit (64-bit counters exceed 2^53). */
  committedRevision: string;
  /** Engine profile the edit was produced under. */
  profileId: number;
  /** SHA-256 of the derived source, hexadecimal. */
  derivedSourceSha256: string;
  /** Key under which the vocal state is stored in the project. */
  sidecarKey: string;
}

/** A vocal edit recorded in the project and what is needed to rebuild it. */
export interface ProjectVocalEditDependency {
  /** Clip the edit belongs to. */
  clipId: number;
  /** Take the edit belongs to; absent for the base source. */
  takeId?: number;
  /** Source the edit was made from; 0 when unknown. */
  originalSourceId: number;
  /** Source carrying the edited audio; 0 when unknown. */
  derivedSourceId: number;
  /** Sample rate of the sources, in hertz. */
  sourceSampleRate: number;
  /** Engine profile the edit was produced under. */
  profileId: number;
  /** Length of the sources, in samples. */
  sourceSampleCount: number;
  /** Decimal revision of the committed edit (64-bit counters exceed 2^53). */
  committedRevision: string;
  /** SHA-256 of the original source, hexadecimal. */
  originalSourceSha256: string;
  /** SHA-256 of the derived source, hexadecimal. */
  derivedSourceSha256: string;
  /** True when the original audio is present in the project. */
  originalPcmAvailable: boolean;
  /** True when the derived audio is present in the project. */
  derivedPcmAvailable: boolean;
  /** Why the dependency cannot be used as is; `'none'` when it can. */
  reason: VocalReason;
  /** Key under which the vocal state is stored in the project. */
  sidecarKey: string;
}

/** Original audio supplied to {@link Project.rehydrateVocalEdits}. */
export interface ProjectVocalOriginalSource {
  /** Id of the source the audio is the original of. */
  sourceId: number;
  /** Mono original audio. */
  mono: Float32Array;
  /** Sample rate of `mono` in hertz. */
  sampleRate: number;
}

/** What {@link Project.rehydrateVocalEdits} did with one dependency. */
export type ProjectVocalRehydrateStatus = 'rehydrated' | 'alreadyReady' | 'unresolved';

/** Outcome of rehydrating one vocal edit. */
export interface ProjectVocalRehydrateItem {
  /** Clip the edit belongs to. */
  clipId: number;
  /** Take the edit belongs to; absent for the base source. */
  takeId?: number;
  /** Source carrying the edited audio. */
  derivedSourceId: number;
  /** What happened to the edit. */
  status: ProjectVocalRehydrateStatus;
  /** Why the edit was left unresolved; `'none'` otherwise. */
  reason: VocalReason;
}

/** Options for {@link Project.rehydrateVocalEdits}. */
export interface ProjectVocalRehydrateOptions {
  /** Polled during the call; a truthy return cancels it. */
  cancel?: () => boolean;
}
