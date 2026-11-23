import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  fixLength,
  init,
  isSonareError,
  SampleBank,
  type SonareError,
  stft,
} from '../dist/index.js';

const MIB = 1024 * 1024;

// What the heap can still hand out, as the memory refusal of an oversized request states it.
function headroomMib(): number {
  let message = '';
  try {
    fixLength({ values: new Float32Array(4), targetSize: 0xffffffff });
  } catch (error) {
    message = (error as Error).message;
  }
  const match = message.match(/more than the (\d+\.\d) MiB available/);
  expect(match).not.toBeNull();
  return Number(match?.[1]);
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

// A call that fits its certain need (the input copy and the complex spectrum) but not the
// magnitude and power it then derives runs out of memory past the lower bound. The heap is
// filled first by samples a bank holds, so the failing call is small and fails quickly.
describe('an allocation past the lower bound', () => {
  const banks: SampleBank[] = [];

  beforeAll(async () => {
    await init();
    // A bank's sample pool is one growing buffer with a size cap, so a bank that refuses the next
    // block (past its cap, or with no room left to grow its pool) is set aside for a fresh one.
    const block = new Float32Array(8 * MIB).fill(0.25);
    banks.push(new SampleBank());
    let fresh = true;
    while (headroomMib() > 320) {
      try {
        banks[banks.length - 1].addSample(block);
        fresh = false;
      } catch (error) {
        if (!isSonareError(error) || error.code !== ErrorCode.OutOfMemory || fresh) {
          throw error;
        }
        banks.push(new SampleBank());
        fresh = true;
      }
    }
  });

  afterAll(() => {
    for (const bank of banks) {
      bank[Symbol.dispose]();
    }
  });

  it('is a coded OutOfMemory naming the input, and releases what the call held', () => {
    // A first call builds the transform plans the module keeps for later calls of the same size.
    expect(
      stft({ samples: new Float32Array(48000), sampleRate: 48000 }).magnitude.cols,
    ).toBeGreaterThan(0);
    const before = headroomMib();
    // The bound is about 20 bytes per sample and the call holds about 36, so a need of 80% of
    // the headroom is admitted and the call then fails.
    const n = Math.floor((0.8 * before * MIB) / 20.1);
    const samples = new Float32Array(n).fill(0.1);
    const error = outOfMemory(() => stft({ samples, sampleRate: 48000 }));
    expect(error.message).toBe(
      `stft: Out of memory; samples is ${n} samples (${(n / 48000).toFixed(2)} s at 48000 Hz)`,
    );

    // Every frame unwinds, so what the call held before the allocation that failed is free again.
    expect(headroomMib()).toBe(before);
    outOfMemory(() => stft({ samples, sampleRate: 48000 }));
    expect(headroomMib()).toBe(before);

    // The module still answers, and a call within the headroom succeeds.
    const small = stft({ samples: samples.subarray(0, 48000), sampleRate: 48000 });
    expect(small.magnitude.cols).toBeGreaterThan(0);
  });
});
