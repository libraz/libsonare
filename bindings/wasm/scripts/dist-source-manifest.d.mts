/** Declarations for the manifest writer/verifier, for the TypeScript tests that drive it. */

export interface SourceSpec {
  roots: string[];
  files: string[];
  ignore: string[];
}

export interface DistModule {
  name: string;
  primary: string;
  artifacts: string[];
  sources: SourceSpec;
  rebuild: string;
  staleOptOut?: boolean;
  toolchain?: 'emscripten';
}

export interface ManifestLocation {
  distDir?: string;
  repoRoot?: string;
  modules?: DistModule[];
}

export const MODULES: DistModule[];
export const ALLOW_STALE_ENV: 'SONARE_ALLOW_STALE_DIST';

export function collectSources(spec: SourceSpec, repoRoot?: string): Promise<Record<string, string>>;
export function manifestPath(moduleName: string, distDir?: string): string;
export function writeManifests(
  names: string[],
  location?: ManifestLocation & { emscriptenVersion?: () => string | null },
): Promise<string[]>;
export function verifyAll(options?: ManifestLocation & { allowStale?: boolean }): Promise<{
  problems: string[];
  provenance: string[];
  artifacts: Record<string, string>;
}>;
export function verifyToolchain(options?: {
  distDir?: string;
  modules?: DistModule[];
  pinned?: string;
}): Promise<string[]>;
export function readArtifacts(names: string[]): Promise<Record<string, string | null>>;
