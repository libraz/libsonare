import { parseRecordJson } from './internal/record_json.js';
import { addon } from './native.js';
import type { Capabilities, CapabilityCatalog } from './types.js';

/** libsonare semantic version of the loaded native library, e.g. `'2.0.0'`. */
export function version(): string {
  return addon.version();
}

/**
 * Aggregate native ABI version: the per-subsystem ABI macros folded into one
 * 32-bit value. It bumps whenever any flat C POD layout changes, so callers can
 * detect an incompatible prebuilt native binary; the package also checks it
 * when the addon loads and throws `SonareError` `AbiMismatch` on skew.
 */
export function abiVersion(): number {
  return addon.abiVersion();
}

/**
 * Build and runtime capabilities of the loaded native library. Feature
 * presence is read here (`capabilities().features.ffmpeg`,
 * `capabilities().features.vocalEdit`, ...); there is no separate probe per
 * feature.
 */
export function capabilities(): Capabilities {
  return addon.capabilities() as Capabilities;
}

/**
 * The loaded native library's processors, parameter descriptors, and built-in
 * presets as one machine-readable catalog. The processor and parameter entries
 * are the same records {@link masteringProcessorCatalog} and
 * {@link masteringInsertParamInfo} return.
 */
export function capabilityCatalog(): CapabilityCatalog {
  return parseRecordJson(addon.capabilityCatalog()) as CapabilityCatalog;
}
