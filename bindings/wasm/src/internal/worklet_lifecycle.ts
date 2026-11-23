/**
 * The lifecycle every stock AudioWorklet registration shares: start the processor once the realm's
 * module is ready, hold the port messages that arrive before it within a bound, announce `ready`,
 * report a refused message or a failed render quantum as `{ type: 'error', request, message, failure }`,
 * and stop rendering after a failure rather than letting the exception escape the audio thread.
 */
import type { WorkletInput, WorkletOutput } from '../worklet/audio_types.js';
import { initializeEmbindInWorklet, isEmbindReady } from '../worklet/embind-init.js';
import { invalidState, refuseType } from './entry.js';
import { errorText, failurePayload } from './failure_payload.js';
import type { WorkletErrorMessage, WorkletPort } from './worklet_messages.js';

/** Port messages held before the processor exists; one more fails its start instead of being dropped. */
const PENDING_MESSAGE_LIMIT = 1024;

interface WorkletBridge {
  process(inputs: WorkletInput, outputs: WorkletOutput): boolean;
}

/** What a registration supplies to {@link WorkletProcessorLifecycle}. */
export interface WorkletLifecycleSpec<B extends WorkletBridge, M> {
  port: WorkletPort | undefined;
  /** Whether the processor needs the WASM module in this realm. */
  usesModule: boolean;
  wasmBinary?: ArrayBuffer | Uint8Array;
  /** Builds the processor once the module is ready; a throw is reported as the `init` failure. */
  create(): B;
  /** The `ready` message to post, or `undefined` when the processor posts its own. */
  ready(bridge: B): Record<string, unknown> | undefined;
  /** Names the processor in the refusal of a message it does not take. */
  kind: string;
  /** Whether the processor takes a port message; any other is refused as a `TypeError` under `request: 'invalid'`. */
  accepts(data: unknown): data is M;
  /** Hands one accepted message to the processor; a throw is reported under the message's `type`. */
  deliver(bridge: B, message: M): void;
}

function messageType(data: unknown): unknown {
  return (data as { type?: unknown } | null)?.type;
}

/** One stock registration's processor: start, message delivery and the guarded render call. */
export class WorkletProcessorLifecycle<B extends WorkletBridge, M> {
  private bridge?: B;
  private failed = false;
  private overflowed = false;
  private readonly pending: unknown[] = [];

  constructor(private readonly spec: WorkletLifecycleSpec<B, M>) {
    if (!spec.usesModule || isEmbindReady()) {
      this.start();
    } else {
      initializeEmbindInWorklet(spec.wasmBinary).then(
        () => this.start(),
        (error: unknown) => this.fail('init', error),
      );
    }
    const port = spec.port;
    const onMessage = (event: { data: unknown }) => this.receive(event.data);
    if (port?.addEventListener) {
      port.addEventListener('message', onMessage);
      port.start?.();
    } else if (port) {
      port.onmessage = onMessage;
    }
  }

  /** The processor once started. */
  get current(): B | undefined {
    return this.bridge;
  }

  /** Renders one quantum; silence before the start, and silence plus `false` once anything failed. */
  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    const bridge = this.bridge;
    if (bridge && !this.failed) {
      try {
        return bridge.process(inputs, outputs);
      } catch (error) {
        this.fail('process', error);
      }
    }
    for (const output of outputs) {
      for (const channel of output) {
        channel.fill(0);
      }
    }
    return !this.failed;
  }

  private receive(data: unknown): void {
    if (this.bridge) {
      this.deliver(this.bridge, data);
    } else if (this.failed) {
      return;
    } else if (this.pending.length < PENDING_MESSAGE_LIMIT) {
      this.pending.push(data);
    } else {
      this.overflowed = true;
    }
  }

  private start(): void {
    try {
      if (this.overflowed) {
        throw invalidState('AudioWorklet initialization message buffer overflowed.');
      }
      const bridge = this.spec.create();
      for (const data of this.pending.splice(0)) {
        this.deliver(bridge, data);
      }
      this.bridge = bridge;
      const ready = this.spec.ready(bridge);
      if (ready !== undefined) {
        this.spec.port?.postMessage?.(ready);
      }
    } catch (error) {
      this.fail('init', error);
    }
  }

  private deliver(bridge: B, data: unknown): void {
    if (!this.spec.accepts(data)) {
      const type = String(messageType(data));
      this.report('invalid', refuseType(`Unrecognized ${this.spec.kind} message: ${type}`));
      return;
    }
    try {
      this.spec.deliver(bridge, data);
    } catch (error) {
      const type = messageType(data);
      this.report(typeof type === 'string' ? type : 'invalid', error);
    }
  }

  private fail(request: 'init' | 'process', error: unknown): void {
    this.failed = true;
    this.pending.length = 0;
    this.report(request, error);
  }

  private report(request: string, error: unknown): void {
    this.spec.port?.postMessage?.({
      type: 'error',
      request,
      message: errorText(error),
      failure: failurePayload(error),
    } satisfies WorkletErrorMessage);
  }
}
