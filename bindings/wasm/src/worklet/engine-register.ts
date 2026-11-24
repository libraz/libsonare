import { ErrorCode } from '../errors.js';
import { codedError, withEntry } from '../internal/entry.js';
import { WorkletProcessorLifecycle } from '../internal/worklet_lifecycle.js';
import type { WorkletPort } from '../internal/worklet_messages.js';
import type { SonareRealtimeEngineWorkletProcessorOptions } from '../public_types_worklet.js';
import type { WorkletInput, WorkletOutput } from './audio_types.js';
import { SonareRealtimeEngineWorkletProcessor } from './engine-processor.js';
import { assertEngineProcessorOptionTypes, assertKnownProcessorOptions } from './guards.js';

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
      private readonly lifecycle: WorkletProcessorLifecycle<
        SonareRealtimeEngineWorkletProcessor,
        unknown
      >;
      readonly port?: WorkletPort;

      constructor(options?: { processorOptions?: SonareRealtimeEngineWorkletProcessorOptions }) {
        super();
        const port = this.port;
        // A missing processorOptions reaches the processor, which refuses the absent numChannels.
        const processorOptions =
          options?.processorOptions ?? ({} as SonareRealtimeEngineWorkletProcessorOptions);
        assertKnownProcessorOptions('engineProcessor', processorOptions);
        assertEngineProcessorOptionTypes('engineProcessor', processorOptions);
        this.lifecycle = new WorkletProcessorLifecycle({
          port,
          usesModule: true,
          wasmBinary: processorOptions.wasmBinary,
          create: () => {
            const bridge = new SonareRealtimeEngineWorkletProcessor({
              ...processorOptions,
              transport: {
                postMessage: (message, transfer) => port?.postMessage?.(message, transfer),
                onMeter: (meter) => port?.postMessage?.(meter),
              },
            });
            for (const message of processorOptions.initialSyncMessages ?? []) {
              bridge.receiveMessage(message);
            }
            for (const command of processorOptions.initialCommands ?? []) {
              bridge.receiveMessage(command);
            }
            return bridge;
          },
          ready: () => ({ type: 'ready', runtimeTarget: 'embind' }),
          // receiveMessage refuses a type it does not know on the engine's syncError channel.
          kind: 'engine',
          accepts: (_data): _data is unknown => true,
          deliver: (bridge, data) => bridge.receiveMessage(data),
        });
      }

      process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
        return this.lifecycle.process(inputs, outputs);
      }
    }
    scope.registerProcessor(name, RegisteredSonareRealtimeEngineWorkletProcessor);
  });
}
