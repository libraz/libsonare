import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, SonareError } from '../../dist/index.js';
import * as worklet from '../../dist/worklet.js';
import {
  clipPageRequestRingBufferByteLength,
  createClipPageRequestRingBuffer,
  createEngineTelemetryRingBuffer,
  createExternalMidiRingBuffer,
  createMeterRingBuffer,
  createScopeRingBuffer,
  createSpectrumRingBuffer,
  engineTelemetryRingBufferByteLength,
  externalMidiRingBufferByteLength,
  init as initWorklet,
  meterRingBufferByteLength,
  readMeterRingBuffer,
  readScopeRingBuffer,
  SonareRealtimeEngineNode,
  scopeRingBufferByteLength,
  spectrumRingBufferByteLength,
} from '../../dist/worklet.js';
import {
  createSonareEngineCommandRingBuffer,
  sonareEngineCommandRingBufferByteLength,
} from '../../src/internal/engine_command_ring.js';

function caught(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

describe('ring creators take one options object', () => {
  it('creates each ring from named options and applies the defaults', () => {
    expect(createMeterRingBuffer({ capacity: 4 }).capacity).toBe(4);
    expect(createMeterRingBuffer().capacity).toBe(128);
    expect(createMeterRingBuffer({}).capacity).toBe(128);
    const spectrum = createSpectrumRingBuffer({ capacity: 3, bands: 8 });
    expect([spectrum.capacity, spectrum.bands]).toEqual([3, 8]);
    expect(createSpectrumRingBuffer().bands).toBe(16);
    const scope = createScopeRingBuffer({ capacity: 2, bands: 4, maxPoints: 3 });
    expect([scope.capacity, scope.bands, scope.maxPoints]).toEqual([2, 4, 3]);
    expect(createScopeRingBuffer().capacity).toBe(64);
    expect(createSonareEngineCommandRingBuffer({ capacity: 8 }).capacity).toBe(8);
    expect(createEngineTelemetryRingBuffer({ capacity: 8 }).capacity).toBe(8);
    expect(createClipPageRequestRingBuffer({ capacity: 8 }).capacity).toBe(8);
    expect(createExternalMidiRingBuffer({ capacity: 8 }).capacity).toBe(8);
  });

  it('treats null and undefined fields as absent', () => {
    const ring = createSpectrumRingBuffer({
      capacity: null,
      bands: undefined,
    } as unknown as Parameters<typeof createSpectrumRingBuffer>[0]);
    expect([ring.capacity, ring.bands]).toEqual([128, 16]);
  });

  it('sizes each ring from named options, capacity required', () => {
    expect(meterRingBufferByteLength({ capacity: 4 })).toBe(
      createMeterRingBuffer({ capacity: 4 }).sharedBuffer.byteLength,
    );
    expect(spectrumRingBufferByteLength({ capacity: 3, bands: 8 })).toBe(
      createSpectrumRingBuffer({ capacity: 3, bands: 8 }).sharedBuffer.byteLength,
    );
    expect(scopeRingBufferByteLength({ capacity: 2, bands: 4, maxPoints: 3 })).toBe(
      createScopeRingBuffer({ capacity: 2, bands: 4, maxPoints: 3 }).sharedBuffer.byteLength,
    );
    expect(sonareEngineCommandRingBufferByteLength({ capacity: 2 })).toBeGreaterThan(0);
    expect(engineTelemetryRingBufferByteLength({ capacity: 2 })).toBeGreaterThan(0);
    expect(clipPageRequestRingBufferByteLength({ capacity: 2 })).toBeGreaterThan(0);
    expect(externalMidiRingBufferByteLength({ capacity: 2 })).toBeGreaterThan(0);
    const missing = caught(() => meterRingBufferByteLength({} as unknown as { capacity: number }));
    expect(missing).toBeInstanceOf(TypeError);
  });

  it('refuses the positional form at the type level', () => {
    const legacyForms = [
      // @ts-expect-error a number is not an options object
      () => createMeterRingBuffer(4),
      // @ts-expect-error a number is not an options object
      () => createScopeRingBuffer(2, 4, 3),
      // @ts-expect-error a number is not an options object
      () => spectrumRingBufferByteLength(2, 4),
      // @ts-expect-error a number is not an options object
      () => sonareEngineCommandRingBufferByteLength(2),
    ];
    expect(legacyForms).toHaveLength(4);
  });

  it('refuses a positional number at run time with a TypeError', () => {
    expect(caught(() => (createMeterRingBuffer as (v: unknown) => unknown)(4))).toBeInstanceOf(
      TypeError,
    );
  });

  it('refuses an unknown option key with a TypeError naming it', () => {
    const creators: Array<[string, () => unknown]> = [
      ['meter', () => createMeterRingBuffer({ capacity: 2, bands: 3 } as never)],
      ['spectrum', () => createSpectrumRingBuffer({ capacity: 2, maxPoints: 3 } as never)],
      ['scope', () => createScopeRingBuffer({ capacity: 2, band: 3 } as never)],
      ['command', () => createSonareEngineCommandRingBuffer({ capacity: 2, extra: 1 } as never)],
      ['bytes', () => meterRingBufferByteLength({ capacity: 2, extra: 1 } as never)],
    ];
    for (const [, run] of creators) {
      const error = caught(run);
      expect(error).toBeInstanceOf(TypeError);
      expect((error as Error).message).toMatch(/bands|maxPoints|band|extra/);
    }
  });

  it('refuses a fractional, non-positive or non-finite value with a RangeError', () => {
    for (const bad of [0, -1, 1.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(caught(() => createMeterRingBuffer({ capacity: bad }))).toBeInstanceOf(RangeError);
      expect(caught(() => meterRingBufferByteLength({ capacity: bad }))).toBeInstanceOf(RangeError);
      expect(caught(() => createSpectrumRingBuffer({ capacity: 2, bands: bad }))).toBeInstanceOf(
        RangeError,
      );
    }
    expect(caught(() => createScopeRingBuffer({ capacity: 2, bands: 65 }))).toBeInstanceOf(
      RangeError,
    );
  });

  it('refuses a non-number value with a TypeError', () => {
    expect(
      caught(() => createMeterRingBuffer({ capacity: '4' } as unknown as { capacity: number })),
    ).toBeInstanceOf(TypeError);
  });
});

describe('ring reads of a malformed ring are coded failures', () => {
  it('reports a descriptor that does not describe a ring as InvalidFormat', () => {
    const ring = createMeterRingBuffer({ capacity: 2 });
    Atomics.store(ring.header, 2, 999);
    const error = caught(() => readMeterRingBuffer(ring));
    expect(isSonareError(error)).toBe(true);
    expect(error).toBeInstanceOf(SonareError);
    expect((error as SonareError).code).toBe(ErrorCode.InvalidFormat);
    expect((error as SonareError).codeName).toBe('InvalidFormat');
  });

  it('reports a scope ring with a wrong descriptor as InvalidFormat', () => {
    const ring = createScopeRingBuffer({ capacity: 2, bands: 4, maxPoints: 3 });
    Atomics.store(ring.header, 3, 5);
    const error = caught(() => readScopeRingBuffer(ring));
    expect((error as SonareError).codeName).toBe('InvalidFormat');
  });

  it('reports a protocol version skew as AbiMismatch', () => {
    const ring = createMeterRingBuffer({ capacity: 2 });
    Atomics.store(ring.header, 0, 1);
    Atomics.store(ring.header, 3, 99);
    const error = caught(() => readMeterRingBuffer(ring));
    expect((error as SonareError).code).toBe(ErrorCode.AbiMismatch);
  });
});

describe('SonareFailurePayload is not a public export', () => {
  it('exports no failure-payload helper from ./worklet', () => {
    expect('errorFromFailurePayload' in worklet).toBe(false);
    expect('failureFromPayload' in worklet).toBe(false);
  });
});

describe('the worklet-internal transport is not a public export', () => {
  it.each([
    'createSonareEngineCommandRingBuffer',
    'pushSonareEngineCommandRingBuffer',
    'popSonareEngineCommandRingBuffer',
    'sonareEngineCommandRingBufferByteLength',
    'SONARE_ENGINE_COMMAND_RECORD_BYTES',
    'SonareEngineCommandType',
    'encodeFrameHi',
    'encodeFrameLo',
    'decodeFrame',
    'ENGINE_MIXER_PARAM_FADER_DB',
    'ENGINE_MIXER_PARAM_PAN',
    'engineMixerMasterTarget',
    'writeSonareEngineTelemetryRingBuffer',
  ])('exports no %s from ./worklet', (name) => {
    expect(name in worklet).toBe(false);
  });
});

describe('onSyncError listeners receive a coded error', () => {
  beforeAll(async () => {
    await init();
    await initWorklet();
  });

  it('rebuilds the posted failure as a SonareError', async () => {
    const port = {
      postMessage: () => undefined,
      onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
    };
    const context = {
      sampleRate: 48000,
      audioWorklet: { addModule: () => Promise.resolve() },
    } as unknown as BaseAudioContext;
    const node = await SonareRealtimeEngineNode.create(context, {
      numChannels: 2,
      mode: 'postMessage',
      nodeFactory: () => {
        queueMicrotask(() =>
          port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent),
        );
        return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
      },
    });
    const seen: Array<{ failure?: Error }> = [];
    node.onSyncError((message) => seen.push(message));
    port.onmessage?.({
      data: {
        type: 'syncError',
        syncType: 'syncTempo',
        message: 'unsupported',
        failure: {
          name: 'SonareError',
          message: 'unsupported',
          code: ErrorCode.NotSupported,
          codeName: 'NotSupported',
        },
      },
    } as MessageEvent<unknown>);
    expect(seen).toHaveLength(1);
    const failure = seen[0].failure;
    expect(failure).toBeInstanceOf(Error);
    expect(isSonareError(failure)).toBe(true);
    expect((failure as SonareError).code).toBe(ErrorCode.NotSupported);
    expect((failure as SonareError).codeName).toBe('NotSupported');
    node.destroy();
  });
});
