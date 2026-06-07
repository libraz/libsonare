import { ErrorCode, SonareError } from './errors';
import type { SonareModule } from './sonare.js';

let wrappedModule: SonareModule | null = null;

/**
 * Shape of the structured info the native `sonareExceptionInfo(ptr)` returns.
 */
interface NativeExceptionInfo {
  code: number;
  codeName: string;
  message: string;
}

/**
 * Turn a thrown native exception pointer into a {@link SonareError}. With
 * emscripten's classic exception handling a C++ throw reaches JS as the raw
 * exception-object pointer (a number); the bound `sonareExceptionInfo` decodes
 * it back into { code, codeName, message }.
 */
function makeSonareError(raw: SonareModule, thrown: number): SonareError {
  let code: number = ErrorCode.Unknown;
  let codeName = 'Unknown';
  let message = `libsonare native exception (${thrown})`;
  try {
    const info = (
      raw as unknown as { sonareExceptionInfo?: (ptr: number) => NativeExceptionInfo }
    ).sonareExceptionInfo?.(thrown);
    if (info) {
      code = info.code ?? code;
      codeName = info.codeName ?? codeName;
      message = info.message || message;
    }
  } catch {
    // Fall back to the generic message if decoding fails.
  }
  return new SonareError(code, codeName, message);
}

/**
 * Wrap the embind module so a native C++ exception (which surfaces as a raw
 * pointer number) is rethrown as a {@link SonareError}. Only function-valued
 * members are wrapped, and the wrapper is cached per member so repeated access
 * stays cheap; non-function members (typed-array heap views, etc.) pass through
 * unchanged. The dedicated realtime `sonare-rt` module is separate and is not
 * affected by this wrapper.
 */
function wrapModuleErrors(raw: SonareModule): SonareModule {
  const cache = new Map<PropertyKey, unknown>();
  const convert = (error: unknown): never => {
    if (typeof error === 'number') {
      throw makeSonareError(raw, error);
    }
    throw error;
  };
  return new Proxy(raw, {
    get(target, prop, receiver) {
      const value = Reflect.get(target, prop, receiver);
      if (typeof value !== 'function') {
        return value;
      }
      const cached = cache.get(prop);
      if (cached) {
        return cached;
      }
      // Wrap as a Proxy (not a plain function) so embind class constructors
      // invoked via `new module.Foo(...)` keep their `[[Construct]]` behaviour
      // and prototype while still converting thrown native pointers.
      const fn = value as (...a: unknown[]) => unknown;
      const wrapped = new Proxy(fn, {
        apply(t, thisArg, args) {
          try {
            return Reflect.apply(t, thisArg, args as unknown[]);
          } catch (error) {
            return convert(error);
          }
        },
        construct(t, args, newTarget) {
          try {
            return Reflect.construct(t, args as unknown[], newTarget) as object;
          } catch (error) {
            return convert(error) as object;
          }
        },
      });
      cache.set(prop, wrapped);
      return wrapped;
    },
  }) as SonareModule;
}

export function setSonareModule(module: SonareModule): void {
  wrappedModule = wrapModuleErrors(module);
}

export function getSonareModule(): SonareModule {
  if (!wrappedModule) {
    throw new Error('Module not initialized. Call init() first.');
  }
  return wrappedModule;
}
