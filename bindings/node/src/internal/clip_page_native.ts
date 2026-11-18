const natives = new WeakMap<object, unknown>();

/** Records the addon handle behind a `ClipPageProvider` so the engine can pass it on. */
export function bindNativeClipPageProvider(provider: object, native: unknown): void {
  natives.set(provider, native);
}

/** The addon handle of a registered provider; any other value is returned unchanged for the addon to refuse. */
export function nativeClipPageProviderOf(value: unknown): unknown {
  return typeof value === 'object' && value !== null && natives.has(value)
    ? natives.get(value)
    : value;
}
