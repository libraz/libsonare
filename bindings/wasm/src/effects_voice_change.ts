import { refuse, refuseType, withEntry } from './internal/entry.js';
import { deinterleavePlanes, interleavePlanes, nativeVoiceConfig } from './internal/pod_config.js';
import { getSonareModule } from './module_state.js';
import type { RealtimeVoiceChangerConfigInput } from './public_types.js';
import {
  assertAudioChannels,
  assertAudioInput,
  assertString,
  requestOnly,
  sampleOneShot,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/** How `formantFactor` relates to the pitch shift in {@link voiceChange}. */
export type FormantMode = 'relative' | 'absolute';

/** Options for {@link voiceChange}. All fields are optional. */
export interface VoiceChangeOptions {
  /** Pitch shift in semitones (negative = down). Default 0. */
  pitchSemitones?: number;
  /** Formant scale factor (>1 brightens, <1 darkens). Default 1. */
  formantFactor?: number;
  /**
   * How `formantFactor` relates to the pitch shift. Default `'relative'`.
   *
   * - `'relative'`: `formantFactor` is the warp applied after the pitch shift, so the
   *   formants end up at `formantFactor * 2^(pitchSemitones / 12)` of the input's. A factor
   *   outside [0.55, 1.65] throws a `RangeError`; it is never clamped.
   * - `'absolute'`: `formantFactor` is the formant shift relative to the input (1 keeps
   *   the formants where they were); the warp applied is
   *   `formantFactor / 2^(pitchSemitones / 12)`. A request whose warp falls outside
   *   [0.55, 1.65] throws a `RangeError` naming the reachable `formantFactor` range for
   *   the given `pitchSemitones`; it is never clamped.
   */
  formantMode?: FormantMode;
}

/** Canonical request form for one-shot voice changing. */
export interface VoiceChangeRequest extends VoiceChangeOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz, an integer in [8000, 384000]. Required. */
  sampleRate: number;
}

/**
 * Apply a voice change by shifting pitch and formants independently.
 *
 * @param request - Mono `samples`, `sampleRate` in Hz (required), and the pitch/formant settings
 *   ({@link VoiceChangeOptions}); or pass `(samples, sampleRate, options?)`.
 * @returns Voice-changed audio
 * @throws TypeError for a wrong-typed argument, a missing `sampleRate` or an unknown key;
 *   RangeError for out-of-domain audio, rate or setting.
 */
export function voiceChange(request: VoiceChangeRequest): Float32Array;
export function voiceChange(
  samples: Float32Array,
  sampleRate: number,
  options?: VoiceChangeOptions,
): Float32Array;
export function voiceChange(...args: unknown[]): Float32Array {
  return withEntry('voiceChange', () => {
    const request = sampleOneShot<VoiceChangeRequest>('voiceChange', args);
    assertAudioInput(request.samples, request.sampleRate, request);
    const formantMode = request.formantMode ?? 'relative';
    assertString(formantMode, 'formantMode');
    return requireModule().voiceChange(
      request.samples,
      request.sampleRate,
      request.pitchSemitones ?? 0.0,
      request.formantFactor ?? 1.0,
      formantMode,
    );
  });
}

/** Canonical request form for offline realtime voice changing. */
export interface RealtimeVoiceChangerRenderRequest {
  /** One (mono) or two (stereo) planar channels of equal length. */
  channels: Float32Array[];
  /** Sample rate in Hz, an integer in [8000, 384000]. Required. */
  sampleRate: number;
  /** Preset id, preset document or flat config. Default `'neutralMonitor'`. */
  config?: RealtimeVoiceChangerConfigInput;
}

/**
 * Applies the realtime voice-changer chain to whole planar buffers in one call.
 *
 * Uses the shared C-ABI renderer, so Python, Node, and WASM use the same
 * fixed block size and latency compensation.
 *
 * @returns The processed channels, with the layout and length of the input.
 * @throws TypeError for a wrong-typed field, a missing `sampleRate` or an unknown key;
 *   RangeError for a channel count other than 1 or 2, unequal channel lengths, out-of-domain
 *   audio or rate, or an unknown preset id or token.
 */
export function realtimeVoiceChangerRender(
  request: RealtimeVoiceChangerRenderRequest,
): Float32Array[] {
  return withEntry('realtimeVoiceChangerRender', () => {
    const fnName = 'realtimeVoiceChangerRender';
    const req = requestOnly<RealtimeVoiceChangerRenderRequest>(fnName, [request]);
    if (req.sampleRate === undefined || req.sampleRate === null) {
      throw refuseType(`sampleRate must be a number`);
    }
    assertAudioChannels(req.channels, req.sampleRate, req);
    const planes = req.channels;
    if (planes.length > 2) {
      throw refuse(`channels must hold 1 or 2 planes`);
    }
    const frames = (planes[0] as Float32Array).length;
    if (planes.some((plane) => plane.length !== frames)) {
      throw refuse(`channels must have equal lengths`);
    }
    const native = withEntry('realtimeVoiceChangerRender', () =>
      nativeVoiceConfig(req.config ?? 'neutralMonitor'),
    );
    const out = requireModule().realtimeVoiceChangerRender(
      interleavePlanes(planes),
      req.sampleRate,
      typeof native === 'string' ? native : JSON.stringify(native),
      planes.length,
    );
    return deinterleavePlanes(out, planes.length);
  });
}
