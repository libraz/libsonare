/**
 * One failure, all three surfaces, one error code.
 *
 * A null-handle constructor has no return code, so each surface reads the code
 * the C ABI recorded beside its detail message; a released handle is object
 * state, refused as InvalidState. Both used to differ by surface. The Python arm
 * is spawned rather than skipped, as in `narrowing-code-parity.test.ts`.
 */

import { execFileSync } from 'node:child_process';
import { beforeAll, describe, expect, it } from 'vitest';
import {
  Mixer as WasmMixer,
  init as wasmInit,
  mixingScenePreset as wasmPreset,
} from '../../wasm/dist/index.js';
import { ErrorCode, mixingScenePreset, Mixer as NodeMixer } from '../src/index.js';

const PYTHON_PACKAGE = new URL('../../python/', import.meta.url).pathname;

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

/** The Python surface, out of process; `body` is one line of statements. */
function pythonCode(body: string): Answer {
  const script = [
    'import libsonare',
    'try:',
    `    ${body}`,
    '    print("no refusal")',
    'except Exception as error:',
    '    print(getattr(error, "code", "no code"))',
  ].join('\n');
  const stdout = execFileSync('uv', ['run', '--locked', 'python', '-c', script], {
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

describe('a library or object-state failure reports one code on every surface', () => {
  it('a malformed scene JSON is InvalidFormat on Node, WASM and Python', () => {
    const answers = {
      node: codeOf(() => NodeMixer.fromJson('{not json', { sampleRate: 48000 })),
      wasm: codeOf(() => WasmMixer.fromJson('{not json', { sampleRate: 48000 })),
      python: pythonCode("libsonare.Mixer.from_json('{not json', sample_rate=48000)"),
    };
    expect(answers).toEqual({
      node: ErrorCode.InvalidFormat,
      wasm: ErrorCode.InvalidFormat,
      python: ErrorCode.InvalidFormat,
    });
  });

  it('a released mixer is InvalidState on Node, WASM and Python', () => {
    const answers = {
      node: codeOf(() => {
        const mixer = NodeMixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
          sampleRate: 48000,
        });
        mixer.destroy();
        mixer.stripCount;
      }),
      wasm: codeOf(() => {
        const mixer = WasmMixer.fromJson(JSON.stringify(wasmPreset('vocalReverbSend')), {
          sampleRate: 48000,
        });
        mixer.destroy();
        mixer.stripCount;
      }),
      python: pythonCode(
        "m = libsonare.Mixer(sample_rate=48000, scene=libsonare.mixing_scene_preset('vocalReverbSend')); m.close(); m.strip_count",
      ),
    };
    expect(answers).toEqual({
      node: ErrorCode.InvalidState,
      wasm: ErrorCode.InvalidState,
      python: ErrorCode.InvalidState,
    });
  });

  it('a well-formed scene is accepted on every surface', () => {
    // Control: the refusals above come from the input, not from a broken constructor.
    expect(
      codeOf(() =>
        NodeMixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
          sampleRate: 48000,
        }),
      ),
    ).toBe('no refusal');
    expect(
      codeOf(() =>
        WasmMixer.fromJson(JSON.stringify(wasmPreset('vocalReverbSend')), { sampleRate: 48000 }),
      ),
    ).toBe('no refusal');
    expect(
      pythonCode(
        "libsonare.Mixer(sample_rate=48000, scene=libsonare.mixing_scene_preset('vocalReverbSend'))",
      ),
    ).toBe('no refusal');
  });
});
