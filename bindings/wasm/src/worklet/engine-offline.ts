import type {
  EngineCaptureStatus,
  EngineMarker,
  EngineTrackLane,
  RealtimeEngine,
} from '../index.js';
import { commitCommand, type EngineCommitContext } from './engine-commit.js';
import { requireChannelCount, requireInteger, requireIntegerOption } from './guards.js';
import type { SonareEngineSyncCaptureMessage, SonareEngineTransportFacade } from './messages.js';
import { SonareEngineCommandType } from './protocol.js';

/** Capture configuration options accepted by the engine's `configureCapture`. */
export interface CaptureOptions {
  bufferFrames: number;
  channels?: number;
  source?: EngineCaptureStatus['source'];
  recordOffsetSamples?: number;
  inputMonitor?: { enabled: boolean; gain?: number };
}

const FLOAT32_MAX = 3.4028234663852886e38;

/**
 * Normalizes capture options into the resolved config carried by the
 * `syncCapture` message, applying defaults and refusing every value either
 * engine would refuse, so both can apply it without failing halfway after the
 * capture buffer was already replaced.
 *
 * @param options Raw capture options.
 * @param defaultChannels Channel count to use when `options.channels` is unset.
 * @throws RangeError if a frame count, channel count or offset is not an
 *   integer, the source is not 'output' or 'input', or the monitor gain is not
 *   a finite 32-bit float.
 */
export function buildCaptureConfig(
  options: CaptureOptions,
  defaultChannels: number,
): Omit<SonareEngineSyncCaptureMessage, 'type'> {
  // The native reader also takes the ordinals 0 and 1; they resolve to the names here.
  const rawSource: unknown = options.source ?? 'output';
  const source = rawSource === 0 ? 'output' : rawSource === 1 ? 'input' : rawSource;
  if (source !== 'output' && source !== 'input') {
    throw new RangeError("capture source must be 'output' or 'input' (or ordinal 0 or 1)");
  }
  const gain = options.inputMonitor?.gain ?? 1;
  if (typeof gain !== 'number' || !Number.isFinite(gain) || Math.abs(gain) > FLOAT32_MAX) {
    throw new RangeError('inputMonitor.gain must be a finite number within the 32-bit float range');
  }
  return {
    // bufferFrames has no default; NaN makes the guard refuse an absent value.
    bufferFrames: requireIntegerOption(options.bufferFrames, Number.NaN, 'bufferFrames', 1),
    channels: requireChannelCount(options.channels, defaultChannels),
    source,
    recordOffsetSamples: requireInteger(options.recordOffsetSamples, 0, 'recordOffsetSamples'),
    inputMonitor: {
      enabled: Boolean(options.inputMonitor?.enabled),
      gain,
    },
  };
}

/**
 * Collaborator surface the transport facade needs from the owning engine: the
 * realtime node (sample-accurate command transport), the offline engine it
 * mirrors, and the tempo/loop setters plus playing-state bookkeeping.
 */
export interface EngineTransportContext extends EngineCommitContext {
  readonly sampleRate: number;
  realtimeNode: {
    play(sampleTime?: number): boolean;
    stop(sampleTime?: number): boolean;
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
    play: (sampleTime) => {
      const ok = ctx.realtimeNode.play(sampleTime);
      if (ok) {
        ctx.setTransportPlaying(true);
      }
      return ok;
    },
    stop: (sampleTime) => {
      const ok = ctx.realtimeNode.stop(sampleTime);
      if (ok) {
        ctx.setTransportPlaying(false);
        ctx.flushPendingInstrumentSync();
      }
      return ok;
    },
    seekPpq: (ppq, sampleTime) =>
      commitCommand(
        ctx,
        {
          type: SonareEngineCommandType.TransportSeekPpq,
          sampleTime: sampleTime ?? -1,
          argFloat: ppq,
        },
        (offline) => offline.seekPpq(ppq, sampleTime),
      ),
    seekSeconds: (seconds, sampleTime) => {
      const timelineSample = Math.max(0, Math.round(seconds * ctx.sampleRate));
      return commitCommand(
        ctx,
        {
          type: SonareEngineCommandType.TransportSeekSample,
          sampleTime: sampleTime ?? -1,
          argInt: timelineSample,
        },
        (offline) => offline.seekSample(timelineSample, sampleTime),
      );
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
      throw new RangeError(`Invalid track id for mixer lane: ${String(entry.trackId)}`);
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
    // Non-negative as well as finite, which is the domain the native setter
    // enforces; checking only finiteness here hands a negative ppq on to be
    // refused a layer down, under that layer's name for the argument.
    if (!Number.isFinite(marker.ppq) || marker.ppq < 0) {
      throw new RangeError(`Invalid marker ppq: ${String(marker.ppq)}`);
    }
    if (marker.id !== undefined) {
      if (!Number.isInteger(marker.id) || marker.id <= 0) {
        throw new RangeError(`Invalid marker id: ${String(marker.id)}`);
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
