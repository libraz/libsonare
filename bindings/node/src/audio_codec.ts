import { refuse, refuseType, refuseUnknownKey, withEntry } from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { isPlainRecord } from './internal/value_guards.js';
import { addon } from './native.js';
import {
  assertAudioChannels,
  assertBoundedInteger,
  assertFloat32Array,
  assertFloatValue,
  assertSampleRate,
  optionsBag,
  requestOnly,
} from './validation.js';

/** Request for {@link readAudioTags}. */
export interface ReadAudioTagsRequest {
  /** Encoded audio file bytes. */
  data: Uint8Array;
}

/** Audio encoder format token. */
export type AudioEncodeFormat = 'wav' | 'flac' | 'mp3' | 'ogg' | 'opus' | 'm4a';

/** Request for {@link encodeAudio}. */
export interface EncodeAudioRequest {
  /** One equal-length planar sample array per channel. */
  channels: Float32Array[];
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Exact lowercase encoder format token. */
  format: AudioEncodeFormat;
  /** Metadata to write. */
  tags?: Readonly<Record<string, string>>;
  /** PCM bit depth for WAV or FLAC. Omit to use the format default. */
  bitDepth?: number;
  /** Lossy encoder quality in [0, 1]. Omit to use the format default. */
  quality?: number;
}

/** Settings of the positional {@link encodeAudio} form. */
export type AudioEncodeOptions = Omit<EncodeAudioRequest, 'channels' | 'sampleRate'>;

function normalizeReadAudioTags(args: readonly unknown[]): ReadAudioTagsRequest {
  let data: unknown;
  const first = args[0];
  const isRawBuffer =
    first instanceof ArrayBuffer ||
    (typeof SharedArrayBuffer !== 'undefined' && first instanceof SharedArrayBuffer);
  if (isPlainRecord(first) && !isRawBuffer) {
    const request = requestOnly<ReadAudioTagsRequest>('readAudioTags', args);
    data = Object.hasOwn(request, 'data') ? request.data : undefined;
  } else {
    if (args.length > 1) {
      throw refuseType('expected readAudioTags(data) or readAudioTags({ data })');
    }
    data = args[0];
  }
  if (data === undefined || data === null) {
    throw refuseType('data is required');
  }
  if (!(data instanceof Uint8Array)) {
    throw refuseType('data must be a Uint8Array');
  }
  return { data };
}

function assertAudioChannelArray(channels: unknown): asserts channels is unknown[] {
  if (!Array.isArray(channels)) {
    throw refuseType('channels must be an array of Float32Array');
  }
}

function assertAudioChannelTypes(channels: unknown[]): asserts channels is Float32Array[] {
  requireListBudget(channels, 'channels');
  for (const [index, channel] of channels.entries()) {
    assertFloat32Array(channel, `channels[${index}]`);
  }
}

function normalizeEncodeAudio(args: readonly unknown[]): EncodeAudioRequest {
  let request: EncodeAudioRequest;
  if (Array.isArray(args[0]) || (args.length > 1 && !isPlainRecord(args[0]))) {
    if (args.length > 3) {
      throw refuseType('expected encodeAudio(channels, sampleRate, options)');
    }
    const channels = args[0] as Float32Array[];
    const sampleRate = args[1];
    const rawOptions = args[2] ?? {};
    if (!isPlainRecord(rawOptions)) {
      throw refuseType('options must be an object');
    }
    const options = optionsBag('encodeAudio', rawOptions);
    for (const key of ['channels', 'sampleRate']) {
      if (Object.hasOwn(options, key)) {
        throw refuseUnknownKey(key, ['bitDepth', 'format', 'quality', 'tags']);
      }
    }
    request = { ...options, channels, sampleRate: sampleRate as number } as EncodeAudioRequest;
  } else {
    request = requestOnly<EncodeAudioRequest>('encodeAudio', args);
  }

  if (!Object.hasOwn(request, 'channels') || request.channels === undefined) {
    throw refuseType('channels is required');
  }
  if (!Object.hasOwn(request, 'sampleRate') || request.sampleRate === undefined) {
    throw refuseType('sampleRate is required');
  }
  if (
    !Object.hasOwn(request, 'format') ||
    request.format === undefined ||
    request.format === null
  ) {
    throw refuseType('format is required');
  }
  assertAudioChannelArray(request.channels);
  if (typeof request.sampleRate !== 'number') {
    throw refuseType('sampleRate must be an integer');
  }
  if (typeof request.format !== 'string') {
    throw refuseType('format must be a string');
  }
  if (request.bitDepth !== undefined && request.bitDepth !== null) {
    if (typeof request.bitDepth !== 'number') {
      throw refuseType('bitDepth must be an integer');
    }
  }
  if (request.quality !== undefined && request.quality !== null) {
    if (typeof request.quality !== 'number') {
      throw refuseType('quality must be a number');
    }
  }
  if (request.tags !== undefined && request.tags !== null) {
    if (!isPlainRecord(request.tags)) {
      throw refuseType('tags must be an object of string values');
    }
    for (const [key, value] of Object.entries(request.tags)) {
      if (typeof value !== 'string') {
        throw refuseType(`tags.${key} must be a string`);
      }
    }
  }

  assertAudioChannelTypes(request.channels);
  assertSampleRate(request.sampleRate);
  assertAudioChannels(request.channels, request.sampleRate);
  if (request.bitDepth !== undefined && request.bitDepth !== null) {
    assertBoundedInteger(request.bitDepth, 'bitDepth', -2147483648, 2147483647);
    if (request.bitDepth === 0) {
      throw refuse('bitDepth must not be 0; omit it for the format default');
    }
  }
  if (request.quality !== undefined && request.quality !== null) {
    assertFloatValue(request.quality, 'quality');
    if (request.quality < 0 || request.quality > 1) {
      throw refuse('quality must be finite and in [0, 1]');
    }
  }
  return request;
}

/**
 * Read normalized metadata without decoding samples.
 *
 * Both the request form and the byte-buffer form pass through the same normalizer.
 */
export function readAudioTags(request: ReadAudioTagsRequest): Record<string, string>;
export function readAudioTags(data: Uint8Array): Record<string, string>;
export function readAudioTags(...args: unknown[]): Record<string, string> {
  return withEntry('readAudioTags', () => {
    const request = normalizeReadAudioTags(args);
    return addon.readAudioTags(request.data) as Record<string, string>;
  });
}

/**
 * Encode planar audio in memory. Both forms require `format`; bit depth and quality use the
 * encoder's defaults when omitted.
 */
export function encodeAudio(request: EncodeAudioRequest): Uint8Array;
export function encodeAudio(
  channels: Float32Array[],
  sampleRate: number,
  options: AudioEncodeOptions,
): Uint8Array;
export function encodeAudio(...args: unknown[]): Uint8Array {
  return withEntry('encodeAudio', () => {
    const request = normalizeEncodeAudio(args);
    return addon.encodeAudio(request.channels, request.sampleRate, request) as Uint8Array;
  });
}
