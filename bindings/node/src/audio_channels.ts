import { addon } from './native.js';
import type { ChannelLayout } from './types_engine.js';

/** Result of {@link decodeChannels}. */
export interface DecodedChannels {
  /** Sample rate of the decoded audio, in Hz. */
  sampleRate: number;
  /** One plane per source channel, in the source's own channel order. All the same length. */
  channels: Float32Array[];
}

/** Options for {@link downmix}. */
export interface DownmixOptions {
  /**
   * Scale the fold so the worst case (every input at full scale, in phase)
   * cannot exceed unity. The scale is a peak divisor, not a loudness match, so
   * typical material comes out a few dB quieter. Default `false`.
   */
  normalize?: boolean;
  /** Fold the LFE plane into the front pair at -3 dB instead of dropping it. Default `false`. */
  includeLfe?: boolean;
}

/**
 * Decode audio bytes once and keep every source channel.
 *
 * {@link Audio.fromMemory} folds a multi-channel source to mono as it decodes;
 * this returns the planes instead. Same format set, size ceiling and
 * decoded-buffer contract as `Audio.fromMemory`. A 5.1 source arrives as
 * `L R C LFE Ls Rs`. Folding the planes with {@link downmix} is not normalised
 * and can exceed full scale.
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
 * The fold is not normalised: the coefficients sum, so a source with several
 * full-scale planes can leave the output past full scale (six planes of 1.0
 * fold to 2.414 on a stereo target). Pass `normalize: true` to scale the output
 * so the worst case stays at or below full scale, or leave headroom before
 * calling.
 *
 * @param channels - One `Float32Array` per channel, all of the same length.
 * @param targetLayout - `0` mono, `1` stereo, `2` 5.1, `3` 7.1.
 * @param options - `normalize` scales the fold so the worst case (every input
 *   at full scale, in phase) cannot exceed unity; `includeLfe` folds the LFE
 *   plane in at -3 dB instead of dropping it. Both default to `false`.
 * @throws SonareError with `codeName: 'InvalidParameter'` for an upmix, an
 *   unknown layout, or no channels; `RangeError` when the channels differ in length;
 *   `TypeError` for an `options` that is not an object or a non-boolean option.
 * @returns One `Float32Array` per channel of the target layout.
 */
export function downmix(
  channels: Float32Array[],
  targetLayout: ChannelLayout,
  options?: DownmixOptions,
): Float32Array[] {
  return addon.downmix(channels, targetLayout, options);
}
