import type { VoicePresetId } from '../public_types.js';

/** Public preset ids in the order of the native preset ordinals. */
export const VOICE_PRESET_IDS: readonly VoicePresetId[] = [
  'neutralMonitor',
  'brightIdol',
  'softWhisper',
  'deepNarrator',
  'robotMascot',
  'darkVillain',
  'hardTune',
  'robotMonotone',
  'choirHarmony',
  'vocoderStrings',
  'megaphone',
  'radioVoice',
];

// Native still spells the preset ids in kebab case; same order as VOICE_PRESET_IDS.
const NATIVE_PRESET_IDS: readonly string[] = [
  'neutral-monitor',
  'bright-idol',
  'soft-whisper',
  'deep-narrator',
  'robot-mascot',
  'dark-villain',
  'hard-tune',
  'robot-monotone',
  'choir-harmony',
  'vocoder-strings',
  'megaphone',
  'radio-voice',
];

/**
 * The native preset ordinal of a public id.
 *
 * @throws TypeError unless `id` is a string; RangeError for an unknown id.
 */
export function voicePresetOrdinal(fnName: string, id: unknown, argName = 'preset'): number {
  if (typeof id !== 'string') {
    throw new TypeError(`${fnName}: ${argName} must be a string`);
  }
  const ordinal = VOICE_PRESET_IDS.indexOf(id as VoicePresetId);
  if (ordinal < 0) {
    throw new RangeError(`${fnName}: ${argName} must be one of ${VOICE_PRESET_IDS.join(', ')}`);
  }
  return ordinal;
}

/** The native (kebab) spelling of a public preset id. */
export function voicePresetIdToNative(fnName: string, id: unknown, argName = 'preset'): string {
  return NATIVE_PRESET_IDS[voicePresetOrdinal(fnName, id, argName)] as string;
}

/** The public id of a native preset id, or `undefined` for one this binding does not know. */
export function voicePresetIdFromNative(native: string): VoicePresetId | undefined {
  const ordinal = NATIVE_PRESET_IDS.indexOf(native);
  return ordinal < 0 ? undefined : VOICE_PRESET_IDS[ordinal];
}
