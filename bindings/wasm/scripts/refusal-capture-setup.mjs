// Records every error that crosses the facade's public surface while the suite runs, for
// tools/api/refusal_spelling.py. Loaded only by vitest.refusal-capture.config.mjs.
import { appendFileSync } from 'node:fs';
import { types } from 'node:util';
import { vi } from 'vitest';

const OUT = process.env.SONARE_REFUSAL_CAPTURE;
if (OUT) {
  Object.defineProperty(globalThis, Symbol.for('libsonare.captureRefusal'), {
    value: true,
    configurable: true,
  });
}

function argFacts(args) {
  const keys = new Set();
  const strings = new Set();
  const seen = new WeakSet();
  const walk = (v, depth) => {
    if (typeof v === 'string') {
      if (v.length < 200) strings.add(v);
      return;
    }
    // A proxy is a test's instrument (a read counter) or a wrapped instance: reading it is not ours.
    if (v === null || typeof v !== 'object' || types.isProxy(v)) return;
    if (depth > 4 || seen.has(v) || ArrayBuffer.isView(v)) return;
    seen.add(v);
    if (Array.isArray(v)) {
      for (let i = 0; i < Math.min(v.length, 64); i++) {
        const d = Object.getOwnPropertyDescriptor(v, i);
        if (d !== undefined && 'value' in d) walk(d.value, depth + 1);
      }
      return;
    }
    if (v instanceof Map) {
      for (const [k, x] of v) {
        if (typeof k === 'string') keys.add(k);
        walk(x, depth + 1);
      }
      return;
    }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return;
    for (const [k, d] of Object.entries(Object.getOwnPropertyDescriptors(v))) {
      if (!d.enumerable) continue;
      keys.add(k);
      if ('value' in d) walk(d.value, depth + 1);
    }
  };
  for (const a of args) walk(a, 0);
  return { keys: [...keys], strings: [...strings] };
}

// The call's arguments as JSON the parity check can join on: a value JSON cannot hold keeps its
// identity as a tagged string, and bulk data is summarized rather than written out.
// FNV-1a over a view's bytes, so two buffers of one length but different content never join.
function byteHash(view) {
  const bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  let h = 0x811c9dc5;
  for (const b of bytes) h = Math.imul(h ^ b, 0x01000193) >>> 0;
  return h.toString(16);
}

function argValues(args) {
  const seen = new WeakSet();
  const encode = (v, depth) => {
    if (v === undefined) return '$undefined';
    if (typeof v === 'number') return Number.isFinite(v) ? v : `$${v}`;
    if (typeof v === 'bigint') return `$bigint:${v}`;
    if (typeof v === 'function') return '$function';
    if (typeof v === 'symbol') return '$symbol';
    if (typeof v === 'string') return v.length < 200 ? v : `$string:${v.length}`;
    if (v === null || typeof v !== 'object') return v;
    if (types.isProxy(v)) return '$proxy';
    if (ArrayBuffer.isView(v)) return `$${v.constructor.name}:${v.length ?? v.byteLength}:${byteHash(v)}`;
    if (v instanceof ArrayBuffer) return `$ArrayBuffer:${v.byteLength}`;
    if (depth > 4 || seen.has(v)) return '$deep';
    seen.add(v);
    if (Array.isArray(v)) {
      if (v.length > 64) return `$Array:${v.length}`;
      return Array.from({ length: v.length }, (_, i) => {
        const d = Object.getOwnPropertyDescriptor(v, i);
        return d === undefined ? '$hole' : 'value' in d ? encode(d.value, depth + 1) : '$accessor';
      });
    }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return `$${v.constructor?.name ?? 'object'}`;
    const out = {};
    for (const [k, d] of Object.entries(Object.getOwnPropertyDescriptors(v))) {
      if (d.enumerable) out[k] = 'value' in d ? encode(d.value, depth + 1) : '$accessor';
    }
    return out;
  };
  return args.map((a) => encode(a, 0));
}

// The core refusal a native translation point attached, in the caller's spelling.
const REFUSAL = Symbol.for('libsonare.refusal');

function refusalFacts(error) {
  const r = error[REFUSAL];
  if (r === undefined) return undefined;
  return {
    code: r.code,
    form: r.form,
    subjects: [...r.subjects],
    cEntry: r.cEntry ?? null,
    raw:
      r.raw === undefined
        ? undefined
        : {
            code: r.raw.code,
            subjects: [...r.raw.subjects],
            callerSpelled: r.raw.callerSpelled,
            fromC: r.raw.fromC,
            flattened: r.raw.flattened,
          },
  };
}

// A coded error is classed by its brand: the Node addon raises it as a plain Error named SonareError.
function isBranded(error) {
  return error.name === 'SonareError' && typeof error.code === 'number';
}

function record(entry, error, args) {
  if (!(error instanceof Error)) return;
  const file = globalThis.__vitest_worker__?.filepath ?? '';
  const requireNative = globalThis[Symbol.for('libsonare.requireNativeRefusal')] === true;
  const row = {
    entry,
    requireNative,
    errorClass: isBranded(error) ? 'SonareError' : error.constructor?.name,
    message: error.message,
    refusal: refusalFacts(error),
    ...argFacts(args),
    args: argValues(args),
    file: file.replace(/.*\/tests\//, 'tests/'),
  };
  appendFileSync(OUT, `${JSON.stringify(row)}\n`);
}

// Recording never replaces the error the caller sees.
function recordQuietly(entry, error, args) {
  try {
    record(entry, error, args);
  } catch {}
}

// Wraps every function and class export; instances a wrapped call returns are wrapped too, so a
// refusal from a method is recorded under Class.method. Calls inside the package are not seen.
async function wrapModule(importOriginal) {
  const mod = await importOriginal();
  const classes = new Map();
  const proxies = new WeakMap();
  const targets = new WeakMap();
  const settled = new WeakMap();
  // A wrapped instance handed back to the package is unwrapped first, so a native call or a private
  // field sees the real object; a container is copied only when it holds one.
  // Only data properties are read: a test's accessor (a throwing getter) is left to the package.
  const unwrap = (v, depth = 0) => {
    if (v === null || typeof v !== 'object') return v;
    const target = targets.get(v);
    if (target !== undefined) return target;
    if (depth > 3 || types.isProxy(v) || ArrayBuffer.isView(v)) return v;
    const isList = Array.isArray(v);
    if (isList && v.length > 64) return v;
    const proto = Object.getPrototypeOf(v);
    if (!isList && proto !== Object.prototype && proto !== null) return v;
    let copy;
    for (const [k, d] of Object.entries(Object.getOwnPropertyDescriptors(v))) {
      if (!('value' in d)) continue;
      const u = unwrap(d.value, depth + 1);
      if (u !== d.value) {
        copy ??= isList ? [...v] : { ...v };
        copy[k] = u;
      }
    }
    return copy ?? v;
  };
  const unwrapAll = (args) => args.map((a) => unwrap(a));
  const isClass = (f) => typeof f === 'function' && /^class\b/.test(Function.prototype.toString.call(f));
  const wrapResult = (v) => {
    if (v instanceof Promise) return v.then(wrapResult);
    if (v !== null && typeof v === 'object' && classes.has(v.constructor)) {
      return wrapInstance(v, classes.get(v.constructor));
    }
    return v;
  };
  const call = (entry, run, args) => {
    let result;
    try {
      result = run();
    } catch (error) {
      recordQuietly(entry, error, args);
      throw error;
    }
    if (result instanceof Promise) {
      // One wrapped promise per original, so a call that hands back a cached promise still does.
      const known = settled.get(result);
      if (known) return known;
      const wrapped = result.then(wrapResult, (error) => {
        recordQuietly(entry, error, args);
        throw error;
      });
      settled.set(result, wrapped);
      return wrapped;
    }
    return wrapResult(result);
  };
  // One wrapper per method function, shared by every instance, so a method reads as the same function
  // on two instances as it does unmocked; the instance comes from the receiver.
  const methodWrappers = new WeakMap();
  const methodWrapper = (entry, original) => {
    const known = methodWrappers.get(original);
    if (known) return known;
    const wrapper = function (...args) {
      const self = targets.get(this) ?? this;
      return call(entry, () => Reflect.apply(original, self, unwrapAll(args)), args);
    };
    methodWrappers.set(original, wrapper);
    return wrapper;
  };
  const wrapInstance = (target, cls) => {
    const known = proxies.get(target);
    if (known) return known;
    const proxy = new Proxy(target, {
      get(t, key) {
        const v = Reflect.get(t, key, t);
        if (typeof key !== 'string' || typeof v !== 'function' || key === 'constructor') return v;
        return methodWrapper(`${cls}.${key}`, v);
      },
    });
    proxies.set(target, proxy);
    targets.set(proxy, target);
    return proxy;
  };
  const out = {};
  for (const [name, v] of Object.entries(mod)) {
    if (typeof v !== 'function') {
      out[name] = v;
      continue;
    }
    const own = v.prototype ? Object.getOwnPropertyNames(v.prototype).filter((k) => k !== 'constructor') : [];
    if (isClass(v) || own.length > 0) classes.set(v, name);
    out[name] = new Proxy(v, {
      apply: (t, self, args) => call(name, () => Reflect.apply(t, unwrap(self), unwrapAll(args)), args),
      construct: (t, args, nt) =>
        call(`${name}.constructor`, () => Reflect.construct(t, unwrapAll(args), nt === out[name] ? t : nt), args),
      get(t, key) {
        const s = Reflect.get(t, key);
        if (typeof key !== 'string' || typeof s !== 'function' || key === 'prototype' || classes.has(s)) return s;
        return (...args) => call(`${name}.${key}`, () => Reflect.apply(s, t, unwrapAll(args)), args);
      },
    });
  }
  // A name the package does not export reads undefined, as it does unmocked. vitest's mock reads the
  // name and then asks `in`, throwing when absent; only that `in`, straight after the read of the
  // same missing name, answers true, so a test's own `name in api` still sees the name absent.
  let justRead;
  return new Proxy(out, {
    get: (t, key) => {
      justRead = key in t ? undefined : key;
      return Reflect.get(t, key);
    },
    has: (t, key) => {
      const answer = key === justRead && key !== 'then' ? true : Reflect.has(t, key);
      justRead = undefined;
      return answer;
    },
  });
}

vi.mock('../src/index.ts', wrapModule);
vi.mock('../dist/index.js', wrapModule);
