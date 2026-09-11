/**
 * One input, all three surfaces, one error code.
 *
 * `validation.ts` justifies raising a branded `SonareError` where its neighbours
 * raise `RangeError` by asserting that "the WASM and Python surfaces answer the
 * same input with the same code". That sentence is the whole reason the class
 * differs, and until this file it was true only by inspection — which decays
 * silently, because nothing about editing the WASM validator tells you a Node
 * docblock depends on it. This drives all three and compares the codes.
 *
 * The input has to be chosen, not picked. A value past the signed 32-bit range
 * is only interesting if it narrows into a value the downstream guards ACCEPT:
 * `2 ** 32 + 1` wraps to 1, which is a positive odd kernel under the ceiling, and
 * `2 ** 32` wraps to 0, which the percussive-event config reads as "use the
 * default". Both therefore had a successful outcome waiting for them. A value
 * that wraps onto something a downstream rule rejects anyway — `2 ** 32 + 1` as
 * `nFft`, which lands on 1 and dies on the even-size rule — goes red for a reason
 * that has nothing to do with narrowing, which reads as a pass of the test you
 * meant to write. The positive controls below pin that property rather than
 * leaving it to the comment: each wrapped value is driven on its own and must
 * succeed.
 *
 * The Python arm is spawned rather than skipped when unavailable. A skip here
 * would restore exactly the hole this file exists to close.
 */

import { execFileSync } from 'node:child_process';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  extractPercussiveEvents as wasmExtractPercussiveEvents,
  hpss as wasmHpss,
  init as wasmInit,
} from '../../wasm/dist/index.js';
import {
  ErrorCode,
  extractPercussiveEvents as nodeExtractPercussiveEvents,
  hpss as nodeHpss,
} from '../src/index.js';

const PYTHON_PACKAGE = new URL('../../python/', import.meta.url).pathname;

const sampleRate = 22050;
const tone = new Float32Array(4096).map((_, i) => Math.sin((2 * Math.PI * 440 * i) / sampleRate));

/** A click train over a quiet bed, so the percussive detector returns events. */
const hits = (() => {
  const n = sampleRate;
  const out = new Float32Array(n).map((_, i) => 0.02 * Math.sin((2 * Math.PI * 220 * i) / n));
  let state = 7;
  const next = (): number => {
    state = (state * 1103515245 + 12345) % 2147483648;
    return state / 1073741824 - 1;
  };
  for (let start = 1000; start < n - 1000; start += 2205) {
    for (let j = 0; j < 200; j++) {
      out[start + j] += 0.9 * Math.exp(-j / 25) * next();
    }
  }
  return out;
})();

/** The code a surface answered with, or a marker that says it did not refuse. */
type Answer = number | 'no refusal';

const codeOf = (run: () => unknown): Answer => {
  try {
    run();
    return 'no refusal';
  } catch (error) {
    const code = (error as { code?: unknown }).code;
    return typeof code === 'number' ? code : Number.NaN;
  }
};

/**
 * The Python surface, out of process. Reports the exception's `.code` on one
 * line, so a missing attribute or a silent success is as visible as a wrong
 * code rather than collapsing into a spawn failure.
 */
function pythonCode(call: string): Answer {
  const script = [
    'import libsonare',
    'try:',
    `    ${call}`,
    '    print("no refusal")',
    'except Exception as error:',
    '    print(getattr(error, "code", "no code"))',
  ].join('\n');
  const stdout = execFileSync('rye', ['run', 'python', '-c', script], {
    cwd: PYTHON_PACKAGE,
    encoding: 'utf8',
  }).trim();
  if (stdout === 'no refusal') {
    return 'no refusal';
  }
  const parsed = Number(stdout);
  expect(Number.isInteger(parsed), `python reported ${stdout} instead of a numeric code`).toBe(
    true,
  );
  return parsed;
}

beforeAll(async () => {
  await wasmInit();
});

describe('a kernel past the signed range reports one code on every surface', () => {
  // Each entry drives the SAME value through the three facades. The wrapped
  // value is carried alongside so the control below can prove it is accepted.
  const cases = [
    {
      name: 'hpss kernelHarmonic',
      passed: 2 ** 32 + 1,
      wrapsTo: 1,
      node: (kernel: number) => nodeHpss({ samples: tone, sampleRate, kernelHarmonic: kernel }),
      wasm: (kernel: number) => wasmHpss({ samples: tone, sampleRate, kernelHarmonic: kernel }),
      python: (kernel: number) =>
        `libsonare.hpss([0.0] * 4096, ${sampleRate}, kernel_harmonic=${kernel})`,
    },
    {
      name: 'extractPercussiveEvents hpssKernelPercussive',
      passed: 2 ** 32,
      wrapsTo: 0,
      node: (kernel: number) =>
        nodeExtractPercussiveEvents({ samples: hits, sampleRate, hpssKernelPercussive: kernel }),
      wasm: (kernel: number) =>
        wasmExtractPercussiveEvents({ samples: hits, sampleRate, hpssKernelPercussive: kernel }),
      python: (kernel: number) =>
        `libsonare.extract_percussive_events([0.0] * ${sampleRate}, ${sampleRate}, hpss_kernel_percussive=${kernel})`,
    },
  ];

  for (const entry of cases) {
    it(`${entry.name}: ${entry.passed} is InvalidParameter on Node, WASM and Python`, () => {
      const answers = {
        node: codeOf(() => entry.node(entry.passed)),
        wasm: codeOf(() => entry.wasm(entry.passed)),
        python: pythonCode(entry.python(entry.passed)),
      };
      // Compared as one object so a disagreement names which surface differs,
      // rather than failing on whichever assertion happened to run first.
      expect(answers).toEqual({
        node: ErrorCode.InvalidParameter,
        wasm: ErrorCode.InvalidParameter,
        python: ErrorCode.InvalidParameter,
      });
    });

    it(`${entry.name}: ${entry.passed} would have been accepted as ${entry.wrapsTo}`, () => {
      // The property that makes the case above about narrowing: the value the
      // narrowing produces is one every downstream rule accepts, so before the
      // check the call SUCCEEDED. Without this the case above could be passing
      // on an unrelated rule that rejects the wrapped value too.
      expect(codeOf(() => entry.node(entry.wrapsTo))).toBe('no refusal');
      expect(codeOf(() => entry.wasm(entry.wrapsTo))).toBe('no refusal');
      expect(pythonCode(entry.python(entry.wrapsTo))).toBe('no refusal');
    });
  }
});
