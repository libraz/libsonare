import type {
  EngineMarker,
  EngineTempoSegment,
  EngineTrackLane,
  RealtimeEngine,
} from '../index.js';
import { capturePunchBound } from '../internal/capture_punch.js';
import { refuse, refuseType, withEntry } from '../internal/entry.js';
import { assignMarkerIds } from '../internal/marker_ids.js';
import type { SonareEngineSyncCaptureMessage } from '../internal/worklet_messages.js';
import type { CaptureOptions, EngineLoop } from '../public_types_engine.js';
import {
  assertFloatValue,
  requestObject,
  requireBoolean,
  secondsToSampleCount,
} from '../validation.js';
import { commitCommand, type EngineCommitContext, requireQueued } from './engine-commit.js';
import { requireInteger, requireIntegerOption } from './guards.js';
import { SonareEngineCommandType } from './protocol.js';

/** The resolved capture setup carried by the `syncCapture` message. */
export type CaptureSyncConfig = Omit<SonareEngineSyncCaptureMessage, 'type'> & {
  /** Punch window in timeline samples; absent when no punch is set. */
  punch?: { startSample: number; endSample: number };
};

/** Refuses a value that is not a plain object, naming the argument. */
export function requireObject(what: string, value: unknown): asserts value is object {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw refuseType(`${what} must be an object`);
  }
}

/**
 * Normalizes capture options into the resolved config carried by the
 * `syncCapture` message, applying defaults and refusing every value either
 * engine would refuse, so both can apply it without failing halfway after the
 * capture buffer was already replaced.
 *
 * @param options Raw capture options.
 * @param defaultNumChannels Plane count to use when `options.numChannels` is unset.
 * @param sampleRate Engine sample rate, for punch bounds given in seconds.
 * @param sampleAtPpq Converts a quarter-note position to a timeline sample.
 * @returns The wire config and the same setup spelled for the offline mirror.
 * @throws TypeError for an unknown key; RangeError if a frame count, plane count or offset is
 *   not an integer, the source is not `'output'` or `'input'`, the monitor gain is not a
 *   finite 32-bit float, or a punch bound is missing, doubly spelled or negative.
 */
export function buildCaptureConfig(
  options: CaptureOptions,
  defaultNumChannels: number,
  sampleRate: number,
  sampleAtPpq: (ppq: number) => number,
): { config: CaptureSyncConfig; offlineOptions: CaptureOptions } {
  requestObject('configureCapture', options, 'options', true);
  const rawSource: unknown = options.source ?? 'output';
  if (typeof rawSource === 'number') {
    throw refuseType("source must be 'output' or 'input', not a number");
  }
  if (rawSource !== 'output' && rawSource !== 'input') {
    throw refuse("capture source must be 'output' or 'input'");
  }
  const source = rawSource;
  let monitor = { enabled: true, gain: 1 };
  if (options.inputMonitor !== undefined && options.inputMonitor !== null) {
    requireObject('inputMonitor', options.inputMonitor);
    const gain = options.inputMonitor.gain ?? 1;
    assertFloatValue(gain, 'inputMonitor.gain');
    monitor = {
      enabled: requireBoolean(options.inputMonitor.enabled, 'inputMonitor.enabled'),
      gain,
    };
  }
  let punch: { startSample: number; endSample: number } | undefined;
  if (options.punch !== undefined && options.punch !== null) {
    requireObject('punch', options.punch);
    const { punch: window } = options;
    punch = {
      startSample: capturePunchBound(
        window.startPpq,
        window.startSample,
        window.startSec,
        ['startPpq', 'startSample', 'startSec'],
        sampleRate,
        sampleAtPpq,
      ),
      endSample: capturePunchBound(
        window.endPpq,
        window.endSample,
        window.endSec,
        ['endPpq', 'endSample', 'endSec'],
        sampleRate,
        sampleAtPpq,
      ),
    };
  }
  const config: CaptureSyncConfig = {
    // capacityFrames has no default; NaN makes the guard refuse an absent value.
    bufferFrames: requireIntegerOption(options.capacityFrames, Number.NaN, 'capacityFrames', 1),
    channels: requireIntegerOption(options.numChannels, defaultNumChannels, 'numChannels', 1),
    source,
    recordOffsetSamples: requireInteger(options.recordOffsetSamples, 0, 'recordOffsetSamples'),
    inputMonitor: monitor,
    ...(punch === undefined ? {} : { punch }),
  };
  return {
    config,
    offlineOptions: {
      capacityFrames: config.bufferFrames,
      numChannels: config.channels,
      source,
      recordOffsetSamples: config.recordOffsetSamples,
      inputMonitor: monitor,
      ...(punch === undefined ? {} : { punch }),
    },
  };
}

/**
 * The transport methods of {@link SonareEngine.transport}. Each queues a
 * sample-accurate command (`renderFrame` omitted: the next block head) and
 * returns nothing; a full command ring throws a `SonareError` with
 * `ErrorCode.OutOfMemory`.
 */
export interface SonareEngineTransportFacade {
  /** Starts playback at `renderFrame`. */
  play(renderFrame?: number): void;
  /** Stops playback at `renderFrame`. */
  stop(renderFrame?: number): void;
  /** Moves the playhead to `ppq` quarter notes at `renderFrame`. */
  seekPpq(ppq: number, renderFrame?: number): void;
  /** Moves the playhead to `timeSec` seconds (non-negative, rounded to a sample) at `renderFrame`. */
  seekSec(timeSec: number, renderFrame?: number): void;
  /** Sets a single finite tempo in (0, 100000] BPM, replacing any tempo map. An invalid value is refused by the offline mirror. */
  setTempo(bpm: number): void;
  /**
   * Installs a tempo map. Each segment needs a finite non-negative `startPpq` and a `bpm` in
   * (0, 100000]; an `endBpm` in the same range ramps within the segment. An empty list clears the map.
   */
  setTempoSegments(segments: readonly EngineTempoSegment[]): void;
  /** Sets the loop region in quarter notes and enables or disables it (`enabled` omitted: enabled). */
  setLoop(loop: EngineLoop): void;
}

/**
 * Collaborator surface the transport facade needs from the owning engine: the
 * realtime node (sample-accurate command transport), the offline engine it
 * mirrors, and the tempo/loop setters.
 */
export interface EngineTransportContext extends EngineCommitContext {
  readonly sampleRate: number;
  realtimeNode: {
    play(renderFrame?: number): boolean;
    stop(renderFrame?: number): boolean;
  };
  offlineEngine: RealtimeEngine;
  setTempo(bpm: number): void;
  setTempoSegments(segments: readonly EngineTempoSegment[]): void;
  setLoop(loop: EngineLoop): void;
}

/**
 * Builds the public transport facade that fans control to both engines. The
 * facade is a field, so each method names its own refusals as
 * `SonareEngine.transport.<method>`.
 */
export function buildTransportFacade(ctx: EngineTransportContext): SonareEngineTransportFacade {
  return {
    play: (renderFrame) =>
      withEntry('SonareEngine.transport.play', () => {
        requireQueued(ctx.realtimeNode.play(renderFrame));
      }),
    stop: (renderFrame) =>
      withEntry('SonareEngine.transport.stop', () => {
        requireQueued(ctx.realtimeNode.stop(renderFrame));
      }),
    seekPpq: (ppq, renderFrame) =>
      withEntry('SonareEngine.transport.seekPpq', () => {
        commitCommand(
          ctx,
          {
            type: SonareEngineCommandType.TransportSeekPpq,
            renderFrame: renderFrame ?? -1,
            argFloat: ppq,
          },
          (offline) => offline.seekPpq(ppq, renderFrame),
        );
      }),
    seekSec: (timeSec, renderFrame) =>
      withEntry('SonareEngine.transport.seekSec', () => {
        // A negative position is refused, never clamped onto the start.
        if (timeSec < 0) {
          throw refuse('timeSec must be non-negative');
        }
        const timelineSample = secondsToSampleCount(timeSec, ctx.sampleRate, 'timeSec');
        commitCommand(
          ctx,
          {
            type: SonareEngineCommandType.TransportSeekSample,
            renderFrame: renderFrame ?? -1,
            argInt: timelineSample,
          },
          (offline) => offline.seekSample(timelineSample, renderFrame),
        );
      }),
    setTempo: (bpm) => withEntry('SonareEngine.transport.setTempo', () => ctx.setTempo(bpm)),
    setTempoSegments: (segments) =>
      withEntry('SonareEngine.transport.setTempoSegments', () => ctx.setTempoSegments(segments)),
    setLoop: (loop) => withEntry('SonareEngine.transport.setLoop', () => ctx.setLoop(loop)),
  };
}

/**
 * Validates and normalizes an append-only mixer-lane declaration.
 *
 * Lane indices are append-only: the new list must start with the already
 * declared lane ids in their current order and may only append new track ids.
 * Returns the lane entries and the resulting ordered id list.
 *
 * @throws if any track id is invalid, ids are duplicated, or the existing lane
 *   order is not preserved.
 */
export function normalizeTrackLanes(
  existing: readonly number[],
  lanes: readonly EngineTrackLane[],
): { entries: readonly EngineTrackLane[]; ids: number[] } {
  const entries = lanes;
  const ids: number[] = [];
  for (const entry of entries) {
    if (!Number.isInteger(entry.trackId) || entry.trackId <= 0) {
      throw refuse(`Invalid track id for mixer lane: ${String(entry.trackId)}`);
    }
    ids.push(entry.trackId);
  }
  if (new Set(ids).size !== ids.length) {
    throw refuse('Duplicate track id in mixer lane list');
  }
  for (let index = 0; index < existing.length; index++) {
    if (ids[index] !== existing[index]) {
      throw refuse(
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
  markers: ReadonlyArray<Omit<EngineMarker, 'id'> & { id?: number }>,
  nextMarkerId: number,
): { resolved: EngineMarker[]; nextMarkerId: number } {
  const seen = new Set<number>();
  for (const marker of markers) {
    // Non-negative as well as finite, which is the domain the native setter
    // enforces; checking only finiteness here hands a negative ppq on to be
    // refused a layer down, under that layer's name for the argument.
    if (!Number.isFinite(marker.ppq) || marker.ppq < 0) {
      throw refuse(`Invalid marker ppq: ${String(marker.ppq)}`);
    }
    if (marker.id !== undefined) {
      if (!Number.isInteger(marker.id) || marker.id <= 0) {
        throw refuse(`Invalid marker id: ${String(marker.id)}`);
      }
      if (seen.has(marker.id)) {
        throw refuse(`Duplicate marker id: ${marker.id}`);
      }
      seen.add(marker.id);
    }
  }
  const assigned = assignMarkerIds(markers, nextMarkerId);
  return {
    resolved: assigned.markers.map((marker) => ({ ...marker, name: marker.name ?? '' })),
    nextMarkerId: assigned.nextId,
  };
}
