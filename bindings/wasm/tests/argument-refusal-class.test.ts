/**
 * The error class of a refused argument.
 *
 * An argument the binding refuses itself is a native `TypeError` (wrong type) or
 * `RangeError` (out of domain), never a coded `SonareError`; a failure the library
 * or an object's state reports keeps its code. The C++ readers decide the class,
 * `sonareExceptionInfo` names it and the module wrapper rebuilds it.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  Mixer,
  mixingScenePresetJson,
  RealtimeEngine,
} from '../dist/index.js';

function thrown(call: () => unknown): unknown {
  try {
    call();
  } catch (error) {
    return error;
  }
  throw new Error('expected the call to throw');
}

function withEngine<T>(body: (engine: RealtimeEngine) => T): T {
  const engine = new RealtimeEngine(48000, 256);
  try {
    return body(engine);
  } finally {
    engine.destroy();
  }
}

describe('argument refusal error class', () => {
  beforeAll(async () => {
    await init();
  });

  it('refuses a wrong-typed field with a TypeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers([{ id: 1, ppq: 'x' as unknown as number, name: 'a', kind: 0 }]),
      );
      expect(error).toBeInstanceOf(TypeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toBe('ppq must be a number');
    });
  });

  it('refuses an out-of-domain field with a RangeError', () => {
    withEngine((engine) => {
      const error = thrown(() => engine.setMarkers([{ id: 1, ppq: -1, name: 'a', kind: 0 }]));
      expect(error).toBeInstanceOf(RangeError);
      expect(isSonareError(error)).toBe(false);
      expect((error as Error).message).toBe(
        'setMarkers: marker ppq must be finite and non-negative',
      );
    });
  });

  it('refuses an array-like that is not one with a TypeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers(null as unknown as Parameters<typeof engine.setMarkers>[0]),
      );
      expect(error).toBeInstanceOf(TypeError);
    });
  });

  it('refuses a length outside the safe-integer domain with a RangeError', () => {
    withEngine((engine) => {
      const error = thrown(() =>
        engine.setMarkers({ length: -1 } as unknown as Parameters<typeof engine.setMarkers>[0]),
      );
      expect(error).toBeInstanceOf(RangeError);
    });
  });

  it('keeps an engine lookup failure a coded SonareError', () => {
    withEngine((engine) => {
      const error = thrown(() => engine.marker(999));
      expect(isSonareError(error)).toBe(true);
      expect((error as { code: number }).code).toBe(ErrorCode.InvalidParameter);
    });
  });

  it('keeps a library failure a coded SonareError', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('drumBusSubgroup'));
    try {
      const error = thrown(() => mixer.busMeter('no-such-bus'));
      expect(isSonareError(error)).toBe(true);
      expect((error as { code: number }).code).toBe(ErrorCode.InvalidState);
    } finally {
      mixer.destroy();
    }
  });
});
