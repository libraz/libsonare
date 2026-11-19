import { createRequire } from 'node:module';
import { checkAbiVersion } from './abi.js';
import { nameNativeError } from './internal/entry.js';
import { NATIVE_ENTRIES } from './internal/native_entries.js';

const require = createRequire(import.meta.url);
const raw = require('../build/Release/sonare-node.node');
checkAbiVersion(raw.abiVersion?.());

/** Marks the addon object whose functions and prototypes already carry the entry wrapper. */
const WRAPPED = Symbol.for('libsonare.node.entryWrapped');

type NativeFunction = (...args: unknown[]) => unknown;

function entryFor(key: string): string {
  return NATIVE_ENTRIES[key] ?? key;
}

// `arguments` rather than a rest parameter: V8 forwards it without allocating, and
// the per-block realtime methods go through this wrapper.
function wrapFunction(fn: NativeFunction, entry: string): NativeFunction {
  return function (this: unknown) {
    try {
      // biome-ignore lint/complexity/noArguments: forwarded without an array allocation on the per-block path
      return Reflect.apply(fn, this, arguments);
    } catch (error) {
      throw nameNativeError(error, entry);
    }
  };
}

function wrapMembers(target: object, owner: string): void {
  for (const name of Object.getOwnPropertyNames(target)) {
    if (name === 'constructor' || name === 'prototype' || name === 'length' || name === 'name') {
      continue;
    }
    const descriptor = Object.getOwnPropertyDescriptor(target, name);
    if (!descriptor || typeof descriptor.value !== 'function' || !descriptor.configurable) {
      continue;
    }
    const entry = entryFor(`${owner}.${name}`);
    Object.defineProperty(target, name, {
      ...descriptor,
      value: wrapFunction(descriptor.value as NativeFunction, entry),
    });
  }
}

function isNativeClass(value: NativeFunction): boolean {
  const proto = (value as { prototype?: object }).prototype;
  return proto !== undefined && Object.getOwnPropertyNames(proto).some((n) => n !== 'constructor');
}

function wrapClass(cls: NativeFunction, name: string): NativeFunction {
  wrapMembers(cls.prototype as object, name);
  wrapMembers(cls, name);
  const entry = entryFor(name);
  return new Proxy(cls, {
    construct(target, args, newTarget) {
      try {
        return Reflect.construct(target, args, newTarget) as object;
      } catch (error) {
        throw nameNativeError(error, entry);
      }
    },
  });
}

function wrapAddon(module: Record<string | symbol, unknown>): Record<string, unknown> {
  const existing = module[WRAPPED] as Record<string, unknown> | undefined;
  if (existing) {
    return existing;
  }
  const wrapped: Record<string, unknown> = {};
  for (const key of Object.keys(module)) {
    const value = module[key];
    if (typeof value !== 'function') {
      wrapped[key] = value;
    } else if (isNativeClass(value as NativeFunction)) {
      wrapped[key] = wrapClass(value as NativeFunction, key);
    } else {
      wrapped[key] = wrapFunction(value as NativeFunction, entryFor(key));
    }
  }
  Object.defineProperty(module, WRAPPED, { value: wrapped });
  return wrapped;
}

// biome-ignore lint/suspicious/noExplicitAny: the addon has no type declarations; facades type each call site
export const addon: any = wrapAddon(raw);
