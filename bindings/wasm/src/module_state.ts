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
 * Recover the native exception-object pointer from a value thrown across the
 * WASM boundary. emscripten surfaces a C++ throw in two shapes depending on the
 * toolchain/exception mode:
 *   - a raw pointer number (older / classic surfacing), or
 *   - a `CppException` object exposing the pointer as `excPtr` (emscripten with
 *     `-fexceptions`).
 * Returns null when the thrown value is neither (a genuine JS error), so the
 * caller rethrows it unchanged.
 */
function nativeExceptionPtr(error: unknown): number | null {
  if (typeof error === 'number') {
    return error;
  }
  if (error !== null && typeof error === 'object') {
    const ptr = (error as { excPtr?: unknown }).excPtr;
    if (typeof ptr === 'number') {
      return ptr;
    }
  }
  return null;
}

/**
 * Turn a thrown native exception pointer into a {@link SonareError}. The bound
 * `sonareExceptionInfo` decodes the pointer back into { code, codeName,
 * message }.
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
 * pointer number or a `CppException` carrying one) is rethrown as a
 * {@link SonareError}. Only function-valued
 * members are wrapped, and the wrapper is cached per member so repeated access
 * stays cheap; non-function members (typed-array heap views, etc.) pass through
 * unchanged. The dedicated realtime `sonare-rt` module is separate and is not
 * affected by this wrapper.
 */
function wrapModuleErrors(raw: SonareModule): SonareModule {
  const cache = new Map<PropertyKey, unknown>();
  const objectCache = new WeakMap<object, unknown>();
  const convert = (error: unknown): never => {
    const ptr = nativeExceptionPtr(error);
    if (ptr !== null) {
      throw makeSonareError(raw, ptr);
    }
    throw error;
  };

  const wrapNativeObject = (value: unknown): unknown => {
    if (value === null || typeof value !== 'object') {
      return value;
    }
    if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer || value instanceof Promise) {
      return value;
    }
    const objectValue = value as object;
    const cached = objectCache.get(objectValue);
    if (cached) {
      return cached;
    }
    const methodCache = new Map<PropertyKey, unknown>();
    const wrapped = new Proxy(objectValue, {
      get(target, prop, receiver) {
        const member = Reflect.get(target, prop, receiver);
        if (typeof member !== 'function') {
          return member;
        }
        const cachedMethod = methodCache.get(prop);
        if (cachedMethod) {
          return cachedMethod;
        }
        const method = member as (...a: unknown[]) => unknown;
        const wrappedMethod = (...args: unknown[]) => {
          try {
            return wrapNativeObject(Reflect.apply(method, target, args));
          } catch (error) {
            return convert(error);
          }
        };
        methodCache.set(prop, wrappedMethod);
        return wrappedMethod;
      },
    });
    objectCache.set(objectValue, wrapped);
    return wrapped;
  };

  const wrapFunction = (value: (...a: unknown[]) => unknown): unknown => {
    const fnCache = new Map<PropertyKey, unknown>();
    return new Proxy(value, {
      get(target, prop, receiver) {
        const member = Reflect.get(target, prop, receiver);
        if (typeof member !== 'function') {
          return member;
        }
        const cachedMember = fnCache.get(prop);
        if (cachedMember) {
          return cachedMember;
        }
        const fn = member as (...a: unknown[]) => unknown;
        const wrappedMember = (...args: unknown[]) => {
          try {
            return wrapNativeObject(Reflect.apply(fn, target, args));
          } catch (error) {
            return convert(error);
          }
        };
        fnCache.set(prop, wrappedMember);
        return wrappedMember;
      },
      apply(t, thisArg, args) {
        try {
          return wrapNativeObject(Reflect.apply(t, thisArg, args as unknown[]));
        } catch (error) {
          return convert(error);
        }
      },
      construct(t, args, newTarget) {
        try {
          return wrapNativeObject(Reflect.construct(t, args as unknown[], newTarget));
        } catch (error) {
          return convert(error) as object;
        }
      },
    });
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
      const wrapped = wrapFunction(value as (...a: unknown[]) => unknown);
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
