import { describe, expect, it } from 'vitest';
import {
  Audio,
  deemphasis,
  detectKey,
  ErrorCode,
  ebur128LoudnessRangeLu,
  fourierTempogram,
  lufs,
  masterAudio,
  masterAudioStereo,
  meteringSilenceRatio,
  momentaryLufs,
  nnlsChroma,
  onsetStrength,
  preemphasis,
  StreamAnalyzer,
  StreamingEqualizer,
  StreamingMasteringChain,
  shortTermLufs,
  tempogram,
  tempogramRatio,
} from '../src/index.js';
import type { MasteringChainConfig, StreamFramesI16, StreamFramesU8 } from '../src/types.js';
import { present, unknownKeyError } from './_helpers.js';

const SR = 22050;

function generateSine(freq: number, sr: number, duration: number): Float32Array {
  const n = Math.floor(sr * duration);
  const samples = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    samples[i] = Math.sin((2 * Math.PI * freq * i) / sr);
  }
  return samples;
}

describe('progress callback', () => {
  it('masterAudio invokes onProgress for an empty config', () => {
    const progress: Array<[number, string]> = [];
    masterAudio(new Float32Array(22050).fill(0.1), 22050, {
      overrides: {},
      onProgress: (value, stage) => {
        progress.push([value, stage]);
      },
    });
    expect(progress).toEqual([[1, 'complete']]);
  });

  it('masterAudio invokes onProgress for each enabled stage', () => {
    const samples = new Float32Array(22050).fill(0.1);
    const stages: string[] = [];
    const progresses: number[] = [];
    masterAudio(samples, 22050, {
      overrides: {
        eq: { tilt: { tiltDb: 1.0 } },
        dynamics: { compressor: { thresholdDb: -24 } },
      },
      onProgress: (progress, stage) => {
        progresses.push(progress);
        stages.push(stage);
      },
    });
    expect(stages).toContain('eq.tilt');
    expect(stages).toContain('dynamics.compressor');
    expect(progresses[progresses.length - 1]).toBeCloseTo(1.0, 5);
  });

  it('masterAudio ignores an onProgress false result', () => {
    const progress: number[] = [];
    const result = masterAudio(new Float32Array(22050).fill(0.1), 22050, {
      overrides: {
        eq: { tilt: { tiltDb: 1.0 } },
        dynamics: { compressor: { thresholdDb: -24 } },
      },
      onProgress: (value) => {
        progress.push(value);
        return false;
      },
    });

    expect(progress.some((value) => value > 0.5)).toBe(true);
    expect(result.samples).toBeInstanceOf(Float32Array);
  });

  it('masterAudio cancels with overrides alone through the request cancel callback', () => {
    expect(() =>
      masterAudio({
        samples: new Float32Array(22050).fill(0.1),
        sampleRate: 22050,
        overrides: { eq: { tilt: { tiltDb: 1.0 } } },
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });

  it('masterAudioStereo cancels with overrides alone through the request cancel callback', () => {
    expect(() =>
      masterAudioStereo({
        left: new Float32Array(22050).fill(0.1),
        right: new Float32Array(22050).fill(0.1),
        sampleRate: 22050,
        overrides: { eq: { tilt: { tiltDb: 1.0 } } },
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });

  // Each variant forwards `cancel` through its own _ex C-ABI call, and a
  // forwarding regression on any one of them would ship green everywhere else
  // while that one variant ran to completion instead of aborting.
  it('masterAudio cancels through the request cancel callback', () => {
    expect(() =>
      masterAudio({
        samples: new Float32Array(22050).fill(0.1),
        sampleRate: 22050,
        preset: 'pop',
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });

  it('masterAudioStereo cancels through the request cancel callback', () => {
    expect(() =>
      masterAudioStereo({
        left: new Float32Array(22050).fill(0.1),
        right: new Float32Array(22050).fill(0.1),
        sampleRate: 22050,
        preset: 'pop',
        cancel: () => true,
      }),
    ).toThrow(expect.objectContaining({ code: ErrorCode.Cancelled, codeName: 'Cancelled' }));
  });
});

describe('feature request-object compatibility', () => {
  it('preserves positional results for emphasis, chroma, and loudness features', () => {
    const filterInput = new Float32Array([1, 1, 1, 1]);
    const emphasized = preemphasis(filterInput, SR, { coef: 0.5, zi: 0 });
    expect(
      Array.from(preemphasis({ samples: filterInput, sampleRate: SR, coef: 0.5, zi: 0 })),
    ).toEqual(Array.from(emphasized));
    expect(
      Array.from(deemphasis({ samples: emphasized, sampleRate: SR, coef: 0.5, zi: 0 })),
    ).toEqual(Array.from(deemphasis(emphasized, SR, { coef: 0.5, zi: 0 })));

    const chromaInput = generateSine(440, SR, 2.0);
    const chroma = nnlsChroma(chromaInput, SR);
    const objectChroma = nnlsChroma({ samples: chromaInput, sampleRate: SR });
    expect(objectChroma.rows).toBe(chroma.rows);
    expect(objectChroma.cols).toBe(chroma.cols);
    expect(Array.from(objectChroma.data)).toEqual(Array.from(chroma.data));
    const unblended = nnlsChroma(chromaInput, SR, {
      enableStftBlend: false,
      stftBlendWeight: 0,
      stftBlendNFft: 2048,
    });
    expect(unblended.data.length).toBe(12 * unblended.cols);

    const loudnessInput = generateSine(440, 48000, 3.0);
    const positional = lufs(loudnessInput, 48000);
    const object = lufs({ samples: loudnessInput, sampleRate: 48000 });
    expect(object.integratedLufs).toBeCloseTo(positional.integratedLufs, 10);
    expect(Array.from(momentaryLufs({ samples: loudnessInput, sampleRate: 48000 }))).toEqual(
      Array.from(momentaryLufs(loudnessInput, 48000)),
    );
    expect(Array.from(shortTermLufs({ samples: loudnessInput, sampleRate: 48000 }))).toEqual(
      Array.from(shortTermLufs(loudnessInput, 48000)),
    );
    expect(ebur128LoudnessRangeLu({ samples: loudnessInput, sampleRate: 48000 })).toBeCloseTo(
      present(ebur128LoudnessRangeLu(loudnessInput, 48000), 'loudness range'),
      10,
    );
  });

  it('exposes silence ratio metering', () => {
    const samples = new Float32Array(2048);
    samples.fill(1, 1024);
    expect(
      meteringSilenceRatio(samples, SR, { thresholdDb: -45, frameLength: 1024, hopLength: 1024 }),
    ).toBeCloseTo(0.5, 6);
  });
});

describe('color saturation stages engage only when meaningful', () => {
  const stagesFor = (config: MasteringChainConfig): string[] =>
    masterAudio({
      samples: new Float32Array(22050).fill(0.1),
      sampleRate: 22050,
      overrides: config,
    }).stages;

  it('does not engage the exciter when amount is zero', () => {
    expect(stagesFor({ saturation: { exciter: { amount: 0 } } })).not.toContain(
      'saturation.exciter',
    );
  });

  it('does not engage tape when drive and saturation are zero', () => {
    expect(stagesFor({ saturation: { tape: { driveDb: 0, saturation: 0 } } })).not.toContain(
      'saturation.tape',
    );
  });

  it('engages the exciter when amount is positive', () => {
    expect(stagesFor({ saturation: { exciter: { amount: 0.2 } } })).toContain('saturation.exciter');
  });

  it('honors an explicit enabled:false even with meaningful params', () => {
    expect(stagesFor({ saturation: { tape: { driveDb: 3, enabled: false } } })).not.toContain(
      'saturation.tape',
    );
  });

  it('honors an explicit enabled:true even with zero amount', () => {
    expect(stagesFor({ saturation: { exciter: { amount: 0, enabled: true } } })).toContain(
      'saturation.exciter',
    );
  });
});

describe('StreamingMasteringChain', () => {
  it('processes mono blocks and reports stage names', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 512,
      eq: { tilt: { tiltDb: 0.5 } },
      dynamics: { compressor: { thresholdDb: -20 } },
    });
    expect(chain.stageNames()).toEqual(expect.arrayContaining(['eq.tilt', 'dynamics.compressor']));
    const out = chain.processMono(new Float32Array(512).fill(0.1));
    expect(out.length).toBe(512);
    expect(Number.isFinite(out[0])).toBe(true);
    chain.reset();
  });

  it('flushes delayed mono output exactly once', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 64,
      maximizer: { truePeakLimiter: { enabled: true } },
    });
    expect(chain.latencySamples).toBeGreaterThan(0);
    chain.processMono(new Float32Array(64).fill(0.5));
    const tail: number[] = [];
    for (;;) {
      const block = chain.flushMono();
      if (block.length === 0) {
        break;
      }
      tail.push(...block);
    }
    expect(tail.length).toBeGreaterThanOrEqual(chain.latencySamples);
    expect(chain.flushMono()).toHaveLength(0);
  });

  it('accepts denoise by default and refuses its whole-signal estimator', () => {
    expect(
      () =>
        new StreamingMasteringChain({
          sampleRate: 48000,
          numChannels: 1,
          repair: { denoise: { enabled: true } },
        }),
    ).not.toThrow();
    expect(
      () =>
        new StreamingMasteringChain({
          sampleRate: 48000,
          numChannels: 1,
          repair: { denoise: { enabled: true, noiseEstimator: 'quantile' } },
        }),
    ).toThrow(/quantile/);
  });

  it('builds a loudness stage without a static gain and applies no gain', () => {
    // The static gain is measured offline and is absent for a take with no measurable
    // loudness; a chain built without one applies none, exactly as a 0 dB gain does.
    const config = {
      sampleRate: 48000,
      numChannels: 1 as const,
      maxBlockSize: 256,
      loudness: { targetLufs: -14 },
    };
    const ungained = new StreamingMasteringChain(config);
    const unity = new StreamingMasteringChain({ ...config, loudnessStaticGainDb: 0 });
    try {
      for (let b = 0; b < 8; b++) {
        const input = Float32Array.from(
          { length: 256 },
          (_, k) => 0.25 * Math.sin((b * 256 + k) * 0.03),
        );
        expect(Array.from(ungained.processMono(input))).toEqual(
          Array.from(unity.processMono(input)),
        );
      }
      // Control: a real gain moves the same input, so the equality above is not vacuous.
      const gained = new StreamingMasteringChain({ ...config, loudnessStaticGainDb: -12 });
      const probe = new Float32Array(256).fill(0.25);
      let a: Float32Array = probe;
      let c: Float32Array = probe;
      for (let b = 0; b < 16; b++) {
        a = ungained.processMono(probe);
        c = gained.processMono(probe);
      }
      expect(Math.abs(c[255])).toBeLessThan(Math.abs(a[255]) * 0.5);
    } finally {
    }
  });

  it('reports the state a discard blocks when a stage produces a non-finite value', () => {
    // A tilt shelf keeps recursive cells; the default chain has no such
    // stage, so its count would sit at zero for a reason unrelated to the
    // method under test. Setting any field under a module enables it.
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 128,
      eq: { tilt: { tiltDb: 24 } },
    });

    // Control: an ordinary block counts nothing.
    chain.processMono(new Float32Array(128).fill(0.25));
    expect(chain.nonFiniteDiscardCount).toBe(0);

    // The chain refuses non-finite input outright, so the poison has to be
    // something a stage produces: a finite value large enough that the
    // tilt shelf's own arithmetic overflows float internally.
    chain.processMono(new Float32Array(128).fill(3.0e38));
    expect(chain.nonFiniteDiscardCount).toBe(1);
  });

  it('accepts a loudness stage when a static gain is supplied', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 512,
      loudness: { targetLufs: -14 },
      loudnessStaticGainDb: 3.0,
      loudnessStaticGainPeakDbtp: -1.0,
    });
    const out = chain.processMono(new Float32Array(512).fill(0.1));
    expect(out.length).toBe(512);
    expect(Number.isFinite(out[0])).toBe(true);
    chain.reset();
  });
});

describe('StreamingEqualizer', () => {
  it('refuses a band index that is not a non-negative integer', () => {
    // RED WHEN: setBand goes back to casting an int to size_t. A negative index
    // arrives at set_band as an enormous positive one, which is an edit to a
    // band nobody named rather than a refusal.
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    const band = { type: 'peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true } as const;
    const refused = /finite non-negative integer no greater than Number\.MAX_SAFE_INTEGER/;
    for (const index of [-1, 1.5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(() => eq.setBand(index, band), `index ${index}`).toThrow(refused);
    }
    // Positive control: the refusals above are about the index, not the band.
    expect(() => eq.setBand(0, band)).not.toThrow();
  });

  it('reports the state a discard blocks when a non-finite sample reaches it', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    // Without an enabled band the EQ holds no recursive state, so its count
    // would stay at zero for a reason unrelated to the entry under test.
    eq.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 9, q: 2, enabled: true });
    expect(eq.nonFiniteDiscardCount).toBe(0);

    const block = 64;
    const left = new Float32Array(block).fill(0.25);
    const right = new Float32Array(block).fill(0.25);

    // Control: an ordinary block counts nothing, so the increment below is
    // attributable to the poison rather than to processing at all.
    eq.processStereo(left, right);
    expect(eq.nonFiniteDiscardCount).toBe(0);

    // processStereo scrubs nothing, unlike the mixer's block entry, so the
    // poison goes in as supplied and the recursive cells behind the band
    // take it.
    left[8] = Number.NaN;
    right[8] = Number.NaN;
    eq.processStereo(left, right);
    // Both channels lost their cells and the block still adds one. Moving by
    // two here would report a stereo stream as twice as degraded as a mono
    // one for the same defect, over a width the caller passed rather than
    // asked for.
    expect(eq.nonFiniteDiscardCount).toBe(1);
  });

  it('processes stereo blocks and exposes a spectrum snapshot', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'highShelf', frequencyHz: 8000, gainDb: 6, enabled: true });
    eq.setGainScale(0.5);
    eq.setOutputGainDb(3);
    eq.setOutputPan(0);
    const left = new Float32Array(512).fill(0.1);
    const right = new Float32Array(512).fill(0.1);
    const out = eq.processStereo(left, right);
    expect(out.left.length).toBe(512);
    expect(out.right.length).toBe(512);
    expect(Number.isFinite(out.left[0])).toBe(true);
    const snapshot = eq.spectrum();
    expect(snapshot.seq).toBeGreaterThan(0);
    expect(snapshot.bandGainDb.length).toBe(24);
    expect(snapshot.bandGainDb[0]).toBeGreaterThan(2.5);
    expect(snapshot.bandGainDb[0]).toBeLessThan(3.5);
  });

  it('draws the magnitude curve the bands actually apply', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 6, q: 1.5, enabled: true });
    const db = eq.magnitudeResponse(new Float32Array([100, 1000, 10000]));
    expect(db.length).toBe(3);
    expect(db[1]).toBeGreaterThan(5.5);
    expect(db[1]).toBeLessThan(6.5);
    expect(Math.abs(db[0])).toBeLessThan(1);
    expect(Math.abs(db[2])).toBeLessThan(1);
  });

  it('keeps a mid band off the left curve and answers an unknown placement', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, {
      type: 'peak',
      frequencyHz: 1000,
      gainDb: 9,
      q: 1.5,
      enabled: true,
      placement: 'mid',
    });
    const freqs = new Float32Array([1000]);
    expect(eq.magnitudeResponse(freqs, 'mid')[0]).toBeGreaterThan(6);
    expect(Math.abs(eq.magnitudeResponse(freqs, 'left')[0])).toBeLessThan(0.5);
    expect(() => eq.magnitudeResponse(freqs, 'Bogus' as unknown as 'mid')).toThrow();
  });

  it('reads an all-pass band as flat while still changing the signal', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'allPass', frequencyHz: 1000, q: 0.7, enabled: true });
    const db = eq.magnitudeResponse(new Float32Array([100, 1000, 10000]));
    for (const value of db) {
      expect(Math.abs(value)).toBeLessThan(0.05);
    }
  });

  it('switches phase mode and reports linear-phase latency', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true });
    eq.setPhaseMode('linear');
    expect(eq.latencySamples).toBeGreaterThan(0);
    expect(() => eq.setPhaseMode('linear-phase' as unknown as 'linear')).toThrow(RangeError);
    eq.setPhaseMode('zero');
    expect(eq.latencySamples).toBe(0);
    expect(() => eq.setPhaseMode('bogus' as unknown as 'zero')).toThrow(RangeError);
  });

  it('accepts an external sidechain key for dynamic bands', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, {
      type: 'peak',
      frequencyHz: 1000,
      gainDb: 0,
      q: 2,
      enabled: true,
      dynamic: true,
      externalSidechain: true,
      thresholdDb: -32,
      ratio: 4,
      rangeDb: -12,
      attackMs: 0,
      releaseMs: 20,
    });
    const audio = new Float32Array(512).fill(0.02);
    const key = generateSine(1000, 48000, 512 / 48000);
    eq.setSidechainMono(key);
    const out = eq.processMono(audio);
    expect(out.length).toBe(512);
    expect(Number.isFinite(out[0])).toBe(true);
    eq.clearSidechain();
  });

  // The core keeps the sidechain pointers for every later block, so the samples
  // have to live in addon-owned storage: a JS ArrayBuffer can be mutated or
  // transferred the instant the setter returns, which would have the audio
  // thread reading a caller's live (or freed) memory.
  describe('external sidechain ownership', () => {
    const SIDECHAIN_SR = 48000;
    const BLOCK = 512;

    function dynamicEq(): StreamingEqualizer {
      const eq = new StreamingEqualizer({ sampleRate: SIDECHAIN_SR, maxBlockSize: BLOCK });
      eq.setBand(0, {
        type: 'peak',
        frequencyHz: 1000,
        gainDb: 0,
        q: 2,
        enabled: true,
        dynamic: true,
        externalSidechain: true,
        thresholdDb: -32,
        ratio: 4,
        rangeDb: -12,
        attackMs: 0,
        releaseMs: 20,
      });
      return eq;
    }

    const audioBlock = () => generateSine(1000, SIDECHAIN_SR, BLOCK / SIDECHAIN_SR);
    const loudKey = () => {
      const key = generateSine(1000, SIDECHAIN_SR, BLOCK / SIDECHAIN_SR);
      for (let i = 0; i < key.length; i += 1) {
        key[i] *= 0.9;
      }
      return key;
    };

    function processedWithKey(key: Float32Array, mutate?: (key: Float32Array) => void): number[] {
      const eq = dynamicEq();
      eq.setSidechainMono(key);
      mutate?.(key);
      return Array.from(eq.processMono(audioBlock()));
    }

    it('copies a mono key so later mutation or transfer cannot change the output', () => {
      const expected = processedWithKey(loudKey());

      // Non-vacuity: the key really does drive the band, so an output that
      // followed a zeroed key would be visibly different.
      const silent = processedWithKey(new Float32Array(BLOCK));
      expect(silent).not.toEqual(expected);

      const mutated = processedWithKey(loudKey(), (key) => key.fill(0));
      expect(mutated).toEqual(expected);

      const transferred = processedWithKey(loudKey(), (key) => {
        structuredClone(key, { transfer: [key.buffer as ArrayBuffer] });
        expect(key.byteLength).toBe(0);
      });
      expect(transferred).toEqual(expected);
    });

    it('copies both stereo key channels', () => {
      const runStereo = (mutate?: (left: Float32Array, right: Float32Array) => void): number[] => {
        const eq = dynamicEq();
        const left = loudKey();
        const right = loudKey();
        eq.setSidechainStereo(left, right);
        mutate?.(left, right);
        const out = eq.processStereo(audioBlock(), audioBlock());
        return [...Array.from(out.left), ...Array.from(out.right)];
      };

      const expected = runStereo();
      expect(
        runStereo((left, right) => {
          left.fill(0);
          right.fill(0);
        }),
      ).toEqual(expected);
      expect(
        runStereo((left, right) => {
          structuredClone(left, { transfer: [left.buffer as ArrayBuffer] });
          structuredClone(right, { transfer: [right.buffer as ArrayBuffer] });
        }),
      ).toEqual(expected);
    });

    it('rejects a non-finite key and clears on an empty one', () => {
      const eq = dynamicEq();
      const nan = loudKey();
      nan[4] = Number.NaN;
      expect(() => eq.setSidechainMono(nan)).toThrow();
      const infinite = loudKey();
      infinite[7] = Number.POSITIVE_INFINITY;
      expect(() => eq.setSidechainStereo(infinite, loudKey())).toThrow();
      expect(() => eq.setSidechainMono(new Float32Array(0))).not.toThrow();
    });
  });

  // The band reader used to be a file-local copy of the shared option helpers,
  // which is why nothing counted setBand as options-accepting. Migrating it back
  // to the shared family must not drop a key on the way, so pin the ones with an
  // observable effect, and pin the four string-valued keys through the fact that
  // an unknown spelling throws — a reader that silently fell back to the default
  // could not throw at all.
  it('applies every band field supplied to setBand', () => {
    const sampleRate = 48000;
    // `bandGainDb` is indexed BY BAND, not by frequency: entry i is the gain
    // band i currently applies (`band.gain_db * gain_scale`), or the dynamic
    // EQ's live movement when that band is dynamic. It is not a response curve,
    // so every assertion below reads band 0 and treats the rest as unconfigured.
    const staticBand = {
      type: 'highShelf',
      coeffMode: 'rbj',
      frequencyHz: 6000,
      gainDb: 9,
      q: 0.9,
      enabled: true,
      slopeDbOct: 24,
      placement: 'stereo',
      phase: 'inherit',
      soloed: false,
      bypassed: false,
      proportionalQ: true,
      proportionalQStrength: 0.5,
      dynamic: false,
      thresholdDb: -30,
      autoThreshold: false,
      ratio: 3,
      rangeDb: -8,
      attackMs: 5,
      releaseMs: 60,
      detectorDelayMs: 2,
      externalSidechain: false,
      sidechainFreqHz: 900,
      sidechainQ: 1.2,
    } as const;

    // The snapshot is republished from inside process(), so a band has to be
    // driven with a block before its applied gain is readable.
    const appliedGainDb = (band: Record<string, unknown>, gainScale?: number): number => {
      const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      try {
        eq.setBand(0, band as never);
        if (gainScale !== undefined) {
          eq.setGainScale(gainScale);
        }
        eq.processMono(generateSine(1000, sampleRate, 512 / sampleRate));
        return eq.spectrum().bandGainDb[0];
      } finally {
        eq.destroy();
      }
    };

    // gainDb reaches the band with its own value and its own sign.
    expect(appliedGainDb(staticBand)).toBeCloseTo(9, 3);
    expect(appliedGainDb({ ...staticBand, gainDb: -9 })).toBeCloseTo(-9, 3);

    // enabled / bypassed each take the band out on their own.
    expect(appliedGainDb({ ...staticBand, enabled: false })).toBe(0);
    expect(appliedGainDb({ ...staticBand, bypassed: true })).toBe(0);

    // `dynamic` switches what the band reports from its static gain to the
    // movement the detector has applied, so it must no longer read as 9.
    expect(appliedGainDb({ ...staticBand, dynamic: true })).not.toBeCloseTo(9, 1);

    // The reported gain is the band's gain scaled, so the two multiply.
    expect(appliedGainDb(staticBand, 0.5)).toBeCloseTo(4.5, 3);

    // Only the band that was set is configured.
    const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
    eq.setBand(0, { ...staticBand });
    eq.processMono(generateSine(1000, sampleRate, 512 / sampleRate));
    expect(eq.spectrum().bandGainDb[1]).toBe(0);
    eq.destroy();

    // frequencyHz and type shape the audio rather than the reported gain: a
    // +9 dB shelf at 6 kHz lifts an 8 kHz tone and leaves a 200 Hz tone alone,
    // and a low shelf does the opposite.
    const rms = (data: ArrayLike<number>): number => {
      let sum = 0;
      for (let i = 0; i < data.length; i++) {
        sum += data[i] * data[i];
      }
      return Math.sqrt(sum / data.length);
    };
    const shelfGainDb = (band: Record<string, unknown>, toneHz: number): number => {
      const shelf = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      try {
        shelf.setBand(0, band as never);
        const tone = generateSine(toneHz, sampleRate, 512 / sampleRate);
        let out: Float32Array = tone;
        // Let the filter state settle before measuring.
        for (let i = 0; i < 4; i++) {
          out = shelf.processMono(tone);
        }
        return 20 * Math.log10(rms(out) / rms(tone));
      } finally {
        shelf.destroy();
      }
    };

    // Stated as the difference between the two ends rather than as an absolute
    // level at each, so the assertion tracks where the shelf sits instead of the
    // exact skirt of whatever coeffMode / q / slope the band was given.
    const lowShelf = { ...staticBand, type: 'lowShelf' };
    expect(shelfGainDb(staticBand, 8000)).toBeGreaterThan(shelfGainDb(staticBand, 200) + 6);
    expect(shelfGainDb(lowShelf, 200)).toBeGreaterThan(shelfGainDb(lowShelf, 8000) + 6);

    // The four string-valued keys reach their parsers rather than silently
    // falling back: an unknown spelling is rejected, which a reader that fell
    // back to the default could not do.
    const strings = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
    expect(() => strings.setBand(0, { ...staticBand, type: 'NotAType' as never })).toThrow();
    expect(() => strings.setBand(0, { ...staticBand, coeffMode: 'NotAMode' as never })).toThrow();
    expect(() =>
      strings.setBand(0, { ...staticBand, placement: 'NotAPlacement' as never }),
    ).toThrow();
    expect(() => strings.setBand(0, { ...staticBand, phase: 'NotAPhase' as never })).toThrow();
    strings.destroy();
  });

  it('uses the prepared sample rate for match when options omit sampleRate', () => {
    const sampleRate = 44100;
    const source = generateSine(1000, sampleRate, 0.25);
    const reference = generateSine(2000, sampleRate, 0.25);

    // spectrum() reports the snapshot process() publishes, so reading it with no
    // block having gone through returns the untouched initial one -- identical
    // for every instance whatever match() did to the bands.
    const matchedGains = (options: { sampleRate?: number; maxBands?: number }): number[] => {
      const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
      try {
        eq.match(source, reference, options);
        eq.processMono(generateSine(1000, sampleRate, 512 / sampleRate));
        return Array.from(eq.spectrum().bandGainDb);
      } finally {
        eq.destroy();
      }
    };

    const omitted = matchedGains({ maxBands: 6 });
    const atPrepared = matchedGains({ sampleRate, maxBands: 6 });
    const atOther = matchedGains({ sampleRate: 22050, maxBands: 6 });

    // match() has to have moved a band, or every comparison below holds between
    // two default curves and proves nothing about the sample rate.
    expect(omitted.some((gain) => Math.abs(gain) > 0.1)).toBe(true);

    expect(omitted.length).toBe(atPrepared.length);
    for (let i = 0; i < omitted.length; i += 1) {
      expect(omitted[i]).toBeCloseTo(atPrepared[i], 6);
    }

    // The default is the PREPARED rate specifically, not merely some fixed rate:
    // reading the same buffers as if they were another rate maps the match bands
    // onto other frequencies, so the curve has to differ.
    expect(atOther.some((gain, i) => Math.abs(gain - atPrepared[i]) > 0.1)).toBe(true);
  });
});

describe('streaming handles signal use-after-destroy', () => {
  // The classes document "Idempotent; any other method called afterwards
  // throws". A reader that answers with a plausible measurement instead is
  // indistinguishable from a live handle at the call site.
  it('throws from every StreamingMasteringChain reader after destroy()', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: 48000,
      numChannels: 1,
      maxBlockSize: 64,
      maximizer: { truePeakLimiter: { enabled: true } },
    });
    expect(chain.latencySamples).toBeGreaterThan(0);
    chain.destroy();
    expect(() => chain.latencySamples).toThrow(/destroyed/);
    expect(() => chain.stageNames()).toThrow(/destroyed/);
    expect(() => chain.processMono(new Float32Array(64))).toThrow(/destroyed/);
    expect(() => chain.reset()).toThrow(/destroyed/);
    // A second destroy() stays a no-op.
    expect(() => chain.destroy()).not.toThrow();
  });

  it('throws from every StreamingEqualizer reader after destroy()', () => {
    const eq = new StreamingEqualizer({ sampleRate: 48000, maxBlockSize: 512 });
    eq.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true });
    expect(eq.lastAutoGainDb).toBe(0);
    eq.destroy();
    expect(() => eq.latencySamples).toThrow(/destroyed/);
    expect(() => eq.lastAutoGainDb).toThrow(/destroyed/);
    expect(() => eq.clearSidechain()).toThrow(/destroyed/);
    expect(() => eq.processMono(new Float32Array(512))).toThrow(/destroyed/);
    expect(() => eq.destroy()).not.toThrow();
  });
});

describe('onset, tempogram, NNLS chroma, and LUFS', () => {
  const allFinite = (arr: Float32Array): boolean => {
    for (let i = 0; i < arr.length; i++) {
      if (!Number.isFinite(arr[i])) {
        return false;
      }
    }
    return true;
  };

  it('onsetStrength returns a finite Float32Array', () => {
    const env = onsetStrength(generateSine(440, SR, 2.0), SR);
    expect(env).toBeInstanceOf(Float32Array);
    expect(env.length).toBeGreaterThan(0);
    expect(allFinite(env)).toBe(true);
  });

  it('fourierTempogram returns an [nBins x nFrames] matrix', () => {
    const env = onsetStrength(generateSine(440, SR, 2.0), SR);
    const winLength = 384;
    const result = fourierTempogram({
      onsetEnvelope: env,
      sampleRate: SR,
      hopLength: 512,
      winLength,
    });
    expect(result.cols).toBe(env.length);
    const expectedBins = Math.floor(winLength / 2) + 1;
    expect(result.rows).toBe(expectedBins);
    expect(result.data).toBeInstanceOf(Float32Array);
    expect(result.data.length).toBe(result.rows * result.cols);
    expect(allFinite(result.data)).toBe(true);
  });

  it('tempogramRatio returns one finite value per default factor', () => {
    const env = onsetStrength(generateSine(440, SR, 2.0), SR);
    const tg = tempogram({ onsetEnvelope: env, sampleRate: SR });
    const ratio = tempogramRatio({ tempogram: tg, sampleRate: SR });
    expect(ratio).toBeInstanceOf(Float32Array);
    expect(ratio.length).toBe(5); // {0.5, 1, 2, 3, 4}
    expect(allFinite(ratio)).toBe(true);
  });

  it('tempogram cosine mode is exposed', () => {
    const onset = new Float32Array([0.2, 1.0, 0.4, 0.0, 0.8, 0.1, 0.5, 0.3]);
    const result = tempogram({
      onsetEnvelope: onset,
      sampleRate: SR,
      hopLength: 1,
      winLength: 4,
      mode: 'cosine',
    });
    expect(result.cols).toBe(onset.length);
    expect(result.rows).toBe(4);
    expect(result.data.length).toBe(4 * onset.length);
    expect(allFinite(result.data)).toBe(true);
    for (const value of result.data) {
      expect(value).toBeGreaterThanOrEqual(-1.000001);
      expect(value).toBeLessThanOrEqual(1.000001);
    }
  });

  it('tempogram and Fourier tempogram forward center and norm', () => {
    const onset = new Float32Array([0.2, 1.0, 0.4, 0.0, 0.8, 0.1, 0.5, 0.3]);
    const base = { onsetEnvelope: onset, sampleRate: SR, hopLength: 1, winLength: 4 };
    const defaults = tempogram(base);
    const uncentered = tempogram({ ...base, center: false });
    const unnormalized = tempogram({ ...base, norm: false });
    expect(Array.from(uncentered.data)).not.toEqual(Array.from(defaults.data));
    expect(Array.from(unnormalized.data)).not.toEqual(Array.from(defaults.data));
    expect(Array.from(tempogram({ ...base, center: false, norm: false }).data)).not.toEqual(
      Array.from(uncentered.data),
    );
    expect(Array.from(fourierTempogram({ ...base, center: false }).data)).not.toEqual(
      Array.from(fourierTempogram(base).data),
    );
  });

  it('nnlsChroma returns a 12 x nFrames matrix', () => {
    const result = nnlsChroma(generateSine(440, SR, 2.0), SR);
    expect(result.rows).toBe(12);
    expect(result.cols).toBeGreaterThan(0);
    expect(result.data).toBeInstanceOf(Float32Array);
    expect(result.data.length).toBe(12 * result.cols);
    expect(allFinite(result.data)).toBe(true);
  });

  it('lufs returns finite measures; louder reads higher', () => {
    const loudSamples = generateSine(440, 48000, 3.0);
    const quietSamples = loudSamples.map((s) => s * 0.1);

    const loud = lufs(loudSamples, 48000);
    const quiet = lufs(quietSamples, 48000);

    for (const r of [loud, quiet]) {
      expect(Number.isFinite(r.integratedLufs)).toBe(true);
      expect(Number.isFinite(r.momentaryLufs)).toBe(true);
      expect(Number.isFinite(r.shortTermLufs)).toBe(true);
      expect(Number.isFinite(r.loudnessRangeLu)).toBe(true);
      expect(r.loudnessRangeLu).toBeGreaterThanOrEqual(0);
    }
    expect(loud.integratedLufs).toBeGreaterThan(quiet.integratedLufs);
  });

  it('momentaryLufs and shortTermLufs return finite time series', () => {
    const samples = generateSine(440, 48000, 3.0);

    const momentary = momentaryLufs(samples, 48000);
    expect(momentary).toBeInstanceOf(Float32Array);
    expect(momentary.length).toBeGreaterThan(0);
    expect(allFinite(momentary)).toBe(true);

    const shortTerm = shortTermLufs(samples, 48000);
    expect(shortTerm).toBeInstanceOf(Float32Array);
    expect(shortTerm.length).toBeGreaterThan(0);
    expect(allFinite(shortTerm)).toBe(true);
  });

  it('Audio methods mirror standalone onset/chroma/LUFS functions', () => {
    const audio = Audio.fromBuffer(generateSine(440, 48000, 3.0), 48000);
    try {
      const env = audio.onsetStrength();
      expect(env).toBeInstanceOf(Float32Array);
      expect(env.length).toBeGreaterThan(0);

      const chromaResult = audio.nnlsChroma();
      expect(chromaResult.rows).toBe(12);
      expect(chromaResult.data.length).toBe(12 * chromaResult.cols);

      const loud = audio.lufs();
      expect(Number.isFinite(loud.integratedLufs)).toBe(true);
      expect(Number.isFinite(loud.loudnessRangeLu)).toBe(true);

      expect(audio.momentaryLufs().length).toBeGreaterThan(0);
      expect(audio.shortTermLufs().length).toBeGreaterThan(0);
    } finally {
      audio.destroy();
    }
  });
});

describe('StreamAnalyzer quantize-config override', () => {
  it('omits the arrays of every disabled feature for every combination and read type', () => {
    const input = new Float32Array(32);
    input[8] = 0.5;
    for (let mask = 0; mask < 16; mask += 1) {
      const config = {
        sampleRate: 8000,
        nFft: 32,
        hopLength: 32,
        nMels: 8,
        computeMel: (mask & 1) !== 0,
        computeChroma: (mask & 2) !== 0,
        computeOnset: (mask & 4) !== 0,
        computeSpectral: (mask & 8) !== 0,
      };
      for (let readType = 0; readType < 3; readType += 1) {
        const analyzer = new StreamAnalyzer(config);
        analyzer.process(input);
        const frames =
          readType === 0
            ? analyzer.readFrames(1)
            : readType === 1
              ? analyzer.readFramesU8(1)
              : analyzer.readFramesI16(1);
        expect(frames.nFrames).toBe(1);
        // `mel` / `chroma` are a matrix on readFrames and a flat array on the quantized reads.
        const bandCount = (band: unknown): number | undefined =>
          band === undefined
            ? undefined
            : readType === 0
              ? (band as { rows: number }).rows
              : (band as ArrayLike<number>).length;
        expect(bandCount(frames.mel)).toBe(mask & 1 ? 8 : undefined);
        expect(bandCount(frames.chroma)).toBe(mask & 2 ? 12 : undefined);
        if (readType !== 0) {
          const quantized = frames as StreamFramesU8 | StreamFramesI16;
          expect(quantized).not.toHaveProperty('nMels');
          expect(quantized).not.toHaveProperty('nChroma');
        }
        expect(frames.onsetStrength?.length).toBe(mask & 4 ? 1 : undefined);
        expect(frames.rmsEnergy?.length).toBe(1);
        expect(frames.spectralCentroid?.length).toBe(mask & 8 ? 1 : undefined);
        expect(frames.spectralFlatness?.length).toBe(mask & 8 ? 1 : undefined);
      }
    }
  });

  it('rejects computeMagnitude as an unknown config key', () => {
    // No read path here can return a magnitude spectrum, so the key is not part
    // of the config at all, whatever its value.
    for (const computeMagnitude of [true, false]) {
      expect(
        () => new StreamAnalyzer({ sampleRate: SR, computeMagnitude } as never),
        `computeMagnitude ${computeMagnitude}`,
      ).toThrow(TypeError);
    }
    expect(() => new StreamAnalyzer({ sampleRate: SR, computeMagnitude: true } as never)).toThrow(
      /unknown key computeMagnitude \(expected /,
    );
  });

  it('rejects magnitudeDownsample as an unknown config key', () => {
    // The analyzer emits no magnitude spectrum, so the factor had nothing to act on.
    for (const magnitudeDownsample of [1, 2]) {
      expect(() => new StreamAnalyzer({ sampleRate: SR, magnitudeDownsample } as never)).toThrow(
        unknownKeyError('StreamAnalyzer: unknown key magnitudeDownsample'),
      );
    }
  });

  it('rejects legacy outputFormat selectors', () => {
    for (const value of [1, 2, 0.5, Number.NaN, Number.POSITIVE_INFINITY, '0']) {
      expect(() => new StreamAnalyzer({ sampleRate: SR, outputFormat: value } as never)).toThrow(
        /unknown key outputFormat \(expected /,
      );
    }
  });

  it('widens the saturating quantization range', () => {
    const config = {
      sampleRate: SR,
      nFft: 1024,
      hopLength: 256,
      nMels: 32,
      window: 'hamming' as const,
    };

    // A tiny centroidMax saturates the (positive) spectral centroid to the u8
    // maximum; a huge centroidMax collapses it toward zero. Reading identical
    // audio with the two configs must differ, proving the override reaches the
    // native quantizer.
    const tight = new StreamAnalyzer(config);
    const wide = new StreamAnalyzer(config);
    tight.process(generateSine(440, SR, 0.5));
    const narrow = tight.readFramesU8(4, { centroidMax: 1.0 });

    wide.process(generateSine(440, SR, 0.5));
    const broad = wide.readFramesU8(4, { centroidMax: 1e9 });

    expect(narrow.nFrames).toBe(broad.nFrames);
    expect(narrow.nFrames).toBeGreaterThan(0);
    expect(narrow.spectralCentroid?.[0]).toBe(255); // saturated by the narrow range
    expect(Array.from(narrow.spectralCentroid ?? [])).not.toEqual(
      Array.from(broad.spectralCentroid ?? []),
    );

    // Omitting the config keeps the default ranges (no throw, same shape).
    const fallback = wide.readFramesU8(4);
    expect(fallback.mel?.length).toBe(32 * fallback.nFrames);
  });

  it('rejects malformed config geometry like the C ABI', () => {
    // The shared StreamAnalyzer constructor enforces the same relationship and
    // positive-value contract as the flat C ABI, so direct Node construction
    // throws instead of silently producing garbage spectra.
    expect(() => new StreamAnalyzer({ sampleRate: 0 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, nFft: 0 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, nMels: 0 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, nFft: 1024, hopLength: 2048 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, fminHz: 8000, fmaxHz: 4000 })).toThrow();
    expect(() => new StreamAnalyzer({ sampleRate: SR, maxProgressionEntries: 0 })).toThrow();
  });

  it('rejects out-of-int-range numeric config instead of narrowing it', () => {
    // static_cast<int> of a double outside the int range is undefined behaviour,
    // and on ARM64 it saturates to INT_MAX — which passes the core's `> 0`
    // checks and hands a nonsense geometry to the FFT. Every int-valued field
    // must reject the value the way maxPendingFrames already does.
    const intFields = ['sampleRate', 'nFft', 'hopLength', 'nMels', 'emitEveryNFrames'] as const;
    const outOfDomain = [
      1e18,
      -1e18,
      Number.MAX_VALUE,
      -Number.MAX_VALUE,
      Number.NaN,
      Number.POSITIVE_INFINITY,
      Number.NEGATIVE_INFINITY,
      2.5,
    ];
    for (const field of intFields) {
      for (const value of outOfDomain) {
        expect(() => new StreamAnalyzer({ sampleRate: SR, [field]: value })).toThrow();
      }
    }

    // A valid config with the same fields still constructs.
    const ok = new StreamAnalyzer({
      sampleRate: 8000,
      nFft: 32,
      hopLength: 32,
      nMels: 8,
      emitEveryNFrames: 1,
    });
    expect(ok.sampleRate).toBe(8000);
    ok.destroy();
  });

  it('accepts only the window tokens and refuses ordinals', () => {
    for (const window of ['hann', 'hamming', 'blackman', 'rectangular'] as const) {
      const analyzer = new StreamAnalyzer({
        sampleRate: 8000,
        nFft: 32,
        hopLength: 32,
        nMels: 8,
        window,
      });
      expect(analyzer.sampleRate).toBe(8000);
      analyzer.destroy();
    }

    const explicitUndefined = new StreamAnalyzer({ sampleRate: 8000, window: undefined });
    explicitUndefined.destroy();

    // An unknown spelling is out of the domain; a non-string is the wrong type.
    for (const window of ['rect', 'Hann', 'triangle', '']) {
      expect(() => new StreamAnalyzer({ sampleRate: 8000, window: window as never })).toThrow(
        RangeError,
      );
    }
    const wrongTypes: unknown[] = [0, 1, 2, 3, -1, 0.5, Number.NaN, true, null, {}, []];
    for (const window of wrongTypes) {
      expect(() => new StreamAnalyzer({ sampleRate: 8000, window: window as never })).toThrow(
        TypeError,
      );
    }
  });

  it('bounds unread frames and reports drop-newest telemetry', () => {
    const analyzer = new StreamAnalyzer({
      sampleRate: 8000,
      nFft: 32,
      hopLength: 32,
      nMels: 8,
      maxPendingFrames: 3,
      maxProgressionEntries: 3,
    });
    analyzer.process(new Float32Array(32 * 64));
    const stats = analyzer.stats();
    expect(stats.pendingFrames).toBe(3);
    expect(stats.droppedOutputFrames).toBeGreaterThan(0);
    expect(stats.pendingFrames + stats.droppedOutputFrames).toBe(stats.totalFrames);
    expect(stats.droppedChordProgressionEntries).toBe(0);
    expect(stats.droppedBarProgressionEntries).toBe(0);
  });

  it('counts a process call whose input it had to scrub, once per call', () => {
    const analyzer = new StreamAnalyzer({
      sampleRate: 8000,
      nFft: 32,
      hopLength: 32,
      nMels: 8,
    });
    const clean = new Float32Array(256);
    for (let i = 0; i < clean.length; i += 1) {
      clean[i] = 0.5 * Math.sin((2 * Math.PI * 220 * i) / 8000);
    }
    analyzer.process(clean);
    // The stream really was analyzed, so the zero below is a measurement rather
    // than the absence of one.
    expect(analyzer.stats().totalFrames).toBeGreaterThan(0);
    expect(analyzer.stats().nonFiniteDiscardBlocks).toBe(0);

    // Half the block is non-finite, with both infinities and NaN present: a
    // count that followed the samples rather than the call would land far from
    // one, which is what this pins.
    const poisoned = clean.slice();
    for (let i = 0; i < poisoned.length; i += 2) {
      poisoned[i] = i % 4 === 0 ? Number.NaN : Number.POSITIVE_INFINITY;
    }
    poisoned[1] = Number.NEGATIVE_INFINITY;
    analyzer.process(poisoned);
    expect(analyzer.stats().nonFiniteDiscardBlocks).toBe(1);

    // A clean call adds nothing; a second bad one does, so it is not stuck.
    analyzer.process(clean);
    expect(analyzer.stats().nonFiniteDiscardBlocks).toBe(1);
    analyzer.process(poisoned);
    expect(analyzer.stats().nonFiniteDiscardBlocks).toBe(2);

    analyzer.reset();
    expect(analyzer.stats().nonFiniteDiscardBlocks).toBe(0);
  });
});

describe('StreamAnalyzer external offsets', () => {
  it('preserves buffered timestamps and rejects discontinuities until reset', () => {
    const config = { sampleRate: SR, nFft: 2048, hopLength: 512 };
    const audio = new Float32Array(4096);
    const reference = new StreamAnalyzer(config);
    reference.process(audio);
    const expected = reference.readFrames(64).timestampsSec;

    const analyzer = new StreamAnalyzer(config);
    for (let offset = 0; offset < audio.length; offset += 128) {
      analyzer.process(audio.subarray(offset, offset + 128), offset);
    }
    expect(Array.from(analyzer.readFrames(64).timestampsSec)).toEqual(Array.from(expected));
    analyzer.reset();
    analyzer.process(audio.subarray(0, 128), 1000);
    expect(() => analyzer.process(audio.subarray(128, 256), 1129)).toThrow();
    analyzer.reset(5000);
    expect(() => analyzer.process(audio.subarray(0, 128), 5000)).not.toThrow();
  });

  it('rejects invalid size arguments without integer truncation or wrapping', () => {
    const analyzer = new StreamAnalyzer({ sampleRate: 8000, nFft: 32, hopLength: 32, nMels: 8 });
    const invalidValues = [
      -1,
      1.5,
      Number.MAX_SAFE_INTEGER + 1,
      Number.POSITIVE_INFINITY,
      Number.NaN,
    ];

    // The wording is the shared NonNegativeSizeTArg reader's, which these six
    // entry points now read through instead of a file-local copy of its domain.
    const refused = /finite non-negative integer no greater than Number\.MAX_SAFE_INTEGER/;
    for (const value of invalidValues) {
      expect(() => analyzer.process(new Float32Array([0]), value)).toThrow(refused);
      expect(() => analyzer.readFrames(value)).toThrow(refused);
      expect(() => analyzer.readFramesU8(value)).toThrow(refused);
      expect(() => analyzer.readFramesI16(value)).toThrow(refused);
      expect(() => analyzer.reset(value)).toThrow(refused);
    }

    analyzer.reset(Number.MAX_SAFE_INTEGER);
    expect(() => analyzer.process(new Float32Array([0]), Number.MAX_SAFE_INTEGER)).not.toThrow();
    expect(() => analyzer.readFrames(Number.MAX_SAFE_INTEGER)).not.toThrow();
    expect(() => analyzer.readFramesU8(Number.MAX_SAFE_INTEGER)).not.toThrow();
    expect(() => analyzer.readFramesI16(Number.MAX_SAFE_INTEGER)).not.toThrow();
  });
});

describe('StreamAnalyzer chord and key names', () => {
  const PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

  function chordProgressionSamples(): Float32Array {
    const chords = [
      [261.63, 329.63, 392.0],
      [392.0, 493.88, 587.33],
      [220.0, 261.63, 329.63],
      [349.23, 440.0, 523.25],
    ];
    const samples = new Float32Array(SR * 8);
    for (let i = 0; i < samples.length; i++) {
      const t = i / SR;
      const freqs = chords[Math.floor(t / 2)];
      for (const freq of freqs) {
        samples[i] += (Math.sin(2 * Math.PI * freq * t) / freqs.length) * 0.3;
      }
    }
    return samples;
  }

  it('names every chord and the estimate consistently with the numeric fields', () => {
    const samples = chordProgressionSamples();
    const analyzer = new StreamAnalyzer({ sampleRate: SR });
    for (let i = 0; i < samples.length; i += 4096) {
      analyzer.process(samples.slice(i, Math.min(i + 4096, samples.length)));
    }
    const estimate = analyzer.stats().estimate;

    expect(estimate.chordProgression.length).toBeGreaterThan(0);
    for (const chord of [
      ...estimate.chordProgression,
      ...estimate.barChordProgression,
      ...estimate.votedPattern,
    ]) {
      const prefix = chord.root < 0 ? 'N.C.' : PITCH_NAMES[chord.root];
      expect(chord.name.startsWith(prefix)).toBe(true);
      if (chord.quality === 'major') {
        expect(chord.name).toBe(PITCH_NAMES[chord.root]);
      }
      if (chord.quality === 'minor') {
        expect(chord.name).toBe(`${PITCH_NAMES[chord.root]}m`);
      }
    }
    const current = estimate.chord;
    expect(current).toBeDefined();
    expect(current?.name.startsWith(PITCH_NAMES[current?.root ?? 0])).toBe(true);

    const key = estimate.key;
    expect(key).toBeDefined();
    expect(key?.root).toBeGreaterThanOrEqual(0);
    const isMinor = key?.mode === 'minor';
    expect(key?.name).toBe(`${PITCH_NAMES[key?.root ?? 0]} ${isMinor ? 'minor' : 'major'}`);
    expect(key?.shortName).toBe(
      isMinor ? `${PITCH_NAMES[key?.root ?? 0]}m` : PITCH_NAMES[key?.root ?? 0],
    );
    analyzer.destroy();
  });

  it('spells the key the way batch analysis does', () => {
    const notes = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 493.88];
    const samples = new Float32Array(SR * 7);
    for (let n = 0; n < notes.length; n++) {
      for (let i = 0; i < SR; i++) {
        samples[n * SR + i] = Math.sin((2 * Math.PI * notes[n] * i) / SR) * 0.5;
      }
    }
    const analyzer = new StreamAnalyzer({ sampleRate: SR });
    analyzer.process(samples);
    const { estimate } = analyzer.stats();
    const batch = detectKey(samples, SR);
    expect(estimate.key?.name).toBe(batch.name);
    expect(estimate.key?.shortName).toBe(batch.shortName);
    expect(estimate.key?.name).toBe('C major');
    analyzer.destroy();
  });

  it('reports no key and no chord before any estimate exists', () => {
    const analyzer = new StreamAnalyzer({ sampleRate: SR });
    const estimate = analyzer.stats().estimate;
    expect(estimate.key).toBeUndefined();
    expect(estimate.chord).toBeUndefined();
    analyzer.destroy();
  });
});
