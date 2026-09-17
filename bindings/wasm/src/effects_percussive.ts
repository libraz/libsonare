/**
 * Percussive event editing: locating struck sounds in audio and rendering an
 * edited set of them back.
 */

import { getSonareModule } from './module_state';
import type { PercussiveEvent, PercussiveEventInput } from './public_types';
import type { ValidateOptions } from './validation';
import { assertPercussiveSeparation, assertSampleRate, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

/**
 * The separation a percussive event's signal is lifted out with. Extraction
 * measures events against it and rendering has to repeat it, so both calls take
 * the same four fields and a render must be given what the extraction was.
 */
export interface PercussiveSeparationOptions {
  /**
   * FFT size and hop the separation and the onset detector share. They cannot be
   * set apart: an event measured on one framing and lifted out on another is not
   * the same signal. Default 2048 and 512. The pair must overlap-add — `nFft`
   * even and at least 2, `hopLength` no more than half of it — because the
   * separation inverts an STFT. A negative value is rejected on its own, before
   * the zero-is-default rule could swallow it.
   */
  nFft?: number;
  /** Hop length in samples. Default 512. */
  hopLength?: number;
  /**
   * Median filter lengths the separation runs, along time and along frequency. A
   * longer harmonic kernel calls more of a sustained sound harmonic. Default 31;
   * any other value must be odd and positive, so an even one is rejected rather
   * than rounded. 1 is legal and degenerate rather than an error: a length-1
   * median is the identity, so both components come back as the source.
   */
  hpssKernelHarmonic?: number;
  /** Vertical median filter length, under the same rule. Default 31. */
  hpssKernelPercussive?: number;
}

/** Canonical request form for {@link extractPercussiveEvents}. */
export interface ExtractPercussiveEventsRequest
  extends PercussiveSeparationOptions,
    ValidateOptions {
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `maxEventMs` is converted to samples with this
   * rate, so a wrong/omitted value caps the spans differently.
   */
  sampleRate: number;
  /**
   * Minimum frames between consecutive onsets. Default 1, and a whole number:
   * 0 is how the default is spelled, so a fractional wait is refused rather
   * than truncated onto it. Negative is rejected.
   */
  onsetWait?: number;
  /**
   * Offset added to the detector's adaptive threshold; raising it finds fewer,
   * stronger hits and lowering it finds more. Default 0.06, so exactly zero is
   * the one value not selectable here — a negative one is accepted and puts the
   * threshold below the default, which is the direction a caller reaching for
   * zero wanted anyway.
   */
  onsetDelta?: number;
  /**
   * Caps a span that no onset follows. It binds at the end of a phrase and at the
   * end of the track; anywhere else the next onset closes the span first. Default
   * 500 ms.
   */
  maxEventMs?: number;
  /**
   * Drops an event whose `percussiveRatio` falls below this; must be in `[0, 1]`.
   * 0 is both the default and the meaningful "keep everything". Raising it is
   * useful on material that is mostly drums and wrong on a dense mix, where it
   * also drops real hits sitting over a loud sustain.
   */
  minPercussiveRatio?: number;
}

/** Canonical request form for {@link renderPercussiveEvents}. */
export interface RenderPercussiveEventsRequest
  extends PercussiveSeparationOptions,
    ValidateOptions {
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `fadeMs` is converted to samples with this rate,
   * so a wrong/omitted value changes the fade length.
   */
  sampleRate: number;
  /** The events to render, with their edits. Source spans must not overlap. */
  events: readonly PercussiveEventInput[];
  /**
   * Fade-out at the tail of each lifted span. Default 5 ms, so a zero-length
   * fade is unreachable here rather than rejected — and a hard cut is not a thing
   * to want anyway, because what the fade shapes is the signal being subtracted,
   * so squaring it off leaves a step. There is deliberately
   * no matching fade-in: a span opens in front of its transient, where the
   * percussive component is near-silent, so cutting square there costs nothing
   * and keeps a muted hit's attack from surviving inside a fade.
   */
  fadeMs?: number;
}

/**
 * Extract editable percussive events from audio alone.
 *
 * Each event is a struck sound located in time: a span in source samples, its
 * detector strength, the percussive peak over the span, the share of the span's
 * energy the separation called percussive, and an identity
 * {@link PercussiveEventEdit}. Edit the events and hand them to
 * {@link renderPercussiveEvents} to apply the result — the source audio is never
 * mutated, and a set whose edits are all identity renders back to the input bit
 * for bit.
 *
 * Onsets are detected on the percussive component rather than on the source, so
 * a harmonic attack is attenuated before the detector sees it instead of being
 * filtered out afterwards. Each onset opens a span that the next one closes,
 * capped by `maxEventMs` and never running past the end of the audio.
 *
 * Each onset is backtracked to the transient's start, which is not optional and
 * is why there is no knob for it: peak-picking lands after the attack, and a span
 * that opened there would report the next hit's peak and leave its own attack
 * behind when muted.
 *
 * An event carries no pitch and is never associated with a {@link NoteObject} —
 * a struck sound has no steady F0 to edit, so the two models are extracted by
 * separate calls.
 *
 * @param request - Audio, its sample rate, and the separation, peak-picking and
 *   span options
 * @returns One {@link PercussiveEvent} per detected hit, in time order; an empty
 *   array when nothing was detected
 * @throws RangeError when the samples or sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on a kernel size that is not an
 *   integer within the 32-bit range, a framing size that is negative or outside
 *   that range, a framing that breaks constant overlap-add, an `onsetWait` that
 *   is fractional, negative or non-finite, a negative or non-finite `onsetDelta`
 *   / `maxEventMs`, or a `minPercussiveRatio` outside `[0, 1]`
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 * // Drop the second hit and push the third 10 ms late.
 * events[1].edit.muted = true;
 * events[2].edit.timeOffsetSamples = Math.round(0.01 * sampleRate);
 * const edited = renderPercussiveEvents({ samples, sampleRate, events });
 * ```
 */
export function extractPercussiveEvents(
  request: ExtractPercussiveEventsRequest,
): PercussiveEvent[] {
  assertSamples('extractPercussiveEvents', request.samples, request.validate !== false);
  assertSampleRate('extractPercussiveEvents', request.sampleRate);
  assertPercussiveSeparation('extractPercussiveEvents', request);
  return requireModule().extractPercussiveEvents(request.samples, request.sampleRate, request);
}

/**
 * Render edited percussive events back over their source audio.
 *
 * Per event the lifted signal is the percussive component over
 * `[onsetSample, offsetSample)` under the tail fade. It is subtracted where it
 * sits and, unless the event is muted, added back at the shifted position scaled
 * by the gain. Only that signal moves, so muting a hit leaves the harmonic
 * content under it sounding and moving one does not drag its neighbours' sustain
 * along.
 *
 * Each event's span and `edit` are read; `strength`, `peakAmplitude` and
 * `percussiveRatio` are ignored, so an extracted event can be passed back as-is,
 * or an event can be built by hand from the span alone. A set whose edits are all
 * identity reproduces the input bit for bit and runs no separation at all.
 *
 * Pass the separation the events were extracted with: a different one lifts a
 * different signal out of the span than the one the events describe. It is
 * validated even when every edit is the identity and no separation runs, so an
 * unusable framing is an error on every set rather than on some of them.
 *
 * Overlap is checked on the source spans only. Where `timeOffsetSamples` lands an
 * event is not, so two moved events may be written over each other, and a shift
 * that pushes the signal past either end is truncated there rather than wrapped.
 *
 * @param request - Source audio, the events to render, the separation and the
 *   tail fade
 * @returns The rendered audio, the same length and sample rate as the input
 * @throws RangeError when the samples or sample rate fail the shared input checks
 * @throws SonareError (`InvalidParameter`) on an event whose span is empty,
 *   reversed or outside the audio, overlapping source spans, a non-finite
 *   `gainDb`, a kernel size that is not an integer within the 32-bit range, a
 *   framing that breaks constant overlap-add, or a negative or non-finite
 *   `fadeMs`
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 *
 * // Lift the loudest hit by 3 dB and leave the rest untouched.
 * const loudest = events.reduce((a, b) => (a.strength >= b.strength ? a : b));
 * const edited = events.map((event) =>
 *   event === loudest ? { ...event, edit: { ...event.edit, gainDb: 3 } } : event,
 * );
 * const rendered = renderPercussiveEvents({ samples, sampleRate, events: edited });
 * ```
 */
export function renderPercussiveEvents(request: RenderPercussiveEventsRequest): Float32Array {
  assertSamples('renderPercussiveEvents', request.samples, request.validate !== false);
  assertSampleRate('renderPercussiveEvents', request.sampleRate);
  assertPercussiveSeparation('renderPercussiveEvents', request);
  return requireModule().renderPercussiveEvents(
    request.samples,
    request.sampleRate,
    request.events,
    request,
  );
}
