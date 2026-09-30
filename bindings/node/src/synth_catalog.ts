import { addon } from './native.js';
import type { SynthEnumTables, SynthPatch } from './types.js';

/**
 * NativeSynth preset catalog names (`'sine'`, `'saw-lead'`, `'e-piano'`,
 * `'drum-kit'`, ...). Use these to discover valid {@link SynthPatch} preset
 * names instead of hardcoding magic strings.
 */
export function synthPresetNames(): string[] {
  return addon.synthPresetNames();
}

/**
 * GS rhythm-set name a rhythm part's `program` selects (`'Standard'`,
 * `'Room'`, `'Jazz'`, ...), or `null` when the module's own tone map defines
 * no set there.
 *
 * @remarks
 * The answer is the module's own map, which is the newest one and reaches
 * every set this build voices; a file selecting an older map reaches fewer.
 */
export function synthGsDrumKitName(program: number): string | null {
  return addon.synthGsDrumKitName(program);
}

/**
 * Whether the GS rhythm set at `program` is voiced apart from Standard: `true`
 * when at least one drum note differs, `false` when the set renders exactly as
 * Standard, `null` when no set sits at `program`.
 *
 * @remarks
 * Derived by applying the set to every note's resolved patch and comparing, so
 * the answer follows the voicing rather than a list that has to be kept in step
 * with it. Four sets GS fills with one-shots share the Standard voicing
 * deliberately, so a picker built from the set list alone offers four choices
 * that change nothing — annotate or disable them with this.
 *
 * @example
 * ```ts
 * const kits = Array.from({ length: 128 }, (_, program) => ({ program, name: synthGsDrumKitName(program) }))
 *   .filter((kit): kit is { program: number; name: string } => kit.name !== null)
 *   .map((kit) => ({ ...kit, placeholder: synthGsDrumKitIsVoicedApart(kit.program) === false }));
 * ```
 */
export function synthGsDrumKitIsVoicedApart(program: number): boolean | null {
  const r: number = addon.synthGsDrumKitIsVoicedApart(program);
  return r < 0 ? null : r === 1;
}

/**
 * Whether melodic Bank Select `bank` on `program` is voiced apart from the
 * capital tone: `true` when the bank has a patch of its own, `false` when it
 * resolves to the capital, `null` when either argument is outside `[0, 127]` —
 * both are seven-bit MIDI values, so `128` is out of range rather than the drum
 * bank here.
 *
 * @remarks
 * Resolving an unvoiced variation to its capital is what GS specifies, so a
 * `false` is correct behaviour rather than a gap — but only this query
 * separates it from a bank that is voiced, which otherwise takes rendering both
 * and comparing. Accepts the GS Bank Select MSB and the GM2 LSB alike, since
 * both address the same variation.
 */
export function synthGsVariationIsVoicedApart(bank: number, program: number): boolean | null {
  const r: number = addon.synthGsVariationIsVoicedApart(bank, program);
  return r < 0 ? null : r === 1;
}

/**
 * Fetch a named catalog preset as a {@link SynthPatch} (the preset name plus
 * the wrapper-section values), so hosts can inspect a preset and tweak fields
 * before binding it. A `"va:"` routing prefix is accepted; unknown names
 * throw.
 */
export function synthPresetPatch(name: string): SynthPatch {
  return addon.synthPresetPatch(name);
}

/**
 * Controller-profile preset names (`'gm'`, `'breath'`, `'breath-aftertouch'`,
 * `'mpe'`). Use these with {@link RealtimeEngine.setControllerProfile} instead
 * of hardcoding magic strings; an unknown name throws.
 */
export function controllerProfileNames(): string[] {
  return addon.controllerProfileNames();
}

/** Return the canonical NativeSynth enum tables from the native C oracle. */
export function synthEnumTables(): SynthEnumTables {
  return addon._synthEnumTables();
}
