import { closeSync, openSync, readSync } from 'node:fs';
import type { RealtimeEngine } from './realtime_engine.js';
import type { ClipPageRequest, FileClipPageProviderOptions } from './types.js';

export class ClipPageProvider {
  private disposed = false;

  constructor(
    private readonly engine: RealtimeEngine,
    readonly id: number,
  ) {}

  supply(pageIndex: number, channels: Float32Array[]): void {
    if (this.disposed) {
      throw new Error('ClipPageProvider is destroyed');
    }
    this.engine.supplyClipPage(this.id, pageIndex, channels);
  }

  clear(pageIndex: number): void {
    if (this.disposed) {
      return;
    }
    this.engine.clearClipPage(this.id, pageIndex);
  }

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
      throw new Error('numChannels, numSamples, and pageFrames must be positive');
    }
    this.fd = openSync(path, 'r');
    this.numChannels = options.numChannels;
    this.numSamples = options.numSamples;
    this.pageFrames = options.pageFrames;
    this.dataOffsetBytes = options.dataOffsetBytes ?? 0;
  }

  supplyPage(pageIndex: number): boolean {
    if (this.fd === null) {
      throw new Error('FileClipPageProvider is destroyed');
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

  supplyRequest(request: ClipPageRequest): boolean {
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
