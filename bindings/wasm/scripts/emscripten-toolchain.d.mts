/** Declarations for the Emscripten toolchain guard, for the TypeScript tests that drive it. */

export const VERSION_FILE: string;
export function pinnedVersion(file?: string): string;
export function parseEmccVersion(text: string): string | null;
export function installedVersion(env?: NodeJS.ProcessEnv): string | null;
export function activationHint(version: string): string;
export function toolchainProblem(options?: { env?: NodeJS.ProcessEnv; pinned?: string }): string | null;
