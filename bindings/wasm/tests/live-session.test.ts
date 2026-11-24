import { readFileSync } from 'node:fs';
import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  LiveSession,
  type LiveSessionConfig,
  RealtimeEngine,
} from '../src/index.js';

const PROCESSING_SOURCE = readFileSync(
  new URL('../../../src/wasm/bindings/realtime/processing.cpp', import.meta.url),
  'utf8',
);
const ENGINE_SOURCE = readFileSync(
  new URL('../../../src/engine/realtime_engine.cpp', import.meta.url),
  'utf8',
);

function codedStub(call: () => unknown, entry: string): void {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(isSonareError(caught)).toBe(true);
  expect(caught).toMatchObject({
    code: ErrorCode.NotSupported,
    codeName: 'NotSupported',
    reason: 'notCompiledIn',
    entry,
    message: `${entry}: live device support is not compiled in`,
  });
}

function methodBody(source: string, method: string): string {
  const signature = source.indexOf(`RealtimeEngineWasm::${method}(`);
  expect(signature, `${method} definition`).toBeGreaterThanOrEqual(0);
  const opening = source.indexOf('{', signature);
  let depth = 0;
  for (let i = opening; i < source.length; i++) {
    if (source[i] === '{') {
      depth++;
    }
    if (source[i] === '}') {
      depth--;
    }
    if (depth === 0) {
      return source.slice(opening + 1, i);
    }
  }
  throw new Error(`unterminated ${method} definition`);
}

describe('WASM LiveSession stub', () => {
  it('reports NotSupported with the compiled-out reason before device enumeration or open', () => {
    codedStub(() => LiveSession.audioDevices(), 'LiveSession.audioDevices');
    codedStub(() => LiveSession.midiSources(), 'LiveSession.midiSources');
    expectTypeOf<ConstructorParameters<typeof LiveSession>>().toEqualTypeOf<
      [config: LiveSessionConfig & { readonly engine: RealtimeEngine }]
    >();
    codedStub(() => new LiveSession({ engine: {} as RealtimeEngine }), 'LiveSession');
  });

  it('keeps the lifecycle and captured MIDI entry names present as coded stubs', () => {
    const session = Object.create(LiveSession.prototype) as LiveSession;
    codedStub(() => session.status(), 'LiveSession.status');
    codedStub(() => session.capturedMidi(), 'LiveSession.capturedMidi');
    codedStub(() => session.destroy(), 'LiveSession.destroy');
    codedStub(() => session[Symbol.dispose](), 'LiveSession.destroy');
  });

  it('checks the live driver before each manual audio dispatch', () => {
    for (const method of [
      'process',
      'processPrepared',
      'processPreparedWithMonitor',
      'processBlockWithMonitor',
    ]) {
      const body = methodBody(PROCESSING_SOURCE, method);
      const guard = body.indexOf('engine_.require_quiescent();');
      const dispatch = body.indexOf('engine_.process');
      expect(guard, `${method} live-driver guard`).toBeGreaterThanOrEqual(0);
      expect(dispatch, `${method} engine call`).toBeGreaterThan(guard);
    }
    expect(ENGINE_SOURCE).toContain('ErrorReason::EngineDeviceDriven');
  });

  it('keeps a missing prepared buffer distinct from a live-driver refusal', async () => {
    await init();
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      let caught: unknown;
      try {
        engine.processPrepared(32);
      } catch (error) {
        caught = error;
      }
      expect(isSonareError(caught)).toBe(true);
      expect(caught).toMatchObject({ code: ErrorCode.InvalidState });
      expect(caught).not.toHaveProperty('reason');
    } finally {
      engine.destroy();
    }
  });
});
