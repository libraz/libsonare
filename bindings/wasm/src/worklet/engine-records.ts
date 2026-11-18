import { externalMidiEvent } from '../internal/external_midi.js';
import type {
  EngineTelemetry,
  EngineTelemetryError,
  EngineTelemetryType,
  ExternalMidiEvent,
} from '../public_types_engine.js';
import { SonareEngineTelemetryError, SonareEngineTelemetryType } from './engine-rings.js';
import type { EngineTelemetryRecord } from './protocol.js';

function lowerCamel(name: string): string {
  return name.charAt(0).toLowerCase() + name.slice(1);
}

/** Converts a wire telemetry record (ordinals, Q8 latency) into the public {@link EngineTelemetry}. */
export function telemetryFromRecord(record: EngineTelemetryRecord): EngineTelemetry {
  const type = SonareEngineTelemetryType[record.type as number];
  const error = SonareEngineTelemetryError[record.error as number];
  if (type === undefined || error === undefined) {
    throw new RangeError(`unknown telemetry ordinal (type ${record.type}, error ${record.error})`);
  }
  return {
    type: lowerCamel(type) as EngineTelemetryType,
    error: lowerCamel(error) as EngineTelemetryError,
    renderFrame: record.renderFrame,
    timelineSample: record.timelineSample,
    audibleTimelineSample: record.audibleTimelineSample,
    graphLatencySamples: record.graphLatencySamplesQ8 / 256,
    value: record.value,
  };
}

/** Normalizes a wire external-MIDI event, whose bytes may arrive as a plain array, into a {@link ExternalMidiEvent}. */
export function externalMidiEventFromWire(event: {
  destinationId?: number;
  renderFrame: number;
  bytes: ArrayLike<number>;
}): ExternalMidiEvent {
  return externalMidiEvent(
    event.destinationId,
    event.renderFrame,
    event.bytes instanceof Uint8Array ? event.bytes : Uint8Array.from(event.bytes),
  );
}
