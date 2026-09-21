import type { SynthEnumTables, SynthPatch } from './instrument_types';
import { projectModule } from './project_internal';

/**
 * Runtime ABI version of the flat project POD layout exposed by this WASM
 * build. Equals {@link EXPECTED_PROJECT_ABI_VERSION} when the arrangement
 * subsystem is compiled in. Mirrors the C-ABI `sonare_project_abi_version`.
 */
export function projectAbiVersion(): number {
  return projectModule().projectAbiVersion();
}

/**
 * NativeSynth preset catalog names (`'sine'`, `'saw-lead'`, `'e-piano'`,
 * `'drum-kit'`, ...). Use these to discover valid {@link SynthPatch} preset
 * names instead of hardcoding magic strings.
 */
export function synthPresetNames(): string[] {
  // Array.from re-roots embind's vector as a plain, structured-cloneable Array.
  return Array.from(projectModule().synthPresetNames());
}

/**
 * GS rhythm-set name a rhythm part's `program` selects (`'Standard'`,
 * `'Room'`, `'TR-808'`, ...), or `null` when the module's own tone map defines
 * no set there.
 *
 * @remarks
 * The answer is the module's own map, which is the newest one and reaches
 * every set this build voices; a file selecting an older map reaches fewer.
 */
export function synthGsDrumKitName(program: number): string | null {
  return projectModule().synthGsDrumKitName(program);
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
  const r = projectModule().synthGsDrumKitIsVoicedApart(program);
  return r < 0 ? null : r === 1;
}

/**
 * Whether melodic Bank Select `bank` on `program` is voiced apart from the
 * capital tone: `true` when the bank has a patch of its own, `false` when it
 * resolves to the capital, `null` when either argument is out of range.
 *
 * @remarks
 * Resolving an unvoiced variation to its capital is what GS specifies, so a
 * `false` is correct behaviour rather than a gap — but only this query
 * separates it from a bank that is voiced, which otherwise takes rendering both
 * and comparing. Accepts the GS Bank Select MSB and the GM2 LSB alike, since
 * both address the same variation.
 */
export function synthGsVariationIsVoicedApart(bank: number, program: number): boolean | null {
  const r = projectModule().synthGsVariationIsVoicedApart(bank, program);
  return r < 0 ? null : r === 1;
}

/**
 * Controller-profile preset names (`'gm'`, `'breath'`, `'breath-aftertouch'`,
 * `'mpe'`). These are the names {@link RealtimeEngine.setControllerProfile}
 * accepts; an unknown one throws rather than resolving to a default.
 */
export function controllerProfileNames(): string[] {
  // Array.from re-roots embind's vector as a plain, structured-cloneable Array.
  return Array.from(projectModule().controllerProfileNames());
}

/**
 * Fetch a named catalog preset as a {@link SynthPatch} (the preset name plus
 * the wrapper-section values), so hosts can inspect a preset and tweak fields
 * before binding it. A `"va:"` routing prefix is accepted; unknown names
 * throw.
 */
export function synthPresetPatch(name: string): SynthPatch {
  // embind returns a val::object whose constructor is not this realm's Object, so a
  // direct return is not structured-cloneable (breaks postMessage to a Worker).
  // Spreading into a fresh literal re-roots it as a plain Object; modRoutings is
  // already a plain member array.
  return { ...projectModule().synthPresetPatch(name) };
}

export function synthEnumTables(): SynthEnumTables {
  return projectModule()._synthEnumTables();
}

export function synthPatchRoundTripForTest(patch: SynthPatch): SynthPatch {
  return projectModule()._synthPatchRoundTrip(patch);
}
