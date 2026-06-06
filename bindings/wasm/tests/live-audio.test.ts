import { afterEach, describe, expect, it } from 'vitest';
import { bindMicrophoneInput } from '../dist/index.js';

class FakeTrack {
  stopped = false;
  stop() {
    this.stopped = true;
  }
}

class FakeSource {
  connected: unknown[] = [];
  disconnected = false;
  connect(node: unknown) {
    this.connected.push(node);
  }
  disconnect() {
    this.disconnected = true;
  }
}

describe('bindMicrophoneInput', () => {
  const originalNavigator = globalThis.navigator;

  afterEach(() => {
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: originalNavigator,
    });
  });

  it('requests microphone audio, connects it to the engine node, and closes cleanly', async () => {
    const track = new FakeTrack();
    const stream = {
      getAudioTracks: () => [track],
    } as unknown as MediaStream;
    const requested: MediaStreamConstraints[] = [];
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: {
        mediaDevices: {
          getUserMedia: async (constraints: MediaStreamConstraints) => {
            requested.push(constraints);
            return stream;
          },
        },
      },
    });

    const source = new FakeSource();
    const context = {
      createMediaStreamSource: (actual: MediaStream) => {
        expect(actual).toBe(stream);
        return source;
      },
    } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;
    const binding = await bindMicrophoneInput(context, { node } as never, {
      audio: { echoCancellation: false },
      video: { width: 640 },
    });

    expect(requested).toEqual([{ audio: { echoCancellation: false }, video: { width: 640 } }]);
    expect(binding.stream).toBe(stream);
    expect(source.connected).toEqual([node]);
    binding.close();
    binding.close();
    expect(source.disconnected).toBe(true);
    expect(track.stopped).toBe(true);
  });

  it('can use a caller-supplied stream without stopping tracks', async () => {
    const track = new FakeTrack();
    const stream = { getAudioTracks: () => [track] } as unknown as MediaStream;
    const source = new FakeSource();
    const context = {
      createMediaStreamSource: () => source,
    } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;

    const binding = await bindMicrophoneInput(context, node, {
      stream,
      stopTracksOnClose: false,
    });
    binding.close();

    expect(source.connected).toEqual([node]);
    expect(track.stopped).toBe(false);
  });
});
