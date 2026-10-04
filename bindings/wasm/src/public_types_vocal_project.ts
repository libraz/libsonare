import type { VocalStateToken, VocalUint64 } from './public_types_vocal_edit';

/** A full source render and persisted vocal session ready for Project apply. */
export interface ProjectVocalEditApplyRequest {
  clipId: number;
  /** Zero selects the clip's base source; a non-zero id selects a take. */
  takeId?: number;
  expectedSourceId: number;
  expectedSourceSampleRate: number;
  expectedSourceSampleCount: number;
  /** Canonical source digest as exactly 64 hexadecimal characters. */
  expectedSourceSha256: string;
  expectedClipLengthPpq: number;
  expectedSourceOffsetPpq: number;
  /** Full-source mono render. The v1 Project adapter requires startSample 0. */
  renderedMono: Float32Array;
  renderedSampleRate: number;
  renderedStartSample?: number;
  renderToken: VocalStateToken;
  /** SVE1 state exported from the committed vocal edit session. */
  sve1: Uint8Array;
}

export interface ProjectVocalEditApplyResult {
  clipId: number;
  takeId: number;
  originalSourceId: number;
  derivedSourceId: number;
  committedRevision: VocalUint64;
  profileId: number;
  derivedSourceSha256: string;
  sidecarKey: string;
}

export type ProjectVocalReason = 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7;

export interface ProjectVocalEditDependency {
  clipId: number;
  takeId: number;
  originalSourceId: number;
  derivedSourceId: number;
  sourceSampleRate: number;
  profileId: number;
  sourceSampleCount: number;
  committedRevision: VocalUint64;
  originalSourceSha256: string;
  derivedSourceSha256: string;
  originalPcmAvailable: boolean;
  derivedPcmAvailable: boolean;
  reason: ProjectVocalReason | number;
  sidecarKey: string;
}

export interface ProjectVocalOriginalSource {
  sourceId: number;
  mono: Float32Array;
  sampleRate: number;
}

/** Native status values from SonareProjectVocalRehydrateStatus. */
export type ProjectVocalRehydrateStatus = 0 | 1 | 2;

export interface ProjectVocalRehydrateItem {
  clipId: number;
  takeId: number;
  derivedSourceId: number;
  status: ProjectVocalRehydrateStatus;
  reason: ProjectVocalReason | number;
}
