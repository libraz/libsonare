/** Declarations for the build-freshness verifier, for the TypeScript tests that drive it. */

export interface SourceSpec {
  suffixes: string[];
  trees: string[];
  excluded: string[];
}

export interface BuildOutput {
  artifact: string;
  sources: SourceSpec;
  rebuild: string;
  staleOptOut?: boolean;
}

export const ALLOW_STALE_ENV: 'SONARE_ALLOW_STALE_ADDON';
export const CORE_SOURCES_PATH: 'tools/abi/core-sources.json';

/** The shared core-source definition, as stored. */
export interface CoreSources {
  suffixes: string[];
  trees: string[];
  excluded_trees: string[];
}

export function loadCoreSources(repoRoot?: string): Promise<CoreSources>;
export function defaultOutputs(repoRoot?: string): Promise<BuildOutput[]>;
export function newestSource(
  spec: SourceSpec,
  repoRoot?: string,
): Promise<{ mtimeMs: number; path: string } | null>;
export function verifyOutputs(options?: {
  allowStale?: boolean;
  repoRoot?: string;
  outputs?: BuildOutput[];
}): Promise<{ problems: string[]; provenance: string[]; artifacts: Record<string, string | null> }>;
export function readDigests(paths: string[]): Promise<Record<string, string | null>>;
