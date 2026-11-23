import { EXPECTED_ABI_VERSION } from './abi.js';
import { ErrorCode, type SonareError } from './errors.js';
import {
  attachRefusal,
  codedError,
  invalidState,
  nameNativeError,
  passForeign,
  type RefusalRecord,
  refuse,
  refuseType,
} from './internal/entry.js';
import { type ModuleFlavour, type ModuleSlot, moduleSlot } from './internal/module_registry.js';
import { NATIVE_ENTRIES } from './internal/native_entries.js';
import type { InitOptions, ModuleFactoryOptions } from './public_types.js';
import type { SonareModule } from './sonare.js';

/** The holder this bundle's module reads go to; the analysis entry binds its own flavour. */
let boundSlot: ModuleSlot | null = null;

/** This bundle's error-translating wrapper over the holder's module, rebuilt if it changes. */
let wrapped: { raw: SonareModule; module: SonareModule } | null = null;

function currentSlot(): ModuleSlot {
  boundSlot ??= moduleSlot('full');
  return boundSlot;
}

/** Point this bundle's module reads at @p flavour's holder. */
export function bindModuleFlavour(flavour: ModuleFlavour): void {
  boundSlot = moduleSlot(flavour);
}

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
  /** Present for a core parameter refusal: its parts, for a facade to re-render. */
  refusal?: RefusalRecord;
}

/** The runtime helpers a module built with native Wasm exceptions exports for reading a throw. */
interface WasmExceptionHelpers {
  getCppExceptionTag?: () => WebAssembly.Tag;
  getCppExceptionThrownObjectFromWebAssemblyException?: (error: WebAssembly.Exception) => number;
}

/**
 * Recover the native exception-object pointer from a value thrown across the
 * WASM boundary. The module is built with native Wasm exception handling, so a
 * C++ throw reaches JS as a `WebAssembly.Exception` carrying the C++ tag, and
 * the module's own helper reads the thrown object's address out of it. A raw
 * pointer number and a `CppException` exposing `excPtr` (JS-emulated
 * exceptions) are still read for a module built the older way.
 * Returns null when the thrown value is none of these (a genuine JS error, or a
 * Wasm exception some other tag raised), so the caller rethrows it unchanged.
 */
export function nativeExceptionPtr(module: SonareModule, error: unknown): number | null {
  if (typeof error === 'number') {
    return error;
  }
  if (typeof WebAssembly.Exception === 'function' && error instanceof WebAssembly.Exception) {
    const helpers = module as unknown as WasmExceptionHelpers;
    const tag = helpers.getCppExceptionTag?.();
    if (tag === undefined || !error.is(tag)) {
      return null;
    }
    return helpers.getCppExceptionThrownObjectFromWebAssemblyException?.(error) ?? null;
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
  let refusal: RefusalRecord | undefined;
  try {
    const info = (
      raw as unknown as { sonareExceptionInfo?: (ptr: number) => NativeExceptionInfo }
    ).sonareExceptionInfo?.(thrown);
    if (info) {
      code = info.code ?? code;
      message = info.message || message;
      kind = info.kind;
      refusal = info.refusal;
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
  const error =
    kind === 'TypeError'
      ? refuseType(message)
      : kind === 'RangeError'
        ? refuse(message)
        : codedError(code as ErrorCode, message);
  return refusal === undefined ? error : attachRefusal(error, refusal);
}

/**
 * Wrap the embind module so a native C++ exception (a `WebAssembly.Exception`
 * carrying the C++ tag, see {@link nativeExceptionPtr}) is rethrown as a
 * {@link SonareError}. Only function-valued
 * members are wrapped, and the wrapper is cached per member so repeated access
 * stays cheap; non-function members (typed-array heap views, etc.) pass through
 * unchanged.
 */
function wrapModuleErrors(raw: SonareModule): SonareModule {
  const cache = new Map<PropertyKey, unknown>();
  const objectCache = new WeakMap<object, unknown>();
  const translate = (error: unknown): unknown => {
    const ptr = nativeExceptionPtr(raw, error);
    if (ptr !== null) {
      return makeSonareError(raw, ptr);
    }
    // embind's use-after-delete is a plain BindingError; report it as object state.
    if (error instanceof Error && EMBIND_DELETED_PATTERN.test(error.message)) {
      return invalidState(error.message);
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
  return invalidState('Module not initialized. Call init() first.');
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
  throw codedError(
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
      throw invalidState(
        `init() options differ from the first call (${key}); options apply on the first call only.`,
      );
    }
  }
}

/**
 * Load @p flavour's module into the realm's holder, or join the load already in
 * flight; a later call whose options differ from the first is refused.
 */
export function initModule(
  flavour: ModuleFlavour,
  options: InitOptions | undefined,
  defaultFactory: () => Promise<(options?: ModuleFactoryOptions) => Promise<unknown>>,
): Promise<void> {
  const slot = moduleSlot(flavour);
  if (slot.module || slot.pending) {
    assertSameInitOptions(slot.firstOptions, options);
    return slot.pending ?? Promise.resolve();
  }
  slot.firstOptions = options && { ...options };
  slot.pending = (async () => {
    try {
      const createModule = options?.moduleFactory ?? (await defaultFactory());
      const created = (await createModule(options)) as unknown as SonareModule;
      assertAbiCompatible(created);
      slot.module = created;
    } catch (error) {
      slot.pending = null;
      slot.firstOptions = undefined;
      throw error;
    }
  })();
  return slot.pending;
}

/** @p flavour's loaded module in this realm, unwrapped, or null before `init()` resolves. */
export function loadedModule(flavour: ModuleFlavour): SonareModule | null {
  return moduleSlot(flavour).module;
}

export function getSonareModule(): SonareModule {
  const raw = currentSlot().module;
  if (!raw) {
    throw notInitializedError();
  }
  if (wrapped?.raw !== raw) {
    wrapped = { raw, module: wrapModuleErrors(raw) };
  }
  return wrapped.module;
}
