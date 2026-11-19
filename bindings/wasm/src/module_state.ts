import { EXPECTED_ABI_VERSION } from './abi.js';
import { ErrorCode, SonareError } from './errors.js';
import { nameNativeError, passForeign } from './internal/entry.js';
import { NATIVE_ENTRIES } from './internal/native_entries.js';
import type { SonareModule } from './sonare.js';

let wrappedModule: SonareModule | null = null;

/** The two messages embind uses for a call on, or an argument that is, a deleted instance. */
const EMBIND_DELETED_PATTERN =
  / instance already deleted$|^Cannot pass deleted object as a pointer of type /;

/**
 * Shape of the structured info the native `sonareExceptionInfo(ptr)` returns.
 */
interface NativeExceptionInfo {
  code: number;
  codeName: string;
  message: string;
  /** Present when the binding refused an argument itself: the JS class to rebuild it as. */
  kind?: 'TypeError' | 'RangeError';
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
export function nativeExceptionPtr(error: unknown): number | null {
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
 * Turn a thrown native exception pointer into a {@link SonareError}, or into a
 * `TypeError` / `RangeError` when an argument was refused (a wrong-typed or
 * out-of-domain value, whether the binding or the library refused it, as opposed
 * to an object-state or other non-argument failure, which stays a coded
 * `SonareError`). The bound `sonareExceptionInfo`
 * decodes the pointer back into { code, codeName, message, kind }, then
 * `sonareReleaseException` drops the reference emscripten's `__cxa_throw` took
 * before rethrowing the pointer into JS.
 *
 * The release is mandatory, not an optimization: no C++ frame catches the
 * exception, so that reference is the only one and nothing else ever drops it.
 * Every rejected input would otherwise leak its exception object for the
 * lifetime of the module — and rejection-as-control-flow (re-validating markers
 * or clips on each edit, a scrub handle seeking every pointer move) is a
 * documented usage of this API. It runs in a `finally` so a decode failure
 * still frees, and after decoding because freeing invalidates the message.
 */
function makeSonareError(raw: SonareModule, thrown: number): Error {
  let code: number = ErrorCode.Unknown;
  let kind: NativeExceptionInfo['kind'];
  let message = `libsonare native exception (${thrown})`;
  try {
    const info = (
      raw as unknown as { sonareExceptionInfo?: (ptr: number) => NativeExceptionInfo }
    ).sonareExceptionInfo?.(thrown);
    if (info) {
      code = info.code ?? code;
      message = info.message || message;
      kind = info.kind;
    }
  } catch {
    // Fall back to the generic message if decoding fails.
  } finally {
    try {
      raw.sonareReleaseException(thrown);
    } catch {
      // A module built before the release binding existed still yields an error
      // object; it just keeps leaking, which is what this replaces.
    }
  }
  if (kind === 'TypeError') {
    return new TypeError(message);
  }
  if (kind === 'RangeError') {
    return new RangeError(message);
  }
  return new SonareError(code as ErrorCode, message);
}

/**
 * Wrap the embind module so a native C++ exception (which surfaces as a raw
 * pointer number or a `CppException` carrying one) is rethrown as a
 * {@link SonareError}. Only function-valued
 * members are wrapped, and the wrapper is cached per member so repeated access
 * stays cheap; non-function members (typed-array heap views, etc.) pass through
 * unchanged.
 */
function wrapModuleErrors(raw: SonareModule): SonareModule {
  const cache = new Map<PropertyKey, unknown>();
  const objectCache = new WeakMap<object, unknown>();
  const translate = (error: unknown): unknown => {
    const ptr = nativeExceptionPtr(error);
    if (ptr !== null) {
      return makeSonareError(raw, ptr);
    }
    // embind's use-after-delete is a plain BindingError; report it as object state.
    if (error instanceof Error && EMBIND_DELETED_PATTERN.test(error.message)) {
      return new SonareError(ErrorCode.InvalidState, error.message);
    }
    return error;
  };
  // The error carries the public entry its native key belongs to ("analyze: ...").
  const convert = (error: unknown, key: string): never => {
    throw nameNativeError(translate(error), NATIVE_ENTRIES[key] ?? key);
  };
  // A callback handed to a native call (progress, cancel) throws as the caller's own: never renamed.
  const foreignArgs = (args: ArrayLike<unknown>): ArrayLike<unknown> => {
    for (let i = 0; i < args.length; i++) {
      if (typeof args[i] === 'function') {
        return Array.from(args, passForeign);
      }
    }
    return args;
  };
  const keyOf = (owner: string | undefined, prop: PropertyKey): string =>
    owner ? `${owner}.${String(prop)}` : String(prop);

  const wrapNativeObject = (value: unknown): unknown => {
    if (value === null || typeof value !== 'object') {
      return value;
    }
    if (ArrayBuffer.isView(value) || value instanceof ArrayBuffer || value instanceof Promise) {
      return value;
    }
    // Plain result data carries no native methods, and a Proxy cannot be
    // structured-cloned, so wrapping it would block postMessage to a worker.
    const proto = Object.getPrototypeOf(value);
    if (Array.isArray(value) || proto === Object.prototype || proto === null) {
      return value;
    }
    const objectValue = value as object;
    const cached = objectCache.get(objectValue);
    if (cached) {
      return cached;
    }
    const methodCache = new Map<PropertyKey, unknown>();
    const owner = (proto?.constructor as { name?: string } | undefined)?.name;
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
        const key = keyOf(owner, prop);
        // `arguments` rather than a rest parameter: forwarded without an array allocation on the per-block path.
        const wrappedMethod = function (this: unknown) {
          try {
            // biome-ignore lint/complexity/noArguments: forwarded without an array allocation on the per-block path
            return wrapNativeObject(Reflect.apply(method, target, foreignArgs(arguments)));
          } catch (error) {
            return convert(error, key);
          }
        };
        methodCache.set(prop, wrappedMethod);
        return wrappedMethod;
      },
    });
    objectCache.set(objectValue, wrapped);
    return wrapped;
  };

  const wrapFunction = (value: (...a: unknown[]) => unknown, name: string): unknown => {
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
        const key = keyOf(name, prop);
        const wrappedMember = function (this: unknown) {
          try {
            // biome-ignore lint/complexity/noArguments: forwarded without an array allocation on the per-block path
            return wrapNativeObject(Reflect.apply(fn, target, foreignArgs(arguments)));
          } catch (error) {
            return convert(error, key);
          }
        };
        fnCache.set(prop, wrappedMember);
        return wrappedMember;
      },
      apply(t, thisArg, args) {
        try {
          return wrapNativeObject(Reflect.apply(t, thisArg, foreignArgs(args as unknown[])));
        } catch (error) {
          return convert(error, name);
        }
      },
      construct(t, args, newTarget) {
        try {
          return wrapNativeObject(
            Reflect.construct(t, foreignArgs(args as unknown[]), newTarget),
          ) as object;
        } catch (error) {
          return convert(error, name) as object;
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
      const wrapped = wrapFunction(value as (...a: unknown[]) => unknown, String(prop));
      cache.set(prop, wrapped);
      return wrapped;
    },
  }) as SonareModule;
}

/** Error thrown when a facade function runs before `init()` resolved. */
export function notInitializedError(): SonareError {
  return new SonareError(ErrorCode.InvalidState, 'Module not initialized. Call init() first.');
}

/** Format a packed ABI version as `0x` plus eight hex digits. */
function hexAbi(version: number): string {
  return `0x${(version >>> 0).toString(16).padStart(8, '0')}`;
}

/**
 * Throw an {@link ErrorCode.AbiMismatch} error unless the freshly created native
 * module reports the C-ABI version this package's TypeScript was built against.
 * A mismatch means the JS glue and the `.wasm` come from different builds, which
 * the `wasmBinary`, `locateFile` and `moduleFactory` options or a stale cache
 * can produce; running on would misread native structs.
 */
export function assertAbiCompatible(created: SonareModule): void {
  const actual =
    typeof created.abiVersion === 'function' ? (created.abiVersion() as number) : undefined;
  if (actual === EXPECTED_ABI_VERSION) {
    return;
  }
  throw new SonareError(
    ErrorCode.AbiMismatch,
    `libsonare ABI mismatch: wasm module reports ${
      actual === undefined ? 'no ABI version' : hexAbi(actual)
    }, this package expects ${hexAbi(EXPECTED_ABI_VERSION)}. ` +
      'The JS glue and the .wasm binary come from different builds; check the wasmBinary, ' +
      'locateFile and moduleFactory options and any cached copy of sonare.wasm.',
  );
}

/**
 * Throw when a later `init(options)` call passes options that differ (shallow,
 * functions by reference) from the first call's. No options always passes.
 */
export function assertSameInitOptions(first: object | undefined, next: object | undefined): void {
  if (next === undefined) {
    return;
  }
  const a = (first ?? {}) as Record<string, unknown>;
  const b = next as Record<string, unknown>;
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const key of keys) {
    if (a[key] !== b[key]) {
      throw new SonareError(
        ErrorCode.InvalidState,
        `init() options differ from the first call (${key}); options apply on the first call only.`,
      );
    }
  }
}

export function setSonareModule(module: SonareModule): void {
  wrappedModule = wrapModuleErrors(module);
}

export function getSonareModule(): SonareModule {
  if (!wrappedModule) {
    throw notInitializedError();
  }
  return wrappedModule;
}
