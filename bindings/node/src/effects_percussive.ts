/**
 * Percussive event editing: locating struck sounds in audio and rendering an
 * edited set of them back.
 */

import { resolveEntryTimes, toSamples } from './internal/effects_common.js';
import { refuse, refuseType, withEntry } from './internal/entry.js';
import { requireListBudget } from './internal/list_budget.js';
import { withRespelled } from './internal/respell.js';
import { addon } from './native.js';
import type { PercussiveEvent, PercussiveEventInput } from './types.js';
import type { OptionsOf } from './validation.js';
import { assertAudioInput, assertInt32, sampleOneShot } from './validation.js';

// The plain-array samples form is read as the Float32Array it stands for before the shared normalizer sees it.
function percussiveRequest<
  R extends { samples: Float32Array | readonly number[]; sampleRate: number },
>(requestName: 'extractPercussiveEvents' | 'renderPercussiveEvents', args: unknown[]): R {
  const first = args[0];
  if (Array.isArray(first)) {
    requireListBudget(first, 'samples');
  }
  const widened = Array.isArray(first) ? [Float32Array.from(first), ...args.slice(1)] : args;
  return sampleOneShot<R & { samples: Float32Array }>(requestName, widened);
}

/**
 * Check the separation an extracted or rendered event set is measured against
 * before the addon narrows it. Every field defaults at 0 here, so a value that
 * wrapped to 0 would select the default and report success, and an explicit 0
 * is refused for the same reason.
 *
 * Either refusal is an argument refusal, a `RangeError`, as the WASM reader
 * raises it and Python raises `SonareValueError`.
 */
function assertPercussiveSeparation(options: PercussiveSeparationOptions): void {
  const fields = ['nFft', 'hopLength', 'hpssKernelHarmonic', 'hpssKernelPercussive'] as const;
  for (const field of fields) {
    const value = options[field];
    if (value === undefined) {
      continue;
    }
    assertInt32(value, field);
    if (value === 0) {
      throw refuse(`${field} must not be 0; omit it for the library value`);
    }
  }
}

/**
 * The separation an event's signal is lifted out with, mixed into both
 * percussive-event requests.
 *
 * Extraction measures events against it and rendering has to repeat it — a
 * different separation lifts a different signal out of the span than the one the
 * events describe — so it is one set of fields both sides take rather than a
 * framing each of them restates.
 *
 * The framing must overlap-add, because the separation inverts an STFT: `nFft`
 * even and at least 2, `hopLength` no more than half of it.
 */
export interface PercussiveSeparationOptions {
  /**
   * FFT size and hop the separation and the onset detector share. They cannot be
   * set apart: an event measured on one framing and lifted out on another is not
   * the same signal. Default 2048 and a quarter of `nFft`. An explicit 0 in any
   * separation field is refused; leave the field out for its default.
   */
  nFft?: number;
  /** Hop length in samples, at most half of `nFft`. Default a quarter of `nFft`. */
  hopLength?: number;
  /**
   * Median filter lengths the separation runs, along time and along frequency. A
   * longer harmonic kernel calls more of a sustained sound harmonic. Default 31.
   */
  hpssKernelHarmonic?: number;
  /** Median filter length along frequency (percussive kernel). Default `31`. */
  hpssKernelPercussive?: number;
}

/** Request for {@link extractPercussiveEvents}: audio plus the separation and onset-detector settings. */
export interface ExtractPercussiveEventsRequest extends PercussiveSeparationOptions {
  /**
   * Source audio. A plain number array is copied to a `Float32Array` first,
   * which is what the native side reads.
   */
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `maxEventMs` is converted to samples with this
   * rate, so a wrong or omitted value caps the spans differently.
   */
  sampleRate: number;
  /**
   * Minimum frames between consecutive onsets, a positive whole number. Default
   * 1; an explicit 0 is refused, and a fractional wait is refused rather than
   * truncated.
   */
  onsetWait?: number;
  /**
   * Offset added to the detector's adaptive threshold; raising it finds fewer,
   * stronger hits. Default 0.06. An explicit 0 is refused because the native
   * configuration would read it as the default; a negative one is accepted.
   */
  onsetDelta?: number;
  /**
   * Caps a span that no onset follows. It binds at the end of a phrase and at
   * the end of the track; anywhere else the next onset closes the span first.
   * Default 500 ms; an explicit 0 is refused.
   */
  maxEventMs?: number;
  /**
   * Drops an event whose `percussiveRatio` falls below this; must be in
   * `[0, 1]`. 0 is both the default and the meaningful "keep everything", so
   * nothing is lost to the rule that an omitted option takes the default.
   *
   * Raising it is useful on material that is mostly drums and wrong on a dense
   * mix, where it also drops real hits sitting over a loud sustain.
   */
  minPercussiveRatio?: number;
}

/** Request for {@link renderPercussiveEvents}: audio, the edited events and the separation they were extracted with. */
export interface RenderPercussiveEventsRequest extends PercussiveSeparationOptions {
  /**
   * Source audio. A plain number array is copied to a `Float32Array` first,
   * which is what the native side reads.
   */
  samples: Float32Array | readonly number[];
  /**
   * Sample rate in Hz. Required: `fadeMs` is converted to samples with this
   * rate, so a wrong or omitted value changes the fade length.
   */
  sampleRate: number;
  /**
   * The events to render, with their edits. Source spans must be non-empty,
   * inside `samples`, and must not overlap.
   */
  events: readonly PercussiveEventInput[];
  /**
   * Fade-out at the tail of each lifted span. Default 5 ms; an explicit 0 is
   * refused rather than read as that default, and a hard cut would leave a step
   * in the output.
   *
   * There is deliberately no matching fade-in: a span opens on an onset, where
   * the percussive component is near-silent the sample before, so cutting square
   * there costs nothing and keeps a muted hit's attack from surviving inside a
   * fade.
   */
  fadeMs?: number;
}

/** Refuse an explicit 0 in fields the native config reads 0 as "use the default" for. */
function refuseZeroDefaults<T extends object>(
  options: T,
  fields: readonly (keyof T & string)[],
): void {
  for (const field of fields) {
    if (options[field] === 0) {
      throw refuse(`${field} must not be 0; omit it for the library value`);
    }
  }
}

/**
 * Extract editable percussive events — struck sounds located in time — from
 * audio alone.
 *
 * Onsets are detected on the percussive component rather than on the source, so
 * a harmonic attack is attenuated before the detector sees it instead of being
 * filtered out afterwards. Each onset opens a span that the next one closes, and
 * every returned event carries its strength, the percussive peak over the span,
 * the share of the span's energy the separation called percussive, and the
 * identity {@link PercussiveEventEdit}. Change the edits and hand the events to
 * {@link renderPercussiveEvents} to hear them; nothing is applied here.
 *
 * Each onset is backtracked to the transient's start, which is not optional and
 * is why there is no knob for it: peak-picking lands after the attack, and a
 * span that opened there would report the next hit's peak and leave its own
 * attack behind when muted.
 *
 * An event carries no pitch and is never associated with a {@link NoteObject};
 * {@link extractNotes} is the separate call that produces those.
 *
 * @param request - Audio, its sample rate, the separation framing, and the
 *   optional detection tuning.
 * @returns One {@link PercussiveEvent} per detected hit, in time order. Audio in
 *   which nothing was detected returns an empty array rather than throwing.
 * @throws {RangeError} `samples` is empty or holds a non-finite sample,
 *   `sampleRate` is out of the supported range, or a separation field,
 *   `onsetWait`, `onsetDelta` or `maxEventMs` is an explicit 0.
 * @throws {SonareError} A separation field is not an
 *   integer within the signed 32-bit range, the framing breaks constant
 *   overlap-add, `maxEventMs` is not positive and finite, or
 *   `minPercussiveRatio` is outside `[0, 1]`.
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 *
 * // Silence the second hit and drag the third 10 ms later.
 * events[1].edit.muted = true;
 * events[2].edit.timeOffsetSamples = Math.round(0.01 * sampleRate);
 * const edited = renderPercussiveEvents({ samples, sampleRate, events });
 * ```
 */
export function extractPercussiveEvents(request: ExtractPercussiveEventsRequest): PercussiveEvent[];
export function extractPercussiveEvents(
  samples: Float32Array | readonly number[],
  sampleRate: number,
  options?: OptionsOf<ExtractPercussiveEventsRequest>,
): PercussiveEvent[];
export function extractPercussiveEvents(...args: unknown[]): PercussiveEvent[] {
  return withEntry('extractPercussiveEvents', () => {
    const request = percussiveRequest<ExtractPercussiveEventsRequest>(
      'extractPercussiveEvents',
      args,
    );
    const { samples: input, sampleRate, ...options } = request;
    const samples = Array.isArray(input) ? toSamples(input) : input;
    assertAudioInput(samples, sampleRate);
    assertPercussiveSeparation(options);
    if (options.onsetWait !== undefined) {
      // Checked here with its four bag-siblings, and for the same reason: 0 is
      // this field's default, and the addon's narrowing truncates onto it.
      assertInt32(options.onsetWait, 'onsetWait');
    }
    refuseZeroDefaults(options, ['onsetWait', 'onsetDelta', 'maxEventMs']);
    return addon.extractPercussiveEvents(samples, sampleRate, options);
  });
}

/**
 * Render edited percussive events over their source audio.
 *
 * Per event the lifted signal is the percussive component over
 * `[onsetSample, offsetSample)` under the tail fade. It is subtracted where it
 * sits and, unless the event is muted, added back at the shifted position scaled
 * by the gain. Only that signal moves, so muting a hit leaves the harmonic
 * content under it sounding and moving one does not drag its neighbours' sustain
 * along.
 *
 * Each event's span and `edit` are read; `strength`, `peakAmplitude` and
 * `percussiveRatio` are ignored, so what {@link extractPercussiveEvents}
 * returned can be handed straight back. A set whose edits are all identity
 * reproduces the input bit for bit and runs no separation at all. The result has
 * the input's length.
 *
 * Pass back the separation the extraction used: a different one lifts a
 * different signal out of the span than the one the events describe. It is
 * validated even when every edit is the identity, so an unusable framing is an
 * error on every set.
 *
 * Overlap is checked on the source spans only. Where `timeOffsetSamples` lands
 * an event is not, so two moved events may be written over each other, and a
 * shift that pushes the signal past either end is truncated there rather than
 * wrapped.
 *
 * @param request - Audio, its sample rate, the events to render, the separation
 *   framing, and the optional tail fade.
 * @returns The rendered audio, the same length as `samples`.
 * @throws {TypeError} `events` is not an array, or one of its entries is not a
 *   plain object.
 * @throws {RangeError} `samples` is empty or holds a non-finite sample,
 *   `sampleRate` is out of the supported range, or a separation field is an
 *   explicit 0.
 * @throws {SonareError} A separation field is not an integer
 *   within the signed 32-bit range, an event's span is empty, reversed or
 *   outside the audio, two source spans overlap, a `gainDb` is not finite, the
 *   framing breaks constant overlap-add, or `fadeMs` is not positive and finite.
 *
 * @example
 * ```ts
 * const events = extractPercussiveEvents({ samples, sampleRate });
 *
 * // Drop every hit the separation was least sure about by 6 dB.
 * const tamed = events.map((event) =>
 *   (event.percussiveRatio ?? 1) < 0.3 ? { ...event, edit: { ...event.edit, gainDb: -6 } } : event,
 * );
 * const output = renderPercussiveEvents({ samples, sampleRate, events: tamed, fadeMs: 10 });
 * ```
 */
export function renderPercussiveEvents(request: RenderPercussiveEventsRequest): Float32Array;
export function renderPercussiveEvents(
  samples: Float32Array | readonly number[],
  sampleRate: number,
  options: OptionsOf<RenderPercussiveEventsRequest>,
): Float32Array;
export function renderPercussiveEvents(...args: unknown[]): Float32Array {
  return withEntry('renderPercussiveEvents', () => {
    const request = percussiveRequest<RenderPercussiveEventsRequest>(
      'renderPercussiveEvents',
      args,
    );
    const { samples: input, sampleRate, events, ...options } = request;
    const samples = Array.isArray(input) ? toSamples(input) : input;
    assertAudioInput(samples, sampleRate);
    if (!Array.isArray(events)) {
      throw refuseType('events must be an array');
    }
    assertPercussiveSeparation(options);
    refuseZeroDefaults(options, ['fadeMs']);
    const { entries: nativeEvents, respelled } = resolveEntryTimes(
      events,
      'events',
      sampleRate,
      true,
    );
    return withRespelled(respelled, () =>
      addon.renderPercussiveEvents(samples, sampleRate, nativeEvents, options),
    );
  });
}
