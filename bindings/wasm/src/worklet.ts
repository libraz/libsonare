import { Mixer } from './index';
import type { AutomationCurve, MixerProcessResult } from './public_types';

export interface SonareWorkletProcessorOptions {
  sceneJson: string;
  sampleRate?: number;
  blockSize?: number;
  stripCount?: number;
  meterIntervalFrames?: number;
}

type WorkletInput = readonly (readonly Float32Array[])[];
type WorkletOutput = Float32Array[][];

export interface SonareWorkletScheduleInsertAutomationMessage {
  type: 'scheduleInsertAutomation';
  stripIndex: number;
  insertIndex: number;
  paramId: number;
  value: number;
  samplePos?: number;
  curve?: AutomationCurve;
}

export interface SonareWorkletSetMeterIntervalMessage {
  type: 'setMeterInterval';
  frames: number;
}

export interface SonareWorkletDestroyMessage {
  type: 'destroy';
}

export type SonareWorkletMessage =
  | SonareWorkletScheduleInsertAutomationMessage
  | SonareWorkletSetMeterIntervalMessage
  | SonareWorkletDestroyMessage;

export interface SonareWorkletMeterSnapshot {
  type: 'meter';
  frame: number;
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  correlation: number;
}

interface WorkletTransport {
  postMessage?: (message: SonareWorkletMeterSnapshot) => void;
  onMeter?: (meter: SonareWorkletMeterSnapshot) => void;
}

interface WorkletPort {
  postMessage?: (message: unknown) => void;
  onmessage?: (event: { data: unknown }) => void;
  addEventListener?: (type: 'message', listener: (event: { data: unknown }) => void) => void;
  start?: () => void;
}

function silence(blockSize: number): Float32Array {
  return new Float32Array(blockSize);
}

function toDb(value: number): number {
  return value > 0 ? 20 * Math.log10(value) : Number.NEGATIVE_INFINITY;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null;
}

function isWorkletMessage(value: unknown): value is SonareWorkletMessage {
  if (!isRecord(value) || typeof value.type !== 'string') {
    return false;
  }
  return (
    value.type === 'scheduleInsertAutomation' ||
    value.type === 'setMeterInterval' ||
    value.type === 'destroy'
  );
}

/**
 * AudioWorklet-style mixer bridge backed by the package's single `sonare.wasm`.
 *
 * The WASM module must already be initialized via `init()` before constructing
 * this bridge. Each AudioWorklet input is treated as one stereo strip:
 * `inputs[strip][0]` is left and `inputs[strip][1]` is right. Missing channels
 * are replaced with preallocated silence.
 */
export class SonareWorkletProcessor {
  readonly sampleRate: number;
  readonly blockSize: number;
  private mixer: Mixer;
  private leftInputs: Float32Array[];
  private rightInputs: Float32Array[];
  private silence: Float32Array;
  private closed = false;
  private processedFrames = 0;
  private lastMeterFrame = 0;
  private meterIntervalFrames: number;
  private transport?: WorkletTransport;

  constructor(options: SonareWorkletProcessorOptions, transport?: WorkletTransport) {
    if (!options.sceneJson) {
      throw new Error('sceneJson is required.');
    }
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.transport = transport;
    this.mixer = Mixer.fromSceneJson(options.sceneJson, this.sampleRate, this.blockSize);
    this.mixer.compile();
    const stripCount = options.stripCount ?? this.mixer.stripCount();
    this.silence = silence(this.blockSize);
    this.leftInputs = Array.from({ length: stripCount }, () => this.silence);
    this.rightInputs = Array.from({ length: stripCount }, () => this.silence);
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.closed) {
      return false;
    }
    const output = outputs[0];
    const leftOut = output?.[0];
    const rightOut = output?.[1];
    if (!leftOut) {
      return true;
    }
    const frames = leftOut.length;
    if (frames !== this.blockSize) {
      return false;
    }

    for (let strip = 0; strip < this.leftInputs.length; strip++) {
      const input = inputs[strip];
      const left = input?.[0];
      const right = input?.[1];
      this.leftInputs[strip] = left && left.length === frames ? left : this.silence;
      this.rightInputs[strip] = right && right.length === frames ? right : this.leftInputs[strip];
    }

    const mixed: MixerProcessResult = this.mixer.processStereo(this.leftInputs, this.rightInputs);
    leftOut.set(mixed.left.subarray(0, frames));
    if (rightOut) {
      rightOut.set(mixed.right.subarray(0, frames));
    }
    this.processedFrames += frames;
    this.publishMeter(mixed.left, mixed.right);
    return true;
  }

  receiveMessage(message: SonareWorkletMessage): void {
    if (this.closed) {
      return;
    }
    if (message.type === 'destroy') {
      this.destroy();
      return;
    }
    if (message.type === 'setMeterInterval') {
      this.meterIntervalFrames = Math.max(0, Math.floor(message.frames));
      return;
    }
    if (message.type === 'scheduleInsertAutomation') {
      this.mixer.scheduleInsertAutomation(
        message.stripIndex,
        message.insertIndex,
        message.paramId,
        message.samplePos ?? this.processedFrames,
        message.value,
        message.curve ?? 'linear',
      );
    }
  }

  destroy(): void {
    if (!this.closed) {
      this.mixer.delete();
      this.closed = true;
    }
  }

  private publishMeter(left: Float32Array, right: Float32Array): void {
    if (!this.transport || this.meterIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastMeterFrame < this.meterIntervalFrames) {
      return;
    }
    this.lastMeterFrame = this.processedFrames;

    let peakL = 0;
    let peakR = 0;
    let sumL = 0;
    let sumR = 0;
    let sumLR = 0;
    for (let i = 0; i < left.length; i++) {
      const l = left[i] ?? 0;
      const r = right[i] ?? 0;
      const absL = Math.abs(l);
      const absR = Math.abs(r);
      if (absL > peakL) {
        peakL = absL;
      }
      if (absR > peakR) {
        peakR = absR;
      }
      sumL += l * l;
      sumR += r * r;
      sumLR += l * r;
    }
    const rmsL = Math.sqrt(sumL / Math.max(1, left.length));
    const rmsR = Math.sqrt(sumR / Math.max(1, right.length));
    const denominator = Math.sqrt(sumL * sumR);
    const meter: SonareWorkletMeterSnapshot = {
      type: 'meter',
      frame: this.processedFrames,
      peakDbL: toDb(peakL),
      peakDbR: toDb(peakR),
      rmsDbL: toDb(rmsL),
      rmsDbR: toDb(rmsR),
      correlation: denominator > 0 ? sumLR / denominator : 0,
    };
    this.transport.onMeter?.(meter);
    this.transport.postMessage?.(meter);
  }
}

export function registerSonareWorkletProcessor(name = 'sonare-worklet-processor'): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareWorkletProcessor extends Base {
    private bridge: SonareWorkletProcessor;
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareWorkletProcessorOptions }) {
      super();
      const port = this.port;
      this.bridge = new SonareWorkletProcessor(options?.processorOptions ?? { sceneJson: '' }, {
        postMessage: (message) => port?.postMessage?.(message),
      });
      const onMessage = (event: { data: unknown }) => {
        if (isWorkletMessage(event.data)) {
          this.bridge.receiveMessage(event.data);
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
      return this.bridge.process(inputs, outputs);
    }
  }
  scope.registerProcessor(name, RegisteredSonareWorkletProcessor);
}
