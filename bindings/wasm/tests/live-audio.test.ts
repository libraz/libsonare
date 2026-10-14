import { afterEach, describe, expect, it } from 'vitest';
import { bindMicrophoneInput } from '../dist/index.js';

class FakeTrack {
  stopped = false;
  stopCalls = 0;
  constructor(private readonly stopError?: Error) {}

  stop() {
    this.stopCalls += 1;
    this.stopped = true;
    if (this.stopError) {
      throw this.stopError;
    }
  }
}

function fakeStream(audioTracks: FakeTrack[], videoTracks: FakeTrack[] = []): MediaStream {
  return {
    getAudioTracks: () => audioTracks,
    getTracks: () => [...audioTracks, ...videoTracks],
  } as unknown as MediaStream;
}

class FakeSource {
  connected: unknown[] = [];
  disconnected = false;
  constructor(
    private readonly connectError?: Error,
    private readonly disconnectError?: Error,
  ) {}

  connect(node: unknown) {
    this.connected.push(node);
    if (this.connectError) {
      throw this.connectError;
    }
  }

  disconnect() {
    this.disconnected = true;
    if (this.disconnectError) {
      throw this.disconnectError;
    }
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
    const videoTrack = new FakeTrack();
    const stream = fakeStream([track], [videoTrack]);
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
    // Close stops only the audio tracks; a video track on the acquired stream
    // is left running.
    expect(videoTrack.stopped).toBe(false);
  });

  // Ownership follows provenance, the rule `ownsWorker` already applies in
  // opfs_clip_pages.ts and worker_client.ts. A supplied MediaStream is
  // page-wide state: its tracks may be feeding a recorder, a level meter or a
  // second AudioContext, so stopping them on close killed a microphone this
  // binding never acquired. Only the explicit opt-in did the right thing, and
  // the caller had to know to write it.
  function suppliedStreamFixture() {
    const track = new FakeTrack();
    const videoTrack = new FakeTrack();
    const stream = fakeStream([track], [videoTrack]);
    const source = new FakeSource();
    const context = {
      createMediaStreamSource: () => source,
    } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;
    return { track, videoTrack, stream, source, context, node };
  }

  it('leaves a caller-supplied stream running by default', async () => {
    const { track, videoTrack, stream, source, context, node } = suppliedStreamFixture();

    const binding = await bindMicrophoneInput(context, node, { stream });
    expect(binding.ownsStream).toBe(false);
    binding.close();

    expect(source.connected).toEqual([node]);
    // The acceptance condition: another consumer can still read the stream.
    expect(track.stopped).toBe(false);
    expect(videoTrack.stopped).toBe(false);
    expect(binding.stream).toBe(stream);
  });

  it('can use a caller-supplied stream without stopping tracks', async () => {
    const { track, videoTrack, stream, source, context, node } = suppliedStreamFixture();

    const binding = await bindMicrophoneInput(context, node, {
      stream,
      stopTracksOnClose: false,
    });
    binding.close();

    expect(source.connected).toEqual([node]);
    expect(track.stopped).toBe(false);
    expect(videoTrack.stopped).toBe(false);
  });

  it('still stops a caller-supplied stream when the caller opts in', async () => {
    const { track, videoTrack, stream, context, node } = suppliedStreamFixture();

    const binding = await bindMicrophoneInput(context, node, {
      stream,
      stopTracksOnClose: true,
    });
    expect(binding.ownsStream).toBe(false);
    binding.close();

    expect(track.stopped).toBe(true);
    expect(videoTrack.stopped).toBe(false);
  });

  it('honours an explicit opt-out on a stream it acquired itself', async () => {
    const track = new FakeTrack();
    const videoTrack = new FakeTrack();
    const stream = fakeStream([track], [videoTrack]);
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
    expect(videoTrack.stopped).toBe(false);
  });

  it('rolls back an acquired stream when source setup fails, preserving the setup error', async () => {
    const setupError = new Error('connect failed');
    const audioTrack = new FakeTrack(new Error('first stop failed'));
    const videoTrack = new FakeTrack();
    const stream = fakeStream([audioTrack], [videoTrack]);
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
    const source = new FakeSource(setupError, new Error('disconnect failed'));
    const context = { createMediaStreamSource: () => source } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;

    const failed = bindMicrophoneInput(context, node, {
      video: true,
      stopTracksOnClose: false,
    });
    await expect(failed).rejects.toBe(setupError);
    expect(requested).toEqual([{ video: true, audio: true }]);
    expect(source.connected).toEqual([node]);
    expect(source.disconnected).toBe(true);
    expect(audioTrack.stopCalls).toBe(1);
    expect(videoTrack.stopCalls).toBe(1);
    expect(audioTrack.stopped).toBe(true);
    expect(videoTrack.stopped).toBe(true);
  });

  it('does not stop a caller-supplied stream when source setup fails', async () => {
    const setupError = new Error('source setup failed');
    const audioTrack = new FakeTrack();
    const videoTrack = new FakeTrack();
    const stream = fakeStream([audioTrack], [videoTrack]);
    const source = new FakeSource(setupError);
    const context = { createMediaStreamSource: () => source } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;

    await expect(
      bindMicrophoneInput(context, node, { stream, stopTracksOnClose: true }),
    ).rejects.toBe(setupError);
    expect(source.disconnected).toBe(true);
    expect(audioTrack.stopCalls).toBe(0);
    expect(videoTrack.stopCalls).toBe(0);
    expect(audioTrack.stopped).toBe(false);
    expect(videoTrack.stopped).toBe(false);
  });

  it('rolls back an acquired stream when source creation fails', async () => {
    const setupError = new Error('source creation failed');
    const audioTrack = new FakeTrack();
    const videoTrack = new FakeTrack();
    const stream = fakeStream([audioTrack], [videoTrack]);
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
    const context = {
      createMediaStreamSource: () => {
        throw setupError;
      },
    } as unknown as AudioContext;
    const node = { port: {} } as unknown as AudioWorkletNode;

    await expect(
      bindMicrophoneInput(context, node, { video: true, stopTracksOnClose: false }),
    ).rejects.toBe(setupError);
    expect(requested).toEqual([{ video: true, audio: true }]);
    expect(audioTrack.stopCalls).toBe(1);
    expect(videoTrack.stopCalls).toBe(1);
    expect(audioTrack.stopped).toBe(true);
    expect(videoTrack.stopped).toBe(true);
  });
});
