import type { RealtimeVoiceChangerPodConfig } from '../public_types.js';
import type { WasmRealtimeVoiceChangerPodConfig } from '../sonare.js';

const NATIVE_SCALE_BY_KIND: Readonly<
  Record<string, WasmRealtimeVoiceChangerPodConfig['pitchCorrectionScale']>
> = {
  major: 'major',
  naturalMinor: 'natural_minor',
  chromatic: 'chromatic',
};

/** Native spells the natural-minor scale `natural_minor`; the public token is `naturalMinor`. */
export function podConfigToNative(
  config: RealtimeVoiceChangerPodConfig,
): WasmRealtimeVoiceChangerPodConfig {
  const kind = config.pitchCorrectionScale;
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  const known = Object.prototype.hasOwnProperty.call(NATIVE_SCALE_BY_KIND, kind);
  const scale = known ? NATIVE_SCALE_BY_KIND[kind] : undefined;
  if (scale === undefined) {
    throw new RangeError(
      `pitchCorrectionScale must be 'major', 'naturalMinor' or 'chromatic', got '${String(config.pitchCorrectionScale)}'`,
    );
  }
  return { ...config, pitchCorrectionScale: scale };
}

/** Inverse of {@link podConfigToNative}. */
export function podConfigFromNative(
  config: WasmRealtimeVoiceChangerPodConfig,
): RealtimeVoiceChangerPodConfig {
  const kind =
    config.pitchCorrectionScale === 'natural_minor' ? 'naturalMinor' : config.pitchCorrectionScale;
  if (kind !== 'major' && kind !== 'naturalMinor' && kind !== 'chromatic') {
    throw new RangeError(
      `Unknown native pitchCorrectionScale '${String(config.pitchCorrectionScale)}'`,
    );
  }
  return { ...config, pitchCorrectionScale: kind };
}
