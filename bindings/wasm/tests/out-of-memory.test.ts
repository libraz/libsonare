import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, fixLength, init, isSonareError, type SonareError } from '../dist/index.js';

// The module's linear memory, captured at instantiation: it is not reachable through the package.
let memory: WebAssembly.Memory | undefined;

function captureMemory(): void {
  for (const name of ['instantiate', 'instantiateStreaming'] as const) {
    const original = WebAssembly[name] as (...args: unknown[]) => Promise<unknown>;
    (WebAssembly as unknown as Record<string, unknown>)[name] = async (...args: unknown[]) => {
      const result = (await original.apply(WebAssembly, args)) as {
        instance?: WebAssembly.Instance;
      } & WebAssembly.Instance;
      const instance = result.instance ?? result;
      for (const value of Object.values(instance.exports)) {
        if (value instanceof WebAssembly.Memory) {
          memory = value;
        }
      }
      return result;
    };
  }
}

function heapBytes(): number {
  if (memory === undefined) {
    throw new Error('the module memory was not captured');
  }
  return memory.buffer.byteLength;
}

function outOfMemory(call: () => unknown): SonareError {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  expect((caught as SonareError).code).toBe(ErrorCode.OutOfMemory);
  return caught as SonareError;
}

// A request larger than the 2 GiB linear-memory cap fails to grow the heap. With memory growth on,
// malloc returns null instead of aborting, so the failure surfaces as a coded error. Every unit
// unwinds, so what the failed call held before the allocation that failed is freed on the way out.
describe('an allocation past the linear-memory cap', () => {
  beforeAll(async () => {
    captureMemory();
    await init();
  });

  it('is a coded OutOfMemory, and the module still answers the next call', () => {
    const error = outOfMemory(() =>
      fixLength({ values: new Float32Array(4), targetSize: 700_000_000 }),
    );
    expect(error.message).toBe('fixLength: Out of memory');
    expect(Array.from(fixLength({ values: new Float32Array([1, 2]), targetSize: 3 }))).toEqual([
      1, 2, 0,
    ]);
  });

  it('returns what the failed call held, so repeating it does not grow the heap', () => {
    // 64 MiB of input is copied into the heap before the output allocation fails; a frame that
    // did not unwind would keep that copy, and each repeat would grow memory by as much again.
    // The heap is read as the memory size, which never shrinks, so only growth is visible.
    const values = new Float32Array(16 * 1024 * 1024).fill(0.25);
    outOfMemory(() => fixLength({ values, targetSize: 700_000_000 }));
    const afterFirst = heapBytes();
    outOfMemory(() => fixLength({ values, targetSize: 700_000_000 }));
    outOfMemory(() => fixLength({ values, targetSize: 700_000_000 }));
    expect(heapBytes()).toBe(afterFirst);

    // A call of the same input size then succeeds, and repeating it reuses its own memory.
    const fitted = fixLength({ values, targetSize: values.length });
    expect(fitted.length).toBe(values.length);
    expect(fitted[values.length - 1]).toBe(0.25);
    const afterSuccess = heapBytes();
    fixLength({ values, targetSize: values.length });
    expect(heapBytes()).toBe(afterSuccess);
  });
});
