/**
 * One failure, every surface, one error code.
 *
 * A null-handle constructor has no return code, so each surface reads the code
 * the C ABI recorded beside its detail message; a released handle is object
 * state, refused as InvalidState. Both used to differ by surface. Each surface
 * pins the same `ErrorCode` in its own suite: Node and WASM here, Python in
 * `test_error_contract.py` and `test_closed_handle_state.py`.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  Mixer as WasmMixer,
  init as wasmInit,
  mixingScenePreset as wasmPreset,
} from '../../wasm/dist/index.js';
import { ErrorCode, mixingScenePreset, Mixer as NodeMixer } from '../src/index.js';

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

beforeAll(async () => {
  await wasmInit();
});

describe('a library or object-state failure reports one code on every surface', () => {
  it('a malformed scene JSON is InvalidFormat on Node and WASM', () => {
    const answers = {
      node: codeOf(() => NodeMixer.fromJson('{not json', { sampleRate: 48000 })),
      wasm: codeOf(() => WasmMixer.fromJson('{not json', { sampleRate: 48000 })),
    };
    expect(answers).toEqual({
      node: ErrorCode.InvalidFormat,
      wasm: ErrorCode.InvalidFormat,
    });
  });

  it('a released mixer is InvalidState on Node and WASM', () => {
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
    };
    expect(answers).toEqual({
      node: ErrorCode.InvalidState,
      wasm: ErrorCode.InvalidState,
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
  });
});
