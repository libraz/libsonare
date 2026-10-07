import { addon } from './native.js';
import type { ChannelLayout } from './types_engine.js';

/** Result of {@link decodeChannels}. */
export interface DecodedChannels {
  /** Sample rate of the decoded audio, in Hz. */
  sampleRate: number;
  /** One plane per source channel, in the source's own channel order. All the same length. */
  channels: Float32Array[];
}

/**
 * Decode audio bytes once and keep every source channel.
 *
 * {@link Audio.fromMemory} folds a multi-channel source to mono as it decodes;
 * this returns the planes instead. Same format set, size ceiling and
 * decoded-buffer contract as `Audio.fromMemory`. A 5.1 source arrives as
 * `L R C LFE Ls Rs`.
 *
 * @param data - Encoded audio bytes such as WAV or MP3.
 * @throws SonareError with `codeName: 'DecodeFailed'` for an empty decode or a
 *   non-finite sample, `'InvalidFormat'` for a declared rate outside the
 *   supported range, and `'InvalidParameter'` for empty input.
 *
 * @example
 * ```ts
 * const { sampleRate, channels } = decodeChannels(bytes);
 * const stereo = channels.length > 2 ? downmix(channels, 1) : channels;
 * ```
 */
export function decodeChannels(data: Buffer | Uint8Array): DecodedChannels {
  return addon.decodeChannels(data);
}

/**
 * Downmix channels to a narrower layout with the ITU-R BS.775 rule.
 *
 * Center and surround enter the front pair at -3 dB and the LFE plane is
 * dropped; a stereo, 5.1 or 7.1 source folds to mono through the same matrix
 * the decoders use. The source layout is the one the channel count names
 * (1, 2, 6 or 8). Supported targets narrow the bed (7.1 to 5.1, 5.1 or 7.1 to
 * stereo, anything modelled to mono) or copy it. A channel count outside
 * 1/2/6/8 folds to mono (`0`) as the unweighted mean of its planes, as the
 * decoders do, and has no other target.
 *
 * @param channels - One `Float32Array` per channel, all of the same length.
 * @param targetLayout - `0` mono, `1` stereo, `2` 5.1, `3` 7.1.
 * @throws SonareError with `codeName: 'InvalidParameter'` for an upmix, an
 *   unknown layout, or no channels; `RangeError` when the channels differ in length.
 * @returns One `Float32Array` per channel of the target layout.
 */
export function downmix(channels: Float32Array[], targetLayout: ChannelLayout): Float32Array[] {
  return addon.downmix(channels, targetLayout);
}
