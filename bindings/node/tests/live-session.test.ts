import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  capabilities,
  ErrorCode,
  type LiveAudioDevice,
  type LiveMidiDevice,
  LiveSession,
  type LiveSessionConfig,
  type LiveSessionStatus,
  SonareError,
} from '../dist/index.js';

function expectUnavailable(call: () => unknown, entry: string): void {
  let caught: unknown;
  try {
    call();
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(SonareError);
  expect(caught).toMatchObject({
    name: 'SonareError',
    code: ErrorCode.NotSupported,
    codeName: 'NotSupported',
    reason: 'notCompiledIn',
    entry,
  });
  expect((caught as Error).message).toBe(`${entry}: live device support is not compiled in`);
}

describe('LiveSession native stub', () => {
  it('publishes the live-device records and config with the shared field names', () => {
    expectTypeOf<LiveAudioDevice>().toMatchTypeOf<{
      readonly uid: string;
      readonly name: string;
      readonly inputChannels: number;
      readonly outputChannels: number;
      readonly isDefaultInput: boolean;
      readonly isDefaultOutput: boolean;
      readonly nominalSampleRate: number;
    }>();
    expectTypeOf<LiveMidiDevice>().toMatchTypeOf<{
      readonly uid: string;
      readonly name: string;
    }>();
    expectTypeOf<LiveSessionConfig>().toMatchTypeOf<{
      readonly audioOutput?: 'default' | { readonly uid: string };
      readonly audioInput?: 'default' | { readonly uid: string };
      readonly midiInput?: { readonly uid: string };
      readonly sampleRate?: number;
      readonly blockSize?: number;
      readonly destinationId?: number;
    }>();
    expectTypeOf<LiveSessionStatus['running']>().toEqualTypeOf<boolean>();
    expectTypeOf<LiveSessionStatus['xrunCount']>().toEqualTypeOf<number>();
  });

  it('reports unavailable live-device support through capabilities', () => {
    expect(capabilities().features.liveDevices).toBe(false);
  });

  it('refuses construction and enumeration with the coded platform reason', () => {
    expectUnavailable(() => new LiveSession({ engine: {} as never }), 'LiveSession');
    expectUnavailable(() => LiveSession.audioDevices(), 'LiveSession.audioDevices');
    expectUnavailable(() => LiveSession.midiSources(), 'LiveSession.midiSources');
  });

  it('keeps lifecycle and read methods present and reports the same refusal', () => {
    const session = Object.create(LiveSession.prototype) as LiveSession;
    expectUnavailable(() => session.status(), 'LiveSession.status');
    expectUnavailable(() => session.capturedMidi(), 'LiveSession.capturedMidi');
    expectUnavailable(() => session.destroy(), 'LiveSession.destroy');
    expectUnavailable(() => session[Symbol.dispose](), 'LiveSession.destroy');
  });
});
