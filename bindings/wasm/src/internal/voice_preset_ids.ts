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

/**
 * A public preset id after the same checks as {@link voicePresetOrdinal}.
 *
 * @throws TypeError unless `id` is a string; RangeError for an unknown id.
 */
export function checkedVoicePresetId(
  fnName: string,
  id: unknown,
  argName = 'preset',
): VoicePresetId {
  return VOICE_PRESET_IDS[voicePresetOrdinal(fnName, id, argName)] as VoicePresetId;
}
