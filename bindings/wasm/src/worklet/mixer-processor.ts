import { failurePayload } from '../_failure_payload.js';
import type { MixerRealtimeBuffer } from '../index.js';
import { Mixer } from '../index.js';
import { leadingSamples, type WorkletInput, type WorkletOutput } from './audio_types.js';
import { initializeEmbindInWorklet, isEmbindReady } from './embind-init.js';
import { isWorkletMessage, requireIntegerOption, resolveScopeSampleRate } from './guards.js';
import type {
  SonareWorkletMessage,
  SonareWorkletProcessorOptions,
  WorkletPort,
  WorkletTransport,
} from './messages.js';
import {
  commitOverwriteWrite,
  encodeFrameHi,
  encodeFrameLo,
  magnitudeToDb,
  meterRingFromSharedBuffer,
  type SharedMeterRingWriter,
  type SharedSpectrumRingWriter,
  SONARE_FLOOR_DB,
  SONARE_METER_RING_CHANNEL_COUNT_OFFSET,
  SONARE_METER_RING_RECORD_FLOATS,
  type SonareWorkletMeterSnapshot,
  type SonareWorkletSpectrumSnapshot,
  spectrumRingFromSharedBuffer,
  tryBeginOverwriteWrite,
} from './protocol.js';

/**
 * AudioWorklet-style mixer bridge backed by the package's single `sonare.wasm`.
 *
 * The WASM module must already be initialized via `init()` before constructing
 * this bridge; the registered processor initializes it itself from
 * `wasmBinary` when it is not (see {@link SonareWorkletProcessorOptions}). Each AudioWorklet input is treated as one stereo strip:
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
  /**
   * Reused meter record, so a publish writes fields instead of allocating one.
   *
   * `targetId` is always the master and the four LUFS / gain-reduction fields
   * are always unavailable here — the mixer worklet does not run the
   * K-weighting filters or the engine's pre-trim input tap — so finite floor
   * values are set once rather than per interval.
   */
  private readonly meterScratch: SonareWorkletMeterSnapshot = {
    type: 'meter',
    targetId: 0,
    frame: 0,
    peakDbL: 0,
    peakDbR: 0,
    rmsDbL: 0,
    rmsDbR: 0,
    correlation: 0,
    truePeakDbL: 0,
    truePeakDbR: 0,
    momentaryLufs: Number.NaN,
    shortTermLufs: Number.NaN,
    integratedLufs: Number.NaN,
    gainReductionDb: Number.NaN,
    inputPeakDbL: SONARE_FLOOR_DB,
    inputPeakDbR: SONARE_FLOOR_DB,
  };
  private spectrumRing?: SharedSpectrumRingWriter;
  private spectrumBands: Float32Array;

  constructor(options: SonareWorkletProcessorOptions, transport?: WorkletTransport) {
    if (!options.sceneJson) {
      throw new TypeError('sceneJson is required.');
    }
    this.sampleRate = resolveScopeSampleRate(options.sampleRate, 'SonareWorkletProcessor');
    this.blockSize = options.blockSize ?? 128;
    // Zero is the off switch for both publications, so the floor is 0.
    this.meterIntervalFrames = requireIntegerOption(
      options.meterIntervalFrames,
      2048,
      'meterIntervalFrames',
      0,
    );
    this.spectrumIntervalFrames = requireIntegerOption(
      options.spectrumIntervalFrames,
      0,
      'spectrumIntervalFrames',
      0,
    );
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
      throw new RangeError('stripCount must match the scene strip count.');
    }
    this.realtime = this.mixer.createRealtimeBuffer();
    // The mixer meters the master it just produced, so the true-peak filter sees
    // every block without the worklet copying the output back in. 4x is the
    // BS.1770-4 Annex 2 minimum.
    this.mixer.configureMeter(this.meterIntervalFrames > 0, 4);
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
    // The mixer's realtime heap buffers are sized to blockSize. A render quantum
    // that differs from blockSize (e.g. a future browser using a quantum other
    // than 128, or a misconfigured blockSize) must NOT return false here:
    // returning false permanently terminates the AudioWorkletProcessor and
    // silently kills the node mid-stream. Instead degrade gracefully by
    // processing min(frames, blockSize) and zero-filling any remainder.
    const usable = Math.min(frames, this.blockSize);

    // Each getter re-checks the native topology and heap, and nothing can change
    // either within one callback, so the views are read once into locals.
    const leftInputs = this.realtime.leftInputs;
    const rightInputs = this.realtime.rightInputs;
    for (let strip = 0; strip < leftInputs.length; strip++) {
      const input = inputs[strip];
      const left = input?.[0];
      const right = input?.[1];
      const leftTarget = leftInputs[strip];
      const rightTarget = rightInputs[strip];
      if (left && left.length >= usable) {
        leftTarget.set(leadingSamples(left, usable));
        if (right && right.length >= usable) {
          rightTarget.set(leadingSamples(right, usable));
        } else {
          rightTarget.set(leadingSamples(left, usable));
        }
      } else {
        leftTarget.fill(0);
        rightTarget.fill(0);
      }
    }

    this.realtime.process(usable);
    const mixedLeft = leadingSamples(this.realtime.outLeft, usable);
    const mixedRight = leadingSamples(this.realtime.outRight, usable);
    if (usable !== frames) {
      // frames > blockSize: zero the tail the mixer did not produce.
      leftOut.fill(0, usable);
      rightOut?.fill(0, usable);
    }
    leftOut.set(mixedLeft);
    rightOut?.set(mixedRight);
    this.processedFrames += usable;
    this.publishMeter();
    this.publishSpectrum(mixedLeft, mixedRight);
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
      // The constructor option's domain; a bad value is refused before any state changes.
      const frames = requireIntegerOption(message.frames, Number.NaN, 'setMeterInterval frames', 0);
      // Toggling metering also toggles the mixer's meter, so a disabled meter
      // costs nothing per block. Re-enabling restarts it: the filter history and
      // the published snapshot are from before the gap, and carrying them
      // forward would report a peak the caller never asked to be measured.
      if (frames > 0 !== this.meterIntervalFrames > 0) {
        this.mixer.configureMeter(frames > 0, 4);
      }
      this.meterIntervalFrames = frames;
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

  private publishMeter(): void {
    // Symmetric with the engine processor: a ring-only configuration still has
    // meters to publish, so the absence of a transport alone must not skip it.
    if ((!this.transport && !this.meterRing) || this.meterIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastMeterFrame < this.meterIntervalFrames) {
      return;
    }
    this.lastMeterFrame = this.processedFrames;

    // Latch the reading into the mixer's own scratch and read the seven fields
    // back as numbers. `meterSnapshot()` returns a fresh embind object, so
    // calling it here would allocate on the render thread once per interval —
    // about 23 per second at the default — no matter what this file then does
    // with the result. Scratch field order, shared with the engine's
    // meterScratchValue: 0 peakDbL, 1 peakDbR, 2 rmsDbL, 3 rmsDbR,
    // 4 correlation, 5 truePeakDbL, 6 truePeakDbR.
    if (!this.mixer.latchMeterSnapshot()) {
      // The meter has never been enabled, so there is no reading to publish.
      return;
    }
    if (this.meterRing) {
      // Fill the reusable record and serialise it straight into the ring, which
      // exists precisely so this thread allocates and posts nothing. The record
      // never escapes, so reuse is safe here in a way it is not on the
      // postMessage path below.
      const meter = this.meterScratch;
      meter.frame = this.processedFrames;
      meter.peakDbL = this.mixer.meterScratchValue(0);
      meter.peakDbR = this.mixer.meterScratchValue(1);
      meter.rmsDbL = this.mixer.meterScratchValue(2);
      meter.rmsDbR = this.mixer.meterScratchValue(3);
      meter.correlation = this.mixer.meterScratchValue(4);
      meter.truePeakDbL = this.mixer.meterScratchValue(5);
      meter.truePeakDbR = this.mixer.meterScratchValue(6);
      this.writeMeterRing(meter);
      return;
    }
    // No ring: this is the structured-clone fallback, already off the
    // zero-allocation contract, and a listener may retain what it is handed. It
    // still reads the latched scalars rather than `meterSnapshot()`, so the one
    // object it does build is the one it posts.
    const meter: SonareWorkletMeterSnapshot = {
      type: 'meter',
      targetId: 0,
      frame: this.processedFrames,
      peakDbL: this.mixer.meterScratchValue(0),
      peakDbR: this.mixer.meterScratchValue(1),
      rmsDbL: this.mixer.meterScratchValue(2),
      rmsDbR: this.mixer.meterScratchValue(3),
      correlation: this.mixer.meterScratchValue(4),
      truePeakDbL: this.mixer.meterScratchValue(5),
      truePeakDbR: this.mixer.meterScratchValue(6),
      // Declared unavailable rather than floored: the mixer worklet does not run
      // the K-weighting filters, and a floor value would read as silence.
      momentaryLufs: Number.NaN,
      shortTermLufs: Number.NaN,
      integratedLufs: Number.NaN,
      gainReductionDb: Number.NaN,
      inputPeakDbL: SONARE_FLOOR_DB,
      inputPeakDbR: SONARE_FLOOR_DB,
    };
    // Alternative channels for one record, not a broadcast pair: a transport
    // that supplies both (the engine registration resolves both to
    // `port.postMessage`) would otherwise deliver every record twice and double
    // the host's peak-hold decay rate.
    if (this.transport?.onMeter) {
      this.transport.onMeter(meter);
    } else {
      this.transport?.postMessage?.(meter);
    }
  }

  private writeMeterRing(meter: SonareWorkletMeterSnapshot): void {
    const ring = this.meterRing;
    if (!ring) {
      return;
    }
    const header = ring.header;
    const records = ring.records;
    const slotStates = ring.slotStates;
    const capacity = ring.capacity;
    // All property reads and numeric coercions happen before the claim. The
    // region after it contains only fixed primitive stores and the commit.
    const frame = +meter.frame;
    const frameLo = encodeFrameLo(frame);
    const frameHi = encodeFrameHi(frame);
    const targetId = +meter.targetId;
    const peakDbL = +meter.peakDbL;
    const peakDbR = +meter.peakDbR;
    const rmsDbL = +meter.rmsDbL;
    const rmsDbR = +meter.rmsDbR;
    const correlation = +meter.correlation;
    const truePeakDbL = +meter.truePeakDbL;
    const truePeakDbR = +meter.truePeakDbR;
    const momentaryLufs = +meter.momentaryLufs;
    const shortTermLufs = +meter.shortTermLufs;
    const integratedLufs = +meter.integratedLufs;
    const gainReductionDb = +meter.gainReductionDb;
    const inputPeakDbL = +meter.inputPeakDbL;
    const inputPeakDbR = +meter.inputPeakDbR;
    const channelCount = 2;
    const writeIndex = tryBeginOverwriteWrite(header, slotStates, capacity);
    if (writeIndex === undefined) {
      return;
    }
    const offset = (writeIndex % capacity) * SONARE_METER_RING_RECORD_FLOATS;
    records[offset] = frameLo;
    records[offset + 1] = frameHi;
    records[offset + 2] = targetId;
    records[offset + 3] = peakDbL;
    records[offset + 4] = peakDbR;
    records[offset + 5] = rmsDbL;
    records[offset + 6] = rmsDbR;
    records[offset + 7] = correlation;
    records[offset + 8] = truePeakDbL;
    records[offset + 9] = truePeakDbR;
    records[offset + 10] = momentaryLufs;
    records[offset + 11] = shortTermLufs;
    records[offset + 12] = integratedLufs;
    records[offset + 13] = gainReductionDb;
    records[offset + 14] = inputPeakDbL;
    records[offset + 15] = inputPeakDbR;
    records[offset + SONARE_METER_RING_CHANNEL_COUNT_OFFSET] = channelCount;
    commitOverwriteWrite(header, slotStates, capacity, writeIndex);
    // An overwrite ring keeps no drop count: readers detect overrun from the
    // cursor distance, so header slot 3 is left at its initial 0.
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
    // One record, one delivery -- see publishMeter above.
    if (this.transport?.onSpectrum) {
      this.transport.onSpectrum(spectrum);
    } else {
      this.transport?.postMessage?.(spectrum);
    }
  }

  private computeSpectrum(left: Float32Array, right: Float32Array): void {
    // Coarse per-render-quantum band energy, NOT a full FFT analyzer: each band
    // is a single-bin DFT (bin = band + 1) evaluated over the current block of n
    // samples. Bins at or above the block Nyquist (band + 1 > floor(n / 2))
    // alias, so the evaluated band count is clamped to floor(n / 2) and any
    // higher bands are pinned to the silence floor. Bin resolution is therefore
    // tied to the render quantum (typically 128 samples); treat the output as a
    // rough spectral tilt, not a precise spectrum.
    const n = Math.max(1, Math.min(left.length, right.length));
    const maxBand = Math.floor(n / 2);
    for (let band = 0; band < this.spectrumBands.length; band++) {
      if (band >= maxBand) {
        this.spectrumBands[band] = magnitudeToDb(0);
        continue;
      }
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
    const header = ring.header;
    const records = ring.records;
    const slotStates = ring.slotStates;
    const capacity = ring.capacity;
    const recordFloats = ring.recordFloats;
    const bandCount = ring.bands;
    const frameLo = encodeFrameLo(frame);
    const frameHi = encodeFrameHi(frame);
    const writeIndex = tryBeginOverwriteWrite(header, slotStates, capacity);
    if (writeIndex === undefined) {
      return;
    }
    const offset = (writeIndex % capacity) * recordFloats;
    records[offset] = frameLo;
    records[offset + 1] = frameHi;
    records[offset + 2] = bandCount;
    for (let band = 0; band < bandCount; band++) {
      records[offset + 3 + band] = bands[band];
    }
    commitOverwriteWrite(header, slotStates, capacity, writeIndex);
    // As in writeMeterRing, header slot 4 (the overflow slot) stays 0.
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
    private bridge?: SonareWorkletProcessor;
    private readonly pendingMessages: SonareWorkletMessage[] = [];
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareWorkletProcessorOptions }) {
      super();
      const port = this.port;
      const processorOptions = options?.processorOptions ?? { sceneJson: '' };
      // A refused message leaves the processor as it was and is reported, not dropped.
      const deliver = (bridge: SonareWorkletProcessor, message: SonareWorkletMessage) => {
        try {
          bridge.receiveMessage(message);
        } catch (error) {
          port?.postMessage?.({
            type: 'error',
            message: error instanceof Error ? error.message : String(error),
            failure: failurePayload(error),
          });
        }
      };
      const start = () => {
        this.bridge = new SonareWorkletProcessor(processorOptions, {
          postMessage: (message) => port?.postMessage?.(message),
        });
        for (const message of this.pendingMessages.splice(0)) {
          deliver(this.bridge, message);
        }
      };
      if (isEmbindReady()) {
        start();
      } else {
        // The module is not initialized in this realm: build it the way the
        // realtime engine processor does, then construct the mixer.
        initializeEmbindInWorklet(processorOptions.wasmBinary)
          .then(() => {
            start();
            port?.postMessage?.({ type: 'ready', runtimeTarget: 'embind' });
          })
          .catch((error: unknown) => {
            port?.postMessage?.({
              type: 'error',
              message: error instanceof Error ? error.message : String(error),
              failure: failurePayload(error),
            });
          });
      }
      const onMessage = (event: { data: unknown }) => {
        if (!isWorkletMessage(event.data)) {
          return;
        }
        if (this.bridge) {
          deliver(this.bridge, event.data);
        } else if (this.pendingMessages.length < 1024) {
          this.pendingMessages.push(event.data);
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
      if (this.bridge) {
        return this.bridge.process(inputs, outputs);
      }
      for (const channel of outputs[0] ?? []) {
        channel.fill(0);
      }
      return true;
    }
  }
  scope.registerProcessor(name, RegisteredSonareWorkletProcessor);
}
