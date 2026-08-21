import { init as initSonareModule, isInitialized } from '../index';
import type { SonareModule } from '../sonare.js';
import type { WorkletInput, WorkletOutput } from './audio_types';
import { SonareRealtimeEngineWorkletProcessor } from './engine-processor';
import {
  isEngineCaptureRequestMessage,
  isEngineCommandRecord,
  isEngineSyncMessage,
  isEngineTransportRequestMessage,
} from './guards';
import type { SonareRealtimeEngineWorkletProcessorOptions, WorkletPort } from './messages';
import { isRecord } from './protocol';

export function registerSonareRealtimeEngineWorkletProcessor(
  name = 'sonare-realtime-engine-processor',
): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
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
      const processorOptions = options?.processorOptions ?? {};
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
        this.routeMessage(event.data);
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

    /**
     * Single dispatch point for the port, shared with the buffered replay so
     * the two cannot recognize different message sets.
     */
    private routeMessage(data: unknown): void {
      const bridge = this.bridge;
      if (!bridge) {
        return;
      }
      if (isEngineCommandRecord(data)) {
        bridge.receiveCommand(data);
      } else if (isEngineSyncMessage(data)) {
        bridge.receiveSync(data);
      } else if (isEngineCaptureRequestMessage(data)) {
        bridge.receiveCaptureRequest(data);
      } else if (isEngineTransportRequestMessage(data)) {
        bridge.receiveTransportRequest(data);
      } else if (isRecord(data) && typeof data.type === 'string' && data.type.startsWith('sync')) {
        // A sync the guard does not accept would otherwise vanish, leaving the
        // live engine out of step with the offline mirror and nothing to see.
        // Report it through the same channel as a sync the engine rejected.
        this.port?.postMessage?.({
          type: 'syncError',
          syncType: data.type,
          message: `Unrecognized worklet sync message: ${data.type}`,
        });
      }
    }

    private replayPendingMessages(): void {
      const messages = this.pendingMessages.splice(0);
      for (const data of messages) {
        this.routeMessage(data);
      }
    }

    private async initializeEmbind(
      options: SonareRealtimeEngineWorkletProcessorOptions,
      port?: WorkletPort,
    ): Promise<void> {
      try {
        const initPromise = (
          globalThis as typeof globalThis & { SonareEmbindInitPromise?: Promise<void> }
        ).SonareEmbindInitPromise;
        if (initPromise) {
          await initPromise;
        }
        if (!isInitialized()) {
          type EmbindModuleFactory = (options?: {
            locateFile?: (path: string, prefix: string) => string;
            wasmBinary?: ArrayBuffer | Uint8Array;
          }) => Promise<SonareModule>;
          const moduleFactory = (
            globalThis as typeof globalThis & {
              SonareEmbindModuleFactory?: EmbindModuleFactory;
            }
          ).SonareEmbindModuleFactory;
          if (!moduleFactory) {
            throw new Error('embind realtime engine module is not initialized.');
          }
          await initSonareModule({
            locateFile: (path) => path,
            wasmBinary: options.wasmBinary,
            moduleFactory,
          });
        }
        if (this.pendingMessagesOverflowed) {
          throw new Error('AudioWorklet initialization message buffer overflowed.');
        }
        this.bridge = new SonareRealtimeEngineWorkletProcessor(options, {
          postMessage: (message, transfer) => port?.postMessage?.(message, transfer),
          onMeter: (meter) => port?.postMessage?.(meter),
        });
        for (const message of options.initialSyncMessages ?? []) {
          this.bridge.receiveSync(message);
        }
        for (const command of options.initialCommands ?? []) {
          this.bridge.receiveCommand(command);
        }
        this.replayPendingMessages();
        port?.postMessage?.({ type: 'ready', runtimeTarget: 'embind' });
      } catch (error) {
        port?.postMessage?.({
          type: 'error',
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }
  }
  scope.registerProcessor(name, RegisteredSonareRealtimeEngineWorkletProcessor);
}
