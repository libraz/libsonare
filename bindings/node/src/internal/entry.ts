/**
 * The entry prefix on a refusal: "<entry>: <bare text>".
 *
 * A refusal is built bare, by one of the formatters below or by the wrapper
 * around every native call, and the bare text is kept on the error object.
 * Each public entry the error leaves through (a class wrapped by
 * {@link entryClass}, a {@link withEntry} scope, the native wrapper itself)
 * recomposes the message from that bare text with its own name, on a sync
 * throw or on the rejection of the promise it returned. The last layer is the
 * outermost one, the entry the consumer called, so it wins by construction,
 * across awaits too, and nothing is prefixed twice. An error nothing marked
 * (a JS engine TypeError for a bad call) passes through untouched; nothing
 * reads a message to decide anything. An error a caller's callback threw
 * through a native call carries the {@link FOREIGN} mark and is never renamed.
 */

import { ErrorCode, type ErrorReason, SonareError } from '../errors.js';

const UNPREFIXED = Symbol('libsonare.unprefixedMessage');

/**
 * Marks an error a caller's callback threw through a native call. A registered
 * symbol, so the native layer (the Node addon, the WASM glue) can set it too.
 */
const FOREIGN = Symbol.for('libsonare.foreignError');

type NamedError = Error & { [UNPREFIXED]?: string; [FOREIGN]?: true };

/** Mark `error` as the caller's own, so no entry renames it on the way out. */
function markForeign(error: unknown): unknown {
  if (error !== null && (typeof error === 'object' || typeof error === 'function')) {
    try {
      Object.defineProperty(error, FOREIGN, { value: true, configurable: true });
    } catch {
      // A frozen or proxied value keeps no mark; only a marked native refusal is ever renamed.
    }
  }
  return error;
}

/** `callback` with a throw it raises marked as the caller's own; a non-function passes through. */
export function passForeign<T>(callback: T): T {
  if (typeof callback !== 'function') {
    return callback;
  }
  const fn = callback as unknown as (...args: unknown[]) => unknown;
  return function (this: unknown) {
    try {
      // biome-ignore lint/complexity/noArguments: forwarded without an array allocation
      return Reflect.apply(fn, this, arguments);
    } catch (error) {
      throw markForeign(error);
    }
  } as unknown as T;
}

function mark<E extends Error>(error: E, text: string): E {
  Object.defineProperty(error, UNPREFIXED, { value: text, configurable: true });
  return error;
}

function setPrefix(error: NamedError, text: string, entry: string): void {
  const before = `${error.name}: ${error.message}`;
  const message = `${entry}: ${text}`;
  error.message = message;
  Object.defineProperty(error, 'entry', {
    value: entry,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  const stack = error.stack;
  if (typeof stack === 'string' && stack.startsWith(before)) {
    error.stack = `${error.name}: ${message}${stack.slice(before.length)}`;
  }
}

/** Refusal of an out-of-domain argument; the entry the caller invoked prefixes its name. */
const REFUSAL = Symbol.for('libsonare.refusal');

/** A core parameter refusal as plain parts: what its native translation point attached. */
export interface RefusalRecord {
  readonly code: number;
  readonly form: 'predicate' | 'wrongType' | 'unknownKey' | 'relation';
  readonly subjects: readonly string[];
  readonly predicate: string;
  readonly expected: string;
  readonly callerSpelled: boolean;
  /** The text the caller read, without the entry prefix. */
  readonly text: string;
  /** The entry key of the C-ABI call that refused; null or absent when a direct core call did. */
  readonly cEntry?: string | null;
  /** Original native subjects, present only in an opt-in refusal capture. */
  readonly raw?: {
    readonly code: number;
    readonly subjects: readonly string[];
    readonly callerSpelled: boolean;
    readonly fromC: boolean;
    readonly flattened?: string;
  };
  readonly domain?: {
    readonly lo?: number;
    readonly hi?: number;
    readonly loIncluded: boolean;
    readonly hiIncluded: boolean;
  };
}

/** How a refused subject reaches the caller: the name it wrote, and how many of the subject's units one of its own holds (the sample rate, for seconds given in place of samples). */
export interface Respelling {
  readonly name: string;
  readonly scale: number;
}

/** Respellings keyed by the subject the caller would otherwise read. */
export type Respelled = Readonly<Record<string, Respelling>>;

/** The respellings of several converted arguments, as one declaration for their call. */
export function mergeRespelled(...parts: readonly Respelled[]): Respelled {
  return Object.assign({}, ...parts);
}

/** Carries @p refusal on @p error, as the native translation points do. */
export function attachRefusal<E extends Error>(error: E, refusal: RefusalRecord): E {
  Object.defineProperty(error, REFUSAL, { value: refusal, configurable: true });
  return error;
}

/** The core refusal @p error carries, or undefined. */
export function refusalOf(error: unknown): RefusalRecord | undefined {
  if (error === null || typeof error !== 'object') {
    return undefined;
  }
  return (error as { [REFUSAL]?: RefusalRecord })[REFUSAL];
}

/**
 * Replaces the refusal @p error carries with @p refusal, whose text @p text is, keeping the error's
 * class, code and any entry prefix already in front of the old text.
 */
export function retextRefusal(error: Error, refusal: RefusalRecord, text: string): void {
  const named = error as NamedError;
  const old = refusalOf(error)?.text ?? '';
  const swap = (message: string): string =>
    message.endsWith(old) ? `${message.slice(0, message.length - old.length)}${text}` : message;
  const before = `${error.name}: ${error.message}`;
  error.message = swap(error.message);
  const stack = error.stack;
  if (typeof stack === 'string' && stack.startsWith(before)) {
    error.stack = `${error.name}: ${error.message}${stack.slice(before.length)}`;
  }
  const unprefixed = named[UNPREFIXED];
  if (unprefixed !== undefined) {
    mark(named, swap(unprefixed));
  }
  const previous = refusalOf(error);
  const raw = previous?.raw ?? refusal.raw;
  attachRefusal(error, {
    ...refusal,
    ...(raw === undefined ? {} : { raw }),
    cEntry: previous?.cEntry ?? null,
  });
}

export function refuse(text: string): RangeError {
  return mark(new RangeError(text), text);
}

/** Refusal of a wrong-typed argument; the entry the caller invoked prefixes its name. */
export function refuseType(text: string): TypeError {
  return mark(new TypeError(text), text);
}

/**
 * Refusal of a key outside a declared set, named by its path from the request
 * root and listing the set deduplicated in byte order, the order every layer
 * lists accepted keys in; `also` names the one other place the key is read.
 */
export function refuseUnknownKey(
  path: string,
  allowed: readonly string[],
  also?: string,
): TypeError {
  const listed = [...new Set(allowed)].sort().join(', ');
  const expected = also === undefined ? listed : `${listed}, or ${also}`;
  return refuseType(`unknown key ${path} (expected ${expected})`);
}

/** Refuse the first key of `record` outside `allowed`, named by its path (the key alone when `path` is empty). */
export function assertDeclaredKeys(record: object, allowed: readonly string[], path: string): void {
  for (const key of Object.keys(record)) {
    if (!allowed.includes(key)) {
      throw refuseUnknownKey(path === '' ? key : `${path}.${key}`, allowed);
    }
  }
}

/** A call the object's current state refuses (released, closed, not prepared). */
export function invalidState(text: string): SonareError {
  return codedError(ErrorCode.InvalidState, text);
}

/** A coded failure that is not an argument refusal; the entry prefixes its name. */
export function codedError(
  code: ErrorCode,
  text: string,
  options?: { cause?: unknown; reason?: ErrorReason },
): SonareError {
  return mark(new SonareError(code, text, options), text);
}

/** Prefix an error a native call raised with its public entry name, marking it bare first. */
export function nameNativeError(error: unknown, entry: string): unknown {
  if (!(error instanceof Error) || (error as NamedError)[FOREIGN]) {
    return error;
  }
  const named = error as NamedError;
  let text = named[UNPREFIXED];
  if (text === undefined) {
    text = error.message;
    mark(named, text);
  }
  setPrefix(named, text, entry);
  return error;
}

function rename(error: unknown, entry: string): unknown {
  if (error instanceof Error && !(error as NamedError)[FOREIGN]) {
    const text = (error as NamedError)[UNPREFIXED];
    if (text !== undefined) {
      setPrefix(error, text, entry);
    }
  }
  return error;
}

// One renamed promise per settled source, so a member that hands back the same promise twice still does.
const renamedPromises = new WeakMap<Promise<unknown>, Promise<unknown>>();

function renameRejection<T>(result: T, entry: string): T {
  if (!(result instanceof Promise)) {
    return result;
  }
  let renamed = renamedPromises.get(result);
  if (renamed === undefined) {
    renamed = result.catch((error: unknown) => {
      throw rename(error, entry);
    });
    renamedPromises.set(result, renamed);
  }
  return renamed as T;
}

/**
 * Run `body` as the public entry `entry`: a marked refusal raised inside it,
 * synchronously or through the returned promise, carries `entry` as its
 * prefix. Used where an exported function forwards to another public one.
 */
export function withEntry<T>(entry: string, body: () => T): T {
  let result: T;
  try {
    result = body();
  } catch (error) {
    throw rename(error, entry);
  }
  return renameRejection(result, entry);
}

type Callable = (...args: unknown[]) => unknown;

function named(fn: Callable, entry: string): Callable {
  const wrapper = function (this: unknown) {
    try {
      // biome-ignore lint/complexity/noArguments: forwarded without an array allocation
      return renameRejection(Reflect.apply(fn, this, arguments), entry);
    } catch (error) {
      throw rename(error, entry);
    }
  };
  // The member keeps the arity and name a consumer reads off it.
  Object.defineProperty(wrapper, 'length', { value: fn.length, configurable: true });
  Object.defineProperty(wrapper, 'name', { value: fn.name, configurable: true });
  return wrapper;
}

const NOT_MEMBERS = new Set<PropertyKey>(['constructor', 'prototype', 'length', 'name']);

function wrapMembers(target: object, owner: string, skip: ReadonlySet<string>): void {
  for (const prop of Object.getOwnPropertyNames(target)) {
    if (NOT_MEMBERS.has(prop) || skip.has(prop)) {
      continue;
    }
    const d = Object.getOwnPropertyDescriptor(target, prop);
    if (!d?.configurable) {
      continue;
    }
    const entry = `${owner}.${prop}`;
    if (typeof d.value === 'function') {
      Object.defineProperty(target, prop, { ...d, value: named(d.value as Callable, entry) });
    } else if (d.get || d.set) {
      Object.defineProperty(target, prop, {
        ...d,
        get: d.get && (named(d.get as Callable, entry) as () => unknown),
        set: d.set && (named(d.set as Callable, entry) as (v: unknown) => void),
      });
    }
  }
}

/**
 * Class decorator naming every public member on its refusals: methods,
 * accessors and statics as `<name>.<member>`, the constructor as `<name>`.
 * `skip` lists the per-block realtime methods left unwrapped so the audio path
 * pays nothing; the conformance check prints that list.
 */
export function entryClass(name: string, skip: readonly string[] = []) {
  // The context is unused and left untyped: ClassDecoratorContext refuses a class with a private constructor.
  return <C extends object>(target: C, _context: unknown): C => {
    const skipped = new Set(skip);
    const cls = target as unknown as { prototype: object } & (new (...args: unknown[]) => object);
    wrapMembers(cls.prototype, name, skipped);
    wrapMembers(cls, name, skipped);
    // A proxy rather than a subclass keeps the exported class's prototype the one its instances carry.
    const named: typeof cls = new Proxy(cls, {
      construct(base, args, newTarget): object {
        try {
          return Reflect.construct(base, args, newTarget === named ? base : newTarget);
        } catch (error) {
          throw rename(error, name);
        }
      },
    });
    Object.defineProperty(cls.prototype, 'constructor', {
      value: named,
      writable: true,
      configurable: true,
    });
    return named as unknown as C;
  };
}
