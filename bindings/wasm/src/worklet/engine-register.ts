import { ErrorCode } from '../errors.js';
import { codedError, invalidState, withEntry } from '../internal/entry.js';
import { errorText, failurePayload } from '../internal/failure_payload.js';
import type { WorkletPort } from '../internal/worklet_messages.js';
import type { SonareRealtimeEngineWorkletProcessorOptions } from '../public_types_worklet.js';
import type { WorkletInput, WorkletOutput } from './audio_types.js';
import { initializeEmbindInWorklet } from './embind-init.js';
import { SonareRealtimeEngineWorkletProcessor } from './engine-processor.js';
import { assertKnownProcessorOptions } from './guards.js';

/**
 * Registers the realtime-engine processor in an AudioWorkletGlobalScope. The processor initializes
 * the WASM module itself when it is not ready and posts `{ type: 'ready' }` once it is up.
 *
 * @param name Processor name. Default `'sonare-realtime-engine-processor'`.
 * @throws SonareError `NotSupported` outside an AudioWorkletGlobalScope.
 */
export function registerSonareRealtimeEngineWorkletProcessor(
  name = 'sonare-realtime-engine-processor',
): void {
  withEntry('registerSonareRealtimeEngineWorkletProcessor', () => {
    const scope = globalThis as unknown as {
      AudioWorkletProcessor?: new () => object;
      registerProcessor?: (processorName: string, processorCtor: unknown) => void;
    };
    if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
      throw codedError(
        ErrorCode.NotSupported,
        'AudioWorkletProcessor is not available in this context.',
      );
    }
    const Base = scope.AudioWorkletProcessor;
    class RegisteredSonareRealtimeEngineWorkletProcessor extends Base {
      private bridge?: SonareRealtimeEngineWorkletProcessor;
      private readonly pendingMessages: unknown[] = [];
      private pendingMessagesOverflowed = false;
      readonly port?: WorkletPort;

      constructor(options?: { processorOptions?: SonareRealtimeEngineWorkletProcessorOptions }) {
        super();
        const port = this.port;
        // A missing processorOptions reaches the processor, which refuses the absent numChannels.
        const processorOptions =
          options?.processorOptions ?? ({} as SonareRealtimeEngineWorkletProcessorOptions);
        assertKnownProcessorOptions('engineProcessor', processorOptions);
        void this.initializeEmbind(processorOptions, port);
        const onMessage = (event: { data: unknown }) => {
          if (!this.bridge) {
            if (this.pendingMessages.length < 1024) {
              this.pendingMessages.push(event.data);
            } else {
              // A long sync before embind initialization must fail visibly rather
              // than drop a provider/page/commit message and leave silent audio.
              this.pendingMessagesOverflowed = true;
            }
            return;
          }
          this.bridge.receiveMessage(event.data);
        };
        if (port?.addEventListener) {
          port.addEventListener('message', onMessage);
          port.start?.();
        } else if (port) {
          port.onmessage = onMessage;
        }
      }

      process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
        if (this.bridge) {
          return this.bridge.process(inputs, outputs);
        }
        const output = outputs[0];
        for (const channel of output ?? []) {
          channel.fill(0);
        }
        return true;
      }

      private replayPendingMessages(): void {
        const messages = this.pendingMessages.splice(0);
        for (const data of messages) {
          this.bridge?.receiveMessage(data);
        }
      }

      private async initializeEmbind(
        options: SonareRealtimeEngineWorkletProcessorOptions,
        port?: WorkletPort,
      ): Promise<void> {
        try {
          await initializeEmbindInWorklet(options.wasmBinary);
          if (this.pendingMessagesOverflowed) {
            throw invalidState('AudioWorklet initialization message buffer overflowed.');
          }
          this.bridge = new SonareRealtimeEngineWorkletProcessor({
            ...options,
            transport: {
              postMessage: (message, transfer) => port?.postMessage?.(message, transfer),
              onMeter: (meter) => port?.postMessage?.(meter),
            },
          });
          for (const message of options.initialSyncMessages ?? []) {
            this.bridge.receiveMessage(message);
          }
          for (const command of options.initialCommands ?? []) {
            this.bridge.receiveMessage(command);
          }
          this.replayPendingMessages();
          port?.postMessage?.({ type: 'ready', runtimeTarget: 'embind' });
        } catch (error) {
          port?.postMessage?.({
            type: 'error',
            message: errorText(error),
            failure: failurePayload(error),
          });
        }
      }
    }
    scope.registerProcessor(name, RegisteredSonareRealtimeEngineWorkletProcessor);
  });
}
