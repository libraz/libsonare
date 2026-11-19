import { refuse, refuseType, withEntry } from './internal/entry.js';
import { panModeToNative } from './internal/mix_scene.js';
import { addon } from './native.js';
import type {
  MixMeterSnapshot,
  MixResult,
  MixSceneDocument,
  MixScenePreset,
  PanMode,
} from './types.js';
import {
  assertAudioInput,
  assertFloatValue,
  assertSampleRate,
  requestOnly,
  requireBoolean,
} from './validation.js';

/** Names of the built-in mixing scene presets accepted by {@link mixingScenePreset}. */
export function mixingScenePresetNames(): MixScenePreset[] {
  return addon.mixingScenePresetNames() as MixScenePreset[];
}

/**
 * Get a built-in mixing scene preset. The returned document loads directly into a {@link Mixer}
 * via {@link Mixer.fromJson}.
 *
 * @param name - Preset name (see {@link mixingScenePresetNames})
 * @returns The scene document
 * @throws TypeError when `name` is not a string; RangeError for an unknown preset name
 */
export function mixingScenePreset(name: MixScenePreset): MixSceneDocument {
  return withEntry('mixingScenePreset', () => {
    const names = withEntry('mixingScenePreset', () => mixingScenePresetNames());
    if (typeof name !== 'string') {
      throw refuseType(`name must be one of ${names.join(', ')}`);
    }
    if (!names.includes(name)) {
      throw refuse(`name must be one of ${names.join(', ')}`);
    }
    return JSON.parse(addon.mixingScenePresetJson(name)) as MixSceneDocument;
  });
}

/** One strip of {@link mixStereo}: a stereo pair and its mixer settings. */
export interface MixStereoStrip {
  /** Left input; every strip has the same length. */
  left: Float32Array;
  /** Right input; same length as `left`. */
  right: Float32Array;
  /** Input trim in dB. Default 0. */
  inputTrimDb?: number;
  /** Fader in dB. Default 0. */
  faderDb?: number;
  /** Pan position in `[-1, 1]`. Default 0 (centre). */
  pan?: number;
  /** Pan mode; absent keeps the strip's own (`'balance'`). Unknown token: `RangeError`. */
  panMode?: PanMode;
  /** Stereo width in `[0, 2]`. Default 1. */
  width?: number;
  /** Mute the strip. Default false. */
  muted?: boolean;
}

/** Inputs for the one-shot {@link mixStereo} facade. */
export interface MixStereoRequest {
  /** Strips to mix, in order; must not be empty. */
  strips: MixStereoStrip[];
  /** Sample rate in Hz, `[8000, 384000]`. Required. */
  sampleRate: number;
}

const STRIP_KEYS = [
  'left',
  'right',
  'inputTrimDb',
  'faderDb',
  'pan',
  'panMode',
  'width',
  'muted',
] as const;
const STRIP_NUMBER_KEYS = ['inputTrimDb', 'faderDb', 'pan', 'width'] as const;

/**
 * One-shot stereo mix of multiple strips through the routing graph + master bus.
 *
 * Each returned per-strip meter reflects only this single one-shot block. The
 * LUFS fields (`momentaryLufs`, `shortTermLufs`, `integratedLufs`) are
 * integrators whose windows a short one-shot mix does not fill, so they read the
 * -120 dB floor sentinel. Use {@link Mixer} for meaningful loudness.
 *
 * The true-peak fields are not integrators and need no streaming: each is a
 * max-hold over the block just processed and is valid from the first one,
 * flooring only on silence. This facade mixes the whole input as a single block
 * and flushes the meters after it, so the reading matches `meteringTruePeakDbtp`
 * over the same signal exactly.
 *
 * @param request - Strips (each a stereo pair and its settings) and the sample rate
 * @throws TypeError for a wrong-typed or missing field or an unknown key; RangeError for no
 *   strips, unequal lengths, out-of-domain audio or rate, or an unknown pan mode
 */
export function mixStereo(request: MixStereoRequest): MixResult;
export function mixStereo(...args: unknown[]): MixResult {
  return withEntry('mixStereo', () => {
    const fnName = 'mixStereo';
    const req = requestOnly<MixStereoRequest>(fnName, args);
    assertSampleRate(req.sampleRate);
    if (!Array.isArray(req.strips)) {
      throw refuseType(`strips must be an array`);
    }
    if (req.strips.length === 0) {
      throw refuse(`strips must not be empty`);
    }
    const left: Float32Array[] = [];
    const right: Float32Array[] = [];
    const options: Record<string, unknown[]> = {};
    req.strips.forEach((strip, index) => {
      const path = `strips[${index}]`;
      if (strip === null || typeof strip !== 'object' || Array.isArray(strip)) {
        throw refuseType(`${path} must be an object`);
      }
      for (const key of Object.keys(strip)) {
        if (!(STRIP_KEYS as readonly string[]).includes(key)) {
          throw refuseType(`unknown option '${key}' in '${path}'`);
        }
      }
      assertAudioInput(strip.left, req.sampleRate, req, `${path}.left`);
      assertAudioInput(strip.right, req.sampleRate, req, `${path}.right`);
      if (strip.left.length !== strip.right.length) {
        throw refuse(`${path}.left and ${path}.right must have equal lengths`);
      }
      left.push(strip.left);
      right.push(strip.right);
      const given: Record<string, unknown> = {};
      for (const key of STRIP_NUMBER_KEYS) {
        if (strip[key] !== undefined && strip[key] !== null) {
          assertFloatValue(strip[key], `${path}.${key}`);
          given[key] = strip[key];
        }
      }
      if (strip.panMode !== undefined && strip.panMode !== null) {
        given.panMode = panModeToNative(strip.panMode, `${path}.panMode`);
      }
      if (strip.muted !== undefined && strip.muted !== null) {
        given.muted = requireBoolean(strip.muted, `${path}.muted`);
      }
      for (const [key, value] of Object.entries(given)) {
        // A strip that leaves a field out stays at its default: the native array holds a hole.
        const column = options[key] ?? new Array(req.strips.length).fill(undefined);
        column[index] = value;
        options[key] = column;
      }
    });
    const native = addon.mixStereo(left, right, req.sampleRate, options);
    return {
      left: native.left,
      right: native.right,
      sampleRate: native.sampleRate,
      meters: native.meters as MixMeterSnapshot[],
    };
  });
}
