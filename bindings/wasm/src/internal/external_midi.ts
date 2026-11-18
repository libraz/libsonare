import type { ExternalMidiEvent } from '../public_types_engine.js';

/** The C ABI tags clock and transport bytes with this destination; the facade omits the field. */
const EVERY_PORT_DESTINATION = 0xffffffff;

/** Builds an {@link ExternalMidiEvent}, leaving `destinationId` out for bytes meant for every port. */
export function externalMidiEvent(
  destinationId: number | undefined,
  renderFrame: number,
  bytes: Uint8Array,
): ExternalMidiEvent {
  return destinationId === undefined || destinationId === EVERY_PORT_DESTINATION
    ? { renderFrame, bytes }
    : { destinationId, renderFrame, bytes };
}
