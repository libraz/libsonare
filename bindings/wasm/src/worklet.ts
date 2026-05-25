import type { MixerRealtimeBuffer } from './index';
import { Mixer } from './index';
import type { AutomationCurve } from './public_types';

export interface SonareWorkletProcessorOptions {
  sceneJson: string;
  sampleRate?: number;
  blockSize?: number;
  stripCount?: number;
  meterIntervalFrames?: number;
  meterSharedBuffer?: SharedArrayBuffer;
  meterRingCapacity?: number;
  spectrumIntervalFrames?: number;
  spectrumBands?: number;
  spectrumSharedBuffer?: SharedArrayBuffer;
  spectrumRingCapacity?: number;
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

export interface SonareWorkletSpectrumSnapshot {
  type: 'spectrum';
  frame: number;
  bands: Float32Array;
}

export type SonareWorkletTransportMessage =
  | SonareWorkletMeterSnapshot
  | SonareWorkletSpectrumSnapshot;

export const SONARE_METER_RING_HEADER_INTS = 4;
export const SONARE_METER_RING_RECORD_FLOATS = 6;
export const SONARE_SPECTRUM_RING_HEADER_INTS = 5;

interface WorkletTransport {
  postMessage?: (message: SonareWorkletTransportMessage) => void;
  onMeter?: (meter: SonareWorkletMeterSnapshot) => void;
  onSpectrum?: (spectrum: SonareWorkletSpectrumSnapshot) => void;
}

export interface SonareMeterRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Float32Array;
  capacity: number;
}

export interface SonareMeterRingReadResult {
  nextReadIndex: number;
  meters: SonareWorkletMeterSnapshot[];
}

export interface SonareSpectrumRingBuffer {
  sharedBuffer: SharedArrayBuffer;
  header: Int32Array;
  records: Float32Array;
  capacity: number;
  bands: number;
}

export interface SonareSpectrumRingReadResult {
  nextReadIndex: number;
  spectra: SonareWorkletSpectrumSnapshot[];
}

interface SharedMeterRingWriter {
  header: Int32Array;
  records: Float32Array;
  capacity: number;
}

interface SharedSpectrumRingWriter {
  header: Int32Array;
  records: Float32Array;
  capacity: number;
  bands: number;
  recordFloats: number;
}

interface WorkletPort {
  postMessage?: (message: unknown) => void;
  onmessage?: (event: { data: unknown }) => void;
  addEventListener?: (type: 'message', listener: (event: { data: unknown }) => void) => void;
  start?: () => void;
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

export function sonareMeterRingBufferByteLength(capacity: number): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  return (
    SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * SONARE_METER_RING_RECORD_FLOATS * Float32Array.BYTES_PER_ELEMENT
  );
}

export function createSonareMeterRingBuffer(capacity = 128): SonareMeterRingBuffer {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const sharedBuffer = new SharedArrayBuffer(sonareMeterRingBufferByteLength(clampedCapacity));
  const ring = meterRingFromSharedBuffer(sharedBuffer, clampedCapacity);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, clampedCapacity);
  Atomics.store(ring.header, 2, SONARE_METER_RING_RECORD_FLOATS);
  Atomics.store(ring.header, 3, 0);
  return { sharedBuffer, header: ring.header, records: ring.records, capacity: ring.capacity };
}

export function readSonareMeterRingBuffer(
  ring: SonareMeterRingBuffer,
  readIndex = 0,
): SonareMeterRingReadResult {
  const writeIndex = Atomics.load(ring.header, 0);
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const meters: SonareWorkletMeterSnapshot[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    const offset = (index % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
    meters.push({
      type: 'meter',
      frame: ring.records[offset],
      peakDbL: ring.records[offset + 1],
      peakDbR: ring.records[offset + 2],
      rmsDbL: ring.records[offset + 3],
      rmsDbR: ring.records[offset + 4],
      correlation: ring.records[offset + 5],
    });
  }
  return { nextReadIndex: writeIndex, meters };
}

export function sonareSpectrumRingBufferByteLength(capacity: number, bands = 16): number {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const clampedBands = Math.max(1, Math.floor(bands));
  return (
    SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT +
    clampedCapacity * (2 + clampedBands) * Float32Array.BYTES_PER_ELEMENT
  );
}

export function createSonareSpectrumRingBuffer(
  capacity = 128,
  bands = 16,
): SonareSpectrumRingBuffer {
  const clampedCapacity = Math.max(1, Math.floor(capacity));
  const clampedBands = Math.max(1, Math.floor(bands));
  const sharedBuffer = new SharedArrayBuffer(
    sonareSpectrumRingBufferByteLength(clampedCapacity, clampedBands),
  );
  const ring = spectrumRingFromSharedBuffer(sharedBuffer, clampedCapacity, clampedBands);
  Atomics.store(ring.header, 0, 0);
  Atomics.store(ring.header, 1, clampedCapacity);
  Atomics.store(ring.header, 2, ring.recordFloats);
  Atomics.store(ring.header, 3, clampedBands);
  Atomics.store(ring.header, 4, 0);
  return {
    sharedBuffer,
    header: ring.header,
    records: ring.records,
    capacity: ring.capacity,
    bands: ring.bands,
  };
}

export function readSonareSpectrumRingBuffer(
  ring: SonareSpectrumRingBuffer,
  readIndex = 0,
): SonareSpectrumRingReadResult {
  const writeIndex = Atomics.load(ring.header, 0);
  const recordFloats = Atomics.load(ring.header, 2) || 2 + ring.bands;
  const bands = Atomics.load(ring.header, 3) || ring.bands;
  const nextReadIndex = Math.max(0, Math.min(readIndex, writeIndex));
  const firstReadable = Math.max(nextReadIndex, writeIndex - ring.capacity);
  const spectra: SonareWorkletSpectrumSnapshot[] = [];
  for (let index = firstReadable; index < writeIndex; index++) {
    const offset = (index % ring.capacity) * recordFloats;
    const values = new Float32Array(bands);
    values.set(ring.records.subarray(offset + 2, offset + 2 + bands));
    spectra.push({ type: 'spectrum', frame: ring.records[offset], bands: values });
  }
  return { nextReadIndex: writeIndex, spectra };
}

function meterRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
): SharedMeterRingWriter {
  const headerBytes = SONARE_METER_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_METER_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const capacity = Math.max(1, Math.floor(existingCapacity || fallbackCapacity || 1));
  const minBytes = sonareMeterRingBufferByteLength(capacity);
  if (sharedBuffer.byteLength < minBytes) {
    throw new Error('meterSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 1, capacity);
  Atomics.store(header, 2, SONARE_METER_RING_RECORD_FLOATS);
  return {
    header,
    records: new Float32Array(
      sharedBuffer,
      headerBytes,
      capacity * SONARE_METER_RING_RECORD_FLOATS,
    ),
    capacity,
  };
}

function spectrumRingFromSharedBuffer(
  sharedBuffer: SharedArrayBuffer,
  fallbackCapacity?: number,
  fallbackBands?: number,
): SharedSpectrumRingWriter {
  const headerBytes = SONARE_SPECTRUM_RING_HEADER_INTS * Int32Array.BYTES_PER_ELEMENT;
  const header = new Int32Array(sharedBuffer, 0, SONARE_SPECTRUM_RING_HEADER_INTS);
  const existingCapacity = Atomics.load(header, 1);
  const existingBands = Atomics.load(header, 3);
  const capacity = Math.max(1, Math.floor(existingCapacity || fallbackCapacity || 1));
  const bands = Math.max(1, Math.floor(existingBands || fallbackBands || 16));
  const recordFloats = 2 + bands;
  const minBytes = sonareSpectrumRingBufferByteLength(capacity, bands);
  if (sharedBuffer.byteLength < minBytes) {
    throw new Error('spectrumSharedBuffer is too small for the requested ring capacity.');
  }
  Atomics.store(header, 1, capacity);
  Atomics.store(header, 2, recordFloats);
  Atomics.store(header, 3, bands);
  return {
    header,
    records: new Float32Array(sharedBuffer, headerBytes, capacity * recordFloats),
    capacity,
    bands,
    recordFloats,
  };
}

function magnitudeToDb(value: number): number {
  return value > 1.0e-12 ? 20 * Math.log10(value) : -120;
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
  private realtime: MixerRealtimeBuffer;
  private closed = false;
  private processedFrames = 0;
  private lastMeterFrame = 0;
  private meterIntervalFrames: number;
  private spectrumIntervalFrames: number;
  private lastSpectrumFrame = 0;
  private transport?: WorkletTransport;
  private meterRing?: SharedMeterRingWriter;
  private spectrumRing?: SharedSpectrumRingWriter;
  private spectrumBands: Float32Array;

  constructor(options: SonareWorkletProcessorOptions, transport?: WorkletTransport) {
    if (!options.sceneJson) {
      throw new Error('sceneJson is required.');
    }
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.spectrumIntervalFrames = Math.max(0, Math.floor(options.spectrumIntervalFrames ?? 0));
    this.transport = transport;
    this.meterRing = options.meterSharedBuffer
      ? meterRingFromSharedBuffer(options.meterSharedBuffer, options.meterRingCapacity)
      : undefined;
    this.spectrumRing = options.spectrumSharedBuffer
      ? spectrumRingFromSharedBuffer(
          options.spectrumSharedBuffer,
          options.spectrumRingCapacity,
          options.spectrumBands,
        )
      : undefined;
    const spectrumBandCount = this.spectrumRing?.bands ?? Math.max(1, options.spectrumBands ?? 16);
    this.spectrumBands = new Float32Array(spectrumBandCount);
    this.mixer = Mixer.fromSceneJson(options.sceneJson, this.sampleRate, this.blockSize);
    this.mixer.compile();
    const sceneStripCount = this.mixer.stripCount();
    const stripCount = options.stripCount ?? sceneStripCount;
    if (stripCount !== sceneStripCount) {
      throw new Error('stripCount must match the scene strip count.');
    }
    this.realtime = this.mixer.createRealtimeBuffer();
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

    for (let strip = 0; strip < this.realtime.leftInputs.length; strip++) {
      const input = inputs[strip];
      const left = input?.[0];
      const right = input?.[1];
      const leftTarget = this.realtime.leftInputs[strip];
      const rightTarget = this.realtime.rightInputs[strip];
      if (left && left.length === frames) {
        leftTarget.set(left);
        if (right && right.length === frames) {
          rightTarget.set(right);
        } else {
          rightTarget.set(left);
        }
      } else {
        leftTarget.fill(0);
        rightTarget.fill(0);
      }
    }

    this.realtime.process(frames);
    leftOut.set(this.realtime.outLeft);
    if (rightOut) {
      rightOut.set(this.realtime.outRight);
    }
    this.processedFrames += frames;
    this.publishMeter(this.realtime.outLeft, this.realtime.outRight);
    this.publishSpectrum(this.realtime.outLeft, this.realtime.outRight);
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
    if (this.meterRing) {
      this.writeMeterRing(meter);
    } else {
      this.transport.postMessage?.(meter);
    }
  }

  private writeMeterRing(meter: SonareWorkletMeterSnapshot): void {
    const ring = this.meterRing;
    if (!ring) {
      return;
    }
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
    ring.records[offset] = meter.frame;
    ring.records[offset + 1] = meter.peakDbL;
    ring.records[offset + 2] = meter.peakDbR;
    ring.records[offset + 3] = meter.rmsDbL;
    ring.records[offset + 4] = meter.rmsDbR;
    ring.records[offset + 5] = meter.correlation;
    Atomics.store(ring.header, 0, writeIndex + 1);
    if (writeIndex + 1 > ring.capacity) {
      Atomics.store(ring.header, 3, writeIndex + 1 - ring.capacity);
    }
  }

  private publishSpectrum(left: Float32Array, right: Float32Array): void {
    if (this.spectrumIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastSpectrumFrame < this.spectrumIntervalFrames) {
      return;
    }
    this.lastSpectrumFrame = this.processedFrames;
    this.computeSpectrum(left, right);
    if (this.spectrumRing) {
      this.writeSpectrumRing(this.processedFrames, this.spectrumBands);
      return;
    }
    const spectrum: SonareWorkletSpectrumSnapshot = {
      type: 'spectrum',
      frame: this.processedFrames,
      bands: new Float32Array(this.spectrumBands),
    };
    this.transport?.onSpectrum?.(spectrum);
    this.transport?.postMessage?.(spectrum);
  }

  private computeSpectrum(left: Float32Array, right: Float32Array): void {
    const n = Math.max(1, Math.min(left.length, right.length));
    for (let band = 0; band < this.spectrumBands.length; band++) {
      const bin = band + 1;
      let real = 0;
      let imag = 0;
      for (let i = 0; i < n; i++) {
        const sample = 0.5 * ((left[i] ?? 0) + (right[i] ?? 0));
        const phase = (-2 * Math.PI * bin * i) / n;
        real += sample * Math.cos(phase);
        imag += sample * Math.sin(phase);
      }
      this.spectrumBands[band] = magnitudeToDb((2 * Math.hypot(real, imag)) / n);
    }
  }

  private writeSpectrumRing(frame: number, bands: Float32Array): void {
    const ring = this.spectrumRing;
    if (!ring) {
      return;
    }
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * ring.recordFloats;
    ring.records[offset] = frame;
    ring.records[offset + 1] = bands.length;
    ring.records.set(bands.subarray(0, ring.bands), offset + 2);
    Atomics.store(ring.header, 0, writeIndex + 1);
    if (writeIndex + 1 > ring.capacity) {
      Atomics.store(ring.header, 4, writeIndex + 1 - ring.capacity);
    }
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
