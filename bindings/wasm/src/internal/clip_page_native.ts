const nativeProviders = new WeakMap<object, unknown>();

// Global symbol, so the worklet bundle can reach the index bundle's class through an engine it was handed.
const PROVIDER_CLASS = Symbol.for('libsonare.wasm.ClipPageProvider');

/** Records the `ClipPageProvider` class whose natives `engineClass` instances accept. */
export function registerClipPageProviderClass(engineClass: object, providerClass: unknown): void {
  Object.defineProperty(engineClass, PROVIDER_CLASS, { value: providerClass });
}

/**
 * The `ClipPageProvider` class of the bundle `engine` was built by, or `fallback`. Each bundle
 * holds its own module instance, so a provider must come from the engine's bundle.
 */
export function clipPageProviderClassFor<T>(engine: object, fallback: T): T {
  const registered = (engine.constructor as { [PROVIDER_CLASS]?: T } | undefined)?.[PROVIDER_CLASS];
  return registered ?? fallback;
}

/** Registers the embind instance behind a `ClipPageProvider`. */
export function bindNativeClipPageProvider(provider: object, native: unknown): void {
  nativeProviders.set(provider, native);
}

/** The embind instance behind a registered provider; any other value unchanged, so the native refuses it. */
export function nativeClipPageProviderOf(value: unknown): unknown {
  return typeof value === 'object' && value !== null && nativeProviders.has(value)
    ? nativeProviders.get(value)
    : value;
}
