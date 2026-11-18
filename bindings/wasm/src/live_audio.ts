import type { SonareRealtimeEngineNode } from './worklet.js';

/** Options for {@link bindMicrophoneInput}: the `getUserMedia` constraints plus stream ownership controls. */
export interface BindMicrophoneInputOptions extends MediaStreamConstraints {
  /** Audio constraints passed to `getUserMedia`. Default `true`. Ignored when `stream` is given. */
  audio?: MediaStreamConstraints['audio'];
  /** Video constraints passed to `getUserMedia`. Default `false`. Ignored when `stream` is given. */
  video?: MediaStreamConstraints['video'];
  /** Passed to `getUserMedia` unchanged. Omitted: not sent. Ignored when `stream` is given. */
  peerIdentity?: MediaStreamConstraints['peerIdentity'];
  /** Passed to `getUserMedia` unchanged. Omitted: not sent. Ignored when `stream` is given. */
  preferCurrentTab?: MediaStreamConstraints['preferCurrentTab'];
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

/** Handle returned by {@link bindMicrophoneInput}; call `close` to disconnect. */
export interface MicrophoneInputBinding {
  /** The bound microphone stream. */
  stream: MediaStream;
  /** The source node feeding the engine node; disconnected by `close`. */
  source: MediaStreamAudioSourceNode;
  /** Whether this binding acquired {@link stream} rather than being handed one. */
  ownsStream: boolean;
  /** Disconnects the source and, when the binding owns the stream (see `stopTracksOnClose`), stops its audio tracks. Safe to call more than once. */
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
  let source: MediaStreamAudioSourceNode | undefined;
  try {
    source = context.createMediaStreamSource(stream);
    const node = 'node' in engine ? engine.node : engine;
    source.connect(node);
  } catch (error) {
    // No binding escaped to the caller, so an acquired stream is still ours to
    // release even when close was explicitly configured to keep tracks alive.
    // Each cleanup step is independent: one broken track or source must not
    // prevent the rest of the acquired stream from being stopped, and the
    // setup error remains the rejection reason.
    try {
      source?.disconnect();
    } catch {
      // Best-effort cleanup must not replace the setup error.
    }
    if (ownsStream) {
      try {
        for (const track of stream.getTracks()) {
          try {
            track.stop();
          } catch {
            // Continue stopping the remaining tracks.
          }
        }
      } catch {
        // A stream boundary can fail while enumerating its tracks.
      }
    }
    throw error;
  }
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
