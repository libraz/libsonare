import { init as initSonareModule, isInitialized } from '../index';
import type { SonareModule } from '../sonare.js';
import type { SonareRtModule } from '../sonare-rt';
import type { WorkletInput, WorkletOutput } from './audio_types';
import { SonareRealtimeEngineWorkletProcessor } from './engine-processor';
import { SonareRtRealtimeEngineRuntime } from './engine-runtime-rt';
import {
  isEngineCaptureRequestMessage,
  isEngineCommandRecord,
  isEngineSyncMessage,
  isEngineTransportRequestMessage,
} from './guards';
import type { SonareRealtimeEngineWorkletProcessorOptions, WorkletPort } from './messages';

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
    private rtBridge?: SonareRtRealtimeEngineRuntime;
    private readonly pendingMessages: unknown[] = [];
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareRealtimeEngineWorkletProcessorOptions }) {
      super();
      const port = this.port;
      const processorOptions = options?.processorOptions ?? {};
      if (processorOptions.runtimeTarget === 'sonare-rt') {
        void this.initializeSonareRt(processorOptions, port);
      } else {
        void this.initializeEmbind(processorOptions, port);
      }
      const onMessage = (event: { data: unknown }) => {
        if (!this.bridge && !this.rtBridge) {
          if (this.pendingMessages.length < 1024) {
            this.pendingMessages.push(event.data);
          }
          return;
        }
        if (isEngineCommandRecord(event.data)) {
          this.bridge?.receiveCommand(event.data);
          this.rtBridge?.receiveCommand(event.data);
        } else if (isEngineSyncMessage(event.data)) {
          this.bridge?.receiveSync(event.data);
          this.rtBridge?.receiveSync(event.data);
        } else if (isEngineCaptureRequestMessage(event.data)) {
          this.bridge?.receiveCaptureRequest(event.data);
          this.rtBridge?.receiveCaptureRequest(event.data, port);
        } else if (isEngineTransportRequestMessage(event.data)) {
          this.bridge?.receiveTransportRequest(event.data);
          this.rtBridge?.receiveTransportRequest(event.data, port);
        }
      };
      if (port?.addEventListener) {
        port.addEventListener('message', onMessage);
        port.start?.();
      } else if (port) {
        port.onmessage = onMessage;
      }
    }

    process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
      if (this.rtBridge) {
        return this.rtBridge.process(inputs, outputs);
      }
      if (this.bridge) {
        return this.bridge.process(inputs, outputs);
      }
      const output = outputs[0];
      for (const channel of output ?? []) {
        channel.fill(0);
      }
      return true;
    }

    private replayPendingMessages(port?: WorkletPort): void {
      const messages = this.pendingMessages.splice(0);
      for (const data of messages) {
        if (isEngineCommandRecord(data)) {
          this.bridge?.receiveCommand(data);
          this.rtBridge?.receiveCommand(data);
        } else if (isEngineSyncMessage(data)) {
          this.bridge?.receiveSync(data);
          this.rtBridge?.receiveSync(data);
        } else if (isEngineCaptureRequestMessage(data)) {
          this.bridge?.receiveCaptureRequest(data);
          this.rtBridge?.receiveCaptureRequest(data, port);
        } else if (isEngineTransportRequestMessage(data)) {
          this.bridge?.receiveTransportRequest(data);
          this.rtBridge?.receiveTransportRequest(data, port);
        }
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
        this.bridge = new SonareRealtimeEngineWorkletProcessor(options, {
          postMessage: (message) => port?.postMessage?.(message),
          onMeter: (meter) => port?.postMessage?.(meter),
        });
        for (const message of options.initialSyncMessages ?? []) {
          this.bridge.receiveSync(message);
        }
        for (const command of options.initialCommands ?? []) {
          this.bridge.receiveCommand(command);
        }
        this.replayPendingMessages(port);
        port?.postMessage?.({ type: 'ready', runtimeTarget: 'embind' });
      } catch (error) {
        port?.postMessage?.({
          type: 'error',
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }

    private async initializeSonareRt(
      options: SonareRealtimeEngineWorkletProcessorOptions,
      port?: WorkletPort,
    ): Promise<void> {
      try {
        if (!options.rtModuleUrl) {
          throw new Error('rtModuleUrl is required for sonare-rt AudioWorklet runtime.');
        }
        const rtModuleUrl = options.rtModuleUrl;
        const memory = new WebAssembly.Memory({ initial: 1024, maximum: 1024, shared: true });
        const globalFactory = (
          globalThis as typeof globalThis & {
            SonareRtModuleFactory?: (options?: {
              wasmMemory?: WebAssembly.Memory;
              wasmBinary?: ArrayBuffer | Uint8Array;
              locateFile?: (path: string) => string;
            }) => Promise<SonareRtModule>;
          }
        ).SonareRtModuleFactory;
        const moduleFactory = globalFactory
          ? { default: globalFactory }
          : ((await import(/* @vite-ignore */ rtModuleUrl)) as {
              default: (options?: {
                wasmMemory?: WebAssembly.Memory;
                wasmBinary?: ArrayBuffer | Uint8Array;
                locateFile?: (path: string) => string;
              }) => Promise<SonareRtModule>;
            });
        const module = await moduleFactory.default({
          wasmMemory: memory,
          wasmBinary: options.rtWasmBinary,
          locateFile: (path) => rtModuleUrl.replace(/[^/]*$/, path),
        });
        this.rtBridge = new SonareRtRealtimeEngineRuntime({
          module,
          memory,
          sampleRate: options.sampleRate,
          blockSize: options.blockSize,
          channelCount: options.channelCount,
          commandSharedBuffer: options.commandSharedBuffer,
          commandRingCapacity: options.commandRingCapacity,
          telemetrySharedBuffer: options.telemetrySharedBuffer,
          telemetryRingCapacity: options.telemetryRingCapacity,
        });
        this.replayPendingMessages(port);
        port?.postMessage?.({ type: 'ready', runtimeTarget: 'sonare-rt' });
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
