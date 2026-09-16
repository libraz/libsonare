import { addon } from './native.js';
import type { SampleDesc, SampleZoneDesc } from './types.js';

type NativeSampleBank = InstanceType<typeof addon.SampleBank>;

/**
 * Host-supplied PCM for the sample synthesis engine: a bank of float samples
 * plus key/velocity keymaps, bound alongside a {@link SynthPatch} whose
 * `engineMode` is `'sample'`.
 *
 * {@link Project.loadSoundFont} is the other door into sampled playback — it
 * parses a container and brings its own generator model. This one takes
 * waveforms a host already has, described by nothing but a keymap, so playing
 * them does not require authoring an SF2. Decoding is the caller's job: the
 * bank takes mono float frames.
 *
 * A bank is built on the control thread and then read as immutable data. Add
 * every sample and zone BEFORE the bounce that binds it starts: the pool is
 * contiguous and moves as it grows, so a sample added while something sounds
 * invalidates the voices reading it.
 *
 * The native handle is not garbage-collected, so release it with
 * {@link destroy} (or `using`, which calls it for you).
 *
 * @example
 * ```typescript
 * using bank = new SampleBank();
 * const index = bank.addSample(pcm, { rootKey: 60, sourceRate: 44100 });
 * bank.addZone({ sampleIndex: index });  // the whole keyboard
 *
 * const audio = project.bounceWithSynthInstrument(
 *   { engineMode: 'sample', sampleSet: 0, sampleBank: bank },
 *   { totalFrames: 24000 },
 * );
 * ```
 */
export class SampleBank {
  private native: NativeSampleBank;
  private disposed = false;

  /** Create an empty bank. */
  constructor() {
    this.native = new addon.SampleBank();
  }

  /**
   * Copy mono float frames into the bank and return the new sample's index,
   * which {@link SampleZoneDesc.sampleIndex} names. The frames are copied, so
   * the array may be reused afterwards.
   *
   * Loop points are clamped inside the sample and a loop mode whose loop
   * survives the clamp empty is dropped, so a malformed loop plays as an
   * unlooped sample rather than as a wrap over nothing. An empty array, and a
   * bank that would exceed 67,108,864 sample points, throw.
   *
   * A NaN or Inf frame, `fineTuneCents` or `sourceRate` throws too, and the
   * bank is left unchanged. Such a value is unattributable once stored: the
   * reader's interpolation spreads one bad frame across the whole sustain, and
   * a bad tuning offset renders the voice silent with no error raised.
   */
  addSample(data: Float32Array, desc: SampleDesc = {}): number {
    return this.native.addSample(data, desc);
  }

  /**
   * Append a key/velocity rectangle to a keymap set, creating any sets below
   * it. A patch names a set; the first zone in it covering a note is the one
   * that sounds.
   *
   * An empty zone covers the whole keyboard at every velocity, and every bound
   * defaults on its own, so narrowing one edge leaves the rest covering
   * everything (see {@link SampleZoneDesc}). A `sampleIndex` the bank does not
   * have, an inverted key or velocity range, and a `setIndex` at or above 4096
   * all throw.
   */
  addZone(zone: SampleZoneDesc = {}): void {
    const { setIndex, ...rest } = zone;
    this.native.addZone(setIndex ?? 0, rest);
  }

  /** Samples added so far. */
  sampleCount(): number {
    return this.native.sampleCount();
  }

  /** Keymap sets the bank has (one past the highest index used). */
  setCount(): number {
    return this.native.setCount();
  }

  /** Release the underlying native bank. Idempotent. */
  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /** Alias for {@link destroy}, provided for cross-binding (WASM) compatibility. */
  delete(): void {
    this.destroy();
  }

  /** Releases the native bank; lets `using` (Node 22+) free it automatically. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
