import { createRequire } from 'node:module';
import type { AnalysisResult, Key } from './types.js';

const require = createRequire(import.meta.url);
const addon = require('../build/Release/sonare-node.node');

/**
 * Audio object wrapping decoded audio samples.
 */
export class Audio {
  private native: InstanceType<typeof addon.Audio>;

  private constructor(native: InstanceType<typeof addon.Audio>) {
    this.native = native;
  }

  /**
   * Load audio from a file path.
   */
  static fromFile(path: string): Audio {
    return new Audio(addon.Audio.fromFile(path));
  }

  /**
   * Create audio from a Float32Array of samples.
   */
  static fromBuffer(samples: Float32Array, sampleRate = 22050): Audio {
    return new Audio(addon.Audio.fromBuffer(samples, sampleRate));
  }

  /**
   * Decode audio from an encoded buffer (WAV, MP3, etc.).
   */
  static fromMemory(data: Buffer | Uint8Array): Audio {
    return new Audio(addon.Audio.fromMemory(data));
  }

  /**
   * Get the audio samples as a Float32Array.
   */
  getData(): Float32Array {
    return this.native.getData();
  }

  /**
   * Get the number of samples.
   */
  getLength(): number {
    return this.native.getLength();
  }

  /**
   * Get the sample rate in Hz.
   */
  getSampleRate(): number {
    return this.native.getSampleRate();
  }

  /**
   * Get the duration in seconds.
   */
  getDuration(): number {
    return this.native.getDuration();
  }

  /**
   * Free the underlying native audio resource.
   */
  destroy(): void {
    this.native.destroy();
  }
}

/**
 * Detect BPM (tempo) from audio samples.
 */
export function detectBpm(samples: Float32Array, sampleRate = 22050): number {
  return addon.detectBpm(samples, sampleRate);
}

/**
 * Detect musical key from audio samples.
 */
export function detectKey(samples: Float32Array, sampleRate = 22050): Key {
  return addon.detectKey(samples, sampleRate);
}

/**
 * Detect beat times from audio samples.
 */
export function detectBeats(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectBeats(samples, sampleRate);
}

/**
 * Detect onset times from audio samples.
 */
export function detectOnsets(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.detectOnsets(samples, sampleRate);
}

/**
 * Run full music analysis on audio samples.
 */
export function analyze(samples: Float32Array, sampleRate = 22050): AnalysisResult {
  return addon.analyze(samples, sampleRate);
}

/**
 * Get the libsonare version string.
 */
export function version(): string {
  return addon.version();
}

export type { AnalysisResult, Key, TimeSignature } from './types.js';
