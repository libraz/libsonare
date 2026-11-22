/**
 * The part of an `AudioContext` a reader takes its clock from.
 * @mirrors AudioContext
 */
export interface AudioClockLike {
  /** Seconds the output device adds. */
  outputLatency?: number;
  /** Not on the platform interface. */
  driftSec?: number;
}

/** A type with no platform interface. */
export interface PlainRecord {
  /** A field. */
  outputLatency?: number;
}
