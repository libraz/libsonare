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

  // Ownership follows provenance, the rule `ownsWorker` already applies in
  // opfs_clip_pages.ts and worker_client.ts. A supplied MediaStream is
  // page-wide state: its tracks may be feeding a recorder, a level meter or a
  // second AudioContext, so stopping them on close killed a microphone this
  // binding never acquired. Only the explicit opt-in did the right thing, and
  // the caller had to know to write it.
  function suppliedStreamFixture() {
    const track = new FakeTrack();
    const stream = { getAudioTracks: () => [track] } as unknown as MediaStream;
    const source = new FakeSource();
    const context = {
      createMediaStreamSource: () => source,
    } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;
    return { track, stream, source, context, node };
  }

  it('leaves a caller-supplied stream running by default', async () => {
    const { track, stream, source, context, node } = suppliedStreamFixture();

    const binding = await bindMicrophoneInput(context, node, { stream });
    expect(binding.ownsStream).toBe(false);
    binding.close();

    expect(source.connected).toEqual([node]);
    // The acceptance condition: another consumer can still read the stream.
    expect(track.stopped).toBe(false);
    expect(binding.stream).toBe(stream);
  });

  it('can use a caller-supplied stream without stopping tracks', async () => {
    const { track, stream, source, context, node } = suppliedStreamFixture();

    const binding = await bindMicrophoneInput(context, node, {
      stream,
      stopTracksOnClose: false,
    });
    binding.close();

    expect(source.connected).toEqual([node]);
    expect(track.stopped).toBe(false);
  });

  it('still stops a caller-supplied stream when the caller opts in', async () => {
    const { track, stream, context, node } = suppliedStreamFixture();

    const binding = await bindMicrophoneInput(context, node, {
      stream,
      stopTracksOnClose: true,
    });
    expect(binding.ownsStream).toBe(false);
    binding.close();

    expect(track.stopped).toBe(true);
  });

  it('honours an explicit opt-out on a stream it acquired itself', async () => {
    const track = new FakeTrack();
    const stream = { getAudioTracks: () => [track] } as unknown as MediaStream;
    Object.defineProperty(globalThis, 'navigator', {
      configurable: true,
      value: { mediaDevices: { getUserMedia: async () => stream } },
    });
    const source = new FakeSource();
    const context = { createMediaStreamSource: () => source } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;

    const binding = await bindMicrophoneInput(context, node, { stopTracksOnClose: false });
    expect(binding.ownsStream).toBe(true);
    binding.close();

    // The caller kept the stream to manage itself; `binding.stream` is still live.
    expect(track.stopped).toBe(false);
  });
});
