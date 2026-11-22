/** Emscripten runtime methods the module exports beside its embind registrations. */
export const RUNTIME_METHODS: ReadonlySet<string>;
/** The embind names the built analysis module registers, sorted. */
export function analysisRegistrations(root?: URL): Promise<string[]>;
/** The declaration text for @p names. */
export function declarationText(names: readonly string[]): string;
