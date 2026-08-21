/**
 * Ownership when a handle-class constructor throws.
 *
 * embind has no GC finalizer, so a constructor that allocates and then throws
 * leaves the native object unreachable and unfreeable: `this` never escapes and
 * the handle was its only reference. A browser app that reads a channel count
 * from a device-change handler and retries on failure leaked one whole DSP
 * chain — retune ring, ISP limiter, reverb, scratch — per attempt. The Node
 * facade already released the handle before rethrowing.
 *
 * The module wrapper caches each factory the first time it is read, so this
 * lives in its own file: the spy has to be installed before anything else in
 * the file constructs a voice changer.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeVoiceChanger } from '../src/index';
import { getSonareModule } from '../src/module_state';

const SR = 48000;
const BLOCK = 128;

describe('RealtimeVoiceChanger construction failure', () => {
  beforeAll(async () => {
    await init();
  });

  it('releases the native handle when prepare() rejects the arguments', () => {
    let created = 0;
    let deleted = 0;
    const module = getSonareModule() as unknown as {
      createRealtimeVoiceChanger: (config: unknown) => { delete: () => void };
    };
    // Read the factory through the property descriptor rather than a plain
    // get: the module wrapper is a Proxy whose `get` trap memoises the wrapper
    // it returns, so reading it normally here would pin the original and the
    // replacement below would never be seen.
    const realCreate = Object.getOwnPropertyDescriptor(module, 'createRealtimeVoiceChanger')
      ?.value as (config: unknown) => { delete: () => void };
    expect(typeof realCreate).toBe('function');
    module.createRealtimeVoiceChanger = (config: unknown) => {
      created++;
      const handle = realCreate(config);
      const realDelete = handle.delete.bind(handle);
      handle.delete = () => {
        deleted++;
        realDelete();
      };
      return handle;
    };
    try {
      // 3 channels is outside the [1, 2] the native prepare() accepts.
      expect(() => new RealtimeVoiceChanger('neutral-monitor', SR, BLOCK, 3)).toThrow();
      expect(created).toBe(1);
      expect(deleted).toBe(1);

      // The success path must not release anything early.
      const ok = new RealtimeVoiceChanger('neutral-monitor', SR, BLOCK, 1);
      expect(created).toBe(2);
      expect(deleted).toBe(1);
      ok.destroy();
      expect(deleted).toBe(2);
    } finally {
      module.createRealtimeVoiceChanger = realCreate;
    }
  });
});
