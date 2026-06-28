import type { EngineCaptureStatus, EngineMarker, EngineTrackLane, RealtimeEngine } from '../index';
import type { SonareEngineSyncCaptureMessage, SonareEngineTransportFacade } from './messages';

/** Capture configuration options accepted by the engine's `configureCapture`. */
export interface CaptureOptions {
  bufferFrames: number;
  channels?: number;
  source?: EngineCaptureStatus['source'];
  recordOffsetSamples?: number;
  inputMonitor?: { enabled: boolean; gain?: number };
}

/**
 * Normalizes capture options into the resolved config carried by the
 * `syncCapture` message, applying integer truncation and defaults.
 *
 * @param options Raw capture options.
 * @param defaultChannels Channel count to use when `options.channels` is unset.
 */
export function buildCaptureConfig(
  options: CaptureOptions,
  defaultChannels: number,
): Omit<SonareEngineSyncCaptureMessage, 'type'> {
  return {
    bufferFrames: Math.trunc(options.bufferFrames),
    channels: Math.trunc(options.channels ?? defaultChannels),
    source: options.source ?? 'output',
    recordOffsetSamples: Math.trunc(options.recordOffsetSamples ?? 0),
    inputMonitor: {
      enabled: Boolean(options.inputMonitor?.enabled),
      gain: options.inputMonitor?.gain ?? 1,
    },
  };
}

/**
 * Collaborator surface the transport facade needs from the owning engine: the
 * realtime node (sample-accurate command transport), the offline engine it
 * mirrors, and the tempo/loop setters plus playing-state bookkeeping.
 */
export interface EngineTransportContext {
  readonly sampleRate: number;
  realtimeNode: {
    play(sampleTime?: number): boolean;
    stop(sampleTime?: number): boolean;
    seekPpq(ppq: number, sampleTime?: number): boolean;
    seekSample(timelineSample: number, sampleTime?: number): boolean;
  };
  offlineEngine: RealtimeEngine;
  setTransportPlaying(playing: boolean): void;
  flushPendingInstrumentSync(): void;
  setTempo(bpm: number): void;
  setTempoSegments(segments: readonly { startPpq: number; bpm: number }[]): void;
  setLoop(startPpq: number, endPpq: number, enabled?: boolean): boolean;
}

/** Builds the public transport facade that fans control to both engines. */
export function buildTransportFacade(ctx: EngineTransportContext): SonareEngineTransportFacade {
  return {
    play: (sampleTime = -1) => {
      const ok = ctx.realtimeNode.play(sampleTime);
      if (ok) {
        ctx.setTransportPlaying(true);
      }
      return ok;
    },
    stop: (sampleTime = -1) => {
      const ok = ctx.realtimeNode.stop(sampleTime);
      if (ok) {
        ctx.setTransportPlaying(false);
        ctx.flushPendingInstrumentSync();
      }
      return ok;
    },
    seekPpq: (ppq, sampleTime = -1) => {
      ctx.offlineEngine.seekPpq(ppq, sampleTime);
      return ctx.realtimeNode.seekPpq(ppq, sampleTime);
    },
    seekSeconds: (seconds, sampleTime = -1) => {
      const timelineSample = Math.max(0, Math.round(seconds * ctx.sampleRate));
      ctx.offlineEngine.seekSample(timelineSample, sampleTime);
      return ctx.realtimeNode.seekSample(timelineSample, sampleTime);
    },
    setTempo: (bpm) => ctx.setTempo(bpm),
    setTempoSegments: (segments) => ctx.setTempoSegments(segments),
    setLoop: (startPpq, endPpq, enabled = true) => ctx.setLoop(startPpq, endPpq, enabled),
  };
}

/**
 * Validates and normalizes an append-only mixer-lane declaration.
 *
 * Lane indices are append-only: the new list must start with the already
 * declared lane ids in their current order and may only append new track ids.
 * Returns the normalized lane entries (numbers coerced to descriptors) and the
 * resulting ordered id list.
 *
 * @throws if any track id is invalid, ids are duplicated, or the existing lane
 *   order is not preserved.
 */
export function normalizeTrackLanes(
  existing: readonly number[],
  lanes: ReadonlyArray<number | EngineTrackLane>,
): { entries: EngineTrackLane[]; ids: number[] } {
  const entries = lanes.map((lane) => (typeof lane === 'number' ? { trackId: lane } : lane));
  const ids: number[] = [];
  for (const entry of entries) {
    if (!Number.isInteger(entry.trackId) || entry.trackId <= 0) {
      throw new Error(`Invalid track id for mixer lane: ${String(entry.trackId)}`);
    }
    ids.push(entry.trackId);
  }
  if (new Set(ids).size !== ids.length) {
    throw new Error('Duplicate track id in mixer lane list');
  }
  for (let index = 0; index < existing.length; index++) {
    if (ids[index] !== existing[index]) {
      throw new Error(
        'Mixer lanes are append-only: keep existing lanes in order and only append new track ids',
      );
    }
  }
  return { entries, ids };
}

/**
 * Resolves a marker set, assigning fresh ids to entries without one and
 * validating explicit ids (positive, unique).
 *
 * @param markers The marker list to resolve.
 * @param nextMarkerId The id counter to draw fresh ids from.
 * @returns The resolved markers and the advanced id counter.
 * @throws on a non-finite ppq, an invalid id, or a duplicate id.
 */
export function resolveMarkerSet(
  markers: ReadonlyArray<{ ppq: number; name?: string; id?: number }>,
  nextMarkerId: number,
): { resolved: EngineMarker[]; nextMarkerId: number } {
  const resolved: EngineMarker[] = [];
  const seen = new Set<number>();
  let counter = nextMarkerId;
  for (const marker of markers) {
    if (!Number.isFinite(marker.ppq)) {
      throw new Error(`Invalid marker ppq: ${String(marker.ppq)}`);
    }
    if (marker.id !== undefined) {
      if (!Number.isInteger(marker.id) || marker.id <= 0) {
        throw new Error(`Invalid marker id: ${String(marker.id)}`);
      }
      if (seen.has(marker.id)) {
        throw new Error(`Duplicate marker id: ${marker.id}`);
      }
    }
    const id = marker.id ?? counter++;
    seen.add(id);
    if (id >= counter) {
      counter = id + 1;
    }
    resolved.push({ id, ppq: marker.ppq, name: marker.name ?? '' });
  }
  return { resolved, nextMarkerId: counter };
}
