import { projectModule } from './project_internal';
import type { SynthEnumTables, SynthPatch } from './project_types';

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
