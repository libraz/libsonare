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
  const stream =
    options.stream ??
    (await navigator.mediaDevices.getUserMedia({
      audio: options.audio ?? true,
      video: false,
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
      if (options.stopTracksOnClose !== false) {
        for (const track of stream.getAudioTracks()) {
          track.stop();
        }
      }
    },
  };
}
