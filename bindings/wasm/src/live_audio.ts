import type { SonareRealtimeEngineNode } from './worklet';

export interface BindMicrophoneInputOptions extends MediaStreamConstraints {
  /**
   * Stream to bind instead of requesting one. Supplying it also transfers
   * ownership: {@link MicrophoneInputBinding.close} leaves its tracks running,
   * because they may be feeding a recorder, a level meter or a second
   * `AudioContext` that this binding knows nothing about.
   */
  stream?: MediaStream;
  /**
   * Whether {@link MicrophoneInputBinding.close} stops the stream's audio
   * tracks. Defaults to whether this binding acquired the stream itself: `true`
   * for a stream it requested through `getUserMedia`, `false` for one the
   * caller supplied. Set it explicitly to override in either direction.
   */
  stopTracksOnClose?: boolean;
}

export interface MicrophoneInputBinding {
  stream: MediaStream;
  source: MediaStreamAudioSourceNode;
  /** Whether this binding acquired {@link stream} rather than being handed one. */
  ownsStream: boolean;
  close(): void;
}

/**
 * Route microphone audio into a realtime engine node.
 *
 * Ownership is decided by provenance, the same rule `ownsWorker` applies in
 * `opfs_clip_pages.ts` and `worker_client.ts`: a resource the caller handed in
 * is the caller's to release. `close()` used to stop the tracks of a supplied
 * stream by default, which killed a microphone the rest of the page was still
 * using.
 */
export async function bindMicrophoneInput(
  context: AudioContext,
  engine: SonareRealtimeEngineNode | AudioWorkletNode,
  options: BindMicrophoneInputOptions = {},
): Promise<MicrophoneInputBinding> {
  const { stream: providedStream, stopTracksOnClose: stopTracksOverride, ...constraints } = options;
  const ownsStream = providedStream === undefined;
  const stopTracksOnClose = stopTracksOverride ?? ownsStream;
  const stream =
    providedStream ??
    (await navigator.mediaDevices.getUserMedia({
      ...constraints,
      audio: constraints.audio ?? true,
      video: constraints.video ?? false,
    }));
  const source = context.createMediaStreamSource(stream);
  const node = 'node' in engine ? engine.node : engine;
  source.connect(node);
  let closed = false;
  return {
    stream,
    source,
    ownsStream,
    close() {
      if (closed) {
        return;
      }
      closed = true;
      source.disconnect();
      if (stopTracksOnClose) {
        for (const track of stream.getAudioTracks()) {
          track.stop();
        }
      }
    },
  };
}
