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
  mixingScenePresetJson as wasmPresetJson,
} from '../../wasm/dist/index.js';
import { ErrorCode, mixingScenePresetJson, Mixer as NodeMixer } from '../src/index.js';

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

describe('a library or object-state failure reports one code on every surface', () => {
  it('a malformed scene JSON is InvalidFormat on Node, WASM and Python', () => {
    const answers = {
      node: codeOf(() => NodeMixer.fromSceneJson('{not json')),
      wasm: codeOf(() => WasmMixer.fromSceneJson('{not json')),
      python: pythonCode("libsonare.Mixer.from_scene_json('{not json')"),
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
        const mixer = NodeMixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'));
        mixer.destroy();
        mixer.stripCount();
      }),
      wasm: codeOf(() => {
        const mixer = WasmMixer.fromSceneJson(wasmPresetJson('vocalReverbSend'));
        mixer.destroy();
        mixer.stripCount();
      }),
      python: pythonCode(
        "m = libsonare.Mixer.from_scene_json(libsonare.mixing_scene_preset_json('vocalReverbSend')); m.close(); m.strip_count()",
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
    expect(codeOf(() => NodeMixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend')))).toBe(
      'no refusal',
    );
    expect(codeOf(() => WasmMixer.fromSceneJson(wasmPresetJson('vocalReverbSend')))).toBe(
      'no refusal',
    );
    expect(
      pythonCode(
        "libsonare.Mixer.from_scene_json(libsonare.mixing_scene_preset_json('vocalReverbSend'))",
      ),
    ).toBe('no refusal');
  });
});
