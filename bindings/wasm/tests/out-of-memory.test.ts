import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, fixLength, init, isSonareError, type SonareError } from '../dist/index.js';

// A request larger than the 2 GiB linear-memory cap fails to grow the heap. With memory growth on,
// malloc returns null instead of aborting, so the failure surfaces as a coded error and the
// module keeps answering; an abort would kill the instance for every later call.
describe('an allocation past the linear-memory cap', () => {
  beforeAll(async () => {
    await init();
  });

  it('is a coded OutOfMemory, and the module still answers the next call', () => {
    let caught: unknown;
    try {
      fixLength({ values: new Float32Array(4), targetSize: 700_000_000 });
    } catch (error) {
      caught = error;
    }
    expect(isSonareError(caught)).toBe(true);
    expect((caught as SonareError).code).toBe(ErrorCode.OutOfMemory);
    expect((caught as SonareError).message).toBe('fixLength: Out of memory');
    expect(Array.from(fixLength({ values: new Float32Array([1, 2]), targetSize: 3 }))).toEqual([
      1, 2, 0,
    ]);
  });
});
