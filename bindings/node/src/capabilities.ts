import { parseCatalogJson } from './internal/record_json.js';
import { addon } from './native.js';
import type { Capabilities, CapabilityCatalog, SystemInfo } from './types.js';

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
 * CPU, memory and parallelism facts of the host. A fact the platform cannot
 * report is absent from its object, never 0.
 */
export function systemInfo(): SystemInfo {
  const raw = JSON.parse(addon.systemInfo()) as {
    cpu?: { logical_cores?: number; physical_cores?: number };
    memory?: { total_gb?: number; available_gb?: number };
    parallel: { enabled: boolean; workers: number; strategy: string };
  };
  const { cpu, memory, parallel } = raw;
  return {
    cpu: {
      ...(cpu?.logical_cores !== undefined && { logicalCores: cpu.logical_cores }),
      ...(cpu?.physical_cores !== undefined && { physicalCores: cpu.physical_cores }),
    },
    memory: {
      ...(memory?.total_gb !== undefined && { totalGb: memory.total_gb }),
      ...(memory?.available_gb !== undefined && { availableGb: memory.available_gb }),
    },
    parallel: {
      enabled: parallel.enabled,
      workers: parallel.workers,
      strategy: parallel.strategy,
    },
  };
}

/**
 * The loaded native library's processors, parameter descriptors, and built-in
 * presets as one machine-readable catalog. The processor and parameter entries
 * are the same records {@link masteringProcessorCatalog} and
 * {@link masteringInsertParamInfo} return.
 */
export function capabilityCatalog(): CapabilityCatalog {
  return parseCatalogJson(addon.capabilityCatalog()) as CapabilityCatalog;
}
