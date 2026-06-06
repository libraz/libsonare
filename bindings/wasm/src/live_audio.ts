import type { SonareRealtimeEngineNode } from './worklet';

export interface BindMicrophoneInputOptions extends MediaStreamConstraints {
  stream?: MediaStream;
  stopTracksOnClose?: boolean;
}

export interface MicrophoneInputBinding {
  stream: MediaStream;
  source: MediaStreamAudioSourceNode;
  close(): void;
}

export async function bindMicrophoneInput(
  context: AudioContext,
  engine: SonareRealtimeEngineNode | AudioWorkletNode,
  options: BindMicrophoneInputOptions = {},
): Promise<MicrophoneInputBinding> {
  const { stream: providedStream, stopTracksOnClose = true, ...constraints } = options;
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
