import { closeSync, openSync, readSync } from 'node:fs';
import { ErrorCode, SonareError } from './errors.js';
import type { RealtimeEngine } from './realtime_engine.js';
import type { FileClipPageProviderOptions } from './types.js';

/** A clip page the player missed or is about to need, positioned by sample. */
export interface ClipPageSampleRequest {
  /** Clip schedule id passed to `setClips`. */
  clipId: number;
  /** Channel the miss happened on. */
  channel: number;
  /** Sample position within the clip source that missed. */
  sample: number;
}

/** Handle to a paged clip source on a {@link RealtimeEngine}; pages are supplied on demand. */
export class ClipPageProvider {
  private disposed = false;

  constructor(
    private readonly engine: RealtimeEngine,
    readonly id: number,
  ) {}

  /** Hand the engine one page of planar channels. Throws `InvalidState` after `destroy()`. */
  supply(pageIndex: number, channels: Float32Array[]): void {
    if (this.disposed) {
      throw new SonareError(ErrorCode.InvalidState, 'ClipPageProvider is destroyed');
    }
    this.engine.supplyClipPage(this.id, pageIndex, channels);
  }

  /** Evict one resident page; a no-op after `destroy()`. */
  clear(pageIndex: number): void {
    if (this.disposed) {
      return;
    }
    this.engine.clearClipPage(this.id, pageIndex);
  }

  /** Release the provider on the engine. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.engine.destroyClipPageProvider(this.id);
  }

  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** A {@link ClipPageProvider} fed from a headerless interleaved float32 file. */
export class FileClipPageProvider extends ClipPageProvider {
  private fd: number | null;
  private readonly numChannels: number;
  private readonly numSamples: number;
  private readonly pageFrames: number;
  private readonly dataOffsetBytes: number;

  constructor(
    engine: RealtimeEngine,
    id: number,
    path: string,
    options: FileClipPageProviderOptions,
  ) {
    super(engine, id);
    if (options.numChannels <= 0 || options.numSamples <= 0 || options.pageFrames <= 0) {
      throw new RangeError('numChannels, numSamples, and pageFrames must be positive');
    }
    this.fd = openSync(path, 'r');
    this.numChannels = options.numChannels;
    this.numSamples = options.numSamples;
    this.pageFrames = options.pageFrames;
    this.dataOffsetBytes = options.dataOffsetBytes ?? 0;
  }

  /** Read one page from the file and supply it; `false` when the page lies outside the file. */
  supplyPage(pageIndex: number): boolean {
    if (this.fd === null) {
      throw new SonareError(ErrorCode.InvalidState, 'FileClipPageProvider is destroyed');
    }
    if (pageIndex < 0) {
      return false;
    }
    const startFrame = pageIndex * this.pageFrames;
    if (startFrame >= this.numSamples) {
      return false;
    }
    const frames = Math.min(this.pageFrames, this.numSamples - startFrame);
    const frameBytes = this.numChannels * Float32Array.BYTES_PER_ELEMENT;
    const buffer = Buffer.allocUnsafe(frames * frameBytes);
    const bytesRead = readSync(
      this.fd,
      buffer,
      0,
      buffer.byteLength,
      this.dataOffsetBytes + startFrame * frameBytes,
    );
    const framesRead = Math.floor(bytesRead / frameBytes);
    if (framesRead <= 0) {
      return false;
    }
    const channels = Array.from({ length: this.numChannels }, () => new Float32Array(framesRead));
    for (let frame = 0; frame < framesRead; ++frame) {
      for (let ch = 0; ch < this.numChannels; ++ch) {
        channels[ch][frame] = buffer.readFloatLE((frame * this.numChannels + ch) * 4);
      }
    }
    this.supply(pageIndex, channels);
    return true;
  }

  /** Supply the page a sample-positioned miss falls in. */
  supplyRequest(request: ClipPageSampleRequest): boolean {
    return this.supplyPage(Math.floor(request.sample / this.pageFrames));
  }

  destroy(): void {
    if (this.fd !== null) {
      closeSync(this.fd);
      this.fd = null;
    }
    super.destroy();
  }
}
