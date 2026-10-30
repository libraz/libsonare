/**
 * The registered voice changer worklet processor answers every refused control message, and a
 * refused construction, with an error reply instead of throwing into the audio thread.
 */

import {
  describe,
  expect,
  it,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

interface Posted {
  type: string;
  [key: string]: unknown;
}

type Ctor = new (
  options: unknown,
) => {
  process(inputs: Float32Array[][], outputs: Float32Array[][]): boolean;
};

function register(): { ctor: Ctor; posted: Posted[]; send: (data: unknown) => void } {
  const posted: Posted[] = [];
  let listener: ((event: { data: unknown }) => void) | undefined;
  let registered: unknown;
  Object.assign(globalThis, {
    AudioWorkletProcessor: class {
      port = {
        postMessage: (message: Posted) => posted.push(message),
        addEventListener: (_type: string, l: (event: { data: unknown }) => void) => {
          listener = l;
        },
        start: () => undefined,
      };
    },
    registerProcessor: (_name: string, ctor: unknown) => {
      registered = ctor;
    },
  });
  registerSonareRealtimeVoiceChangerWorkletProcessor('sonare-test-voice-changer');
  return {
    ctor: registered as Ctor,
    posted,
    send: (data) => listener?.({ data }),
  };
}

describe('registered voice changer worklet processor', () => {
  setupWorklet();

  it('answers a refused control message with an error reply and keeps rendering', () => {
    const { ctor, posted, send } = register();
    const instance = new ctor({
      processorOptions: { preset: 'neutral-monitor', sampleRate: 48000, blockSize: 128 },
    });
    send({ type: 'noteOn', note: 60, velocity: 2 });
    send({ type: 'setConfig', config: {} });
    send({ type: 'bogus' });
    const errors = posted.filter((message) => message.type === 'error');
    expect(errors.map((message) => message.request)).toEqual(['noteOn', 'setConfig', 'invalid']);
    for (const message of errors) {
      expect(typeof message.message).toBe('string');
      expect(message.failure).toBeDefined();
    }
    const out = [new Float32Array(128)];
    expect(instance.process([[new Float32Array(128)]], [out])).toBe(true);
    send({ type: 'destroy' });
  });

  it('reports a refused construction as an error reply and renders silence', () => {
    const { ctor, posted } = register();
    const instance = new ctor({
      processorOptions: { preset: 'neutral-monitor', sampleRate: 48000, channelCount: 0 },
    });
    expect(posted.map((message) => [message.type, message.request])).toEqual([['error', 'init']]);
    const out = [new Float32Array(128).fill(1)];
    expect(instance.process([[new Float32Array(128)]], [out])).toBe(true);
    expect(out[0].every((sample) => sample === 0)).toBe(true);
  });
});
