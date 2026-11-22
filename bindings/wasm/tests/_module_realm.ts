import { type ModuleSlot, moduleSlot } from '../src/internal/module_registry.js';

/**
 * Empties every module holder in this realm, so a test can observe `init()` from
 * a state no earlier test has touched. `vi.resetModules()` alone no longer does
 * that: the holder lives on `globalThis`, shared by every bundle of one build.
 */
export function clearModuleRealm(): void {
  const realm = globalThis as unknown as Record<symbol, Record<string, unknown> | undefined>;
  for (const key of Object.getOwnPropertySymbols(realm)) {
    const slot = realm[key];
    if (key.description?.startsWith('libsonare.module/') && slot) {
      slot.module = null;
      slot.pending = null;
      slot.firstOptions = undefined;
    }
  }
}

/**
 * Hands the module the built `dist/` entry loaded to the unbundled `src/`
 * modules a test imports beside it. They run under the build id `source`, so
 * their holder is a different one; call after `init()` from `dist/`.
 */
export function shareBuiltModuleWithSource(): void {
  const realm = globalThis as unknown as Record<symbol, { module: unknown } | undefined>;
  const built = Object.getOwnPropertySymbols(realm).find((key) => {
    const description = key.description ?? '';
    return (
      description.startsWith('libsonare.module/') &&
      description.endsWith('/full') &&
      description !== 'libsonare.module/source/full' &&
      realm[key]?.module
    );
  });
  if (!built) {
    throw new Error('no built module is loaded in this realm; call init() from dist first');
  }
  moduleSlot('full', 'source').module = realm[built]?.module as ModuleSlot['module'];
}
