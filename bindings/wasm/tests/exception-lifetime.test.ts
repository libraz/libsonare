/**
 * Lifetime of a native exception object surfaced across the WASM boundary.
 *
 * emscripten's `__cxa_throw` takes a reference on the exception object and
 * rethrows the raw pointer into JS. No C++ frame catches it, so that reference
 * is the only one and nothing drops it on its own — the module wrapper has to.
 * Without the release, every rejected input leaks its exception object for the
 * lifetime of the module, and rejection-as-control-flow (re-validating markers
 * or clips on every edit, a scrub handle seeking on every pointer move) is a
 * documented use of this API.
 *
 * The observable is the allocator, not a heap statistic: the HEAP views are not
 * exported onto the module and linear memory only grows in large steps, so a
 * few-hundred-bytes-per-throw leak hides behind that granularity. A released
 * object is instead handed straight back by the allocator, so the exception
 * pointer stops moving entirely. The rejection driven here (`chromaMethod: 5`)
 * is checked before any audio is copied, so the exception object is the only
 * allocation in flight and the address is exactly repeatable.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, isSonareError } from '../src/index';
import { getSonareModule } from '../src/module_state';
import type { SonareModule } from '../src/sonare.js';

/** The unwrapped module: it throws the raw pointer instead of a SonareError. */
let raw: SonareModule;

type ReleaseHelper = { sonareReleaseException?: (ptr: number) => void };

const SAMPLES = new Float32Array(8);
const REJECTED_CHROMA_METHOD = 5;

beforeAll(async () => {
  const createModule = (await import('../dist/sonare.js')).default;
  await init({
    moduleFactory: async (options) => {
      raw = await createModule(options);
      return raw;
    },
  });
});

/** Drives the rejection on @p module. `chromaMethod` is validated first. */
function reject(module: SonareModule): unknown {
  try {
    module.detectChords(
      SAMPLES,
      22050,
      0.5,
      0.25,
      0.5,
      false,
      2048,
      512,
      false,
      false,
      8,
      false,
      0,
      0,
      false,
      REJECTED_CHROMA_METHOD,
    );
  } catch (error) {
    return error;
  }
  return undefined;
}

/** Rejects on the UNWRAPPED module; the caller owns the returned reference. */
function throwUnwrapped(): number {
  const caught = reject(raw);
  expect(typeof caught, 'an unwrapped native throw surfaces as a pointer number').toBe('number');
  return caught as number;
}

function release(ptr: number): void {
  (raw as unknown as ReleaseHelper).sonareReleaseException?.(ptr);
}

/** One released round trip; returns the address the object occupied. */
function throwAndRelease(): number {
  const ptr = throwUnwrapped();
  release(ptr);
  return ptr;
}

describe('native exception objects are released once surfaced', () => {
  beforeAll(() => {
    // The first few rounds settle the allocator (the embind glue makes its own
    // one-off allocations); every address after that is the steady state.
    for (let i = 0; i < 8; i++) {
      throwAndRelease();
    }
  });

  it('ships the release helper in the built module', () => {
    // The decoder alone is not enough: reading the pointer does not drop the
    // reference emscripten took, so the binding has to exist to be callable.
    expect(typeof (raw as unknown as ReleaseHelper).sonareReleaseException).toBe('function');
  });

  it('hands the same address back to every released throw', () => {
    const addresses = new Set<number>();
    for (let i = 0; i < 64; i++) {
      addresses.add(throwAndRelease());
    }
    expect(addresses.size, `expected one reused address, saw ${[...addresses].join(', ')}`).toBe(1);
  });

  it('does not advance the allocator across many wrapper-converted throws', () => {
    const before = throwAndRelease();

    // The public path: the wrapper in module_state.ts catches the pointer,
    // decodes it into a SonareError, and releases it in a finally.
    const wrapped = getSonareModule();
    for (let i = 0; i < 200; i++) {
      const error = reject(wrapped);
      expect(isSonareError(error)).toBe(true);
    }

    expect(throwAndRelease()).toBe(before);
  });

  it('still decodes the native message, which the release invalidates', () => {
    // The object is freed when the count reaches zero, so the message has to be
    // read before the release. A regression would surface as the generic
    // "libsonare native exception (<ptr>)" fallback text.
    const error = reject(getSonareModule());
    expect(isSonareError(error)).toBe(true);
    expect((error as Error).message).toMatch(/chromaMethod/);
  });

  it('detects a leak, so the assertions above are not vacuous', () => {
    // Deliberately skip the release: the exception objects pile up and the
    // allocator has to keep handing back fresh addresses. Runs last because it
    // permanently leaks into this module instance.
    const first = throwUnwrapped();
    const addresses = new Set<number>([first]);
    for (let i = 0; i < 32; i++) {
      addresses.add(throwUnwrapped());
    }
    expect(addresses.size, 'an unreleased throw must not reuse the same address').toBeGreaterThan(
      1,
    );
  });
});
