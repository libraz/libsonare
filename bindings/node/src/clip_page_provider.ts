import { closeSync, openSync, readSync } from 'node:fs';
import { ErrorCode, SonareError } from './errors.js';
import { bindNativeClipPageProvider } from './internal/clip_page_native.js';
import { entryClass } from './internal/entry.js';
import { addon } from './native.js';
import type { ClipPageProviderOptions, FileClipPageProviderOptions } from './types.js';
import { requestObject } from './validation.js';

/** A clip page the player missed or is about to need, positioned by sample. */
export interface ClipPageSampleRequest {
  /** Clip schedule id passed to `setClips`. */
  clipId: number;
  /** Channel the miss happened on. */
  channel: number;
  /** Sample position within the clip source that missed. */
  sample: number;
}

interface NativeClipPageProvider {
  supply(pageIndex: number, channels: Float32Array[]): void;
  clear(pageIndex: number): void;
  destroy(): void;
}

/** A paged clip source: pages are supplied on demand and the provider is passed as a clip's `pageProvider`. */
@entryClass('ClipPageProvider')
export class ClipPageProvider {
  private readonly native: NativeClipPageProvider;

  constructor(options: ClipPageProviderOptions) {
    requestObject('ClipPageProvider', options, 'options', true);
    this.native = new addon.ClipPageProvider(
      options.numChannels,
      options.numSamples,
      options.pageFrames,
    );
    bindNativeClipPageProvider(this, this.native);
  }

  /** Hand the provider one page of planar channels. Throws `InvalidState` after `destroy()`. */
  supply(pageIndex: number, channels: Float32Array[]): void {
    this.native.supply(pageIndex, channels);
  }

  /** Evict one resident page. Throws `InvalidState` after `destroy()`. */
  clear(pageIndex: number): void {
    this.native.clear(pageIndex);
  }

  /** Release the provider; clips already scheduled keep playing. Idempotent. */
  destroy(): void {
    this.native.destroy();
  }

  /** Releases the provider through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

/** A {@link ClipPageProvider} fed from a headerless interleaved float32 file. */
@entryClass('FileClipPageProvider')
export class FileClipPageProvider extends ClipPageProvider {
  private fd: number | null;
  private readonly channelCount: number;
  private readonly frameCount: number;
  private readonly pageFrames: number;
  private readonly dataOffsetBytes: number;

  constructor(options: FileClipPageProviderOptions) {
    requestObject('FileClipPageProvider', options, 'options', true);
    super({
      numChannels: options.numChannels,
      numSamples: options.numSamples,
      pageFrames: options.pageFrames,
    });
    try {
      this.fd = openSync(options.path, 'r');
    } catch (err) {
      super.destroy();
      throw err;
    }
    this.channelCount = options.numChannels;
    this.frameCount = options.numSamples;
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
    if (startFrame >= this.frameCount) {
      return false;
    }
    const frames = Math.min(this.pageFrames, this.frameCount - startFrame);
    const frameBytes = this.channelCount * Float32Array.BYTES_PER_ELEMENT;
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
    const channels = Array.from({ length: this.channelCount }, () => new Float32Array(framesRead));
    for (let frame = 0; frame < framesRead; ++frame) {
      for (let ch = 0; ch < this.channelCount; ++ch) {
        channels[ch][frame] = buffer.readFloatLE((frame * this.channelCount + ch) * 4);
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
