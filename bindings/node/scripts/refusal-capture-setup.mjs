// Records every error that crosses the facade's public surface while the suite runs, for
// tools/api/refusal_spelling.py. Loaded only by vitest.refusal-capture.config.mjs.
import { appendFileSync } from 'node:fs';
import { vi } from 'vitest';

const OUT = process.env.SONARE_REFUSAL_CAPTURE;

function argFacts(args) {
  const keys = new Set();
  const strings = new Set();
  const seen = new WeakSet();
  const walk = (v, depth) => {
    if (typeof v === 'string') {
      if (v.length < 200) strings.add(v);
      return;
    }
    if (v === null || typeof v !== 'object' || depth > 4 || seen.has(v) || ArrayBuffer.isView(v)) return;
    seen.add(v);
    if (Array.isArray(v)) {
      for (const x of v.slice(0, 64)) walk(x, depth + 1);
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
    for (const [k, x] of Object.entries(v)) {
      keys.add(k);
      walk(x, depth + 1);
    }
  };
  for (const a of args) walk(a, 0);
  return { keys: [...keys], strings: [...strings] };
}

// The core refusal a native translation point attached, in the caller's spelling.
const REFUSAL = Symbol.for('libsonare.refusal');

function refusalFacts(error) {
  const r = error[REFUSAL];
  if (r === undefined) return undefined;
  return { code: r.code, form: r.form, subjects: [...r.subjects], cEntry: r.cEntry ?? null };
}

function record(entry, error, args) {
  if (!(error instanceof Error)) return;
  const file = globalThis.__vitest_worker__?.filepath ?? '';
  const row = {
    entry,
    errorClass: error.constructor?.name,
    message: error.message,
    refusal: refusalFacts(error),
    ...argFacts(args),
    file: file.replace(/.*\/tests\//, 'tests/'),
  };
  appendFileSync(OUT, `${JSON.stringify(row)}\n`);
}

// Wraps every function and class export; instances a wrapped call returns are wrapped too, so a
// refusal from a method is recorded under Class.method. Calls inside the package are not seen.
async function wrapModule(importOriginal) {
  const mod = await importOriginal();
  const classes = new Map();
  const proxies = new WeakMap();
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
      record(entry, error, args);
      throw error;
    }
    if (result instanceof Promise) {
      return result.then(wrapResult, (error) => {
        record(entry, error, args);
        throw error;
      });
    }
    return wrapResult(result);
  };
  const wrapInstance = (target, cls) => {
    const known = proxies.get(target);
    if (known) return known;
    const proxy = new Proxy(target, {
      get(t, key) {
        const v = Reflect.get(t, key, t);
        if (typeof key !== 'string' || typeof v !== 'function' || key === 'constructor') return v;
        return (...args) => call(`${cls}.${key}`, () => Reflect.apply(v, t, args), args);
      },
    });
    proxies.set(target, proxy);
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
      apply: (t, self, args) => call(name, () => Reflect.apply(t, self, args), args),
      construct: (t, args, nt) =>
        call(`${name}.constructor`, () => Reflect.construct(t, args, nt === out[name] ? t : nt), args),
      get(t, key) {
        const s = Reflect.get(t, key);
        if (typeof key !== 'string' || typeof s !== 'function' || key === 'prototype' || classes.has(s)) return s;
        return (...args) => call(`${name}.${key}`, () => Reflect.apply(s, t, args), args);
      },
    });
  }
  return out;
}

vi.mock('../src/index.ts', wrapModule);
