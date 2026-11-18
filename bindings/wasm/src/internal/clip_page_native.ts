const nativeProviders = new WeakMap<object, unknown>();

// Global symbol, so the worklet bundle can reach the index bundle's factory through an engine it was handed.
const OPFS_PROVIDER_FACTORY = Symbol.for('libsonare.wasm.createOpfsClipPageProvider');

/** Records the OPFS provider factory whose providers `engineClass` instances accept. */
export function registerOpfsClipPageProviderFactory(engineClass: object, factory: unknown): void {
  Object.defineProperty(engineClass, OPFS_PROVIDER_FACTORY, { value: factory });
}

/**
 * The OPFS provider factory of the bundle `engine` was built by, or `fallback`. Each bundle
 * holds its own module instance, so a provider must come from the engine's bundle.
 */
export function opfsClipPageProviderFactoryFor<T>(engine: object, fallback: T): T {
  const registered = (engine.constructor as { [OPFS_PROVIDER_FACTORY]?: T } | undefined)?.[
    OPFS_PROVIDER_FACTORY
  ];
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
